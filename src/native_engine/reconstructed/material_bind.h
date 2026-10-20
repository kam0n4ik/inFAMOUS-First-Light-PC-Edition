// SecondSonPC — reconstruction of eboot 0x448640, a mesh pass's material setup (our name "Material::Bind"; no symbols
// in the eboot). Native Engine R5-D (docs/NATIVE_RECOMP_R5_D.md; CITY_ENGINE_RESOURCES.md, REA_ENGINE_REVERSE.md).
//
// What it does [A: disassembly; E: bit-exact offline comparison with the eboot's code, calls recorded]:
//
//   System V: (rdi pass P, rsi context R, rdx material M) -> nothing (every caller writes RAX before reading it)
//   cue = R + 0x128 (the context's constant update engine)
//   M == 0: nothing
//   D = [M + 0x28] (the material's descriptor block)
//   (u32 [D] | (u64 ([D + 4] & 0xfff) << 32)) != 0 (its first V#, the material constants, has an address):
//       0x169d0 CUE::SetConstantBuffers(cue, stage 1, slot 13, 1, D); the same for stage 2
//   for i < u32 [P + 0x80] (re-read every round):       the samplers
//       0x16eb0 CUE::SetSamplers(cue, stage(u8 [P + 0x71 + 4 i]), u8 [P + 0x70 + 4 i], 1,
//                                D + 0x20 + (s32 [P + 0x84]) * 32 + u16 [P + 0x72 + 4 i] * 16)
//   for i < u32 [P + 0x6c] (re-read every round):       the textures
//       0x16d10 CUE::SetTextures(cue, stage(u8 [P + 0x2d + 4 i]), u8 [P + 0x2c + 4 i], 1,
//                                D + 0x20 + u16 [P + 0x2e + 4 i] * 32)
//   stage(x) = 0x27530(x) = u32 [0x987c80 + x * 4] (the pass's stage numbering -> the CUE's)
//
// The pass's tables are the program's API-slot bindings (0x447190 builds them, REA_ENGINE_REVERSE.md); the
// descriptors are the material's constant memory. The function writes nothing itself: everything it does goes
// through the three CUE setters (cue_set_buffers.h), so its behaviour is the sequence of their calls.
//
// Verification: the original runs as a clone whose three setter calls go to recorders (guest_code_clone.h
// CodeRedirect), the reconstruction with the same recorders; the recorded calls (setter, the five argument
// registers whole, the descriptor bytes at src) are compared. Nothing reaches the CUE or the command buffer twice.
//
// Installation: CITY-OPT Phase 6's material probe (draw_runs.cpp) is on this entry under SSPC_CITY_ENGINE_GBUFFER_RUNS;
// the reconstruction then goes in below the probe (native_function_registry.h InstallFunction).
#pragma once

#include <string>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kMaterialBindSpec; ///< 0x448640, key material_bind

namespace MaterialBind {

using Fn = SSPC_SYSV_ABI void (*)(u64 pass, u64 context, u64 material);
/// A CUE setter as the function calls it: whole argument registers (each 32-bit one zero-extended, as the original's).
using SetterFn = SSPC_SYSV_ABI u64 (*)(u64 cue, u64 stage, u64 slot, u64 count, u64 src);
/// 0x27530: the pass's stage number -> the CUE's.
using StageFn = SSPC_SYSV_ABI u64 (*)(u64 stage);

enum Setter : u32 { kConstantBuffers = 0, kSamplers = 1, kTextures = 2, kSetters = 3 };
inline constexpr u64 kSetterRva[kSetters] = {0x169d0, 0x16eb0, 0x16d10};
inline constexpr u32 kSetterBytes[kSetters] = {16, 16, 32}; ///< the descriptor each call reads at src
inline constexpr u64 kStageRva = 0x27530;

SSPC_SYSV_ABI void Native(u64 pass, u64 context, u64 material);

/// One setter call recorded instead of made.
struct Call {
    u64 setter;
    u64 cue;
    u64 stage;
    u64 slot;
    u64 count;
    u64 src;
    u8 data[32]; ///< the descriptor at src (16 or 32 bytes; rest 0)
};
inline constexpr u32 kMaxCalls = 256;
struct Recording {
    u32 calls = 0;
    bool overflow = false;
    Call call[kMaxCalls];
};

/// Runs `fn` with its setter calls recorded into `out` (`recording_original`: the clone with redirected calls).
void RunRecorded(Fn fn, bool recording_original, u64 pass, u64 context, u64 material, Recording& out);
bool SameRecordings(const Recording& a, const Recording& b);

/// For tests: prepares like the install does (callee addresses at module_base, the recording clone).
bool Prepare(u64 module_base, std::string& error);
Fn RecordingOriginal();

void UseImplementation(Fn fn, const char* source, void (*redirect)(u64 from, u64 to));
Fn Implementation();
const char* ImplementationSource();

} // namespace MaterialBind
} // namespace SecondSon::NativeEngine::Reconstructed
