// SecondSonPC — fixes of races in the game's own code that the PC's timing exposes (docs/GAME_FIXES.md).
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::GameFixes {

/// Installs every fix whose original bytes match exactly (another game version: nothing is written). Call before any
/// guest code runs (the loader's window after the eboot is loaded). SSPC_FIX_REFCOUNT_RACE=0: that fix off.
void Install(u64 eboot_base);

} // namespace SecondSon::Engine::GameFixes
