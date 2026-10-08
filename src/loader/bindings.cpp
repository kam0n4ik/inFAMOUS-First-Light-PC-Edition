// SecondSonPC — static binding table.
#include "loader/bindings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <windows.h>

#include "base/log.h"
#include "runtime/exports.h"
#include "trace/import_counter.h"

namespace SecondSon::Loader {

namespace {

const ImportInfo kImportTable[] = {
#include "loader/generated/import_table.inc"
};

std::string Key(std::string_view nid, std::string_view library, std::string_view module) {
    std::string k;
    k.reserve(nid.size() + library.size() + module.size() + 2);
    k.append(nid).append(1, '\0').append(library).append(1, '\0').append(module);
    return k;
}

// ---- generic stubs: "mov edi, index ; jmp [rip+handler]" ----------------------------------
struct StubInfo {
    std::string name;
    std::string library;
    bool pending = false; // LLE import whose module is not loaded yet
    std::atomic<u64> calls{0};
};

constexpr size_t kStubSize = 16;
constexpr size_t kMaxStubs = 4096;

std::mutex g_stub_mutex;
u8* g_stub_page = nullptr; // [0..8): handler address, stubs from offset 16
std::array<StubInfo, kMaxStubs> g_stubs;
size_t g_stub_count = 0;
std::unordered_map<std::string, u64> g_pending_by_key;

SSPC_SYSV_ABI u64 StubHandler(u64 index) {
    if (index >= kMaxStubs) {
        return 0;
    }
    auto& s = g_stubs[index];
    if (s.calls.fetch_add(1, std::memory_order_relaxed) == 0) {
        if (s.pending) {
            SSPC_LOG(Loader, Error, "unresolved LLE import {} ({}) called before its module was "
                                    "loaded, returning 0", s.name, s.library);
        } else {
            SSPC_LOG(Loader, Warning, "COMMON_STUB {} ({}) called, returning 0", s.name, s.library);
        }
    }
    return 0;
}

u64 MakeStub(std::string name, std::string library, bool pending) {
    std::scoped_lock lock{g_stub_mutex};
    if (g_stub_page == nullptr) {
        g_stub_page = static_cast<u8*>(VirtualAlloc(nullptr, kStubSize * (kMaxStubs + 1),
                                                    MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        if (g_stub_page == nullptr) {
            SSPC_LOG(Loader, Critical, "cannot allocate the stub page: {}", GetLastError());
            return 0;
        }
        std::memset(g_stub_page, 0xCC, kStubSize * (kMaxStubs + 1));
        const u64 handler = reinterpret_cast<u64>(&StubHandler);
        std::memcpy(g_stub_page, &handler, sizeof(handler));
    }
    if (g_stub_count >= kMaxStubs) {
        SSPC_LOG(Loader, Critical, "out of stub slots");
        return 0;
    }
    const size_t index = g_stub_count++;
    g_stubs[index].name = std::move(name);
    g_stubs[index].library = std::move(library);
    g_stubs[index].pending = pending;

    u8* t = g_stub_page + kStubSize * (index + 1);
    t[0] = 0xBF; // mov edi, imm32
    const u32 imm = static_cast<u32>(index);
    std::memcpy(t + 1, &imm, 4);
    t[5] = 0xFF; // jmp qword ptr [rip + disp32]
    t[6] = 0x25;
    const s32 disp = static_cast<s32>(g_stub_page - (t + 11));
    std::memcpy(t + 7, &disp, 4);
    FlushInstructionCache(GetCurrentProcess(), t, kStubSize);
    return reinterpret_cast<u64>(t);
}

Trace::Owner TraceOwner(Owner owner) {
    switch (owner) {
    case Owner::Native:
        return Trace::Owner::Native;
    case Owner::Observed:
        return Trace::Owner::Observed;
    case Owner::CommonStub:
        return Trace::Owner::CommonStub;
    default:
        return Trace::Owner::Bridge;
    }
}

bool Contains(const std::vector<std::string>& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

std::string_view OwnerName(Owner owner) {
    switch (owner) {
    case Owner::Native:
        return "NATIVE";
    case Owner::Observed:
        return "OBSERVED";
    case Owner::Bridge:
        return "BRIDGE";
    case Owner::CommonStub:
        return "COMMON_STUB";
    case Owner::Unresolved:
        return "UNRESOLVED";
    case Owner::Lle:
        return "LLE";
    }
    return "?";
}

std::span<const ImportInfo> Bindings::ImportTable() {
    return kImportTable;
}

bool Bindings::Build(std::span<const Runtime::Export> natives, std::span<const ProviderEntry> bridge,
                     const BindingOptions& options) {
    bool ok = true;
    bindings_.clear();
    bindings_.reserve(std::size(kImportTable));

    std::unordered_map<std::string, const ProviderEntry*> bridge_by_key;
    for (const auto& b : bridge) {
        bridge_by_key.emplace(Key(b.nid, b.library, b.module), &b);
    }
    std::unordered_map<std::string, const Runtime::Export*> native_by_key;
    for (const auto& n : natives) {
        if (Contains(options.bridged_groups, n.group) || Contains(options.no_bind, n.name)) {
            SSPC_LOG(Loader, Warning, "DIAGNOSTIC: {} ({}) not bound, group '{}' -> BRIDGE", n.name,
                     n.library, n.group);
            continue;
        }
        native_by_key.emplace(Key(n.nid, n.library, n.module), &n); // first entry wins
    }

    const bool counter = options.import_counter && !options.classify_only;
    if (counter && !Trace::Init(std::size(kImportTable) + 64)) {
        SSPC_LOG(Trace, Error, "import counter disabled (allocation failed)");
    }

    for (const auto& row : kImportTable) {
        Binding b;
        b.info = &row;
        if (row.phase0 == Phase0::Lle) {
            b.owner = Owner::Lle;
            bindings_.push_back(b);
            continue;
        }
        const auto key = Key(row.nid, row.library, row.module);
        const auto nit = native_by_key.find(key);
        const auto bit = bridge_by_key.find(key);
        const auto* native = nit != native_by_key.end() ? nit->second : nullptr;
        const auto* br = bit != bridge_by_key.end() ? bit->second : nullptr;

        if (native != nullptr && native->original == nullptr) {
            b.owner = Owner::Native;
            b.implementation = reinterpret_cast<u64>(native->function);
            b.group = native->group;
        } else if (native != nullptr) {
            if (br == nullptr) {
                SSPC_LOG(Loader, Error, "observer {}: no BRIDGE implementation to observe", row.name);
                ok = false;
                continue;
            }
            if (!options.classify_only) {
                *native->original = const_cast<void*>(br->address);
            }
            b.owner = Owner::Observed;
            b.implementation = reinterpret_cast<u64>(native->function);
            b.group = native->group;
        } else if (br != nullptr) {
            b.owner = Owner::Bridge;
            b.implementation = reinterpret_cast<u64>(br->address);
        } else if (row.kind == Kind::Function) {
            b.owner = Owner::CommonStub;
            b.implementation = MakeStub(row.name, row.library, false);
        } else {
            b.owner = Owner::Unresolved;
        }
        b.address = b.implementation;
        if (counter && b.implementation != 0 && row.kind == Kind::Function) {
            if (void* thunk = Trace::Wrap(row.nid, row.library, row.module, row.name,
                                          TraceOwner(b.owner),
                                          reinterpret_cast<void*>(b.implementation))) {
                b.address = reinterpret_cast<u64>(thunk);
            }
        }
        bindings_.push_back(b);
    }
    if (counter) {
        Trace::Seal();
    }

    // Every SecondSonPC export must serve an import of the game (catches key typos).
    std::unordered_map<std::string, bool> imported;
    for (const auto& row : kImportTable) {
        imported.emplace(Key(row.nid, row.library, row.module), true);
    }
    for (const auto& n : natives) {
        if (!imported.contains(Key(n.nid, n.library, n.module))) {
            SSPC_LOG(Loader, Error, "export {} ({}/{} {}) is not imported by the game", n.name,
                     n.library, n.module, n.nid);
            ok = false;
        }
    }

    by_address_.clear();
    for (const auto& b : bindings_) {
        if (b.address != 0) {
            by_address_.emplace(b.address, &b);
        }
        if (b.implementation != 0) {
            by_address_.emplace(b.implementation, &b);
        }
    }
    return ok;
}

const Binding* Bindings::Find(std::string_view nid, std::string_view library,
                              std::string_view module) const {
    // ~560 rows; a linear scan per relocation symbol is cheap next to everything else at load.
    for (const auto& b : bindings_) {
        if (b.owner != Owner::Lle && nid == b.info->nid && library == b.info->library &&
            module == b.info->module) {
            return &b;
        }
    }
    return nullptr;
}

const Binding* Bindings::FindByAddress(u64 address) const {
    const auto it = by_address_.find(address);
    return it != by_address_.end() ? it->second : nullptr;
}

OwnerCounts Bindings::Counts() const {
    OwnerCounts c;
    for (const auto& b : bindings_) {
        switch (b.owner) {
        case Owner::Native:
            ++c.native;
            break;
        case Owner::Observed:
            ++c.observed;
            break;
        case Owner::Bridge:
            ++c.bridge;
            break;
        case Owner::CommonStub:
            ++c.common_stub;
            break;
        case Owner::Unresolved:
            ++c.unresolved;
            break;
        case Owner::Lle:
            ++c.lle;
            break;
        }
    }
    return c;
}

void Bindings::LogSummary() const {
    const auto c = Counts();
    SSPC_LOG(Loader, Info,
             "binding table: {} imports = NATIVE {} + OBSERVED {} + BRIDGE {} + COMMON_STUB {} + "
             "UNRESOLVED {} (objects) | LLE {} (game modules)",
             bindings_.size() - c.lle, c.native, c.observed, c.bridge, c.common_stub, c.unresolved,
             c.lle);
    for (const auto& b : bindings_) {
        if (b.owner == Owner::Native || b.owner == Owner::Observed) {
            SSPC_LOG(Loader, Info, "  {:<8} {:<44} {}/{} [{}]", OwnerName(b.owner), b.info->name,
                     b.info->library, b.info->module, b.group);
        }
    }
    for (const auto& b : bindings_) {
        if (b.owner == Owner::CommonStub || b.owner == Owner::Unresolved) {
            SSPC_LOG(Loader, Info, "  {:<11} {:<44} {}/{}", OwnerName(b.owner), b.info->name,
                     b.info->library, b.info->module);
        }
    }
}

u64 PendingImportStub(std::string_view nid, std::string_view library) {
    const auto key = Key(nid, library, "");
    {
        std::scoped_lock lock{g_stub_mutex};
        if (const auto it = g_pending_by_key.find(key); it != g_pending_by_key.end()) {
            return it->second;
        }
    }
    const u64 stub = MakeStub(std::string(nid), std::string(library), true);
    std::scoped_lock lock{g_stub_mutex};
    g_pending_by_key.emplace(key, stub);
    return stub;
}

} // namespace SecondSon::Loader
