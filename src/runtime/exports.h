// SecondSonPC — functions this runtime provides to the game (keyed exactly like the game imports).
#pragma once

#include <span>

namespace SecondSon::Runtime {

struct Export {
    const char* nid;
    const char* library;
    const char* module;
    const char* name;
    void* function;  // SSPC_SYSV_ABI host function (for an object import: the object itself)
    void** original; // observer: receives the implementation being observed; nullptr = NATIVE
    const char* group = ""; // SecondSonPC subsystem ("loader", "sysmodule", "system", "fs", ...);
                            // --bridge <group> sends the whole group back to BRIDGE (diagnostics)
    bool phase0 = false;    // also bound by the Phase 0 loader path (shadPS4 Core::Linker)
};

/// All exports, subsystem by subsystem. The first entry for an import key wins.
std::span<const Export> Exports();

} // namespace SecondSon::Runtime
