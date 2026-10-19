// SecondSonPC — which of the engine's mesh draw functions record which render pass (CITY-OPT Phase 3, diagnostics,
// SSPC_DIAG_DRAW_CENSUS=1; off by default; docs/CITY_ENGINE_REVERSE.md).
//
// Entry probes (guest_probe.h, read-only) on the four callers of the engine's drawIndex wrapper 0x623c60: 0x1a1960 and
// 0x1a1770 (a mesh draw: arg 2 the pool context R, its key {pass, sub} at R + 0xde90), 0x448b30, 0x43d480. A log line
// every 10 s: the calls per frame by function and pass; for 0x1a1960 how many draws replace vertex streams (the draw's
// per-instance override, arg 3 + 0x20) and the meshes' index buffers seen.
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::Render::DrawCensus {

/// The loader's window before the guest runs; false (logged) when the switch is set but a function does not hash.
bool InstallFromEnvironment(u64 eboot_base);

} // namespace SecondSon::Engine::Render::DrawCensus
