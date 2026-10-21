// SecondSonPC R7-G — eboot 0x1a2030 "GBuffer::MeshPass" in the registry (key gbuffer_mesh_pass; docs/
// NATIVE_ENGINE_R7_GBUFFER.md). The implementation is the native geometry pass (r7/gbuffer/geometry_pass.h).
//
// Verification at the level of the whole function (G2): the original runs as a clone whose calls to the recorded
// callees go to the trace thunks (gbuffer_trace.h), the native pass runs with its callee table aimed at the same
// thunks; both on the same inputs, each with its own shadow of the render context's fields the pass touches
// (R + 0xd190 .. 0xde90, copied from the live context), the same arena for the frame allocations / uploads. Compared:
// every recorded call in order with its arguments and the bytes its pointer arguments point at (the matrices, the
// instance records, V#s), the shadows at the end, the arena's bytes (records and constant uploads).
//   recorded, not run:  0x612110 (returns a fixed sequence), 0x448640, 0x1a0a80, 0x16b70, 0x1a1b90 (writes a fixed
//                       0x40-byte result), 0x11be90 (an arena block + a fixed V#), 0x11bed0, 0x446490, 0x445e00,
//                       0x11e5d0, 0x450460, 0x1742f0, 0x120280, 0x6229a0, 0x6270e0, 0x1a1960, 0x1f0870 (an arena
//                       block), 0x451440, 0x451290, 0x1a1770, 0x984360
//   recorded and run:   0x11c890, 0x11cde0 (they write only the shadow's constant blocks; the pass reads their
//                       changed bytes back)
//   run, not recorded:  0x26ac70, 0x61e260 (-> 0x61f430), 0x635350 (they only write the caller's stack)
// Modes: verify / guarded / native / bench as every reconstruction (native_function_registry.h); explicit_only.
//
// SSPC_R7G_STATS=1: the native pass counts its branches (coverage) and what G3 asks about, reported every 10 s.
// native / guarded: calls that may take a branch the game has not verified (pass 0x1d, M + 0x126, the dither) run the
// original; SSPC_R7G_PASS_ALL=1 lets the native pass take them too (verified offline only).
// SSPC_R7G_PREFETCH=<n>: the native pass prefetches instances n keys ahead (G4 experiment; off by default).
#pragma once

#include <string>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kGBufferMeshPassSpec; ///< 0x1a2030, key gbuffer_mesh_pass

namespace GBufferMeshPass {

using PassFn = SSPC_SYSV_ABI void (*)(u64 m, u64 chunk);

/// Callee addresses at module_base, the recording clone. False with `error` when the clone cannot be made.
bool Prepare(u64 module_base, std::string& error);
PassFn RecordingPass();

/// One comparison of the original (recording clone) with the native pass: "" when equal, else the first difference;
/// `skipped` when the call cannot be compared (a trace overflow).
std::string Check(u64 m, u64 chunk, bool& skipped);

/// Generated cases (the install's self-check, the offline tests): "" or the first failure. Needs the image mapped and
/// its data writable (the light table, the dither globals are set per case and restored).
std::string SelfCheck(u32 cases, u64 seed);

/// Tests only: 1 - the native pass skips the stencil cache; 2 - it writes a wrong record field; 0 - none.
void InjectFault(u32 fault);

void EnableStats(bool on);
std::string StatsText(double frames, bool reset);

} // namespace GBufferMeshPass

} // namespace SecondSon::NativeEngine::Reconstructed
