// SecondSonPC — host-side compute shaders the backend runs that are not the game's (Phase 4, P4.4): the GCN tiling
// (detiling) kernels. The SPIR-V is the bridge's build output (host_shaders/tiling.comp, GPL-2.0-or-later), handed over
// by the adapter layer (src/bridge/shadps4/host_shaders_host.cpp); the runtime never includes the generated headers.
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Gpu::HostShaders {

/// The tiling kernel for `num_bits` per element (8, 16, 32, 64, 96, 128), macro- or micro-tiled; empty when not
/// installed or no such variant.
std::span<const u32> Tiling(u32 num_bits, bool macro);

/// The adapter layer installs its provider at start-up.
void Install(std::span<const u32> (*provider)(u32 num_bits, bool macro));

} // namespace SecondSon::Gpu::HostShaders
