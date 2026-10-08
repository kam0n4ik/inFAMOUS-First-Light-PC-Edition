// SecondSonPC — TEMPORARY Phase 1-4 bridge (removed in Phase 5): the shadPS4 HLE functions that
// can serve the game's imports (generated/bridge_exports.cpp, from tools/gen_bindings.py).
#pragma once

#include <span>

#include "loader/bindings.h"

namespace SecondSon::Bridge {

using BridgeExport = Loader::ProviderEntry;

std::span<const BridgeExport> BridgeExports();

} // namespace SecondSon::Bridge
