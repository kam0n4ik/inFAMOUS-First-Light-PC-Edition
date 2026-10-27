// SecondSonPC — TEMPORARY Phase 1-4 bridge (removed in Phase 5): the shadPS4 HLE functions that
// can serve the game's imports (generated/bridge_exports.cpp, from tools/gen_bindings.py).
#pragma once

#include <span>

#include "loader/bindings.h"

namespace SecondSon::Bridge {

using BridgeExport = Loader::ProviderEntry;

std::span<const BridgeExport> BridgeExports();

/// BridgeExports() and the hand-written entries for the imports only inFAMOUS: First Light has
/// (bridge_exports_first_light.cpp): what the binding table is built from.
std::span<const BridgeExport> AllBridgeExports();

} // namespace SecondSon::Bridge
