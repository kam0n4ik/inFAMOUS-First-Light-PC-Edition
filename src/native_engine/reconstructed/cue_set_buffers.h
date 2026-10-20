// SecondSonPC — reconstruction of the CUE's resource setters (our names; no symbols in the eboot): eboot 0x16aa0
// "CUE::SetVertexBuffers", 0x16b70 "CUE::SetBuffers" (Native Engine R5-C, docs/NATIVE_RECOMP_CUE.md), and the three the
// material setup 0x448640 calls (R5-D, docs/NATIVE_RECOMP_R5_D.md): 0x169d0 "CUE::SetConstantBuffers", 0x16d10
// "CUE::SetTextures", 0x16eb0 "CUE::SetSamplers".
//
// What they do [A: disassembly, RENDER_ENGINE_ARCHITECTURE.md §4, CITY_ENGINE_RESOURCES.md; E: bit-exact offline
// comparison with the eboot's code, calls recorded]: `count` descriptors (from `src`, 16 bytes each - V#, S# - or 32 for
// the textures' T#) go to the API slots `slot`, `slot + 1`, ... of shader stage `stage` of a constant update engine
// (CUE) object. Where each one goes is the bound shader's user-data layout, a table of u16 per API slot at
// [cue + 0xc180 + stage * 8]: resources (buffers and textures) at table + 0x22, samplers at + 0xc2, constant buffers at
// + 0xe2, vertex buffers at + 0x10a. The five bodies are the same code with that offset and the descriptor size.
//
//   System V: (rdi cue, esi stage, edx slot, ecx count, r8 src) -> rax = cue (the original leaves it there)
//
//   entry 0xffff      the shader does not use the slot: nothing
//   bit 15 set        setUserDataRegion 0x626520([cue + 0xc090], stage, entry & 0x3fff, descriptor, size / 4):
//                     SET_SH_REG of the descriptor's dwords straight into the command buffer
//   otherwise         the descriptor to the stage's shadow at cue + 0x90 + stage * 0x1800 + (entry & 0x3fff) * 4
//   always            cue + 0xc1d0 + stage = 1 (the stage's resources changed: the commit 0x14ad0 snapshots the shadow)
//
// Address arithmetic as the original's: stage is sign-extended for the table and the byte, the shadow offset is
// (s32)(stage * 0x600) * 4; slot is a u32 index; the table entry and the descriptor are read for every slot (no
// caching across the callee's call); each descriptor is read whole before it is written (one vmovups load - xmm, or
// ymm for T# - then the store).
//
// Verification: both run on shadow copies of the touched part of the CUE (the stage's shadow window, the touched
// entries, +0xc080..+0xc1e0), the original as a clone whose call to 0x626520 goes to a recorder
// (guest_code_clone.h CodeRedirect), the reconstruction with the same recorder; the copies and the recorded calls
// (target arguments and the descriptor bytes) are compared. No double command-buffer writes in game memory.
#pragma once

#include <string>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kCueSetVertexBuffersSpec;   ///< 0x16aa0, key cue_set_vertex_buffers
extern const FunctionSpec kCueSetBuffersSpec;         ///< 0x16b70, key cue_set_buffers
extern const FunctionSpec kCueSetConstantBuffersSpec; ///< 0x169d0, key cue_set_constant_buffers
extern const FunctionSpec kCueSetTexturesSpec;        ///< 0x16d10, key cue_set_textures
extern const FunctionSpec kCueSetSamplersSpec;        ///< 0x16eb0, key cue_set_samplers

namespace CueSetBuffers {

enum Kind : u32 { kVertexBuffers = 0, kBuffers = 1, kConstantBuffers = 2, kTextures = 3, kSamplers = 4, kKinds = 5 };

inline constexpr u64 kRva[kKinds] = {0x16aa0, 0x16b70, 0x169d0, 0x16d10, 0x16eb0};
inline constexpr u32 kTableOffset[kKinds] = {0x10a, 0x22, 0xe2, 0x22, 0xc2};
inline constexpr u32 kDescriptorBytes[kKinds] = {16, 16, 16, 32, 16};
inline constexpr u32 kMaxDescriptorBytes = 32;
inline constexpr u64 kCalleeRva = 0x626520; ///< setUserDataRegion (GNMX_RECONSTRUCTION.md)
inline constexpr u32 kMaxSlots = 64;        ///< verification: slot + count within the 64 resource entries

using Fn = SSPC_SYSV_ABI u64 (*)(u64 cue, u32 stage, u32 slot, u32 count, u64 src);
/// setUserDataRegion as the setters call it; whole argument registers (the original zero-extends each 32-bit one).
using CalleeFn = SSPC_SYSV_ABI u64 (*)(u64 context, u64 stage, u64 sgpr, u64 values, u64 count);
/// The recompiled module's call redirection for the calling thread (recomp_set_redirect; 0, 0 clears).
using RedirectFn = void (*)(u64 from, u64 to);

SSPC_SYSV_ABI u64 NativeVertexBuffers(u64 cue, u32 stage, u32 slot, u32 count, u64 src);
SSPC_SYSV_ABI u64 NativeBuffers(u64 cue, u32 stage, u32 slot, u32 count, u64 src);
SSPC_SYSV_ABI u64 NativeConstantBuffers(u64 cue, u32 stage, u32 slot, u32 count, u64 src);
SSPC_SYSV_ABI u64 NativeTextures(u64 cue, u32 stage, u32 slot, u32 count, u64 src);
SSPC_SYSV_ABI u64 NativeSamplers(u64 cue, u32 stage, u32 slot, u32 count, u64 src);
/// The hand-written implementation of `kind`.
Fn HandWritten(Kind kind);
/// The spec of `kind` (registry).
const FunctionSpec& Spec(Kind kind);

/// One call recorded instead of made: the five argument registers whole, and the descriptor they point at.
struct Call {
    u64 context;
    u64 stage;
    u64 sgpr;
    u64 values;
    u64 count;
    u8 data[kMaxDescriptorBytes]; ///< the `count` dwords at `values` when the call was made (up to 32 bytes; rest 0)
};
inline constexpr u32 kMaxCalls = kMaxSlots;
struct Recording {
    u32 calls = 0;
    bool overflow = false;
    Call call[kMaxCalls];
};

/// The recorder (System V, the callee's signature): appends to the calling thread's current recording.
SSPC_SYSV_ABI u64 Record(u64 context, u64 stage, u64 sgpr, u64 values, u64 count);

/// Runs `fn` with its calls to 0x626520 recorded into `out` (`recording_original`: a clone whose call is already
/// redirected to Record; otherwise the reconstruction of `kind` with its callee redirected).
u64 RunRecorded(Kind kind, Fn fn, bool recording_original, u64 cue, u32 stage, u32 slot, u32 count, u64 src,
                Recording& out);

/// The clone of the original whose call to 0x626520 goes to Record (made by the install's prepare step; nullptr
/// before it), and the callee address the reconstruction calls.
Fn RecordingOriginal(Kind kind);
u64 CalleeAddress();

/// For tests: prepares like the install does (callee at module_base + 0x626520, the recording clone).
bool Prepare(Kind kind, u64 module_base, std::string& error);

bool Replay(Kind kind, const std::string& bytes, std::string& report);

void UseImplementation(Kind kind, Fn fn, const char* source, RedirectFn redirect);
Fn Implementation(Kind kind);
const char* ImplementationSource(Kind kind);

} // namespace CueSetBuffers
} // namespace SecondSon::NativeEngine::Reconstructed
