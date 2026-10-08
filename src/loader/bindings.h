// SecondSonPC — static binding table: every import of the game modules and who serves it.
// The table itself is generated (tools/gen_bindings.py -> generated/import_table.inc). At start-up
// each non-LLE import gets exactly one owner:
//   NATIVE      SecondSonPC implementation (Runtime::Exports)
//   OBSERVED    SecondSonPC observer in front of the BRIDGE implementation
//   BRIDGE      temporary shadPS4 implementation (Phase 1-4), referenced by its C++ name
//   COMMON_STUB explicit SecondSonPC stub: logs the first call and returns 0 (what Phase 0's
//               generic stub did); every one of them is listed at start-up
//   UNRESOLVED  object nobody provides (the relocation is left untouched, as in Phase 0)
// LLE imports (libc, libSceFios2) are served by the game's own modules through their exports.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Loader {

enum class Kind : u8 { Function, Object };
enum class Phase0 : u8 { Hle, CommonStub, UnresolvedObject, Lle };
enum ImporterBits : u8 { kEboot = 1, kLibc = 2, kFios2 = 4 };

/// One row of generated/import_table.inc.
struct ImportInfo {
    const char* nid;
    const char* library;
    const char* module;
    const char* name;
    Kind kind;
    const char* subsystem;
    u8 importers;
    Phase0 phase0;
};

/// An implementation offered by a provider (the BRIDGE table uses this layout).
struct ProviderEntry {
    const char* nid;
    const char* library;
    const char* module;
    const void* address;
};

enum class Owner : u8 { Native, Observed, Bridge, CommonStub, Unresolved, Lle };
std::string_view OwnerName(Owner owner);

struct Binding {
    const ImportInfo* info = nullptr;
    Owner owner = Owner::Unresolved;
    u64 address = 0;           // what a relocation slot receives (the counter thunk, if enabled)
    u64 implementation = 0;    // the function itself
    const char* group = "";    // SecondSonPC subsystem group of a NATIVE/OBSERVED entry
};

struct BindingOptions {
    bool import_counter = true;
    bool classify_only = false; // names addresses only (snapshots): no counter, observers untouched
    std::vector<std::string> bridged_groups; // --bridge <group>: NATIVE group forced back to BRIDGE
    std::vector<std::string> no_bind;        // --no-bind <name>: export not bound (diagnostics)
};

struct OwnerCounts {
    u32 native = 0;
    u32 observed = 0;
    u32 bridge = 0;
    u32 common_stub = 0;
    u32 unresolved = 0;
    u32 lle = 0;
};

class Bindings {
public:
    static std::span<const ImportInfo> ImportTable();

    /// Assigns an owner to every import. Returns false (and logs why) on an inconsistency.
    bool Build(std::span<const Runtime::Export> natives, std::span<const ProviderEntry> bridge,
               const BindingOptions& options);

    /// Binding for an import key; nullptr if the key is not in the table (or is LLE).
    const Binding* Find(std::string_view nid, std::string_view library, std::string_view module) const;
    /// Binding whose `address` or `implementation` equals `address` (snapshots, diagnostics).
    const Binding* FindByAddress(u64 address) const;

    OwnerCounts Counts() const;
    std::span<const Binding> All() const {
        return bindings_;
    }
    void LogSummary() const;

private:
    std::vector<Binding> bindings_; // same order as ImportTable()
    std::unordered_map<u64, const Binding*> by_address_;
};

/// Address of a generic stub for an import that cannot be resolved yet (LLE module not loaded):
/// logs and returns 0, like Phase 0's stub. Distinct per (nid, library) for the log.
u64 PendingImportStub(std::string_view nid, std::string_view library);

} // namespace SecondSon::Loader
