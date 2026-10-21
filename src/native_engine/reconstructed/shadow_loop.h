// SecondSonPC — the shadow pass's orchestrators in the registry (R6-S; docs/NATIVE_RENDER_R6_SHADOWS.md §2, §4.3, §10):
// eboot 0x43d590 "Shadow::CastersLoop" and 0x43d480 "Shadow::InstancedDraw" (our names; no symbols in the eboot).
//
// The implementation (R7-S, SSPC_R7_SHADOWS_IMPL): the hand-written reconstruction (shadow_casters.h, "hand") or the
// recompiled functions (recomp_43d590 / recomp_43d480 of the module SSPC_NATIVE_ENGINE_RECOMP names, "recomp"; the
// default when that variable is set - R6-S behaviour). explicit_only: installed only when SSPC_NATIVE_ENGINE_FUNCS
// names them; off by default.
//
// 0x43d480 is installed only below CITY-OPT's entry probe on it (shadow draw runs, guest_probe.h): its prepare refuses
// the install when no probe is there - never a second jump on that entry.
//
// Verification (shadow_trace.h, the R6-G method): the original as a clone whose recorded callees go to the trace
// thunks, the recompiled function with the same thunks (recomp_set_redirects), both on shadow copies of everything
// the function writes:
//   0x43d480 writes only its stack: no shadow; 0x61e260 (a V# into its frame) runs for real, 9 callees recorded.
//   0x43d590 writes the context's two constant blocks R+0xd190..+0xd390 (through 0x11c890 / 0x11cde0, which run for
//     real, and its own copies and flags): each run gets a shadow context, the live one copied whole;
//     the DCB allocations 0x11be90 and the frame scratch 0x1f0870 allocate from the run's arena; the generator
//     0x612110 runs on the run's copy of its state (the live state is read once, never written); the CUE/Gnm calls,
//     the draws, the material bind and the parameter writers are recorded, not made.
// The traces (calls, argument registers, the V# the setters get), the shadow contexts' ranges and the arenas are
// compared exactly; then the original runs (verify) - or the recompiled function (guarded).
#pragma once

#include <string>

#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/shadow_trace.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kShadowCastersLoopSpec;   ///< 0x43d590, key shadow_casters_loop
extern const FunctionSpec kShadowInstancedDrawSpec; ///< 0x43d480, key shadow_instanced_draw

namespace ShadowLoop {

using LoopFn = SSPC_SYSV_ABI void (*)(u64 pass, u64 args);
using DrawFn = SSPC_SYSV_ABI void (*)(u64 pass, u64 r, u64 records, u64 n, u64 mesh);
using RedirectsFn = int (*)(const u64* from, const u64* to, u32 n); ///< recomp_set_redirects

inline constexpr u64 kBlocksBegin = 0xd190; ///< R: the two constant blocks and their DCB pointers / flags
inline constexpr u64 kBlocksEnd = 0xd390;
/// A shadow context: the whole context copied (the R6-G context size: what the pass's functions address in R)
inline constexpr u64 kShadowBytes = 0xde80;

/// For tests and the install: callee addresses at module_base, the recording clones. False with `error`.
bool Prepare(u64 module_base, std::string& error);
/// The implementations (the recompiled functions) and the module's redirection.
void UseImplementation(LoopFn loop, DrawFn draw, RedirectsFn redirects);
/// R7-S: the hand-written reconstruction (shadow_casters.h) as the implementation of both.
bool UseHandImplementation(u64 module_base, std::string& error);
/// The calling thread's last recording run (0: the original's, 1: the implementation's) of CheckLoop / CheckDraw.
const STrace::Trace& LastTrace(u32 run);

/// One comparison on the calling thread: "" when equal, else the first difference; `skipped` when not comparable
/// (a trace or arena overflow).
std::string CheckLoop(u64 pass, u64 args, bool& skipped);
std::string CheckDraw(u64 pass, u64 r, u64 records, u64 n, u64 mesh, bool& skipped);

/// Generated cases (the install's self-checks, also the offline tests): "" or the first failure.
std::string SelfCheckLoop(u32 cases, u64 seed);
std::string SelfCheckDraw(u32 cases, u64 seed);

/// Tests only: 1 - the implementation's 0x43d480 runs see one more instance; 0 - none.
void InjectFault(u32 fault);

} // namespace ShadowLoop

} // namespace SecondSon::NativeEngine::Reconstructed
