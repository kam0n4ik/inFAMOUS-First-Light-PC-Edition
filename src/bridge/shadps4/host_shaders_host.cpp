// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — the adapter that hands the bridge's built tiling kernels (host_shaders/tiling.comp, compiled to SPIR-V
// by the bridge's build) to SecondSonPC's backend (Phase 4, P4.4; runtime/gpu/host_shaders.h). Only data crosses.
#include "runtime/gpu/host_shaders.h"

#include "video_core/host_shaders/tiling_macro_128_comp.h"
#include "video_core/host_shaders/tiling_macro_16_comp.h"
#include "video_core/host_shaders/tiling_macro_32_comp.h"
#include "video_core/host_shaders/tiling_macro_64_comp.h"
#include "video_core/host_shaders/tiling_macro_8_comp.h"
#include "video_core/host_shaders/tiling_macro_96_comp.h"
#include "video_core/host_shaders/tiling_micro_128_comp.h"
#include "video_core/host_shaders/tiling_micro_16_comp.h"
#include "video_core/host_shaders/tiling_micro_32_comp.h"
#include "video_core/host_shaders/tiling_micro_64_comp.h"
#include "video_core/host_shaders/tiling_micro_8_comp.h"
#include "video_core/host_shaders/tiling_micro_96_comp.h"

namespace SecondSon::Bridge {

namespace {

std::span<const u32> Tiling(u32 num_bits, bool macro) {
    switch (num_bits) {
    case 8:
        return macro ? std::span<const u32>(TILING_MACRO_8_COMP) : std::span<const u32>(TILING_MICRO_8_COMP);
    case 16:
        return macro ? std::span<const u32>(TILING_MACRO_16_COMP) : std::span<const u32>(TILING_MICRO_16_COMP);
    case 32:
        return macro ? std::span<const u32>(TILING_MACRO_32_COMP) : std::span<const u32>(TILING_MICRO_32_COMP);
    case 64:
        return macro ? std::span<const u32>(TILING_MACRO_64_COMP) : std::span<const u32>(TILING_MICRO_64_COMP);
    case 96:
        return macro ? std::span<const u32>(TILING_MACRO_96_COMP) : std::span<const u32>(TILING_MICRO_96_COMP);
    case 128:
        return macro ? std::span<const u32>(TILING_MACRO_128_COMP) : std::span<const u32>(TILING_MICRO_128_COMP);
    default:
        return {};
    }
}

} // namespace

void InstallHostShaders(); // declared in shader_bridge_hooks.h (the bootstrap's)
void InstallHostShaders() {
    Gpu::HostShaders::Install(&Tiling);
}

} // namespace SecondSon::Bridge
