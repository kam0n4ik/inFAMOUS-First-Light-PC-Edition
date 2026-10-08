// SecondSonPC — libSceSysmodule (NATIVE): the front-end of the Loader for module loading.
// Game modules (/app0/sce_module/*.prx) are loaded by Loader::Linker; firmware modules are never
// loaded: their functions are served by the binding table, and loading them is a bookkeeping
// success, exactly as in the Phase 0 oracle.
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Runtime::Sysmodule {

/// sceSysmodulePreloadModuleForLibkernel: called by the Loader on "Game:Main" before the entry.
s32 PreloadForLibkernel();

std::span<const Export> SysmoduleExports();

} // namespace SecondSon::Runtime::Sysmodule
