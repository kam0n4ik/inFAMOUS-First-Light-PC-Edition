// SecondSonPC — functions this runtime provides to the game (keyed exactly like the game imports).
#pragma once

#include <span>

namespace SecondSon::Runtime {

struct Export {
    const char* nid;
    const char* library;
    const char* module;
    const char* name;
    void* function;  // SSPC_SYSV_ABI host function
    void** original; // observer: receives the implementation being observed; nullptr = NATIVE
};

std::span<const Export> Exports();

} // namespace SecondSon::Runtime
