// SecondSonPC — one loaded PS4 module: mapping, dynamic info, exports.
#include "loader/module.h"

#include <algorithm>
#include <cstring>

#include "base/log.h"
#include "loader/eh_frame.h"
#include "loader/host.h"
#include "loader/image_file.h"

namespace SecondSon::Loader {

namespace {

// Load bases used by the console loader (and the Phase 0 oracle).
constexpr u64 kExecutableLoadBase = 0x400000;
constexpr u64 kGameModuleLoadBase = 0x80000000;
constexpr u64 kSystemModuleLoadBase = 0x800000000;
constexpr u64 kBlockAlign = 0x1000;

u64 AlignUp(u64 value, u64 align) {
    return (value + align - 1) & ~(align - 1);
}

u64 AlignedSize(const Elf::ProgramHeader& p) {
    return p.align != 0 ? (p.memsz + (p.align - 1)) & ~(p.align - 1) : p.memsz;
}

const char* ProtName(u32 flags) {
    static constexpr const char* kNames[8] = {"(___)", "(__X)", "(_W_)", "(_WX)",
                                              "(R__)", "(R_X)", "(RW_)", "(RWX)"};
    return kNames[flags & 7];
}

const char* PhdrTypeName(u32 type) {
    switch (type) {
    case Elf::PT_LOAD:
        return "Loadable";
    case Elf::PT_DYNAMIC:
        return "Dynamic";
    case Elf::PT_INTERP:
        return "Interpreter Path";
    case Elf::PT_TLS:
        return "Thread-Local Storage";
    case Elf::PT_SCE_DYNLIBDATA:
        return "SCE Dynamic Library Data";
    case Elf::PT_SCE_PROCPARAM:
        return "SCE Process Parameters";
    case Elf::PT_SCE_MODULE_PARAM:
        return "SCE Module Parameters";
    case Elf::PT_SCE_RELRO:
        return "SCE Read-Only After Relocation";
    case Elf::PT_GNU_EH_FRAME:
        return "GNU Entry Header Frame";
    case Elf::PT_SCE_COMMENT:
        return "SCE Comment";
    case Elf::PT_SCE_LIBVERSION:
        return "SCE Library Version";
    default:
        return "?";
    }
}

/// Module/library descriptor value: u32 name offset, then version, then u16 id.
ModuleRef MakeModuleRef(u64 value, const char* strtab, u64 strtab_size) {
    ModuleRef r;
    const u32 name_offset = static_cast<u32>(value & 0xFFFFFFFF);
    r.name = name_offset < strtab_size ? strtab + name_offset : "";
    r.version_minor = static_cast<u8>((value >> 32) & 0xFF);
    r.version_major = static_cast<u8>((value >> 40) & 0xFF);
    r.id = static_cast<u16>(value >> 48);
    r.enc_id = EncodeId(r.id);
    return r;
}

LibraryRef MakeLibraryRef(u64 value, const char* strtab, u64 strtab_size) {
    LibraryRef r;
    const u32 name_offset = static_cast<u32>(value & 0xFFFFFFFF);
    r.name = name_offset < strtab_size ? strtab + name_offset : "";
    r.version = static_cast<u16>((value >> 32) & 0xFFFF);
    r.id = static_cast<u16>(value >> 48);
    r.enc_id = EncodeId(r.id);
    return r;
}

} // namespace

std::string EncodeId(u64 value) {
    static constexpr std::string_view kCodes =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+-";
    std::string enc;
    if (value < 0x40) {
        enc += kCodes[value];
    } else if (value < 0x1000) {
        enc += kCodes[(value >> 6) & 0x3F];
        enc += kCodes[value & 0x3F];
    } else {
        enc += kCodes[(value >> 12) & 0x3F];
        enc += kCodes[(value >> 6) & 0x3F];
        enc += kCodes[value & 0x3F];
    }
    return enc;
}

std::unique_ptr<Module> Module::Load(Host& host, const std::filesystem::path& host_path,
                                     std::string guest_path, s32 id, u32& max_tls_index,
                                     std::string& error) {
    ImageFile file;
    if (!file.Open(host_path, error)) {
        return nullptr;
    }
    auto m = std::unique_ptr<Module>(new Module());
    m->host_path = host_path;
    m->guest_path = std::move(guest_path);
    m->name = host_path.filename().string();
    m->id = id;
    if (!m->Map(host, file, max_tls_index, error) || !m->ParseDynamic(error)) {
        return nullptr;
    }
    m->CollectExports();
    return m;
}

bool Module::Map(Host& host, const ImageFile& file, u32& max_tls_index, std::string& error) {
    const auto& header = file.Header();
    e_type_ = header.type;
    entry_ = header.entry;
    const auto phdrs = file.ProgramHeaders();

    base_size = 0;
    for (const auto& p : phdrs) {
        if (p.memsz != 0 && (p.type == Elf::PT_LOAD || p.type == Elf::PT_SCE_RELRO)) {
            base_size = std::max(base_size, p.vaddr + AlignedSize(p));
        }
    }
    aligned_base_size = AlignUp(base_size, kBlockAlign);

    const bool is_executable = e_type_ == Elf::ET_SCE_EXEC || e_type_ == Elf::ET_SCE_DYNEXEC;
    constexpr bool kSystemModule = false; // SecondSonPC never loads firmware modules
    const u64 load_base = is_executable    ? kExecutableLoadBase
                          : kSystemModule ? kSystemModuleLoadBase
                                          : kGameModuleLoadBase;
    base_ = host.ReserveModule(load_base, aligned_base_size + kTrampolineSize, name);
    if (base_ == 0) {
        error = fmt::format("cannot reserve {:#x} bytes for {}", aligned_base_size + kTrampolineSize, name);
        return false;
    }
    SSPC_LOG(Loader, Info, "loading module {} to {:#x}", name, base_);

    const u64 trampoline = base_ + aligned_base_size;
    if (!host.MapTrampoline(trampoline, kTrampolineSize, name)) {
        error = fmt::format("cannot map the trampoline area of {}", name);
        return false;
    }
    host.RegisterPatchModule(base_, aligned_base_size, trampoline, kTrampolineSize, name);
    SSPC_LOG(Loader, Info, "{}: base {:#x}, size {:#x}, aligned {:#x}", name, base_, base_size,
             aligned_base_size);

    const bool static_patching = host.StaticPatchingEnabled();
    std::vector<std::pair<u64, u64>> executable_segments;
    std::vector<uintptr_t> function_starts;
    std::vector<CodeRange> functions;

    const auto add_segment_info = [this](const Elf::ProgramHeader& p) {
        if (segment_count < segments.size()) {
            segments[segment_count++] = SegmentInfo{base_ + p.vaddr, static_cast<u32>(AlignedSize(p)),
                                                    static_cast<s32>(p.flags)};
        } else {
            SSPC_LOG(Loader, Error, "{}: too many segments", name);
        }
    };

    for (size_t i = 0; i < phdrs.size(); ++i) {
        const auto& p = phdrs[i];
        switch (p.type) {
        case Elf::PT_LOAD:
        case Elf::PT_SCE_RELRO: {
            if (p.memsz == 0) {
                SSPC_LOG(Loader, Error, "{}: p_memsz == 0 in {}", name, PhdrTypeName(p.type));
                continue;
            }
            const u64 address = base_ + p.vaddr;
            const u64 size = AlignedSize(p);
            SSPC_LOG(Loader, Info, "{}: [{}] {} at {:#x}, file {:#x}, memory {:#x} {}", name, i,
                     PhdrTypeName(p.type), address, p.filesz, size, ProtName(p.flags));
            if (!host.MapSegment(address, size, p.flags, kSystemModule, name)) {
                error = fmt::format("{}: cannot map segment {} at {:#x}", name, i, address);
                return false;
            }
            if (!file.Read(p.offset, p.filesz, reinterpret_cast<void*>(address))) {
                error = fmt::format("{}: segment {} is not stored in the file", name, i);
                return false;
            }
            add_segment_info(p);
            if ((p.flags & Elf::PF_EXEC) != 0) {
                host.PrePatch(address, p.filesz);
                if (static_patching) {
                    executable_segments.emplace_back(address, p.filesz);
                }
            }
            break;
        }
        case Elf::PT_DYNAMIC:
            add_segment_info(p);
            if (p.filesz != 0) {
                dynamic_ = file.ReadSegment(p);
            }
            if (dynamic_.empty()) {
                error = fmt::format("{}: empty PT_DYNAMIC", name);
                return false;
            }
            break;
        case Elf::PT_SCE_DYNLIBDATA:
            if (p.filesz != 0) {
                dynamic_data_ = file.ReadSegment(p);
            }
            if (dynamic_data_.empty()) {
                error = fmt::format("{}: empty PT_SCE_DYNLIBDATA", name);
                return false;
            }
            break;
        case Elf::PT_TLS:
            tls.init_image_size = static_cast<u32>(p.filesz);
            tls.align = static_cast<u32>(p.align);
            tls.image_vaddr = base_ + p.vaddr;
            tls.image_size = static_cast<u32>(AlignedSize(p));
            tls.modid = ++max_tls_index;
            SSPC_LOG(Loader, Info, "{}: TLS image {:#x}, size {} (init {}), align {}, modid {}", name,
                     tls.image_vaddr, tls.image_size, tls.init_image_size, tls.align, tls.modid);
            break;
        case Elf::PT_SCE_PROCPARAM:
            proc_param = base_ + p.vaddr;
            break;
        case Elf::PT_GNU_EH_FRAME: {
            eh_frame_hdr_addr = p.vaddr;
            eh_frame_hdr_size = static_cast<u32>(p.memsz);
            const u64 start = base_ + p.vaddr;
            const u64 end = start + p.memsz;
            EhFrameHeader hdr;
            std::string eh_error;
            if (DecodeEhFrameHeader(start, end, hdr, eh_error)) {
                if (static_patching && !DecodeEhFrameTable(hdr, end, function_starts, eh_error)) {
                    SSPC_LOG(Loader, Error, "{}: .eh_frame_hdr table: {}", name, eh_error);
                }
                if (static_patching) {
                    // Exact function ranges for SecondSonPC's FS patcher. A range inside
                    // .eh_frame/.eh_frame_hdr is data, never decoded as code (the FDEs of functions the
                    // linker discarded point there; eboot.bin has ~3400, none of them in the search table).
                    const u64 frame_end = hdr.eh_frame_ptr < start ? start : base_ + aligned_base_size;
                    std::string fde_error;
                    if (!DecodeEhFrameFunctions(hdr, end, frame_end, functions, fde_error)) {
                        SSPC_LOG(Loader, Error, "{}: .eh_frame FDEs: {}", name, fde_error);
                        functions.clear();
                    }
                    const u64 data_lo = std::min(hdr.eh_frame_ptr, start);
                    const u64 data_hi = std::max(frame_end, end);
                    std::erase_if(functions,
                                  [&](const CodeRange& f) { return f.begin < data_hi && data_lo < f.end; });
                }
                eh_frame_addr = hdr.eh_frame_ptr - base_;
                eh_frame_size = static_cast<u32>(eh_frame_hdr_addr > eh_frame_addr
                                                     ? eh_frame_hdr_addr - eh_frame_addr
                                                     : aligned_base_size - eh_frame_hdr_addr);
            } else if (!eh_error.empty()) {
                SSPC_LOG(Loader, Error, "{}: .eh_frame_hdr: {}", name, eh_error);
            }
            break;
        }
        default:
            SSPC_LOG(Loader, Debug, "{}: program header [{}] {} ({:#x}) not used", name, i,
                     PhdrTypeName(p.type), p.type);
        }
    }
    SSPC_LOG(Loader, Info, "{}: entry {:#x}", name, base_ + entry_);

    if (static_patching) {
        StaticPatchSummary total;
        for (const auto& [address, size] : executable_segments) {
            const auto r = host.StaticPatch(address, size, function_starts, functions);
            total.functions += r.functions;
            total.patched += r.patched;
            total.unsupported += r.unsupported;
        }
        if (total.functions == 0 && !executable_segments.empty()) {
            SSPC_LOG(Loader, Warning, "{}: static CPU patching found no function boundaries", name);
        } else if (total.functions != 0) {
            SSPC_LOG(Loader, Info, "{}: static CPU patching: {} functions, {} patched, {} unsupported",
                     name, total.functions, total.patched, total.unsupported);
        }
    }
    return true;
}

bool Module::ParseDynamic(std::string& error) {
    const auto* begin = reinterpret_cast<const Elf::Dynamic*>(dynamic_.data());
    const size_t count = dynamic_.size() / sizeof(Elf::Dynamic);
    const auto in_data = [this](u64 offset, u64 size) {
        return offset <= dynamic_data_.size() && dynamic_data_.size() - offset >= size;
    };

    // Pass 1: table locations (names in pass 2 need the string table).
    u64 strtab = 0, symtab = 0, symtab_size = 0, rela = 0, rela_size = 0, jmprel = 0, jmprel_size = 0;
    for (size_t i = 0; i < count && begin[i].tag != Elf::DT_NULL; ++i) {
        const auto& d = begin[i];
        switch (d.tag) {
        case Elf::DT_SCE_STRTAB:
            strtab = d.value;
            break;
        case Elf::DT_SCE_STRSZ:
            strtab_size_ = d.value;
            break;
        case Elf::DT_SCE_SYMTAB:
            symtab = d.value;
            break;
        case Elf::DT_SCE_SYMTABSZ:
            symtab_size = d.value;
            break;
        case Elf::DT_SCE_RELA:
            rela = d.value;
            break;
        case Elf::DT_SCE_RELASZ:
            rela_size = d.value;
            break;
        case Elf::DT_SCE_JMPREL:
            jmprel = d.value;
            break;
        case Elf::DT_SCE_PLTRELSZ:
            jmprel_size = d.value;
            break;
        case Elf::DT_INIT:
            init_addr = d.value;
            break;
        case Elf::DT_FINI:
            fini_addr = d.value;
            break;
        default:
            break;
        }
    }
    if (!in_data(strtab, strtab_size_) || !in_data(symtab, symtab_size) || !in_data(rela, rela_size) ||
        !in_data(jmprel, jmprel_size)) {
        error = fmt::format("{}: dynamic tables outside PT_SCE_DYNLIBDATA", name);
        return false;
    }
    strtab_ = reinterpret_cast<const char*>(dynamic_data_.data() + strtab);
    symtab_ = reinterpret_cast<const Elf::Symbol*>(dynamic_data_.data() + symtab);
    symtab_count_ = symtab_size / sizeof(Elf::Symbol);
    rela_ = reinterpret_cast<const Elf::Rela*>(dynamic_data_.data() + rela);
    rela_count_ = rela_size / sizeof(Elf::Rela);
    jmprel_ = reinterpret_cast<const Elf::Rela*>(dynamic_data_.data() + jmprel);
    jmprel_count_ = jmprel_size / sizeof(Elf::Rela);

    // Pass 2: module and library descriptors.
    for (size_t i = 0; i < count && begin[i].tag != Elf::DT_NULL; ++i) {
        const auto& d = begin[i];
        switch (d.tag) {
        case Elf::DT_SCE_NEEDED_MODULE:
            import_modules_.push_back(MakeModuleRef(d.value, strtab_, strtab_size_));
            break;
        case Elf::DT_SCE_MODULE_INFO: {
            auto& m = export_modules_.emplace_back(MakeModuleRef(d.value, strtab_, strtab_size_));
            const std::string full = m.name + ".sprx";
            std::memcpy(info_name.data(), full.data(), std::min(full.size(), info_name.size() - 1));
            break;
        }
        case Elf::DT_SCE_IMPORT_LIB:
            import_libs_.push_back(MakeLibraryRef(d.value, strtab_, strtab_size_));
            break;
        case Elf::DT_SCE_EXPORT_LIB:
            export_libs_.push_back(MakeLibraryRef(d.value, strtab_, strtab_size_));
            break;
        default:
            break;
        }
    }
    rela_bits_.assign((rela_count_ + jmprel_count_ + 7) / 8, 0);
    SSPC_LOG(Loader, Info,
             "{}: {} relocations + {} PLT, {} symbols, {} import modules, {} import libraries, "
             "{} export libraries, DT_INIT {:#x}",
             name, rela_count_, jmprel_count_, symtab_count_, import_modules_.size(),
             import_libs_.size(), export_libs_.size(), init_addr);
    return true;
}

void Module::CollectExports() {
    for (u64 i = 0; i < symtab_count_; ++i) {
        const auto& sym = symtab_[i];
        const std::string_view id = String(sym.name);
        const auto h1 = id.find('#');
        const auto h2 = h1 == std::string_view::npos ? h1 : id.find('#', h1 + 1);
        if (h2 == std::string_view::npos || id.find('#', h2 + 1) != std::string_view::npos) {
            continue;
        }
        const auto* lib = FindLibrary(id.substr(h1 + 1, h2 - h1 - 1));
        const auto* mod = FindModule(id.substr(h2 + 1));
        if (lib == nullptr || mod == nullptr) {
            SSPC_LOG(Loader, Error, "{}: symbol {} names an unknown library/module", name, id);
            continue;
        }
        const u8 bind = sym.Bind();
        const u8 type = sym.Type();
        if ((bind != Elf::STB_GLOBAL && bind != Elf::STB_WEAK) ||
            (type != Elf::STT_FUNC && type != Elf::STT_OBJECT) || sym.value == 0) {
            continue;
        }
        exports_.push_back(ExportedSymbol{std::string(id.substr(0, h1)), lib->name, mod->name,
                                          base_ + sym.value, type == Elf::STT_OBJECT});
    }
    if (!exports_.empty()) {
        SSPC_LOG(Loader, Info, "{}: {} exported symbols", name, exports_.size());
    }
}

s32 Module::Start(u64 args, const void* argp, void* param) const {
    using EntryFunc = SSPC_SYSV_ABI s32 (*)(u64, const void*, void*);
    SSPC_LOG(Loader, Info, "module started: {} (DT_INIT {:#x})", name, base_ + init_addr);
    return reinterpret_cast<EntryFunc>(base_ + init_addr)(args, argp, param);
}

const LibraryRef* Module::FindLibrary(std::string_view enc_id) const {
    for (const auto& lib : import_libs_) {
        if (lib.enc_id == enc_id) {
            return &lib;
        }
    }
    for (const auto& lib : export_libs_) {
        if (lib.enc_id == enc_id) {
            return &lib;
        }
    }
    return enc_id.empty() && !export_libs_.empty() ? &export_libs_[0] : nullptr;
}

const ModuleRef* Module::FindModule(std::string_view enc_id) const {
    for (const auto& mod : import_modules_) {
        if (mod.enc_id == enc_id) {
            return &mod;
        }
    }
    for (const auto& mod : export_modules_) {
        if (mod.enc_id == enc_id) {
            return &mod;
        }
    }
    return enc_id.empty() && !export_modules_.empty() ? &export_modules_[0] : nullptr;
}

u64 Module::FindExport(std::string_view nid, std::string_view library, std::string_view module) const {
    for (const auto& e : exports_) {
        if (e.nid == nid && e.library == library && e.module == module) {
            return e.address;
        }
    }
    return 0;
}

bool Module::ExportsLibrary(const LibraryRef& lib) const {
    return std::any_of(export_libs_.begin(), export_libs_.end(),
                       [&](const LibraryRef& l) { return l.SameAs(lib); });
}

bool Module::ExportsModule(const ModuleRef& mod) const {
    return std::any_of(export_modules_.begin(), export_modules_.end(),
                       [&](const ModuleRef& m) { return m.SameAs(mod); });
}

} // namespace SecondSon::Loader
