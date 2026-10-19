// SecondSonPC — the engine's runs of mesh draws, told to the command processor (CITY-OPT Phase 5,
// SSPC_CITY_ENGINE_OPT=1; off by default; docs/CITY_ENGINE_OPTIMIZATION.md).
//
// Two draw functions of the engine record ~70 % of the city's draws: 0x1a1960 (a mesh instance: the CUE's VS vertex
// table, triangle list, 16-bit indices, commit, drawIndex) and 0x43d480 (the shadow casters' instanced draw: the same
// plus the instances' constant buffer and NUM_INSTANCES). Draw after draw of one material they change only the VS
// vertex table and the index range (0x1a1960) - docs/CITY_ENGINE_REVERSE.md.
//
// Entry probes (guest_probe.h) on both run the function unchanged; then they tag the draw it wrote: the driver's
// drawIndex leaves a 4-dword NOP after the draw packet whose body the hardware ignores and whose first dword it zeroes
// (native_render_ops.h, the P5.3 direct path) - the tag goes into that body, so the stream gets no dword more. The
// command processor reads the tag as a hint only: whether a tagged draw joins the run before it is decided from its
// registers and preparation (parallel_cp.h, SSPC_CITY_ENGINE_OPT).
#pragma once

#include <atomic>

#include "base/abi.h"

namespace SecondSon::Engine::Render::DrawRuns {

inline constexpr u32 kRunMagic = 0x52505353u; ///< "SSPR": the first body dword of a tagged draw's trailing NOP
inline constexpr u32 kTrailingNopHeader = 0xC0021000u; ///< that NOP: type 3, 3 body dwords (kDrawNopHeader)

enum class Source : u8 {
    None = 0,
    Mesh = 1,   ///< 0x1a1960
    Shadow = 2, ///< 0x43d480
};

/// The tag of the trailing NOP at `nop` (4 dwords readable): None when it carries none.
inline Source TagOf(const u32* nop) {
    return nop[0] == kTrailingNopHeader && nop[1] == kRunMagic ? static_cast<Source>(nop[2] & 0xFF) : Source::None;
}

struct Counters {
    std::atomic<u64> calls[3]{};    ///< by Source
    std::atomic<u64> tagged[3]{};
    std::atomic<u64> no_nop{0};     ///< the function wrote no draw with a trailing NOP (nothing tagged)
};
const Counters& Stats();

/// The loader's window before the guest runs. SSPC_CITY_ENGINE_OPT=1|verify with SecondSonPC's GPU backend only (the
/// bridge's command processor does not know the tag; it ignores a NOP's body anyway). False (logged) when the switch is
/// set but nothing was installed.
bool InstallFromEnvironment(u64 eboot_base, bool native_backend);

/// The probes are installed (the processor looks for tags then).
bool On();

} // namespace SecondSon::Engine::Render::DrawRuns
