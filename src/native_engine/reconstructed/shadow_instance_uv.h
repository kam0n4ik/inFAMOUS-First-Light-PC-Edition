// SecondSonPC — reconstruction of eboot 0x451700, "ShadowInstanceUv" (our name; no symbols in the eboot).
// Native Engine R5-F (docs/NATIVE_RECOMP_R5_F_SHADOW.md). A leaf called only by the shadow pass loop 0x43d590
// (the instanced path, at 0x43e2be, once per instance when the pass carries extra per-instance data, R + 0x94 > 0).
//
// What it does [A: disassembly + Ghidra; E: bit-exact offline comparison with the eboot's code]: for each instance
// record of the chunk whose kind (int at records[i*? ...]) is 2, it computes a texture-atlas tile offset (a float2)
// from the atlas dimensions and the instance's tile index, and stores it into the shadow instance record at
// param_2 + 0x40. The loop overwrites the same 8 bytes every matching iteration, so the stored result is the last
// matching record's.
//
//   System V: (rdi records int*, rsi out, rdx atlas, ecx unused, r8 table, r9 instance) -> void
//
//   count = records[1]; start = records[0]; for i in [start, start+count):
//     if records[i + 2] == 2:                         (one int per record slot; +2 and +6 are dword indices)
//       tile = records[i + 6]
//       cell = &table[(s32)atlas[0x5c] * 0x20 + (s32)atlas[0x60] * 0x10 + 0x20 + tile * 4]
//       w = cell[0]; h = cell[1]                       (atlas grid: columns w, rows h)   [A]
//       n = (u8)instance[0x11] % (w * h)               (the instance's tile pick)        [A]
//       *(float2*)(out + 0x40) = { (n % w) / (float)w, (n / w) / (float)h }
//
// Address arithmetic and the float math are the original's: 1.0f / (float)dim then multiply, in the original's
// operand order (the result is integer-derived, never NaN, so it is bit-exact on the same CPU and MXCSR).
//
// Verification: both run on shadow copies of a 0x48-byte window around param_2 (only +0x40..+0x47 is written); the
// other pointers are read-only, so there is no double side effect. Division by a zero atlas dimension would #DE in
// the original too; generated and in-game inputs have nonzero dimensions.
#pragma once

#include <string>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kShadowInstanceUvSpec; ///< 0x451700, key shadow_instance_uv

namespace ShadowInstanceUv {

inline constexpr u32 kOutBytes = 0x48; ///< the window shadowed around param_2 (writes land at +0x40, 8 bytes)

/// The reconstruction itself (System V, as the guest calls it).
SSPC_SYSV_ABI void Native(u64 records, u64 out, u64 atlas, u64 unused, u64 table, u64 instance);

using Fn = SSPC_SYSV_ABI void (*)(u64, u64, u64, u64, u64, u64);

/// Runs `original` and the reconstruction on copies of the out window and compares the written 8 bytes bit for bit.
bool Compare(Fn original, u64 records, u64 out, u64 atlas, u64 table, u64 instance, u8* a, u8* b);

/// A recorded mismatch case (native_engine_verify.h) replayed against `original`.
bool Replay(Fn original, const std::string& bytes, std::string& report);

/// The implementation the dispatcher, the comparisons and the self-check use: the hand-written reconstruction
/// (Native) by default; the static recompiler's (SSPC_NATIVE_ENGINE_RECOMP) is put here before the install, so the
/// same self-check, verify and guarded modes judge it.
void UseImplementation(Fn fn, const char* source);
Fn Implementation();
const char* ImplementationSource();

} // namespace ShadowInstanceUv
} // namespace SecondSon::NativeEngine::Reconstructed
