// SecondSonPC — module list, relocation, TLS and program start (Phase 1 Loader).
#include "loader/linker.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "base/log.h"
#include "loader/bindings.h"
#include "loader/elf.h"
#include "loader/host.h"

namespace SecondSon::Loader {

namespace {

Linker* g_active = nullptr;

constexpr u64 kFlexibleMemoryBase = 64ull << 20;  // ORBIS_KERNEL_FLEXIBLE_MEMORY_BASE
constexpr u64 kFlexibleMemorySize = 512ull << 20; // ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE
constexpr u64 kFw500 = 0x5000000;
constexpr u64 kTcbSize = 0x40;
constexpr u64 kTlsAllocAlign = 0x20;

u64 AlignUp(u64 value, u64 align) {
    return (value + align - 1) & ~(align - 1);
}

/// Process entry parameters as the console kernel passes them to the executable's entry.
struct EntryParams {
    int argc;
    u32 padding;
    const char* argv[33];
    u64 entry_addr;
};
EntryParams g_entry_params{};

SSPC_SYSV_ABI void ProgramExit() {
    SSPC_LOG(Loader, Error, "guest called the process exit function");
}

/// Jumps to the executable entry with the console's stack/register layout. Never returns.
[[noreturn]] void RunMainEntry(EntryParams* params) {
    asm volatile("andq $-16, %%rsp\n" // align the stack
                 "subq $8, %%rsp\n"   // the entry expects a misaligned stack, like after a call
                 "pushq 8(%1)\n"      // argc/argv are also found on the stack
                 "pushq 0(%1)\n"
                 "movq %1, %%rdi\n" // rdi = entry params
                 "movq %2, %%rsi\n" // rsi = exit function
                 "jmp *%0\n"        // no return address: there is no coming back
                 :
                 : "r"(params->entry_addr), "r"(params), "r"(&ProgramExit)
                 : "rax", "rsi", "rdi");
    __builtin_unreachable();
}

} // namespace

Linker::Linker(Host& host, const Bindings& bindings, GuestPathResolver resolver)
    : host_{host}, bindings_{bindings}, resolver_{std::move(resolver)} {
    g_active = this;
}

Linker::~Linker() {
    if (g_active == this) {
        g_active = nullptr;
    }
}

Linker* Linker::Active() {
    return g_active;
}

s32 Linker::LoadModule(std::string_view guest_path, bool is_dynamic) {
    std::scoped_lock lock{mutex_};
    const auto host_path = resolver_(guest_path);
    if (host_path.empty()) {
        SSPC_LOG(Loader, Error, "{}: no host file", guest_path);
        return -1;
    }
    const s32 id = static_cast<s32>(modules_.size());
    std::string error;
    auto module = Module::Load(host_, host_path, std::string(guest_path), id, max_tls_index_, error);
    if (!module) {
        SSPC_LOG(Loader, Error, "cannot load {} ({}): {}", guest_path, host_path.string(), error);
        return -1;
    }
    num_static_modules_ += is_dynamic ? 0 : 1;
    modules_.push_back(std::move(module));
    return id;
}

s32 Linker::LoadAndStartModule(std::string_view guest_path, u64 args, const void* argp,
                               s32* result) {
    {
        std::scoped_lock lock{mutex_};
        for (const auto& m : modules_) {
            if (m->guest_path == guest_path) {
                return m->id;
            }
        }
    }
    const s32 handle = LoadModule(guest_path, true);
    if (handle < 0) {
        return -1;
    }
    Module& module = *GetModule(handle);
    RelocateAnyImports(module);

    // A new TLS image makes __tls_get_addr grow each thread's DTV lazily.
    if (module.tls.image_size != 0) {
        std::scoped_lock lock{mutex_};
        ++generation_;
    }

    auto* param = reinterpret_cast<ProcParam*>(module.proc_param);
    if (param != nullptr && param->size < 0x18) {
        SSPC_LOG(Loader, Error, "{}: invalid module param size {:#x}", module.name, param->size);
    }
    const s32 ret = module.Start(args, argp, param);
    SSPC_LOG(Loader, Info, "{}: module start returned {:#x}", module.name, static_cast<u32>(ret));
    if (result != nullptr) {
        *result = ret;
    }
    return handle;
}

Module* Linker::GetModule(s32 handle) const {
    std::scoped_lock lock{mutex_};
    return handle >= 0 && static_cast<size_t>(handle) < modules_.size() ? modules_[handle].get()
                                                                        : nullptr;
}

size_t Linker::ModuleCount() const {
    std::scoped_lock lock{mutex_};
    return modules_.size();
}

Module* Linker::FindByAddress(u64 address) const {
    std::scoped_lock lock{mutex_};
    for (const auto& m : modules_) {
        if (m->Contains(address)) {
            return m.get();
        }
    }
    return nullptr;
}

ProcParam* Linker::GetProcParam() const {
    std::scoped_lock lock{mutex_};
    return modules_.empty() ? nullptr : reinterpret_cast<ProcParam*>(modules_[0]->proc_param);
}

u64 Linker::SdkVersion() const {
    const auto* p = GetProcParam();
    return p != nullptr ? p->sdk_version : 0;
}

void Linker::RelocateAll() {
    std::scoped_lock lock{mutex_};
    for (auto& m : modules_) {
        Relocate(*m);
    }
}

void Linker::RelocateAnyImports(Module& m) {
    std::scoped_lock lock{mutex_};
    Relocate(m);
    for (const auto& exported : m.ExportModules()) {
        for (auto& other : modules_) {
            const auto imports = other->ImportModules();
            if (std::any_of(imports.begin(), imports.end(),
                            [&](const ModuleRef& r) { return r.name == exported.name; })) {
                Relocate(*other);
            }
        }
    }
}

void Linker::Relocate(Module& m) {
    const auto relas = m.Relocations();
    const auto plts = m.PltRelocations();
    const u32 num_relas = static_cast<u32>(relas.size());
    u64 applied = 0, pending = 0;

    const auto process = [&](const Elf::Rela& rel, u32 index, bool plt) {
        const u32 bit = (plt ? num_relas : 0) + index;
        if (m.TestRelaBit(bit)) {
            return;
        }
        const u32 type = rel.Type();
        s64 addend = rel.addend;
        const u64 slot = m.Base() + rel.offset;
        bool resolved = false;
        bool is_function = false;
        u64 value = 0;
        std::string_view sym_name;

        switch (type) {
        case Elf::R_X86_64_RELATIVE:
            value = m.Base() + addend;
            resolved = true;
            m.SetRelaBit(bit);
            break;
        case Elf::R_X86_64_DTPMOD64:
            value = m.tls.modid;
            resolved = true;
            m.SetRelaBit(bit);
            break;
        case Elf::R_X86_64_GLOB_DAT:
        case Elf::R_X86_64_JUMP_SLOT:
            addend = 0;
            [[fallthrough]];
        case Elf::R_X86_64_64: {
            if (rel.SymbolIndex() >= m.SymbolCount()) {
                SSPC_LOG(Loader, Error, "{}+{:#x}: symbol index {} out of range", m.name, rel.offset,
                         rel.SymbolIndex());
                return;
            }
            const auto& sym = m.Symbols()[rel.SymbolIndex()];
            const u8 sym_type = sym.Type();
            if (sym_type != Elf::STT_FUNC && sym_type != Elf::STT_OBJECT && sym_type != Elf::STT_NOTYPE) {
                SSPC_LOG(Loader, Error, "{}+{:#x}: unknown symbol type {}", m.name, rel.offset, sym_type);
                return;
            }
            is_function = sym_type == Elf::STT_FUNC;
            u64 sym_address = 0;
            switch (sym.Bind()) {
            case Elf::STB_LOCAL:
                sym_address = m.Base() + sym.value;
                m.SetRelaBit(bit);
                break;
            case Elf::STB_GLOBAL:
            case Elf::STB_WEAK:
                sym_name = m.String(sym.name);
                if (Resolve(m, sym_name, sym_type, sym_address)) {
                    m.SetRelaBit(bit);
                }
                break;
            default:
                SSPC_LOG(Loader, Error, "{}+{:#x}: unknown symbol binding {}", m.name, rel.offset,
                         sym.Bind());
                return;
            }
            resolved = sym_address != 0;
            value = resolved ? sym_address + addend : 0;
            break;
        }
        default:
            SSPC_LOG(Loader, Info, "{}+{:#x}: unsupported relocation type {:#x}", m.name, rel.offset, type);
        }

        if (resolved) {
            std::memcpy(reinterpret_cast<void*>(slot), &value, sizeof(value));
            ++applied;
        } else if (is_function) {
            ++pending;
            SSPC_LOG(Loader, Debug, "{}+{:#x} ({}): function not patched", m.name, rel.offset, sym_name);
        }
    };

    for (u32 i = 0; i < relas.size(); ++i) {
        process(relas[i], i, false);
    }
    for (u32 i = 0; i < plts.size(); ++i) {
        process(plts[i], i, true);
    }
    SSPC_LOG(Loader, Debug, "relocated {}: {} slots written, {} functions pending", m.name, applied,
             pending);
}

bool Linker::Resolve(const Module& m, std::string_view name, u8 sym_type, u64& address) {
    address = 0;
    const auto h1 = name.find('#');
    const auto h2 = h1 == std::string_view::npos ? h1 : name.find('#', h1 + 1);
    const bool three_parts = h2 != std::string_view::npos && name.find('#', h2 + 1) == std::string_view::npos;
    const LibraryRef* library = nullptr;
    const ModuleRef* module = nullptr;
    std::string_view nid = name;
    if (three_parts) {
        nid = name.substr(0, h1);
        library = m.FindLibrary(name.substr(h1 + 1, h2 - h1 - 1));
        module = m.FindModule(name.substr(h2 + 1));
    } else if (h1 == std::string_view::npos && sym_type == Elf::STT_NOTYPE) {
        library = m.FindLibrary("");
        module = m.FindModule("");
    } else {
        SSPC_LOG(Loader, Error, "{}: cannot resolve symbol {}", m.name, name);
        return false;
    }
    if (library == nullptr || module == nullptr) {
        SSPC_LOG(Loader, Error, "{}: symbol {} names an unknown library/module", m.name, name);
        return false;
    }

    // 1) the binding table (NATIVE / OBSERVED / BRIDGE / COMMON_STUB)
    if (const Binding* b = bindings_.Find(nid, library->name, module->name)) {
        if (b->owner == Owner::Unresolved) {
            return false; // object nobody provides: the slot keeps its file contents
        }
        address = b->address;
        return b->owner != Owner::CommonStub; // stub slots are retried on later passes
    }

    // 2) exports of the loaded game modules (LLE: libc, libSceFios2)
    for (const auto& other : modules_) {
        if (!other->ExportsLibrary(*library) || !other->ExportsModule(*module)) {
            continue;
        }
        if (const u64 a = other->FindExport(nid, library->name, module->name)) {
            address = a;
            return true;
        }
    }

    // 3) not available (yet): functions get a logging stub, objects stay unresolved.
    const bool in_table = std::any_of(Bindings::ImportTable().begin(), Bindings::ImportTable().end(),
                                      [&](const ImportInfo& i) {
                                          return nid == i.nid && library->name == i.library &&
                                                 module->name == i.module;
                                      });
    if (!in_table) {
        SSPC_LOG(Loader, Warning, "{}: import {} ({}/{}) is not in the binding table", m.name, nid,
                 library->name, module->name);
    }
    if (sym_type != Elf::STT_OBJECT) {
        address = PendingImportStub(nid, library->name);
    }
    return false;
}

// ---------------------------------------------------------------------------------------- TLS

void* Linker::AllocateTls(bool primary) {
    const u64 total = AlignUp(static_tls_size_, kTlsAllocAlign) + kTcbSize;
    if (primary) {
        return host_.MapPrimaryTls(AlignUp(total, 16ull << 10));
    }
    if (heap_api_ != nullptr && heap_api_->heap_malloc != nullptr) {
        return heap_api_->heap_malloc(total);
    }
    return std::malloc(total);
}

void Linker::FreeTls(void* pointer) {
    if (heap_api_ != nullptr && heap_api_->heap_free != nullptr) {
        heap_api_->heap_free(pointer);
    } else {
        std::free(pointer);
    }
}

Module* Linker::ModuleByTlsId(u64 modid) const {
    for (const auto& m : modules_) {
        if (m->tls.modid == modid) {
            return m.get();
        }
    }
    return nullptr;
}

GuestTcb* Linker::CreateTcb(void* thread, bool initial) {
    std::scoped_lock lock{tcb_mutex_};
    void* block = AllocateTls(initial);
    if (block == nullptr) {
        SSPC_LOG(Loader, Critical, "cannot allocate a guest TCB (initial={})", initial);
        return nullptr;
    }
    const u32 num_dtvs = max_tls_index_;
    auto* dtv = new DtvEntry[num_dtvs + 2]{};

    u8* base = static_cast<u8*>(block);
    auto* tcb = reinterpret_cast<GuestTcb*>(base + static_tls_size_);
    std::memset(block, 0, static_tls_size_);
    tcb->self = tcb;
    tcb->dtv = dtv;
    dtv[0].counter = generation_; // generation; libkernel keeps the DTV size in dtv[1]
    dtv[1].counter = num_dtvs;

    // Static TLS holds the executable's image only; other modules are allocated lazily.
    const Module& main = *modules_[0];
    u8* dest = base + static_tls_size_ - main.tls.offset;
    if (main.tls.image_size != 0) {
        if (main.tls.image_vaddr != 0) {
            std::memcpy(dest, reinterpret_cast<const void*>(main.tls.image_vaddr),
                        main.tls.init_image_size);
        }
        dtv[main.tls.modid + 1].pointer = dest;
    }
    tcb->thread = thread;
    tcb->fiber = nullptr;
    return tcb;
}

void Linker::DestroyTcb(GuestTcb* tcb) {
    std::scoped_lock lock{tcb_mutex_};
    auto* dtv = tcb->dtv;
    const u64 num_dtvs = dtv[1].counter;
    if (num_dtvs > max_tls_index_) {
        SSPC_LOG(Loader, Critical, "DTV of TCB {} is out of bounds ({} > {})", static_cast<void*>(tcb),
                 num_dtvs, max_tls_index_);
        return;
    }
    u8* tls_base = reinterpret_cast<u8*>(tcb) - static_tls_size_;
    for (u64 i = 1; i < num_dtvs; ++i) {
        u8* p = dtv[i + 1].pointer;
        if (p != nullptr && (p < tls_base || reinterpret_cast<u8*>(tcb) < p)) {
            FreeTls(p);
        }
    }
    delete[] dtv;
    FreeTls(tls_base);
}

void* Linker::TlsGetAddr(u64 modid, u64 offset) {
    std::scoped_lock lock{mutex_};
    auto* tcb = static_cast<GuestTcb*>(host_.CurrentTcb());
    DtvEntry* dtv = tcb->dtv;
    if (dtv[0].counter != generation_) {
        // A dynamic module was loaded since this DTV was built: grow it.
        const u64 old_count = dtv[1].counter;
        if (max_tls_index_ <= old_count) {
            SSPC_LOG(Loader, Critical, "TLS module unloading is not supported");
        }
        auto* grown = new DtvEntry[max_tls_index_ + 2]{};
        std::memcpy(grown + 2, dtv + 2, old_count * sizeof(DtvEntry));
        grown[0].counter = generation_;
        grown[1].counter = max_tls_index_;
        delete[] dtv;
        tcb->dtv = grown;
        dtv = grown;
    }
    u8* addr = dtv[modid + 1].pointer;
    if (addr == nullptr) {
        const Module* m = ModuleByTlsId(modid);
        if (m == nullptr) {
            SSPC_LOG(Loader, Critical, "__tls_get_addr: no module with TLS id {}", modid);
            return nullptr;
        }
        u8* dest = nullptr;
        if (heap_api_ != nullptr && heap_api_->heap_malloc != nullptr) {
            dest = static_cast<u8*>(heap_api_->heap_malloc(m->tls.image_size));
        } else {
            dest = static_cast<u8*>(std::malloc(m->tls.image_size));
        }
        std::memcpy(dest, reinterpret_cast<const void*>(m->tls.image_vaddr), m->tls.init_image_size);
        std::memset(dest + m->tls.init_image_size, 0, m->tls.image_size - m->tls.init_image_size);
        dtv[modid + 1].pointer = dest;
        addr = dest;
    }
    return addr + offset;
}

void Linker::SetHeapApi(void** functions) {
    heap_api_ = reinterpret_cast<HeapApi*>(functions);
    SSPC_LOG(Loader, Info, "application heap API set ({})", static_cast<void*>(functions));
}

// ------------------------------------------------------------------------------ program start

void Linker::Execute(std::vector<std::string> args, std::function<void()> preload,
                     std::function<void()> before_entry) {
    Module& main = *modules_[0];
    static_tls_size_ = main.tls.offset = main.tls.image_size;
    SSPC_LOG(Loader, Info, "static TLS: {} bytes ({} TLS modules so far)", static_tls_size_, max_tls_index_);
    SSPC_LOG(Loader, Info, "libSceLibcInternal: no LLE module; its imports are served by the binding table");

    RelocateAll();

    // Direct/flexible memory regions from the executable's SceKernelMemParam.
    const ProcParam* proc = GetProcParam();
    u64 flexible = kFlexibleMemorySize;
    bool extended1 = true;
    bool extended2 = true;
    KernelMemParam mem{};
    if (proc != nullptr && proc->size >= offsetof(ProcParam, mem_param) + sizeof(void*) &&
        proc->mem_param != nullptr) {
        mem = *proc->mem_param;
        if (mem.size >= offsetof(KernelMemParam, flexible_memory_size) + sizeof(u64*) &&
            mem.flexible_memory_size != nullptr) {
            flexible = *mem.flexible_memory_size + kFlexibleMemoryBase;
        }
    }
    if (mem.size < offsetof(KernelMemParam, extended_memory_1) + sizeof(u64*)) {
        mem.extended_memory_1 = nullptr;
    }
    if (mem.size < offsetof(KernelMemParam, extended_memory_2) + sizeof(u64*)) {
        mem.extended_memory_2 = nullptr;
    }
    if (proc != nullptr && proc->sdk_version < kFw500) {
        extended1 = mem.extended_memory_1 != nullptr && *mem.extended_memory_1 != 0;
        extended2 = mem.extended_memory_2 != nullptr && *mem.extended_memory_2 != 0;
    }
    SSPC_LOG(Loader, Info, "SDK {:#x}, flexible memory {:#x}, extended memory {}/{}",
             proc != nullptr ? proc->sdk_version : 0, flexible, extended1, extended2);
    host_.SetupMemoryRegions(flexible, extended1, extended2);

    args_ = std::move(args);
    preload_ = std::move(preload);
    before_entry_ = std::move(before_entry);
    host_.StartGuestThread([this] { MainThread(); });
}

void Linker::MainThread() {
    host_.SetCurrentThreadName("Game:Main");
    if (preload_) {
        preload_(); // libSceSysmodule preloads the game's PRX (libSceFios2, libc)
    }
    if (!host_.InitGnmDriverMemory()) {
        SSPC_LOG(Loader, Critical, "cannot emulate libSceGnmDriver's start-up mapping");
    }
    constexpr int kMaxArgs = sizeof(g_entry_params.argv) / sizeof(g_entry_params.argv[0]);
    g_entry_params.argc = static_cast<int>(std::min<size_t>(args_.size(), kMaxArgs));
    for (int i = 0; i < g_entry_params.argc; ++i) {
        g_entry_params.argv[i] = args_[i].c_str();
    }
    g_entry_params.entry_addr = modules_[0]->EntryAddress();
    SSPC_LOG(Loader, Info, "entering {} at {:#x} (argc {})", modules_[0]->name,
             g_entry_params.entry_addr, g_entry_params.argc);
    if (before_entry_) {
        before_entry_();
    }
    Log::Flush();
    RunMainEntry(&g_entry_params);
}

} // namespace SecondSon::Loader
