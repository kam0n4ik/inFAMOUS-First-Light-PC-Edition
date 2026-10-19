// SecondSonPC — reconstruction of eboot 0x11c890, "ObjectConstants::Update" (our name; no symbols in the eboot).
// Pilot of Native Engine Reconstruction: docs/NATIVE_ENGINE_RECONSTRUCTION.md §3, docs/NATIVE_ENGINE_FUNCTIONS.md.
//
// What it does [A: disassembly + Ghidra; E: bit-exact offline comparison with the eboot's code]: the per-draw object
// constants of a draw context (R + 0xd190, R a pool context of the render passes) are refreshed from the object's
// matrices, each block only when its bit in `flags` is set, and the block's "changed" byte (+0x158) is OR-ed with
// whether any value differs from what the block held - the caller (0x1a2030 mesh pass loop, 0x43d590 shadow pass,
// 0x19cf20, 0x19fbd0, 0x3f93e0, 0x561810, 0x5a8b80) re-uploads the constants only when that byte is set.
//
//   System V: (rdi world[4], rsi previous world[4], rdx view-projection[4], rcx object, r8 extra[4], r9d value,
//              [rsp+8] flags (u32, low byte used), [rsp+16] block)            float4 rows, row-major; returns nothing
//
//   flags bit | block offset | content
//   0         | +0x00..0x3f  | world rows 0-2 with w = 0, row 3 with w = 1
//   7         | +0x40..0x7f  | the same of the previous world (motion vectors) [H: previous-frame transform]
//   1         | +0x80..0xaf  | world rows 0-2 normalised (rsqrt + one Newton step), w = 0 [H: normal matrix]
//   2         | +0xb0..0xef  | world (rows 0-2 w = 0, row 3 implicit w = 1) x view-projection
//   3         | +0xf0..0x12f | `extra` like bit 0 [H: another transform, e.g. light/shadow space]
//   4         | +0x130 (2 x f32) | from object+0x10: (1, -k) if bit 0 set, else (0, 1) when k == 1, else (-1, k);
//             |              |  k = byte 2 of object+0x10 / 64  [H: fade / dissolve parameters]
//   5         | +0x138 (s32) | value
//   6         | +0x13c (s32) | value
//   -         | +0x158 (u8)  | |= any block above differed (ordered "not equal": NaN compares as different for
//             |              |   the vector blocks, as equal for +0x130 - the original's own vcmpeqps / vucomiss)
//
// Data it embeds (checked at install): constants at eboot 0x9934e0 (-1.0 x4), 0x9934f0 (0.5 x4), 0x993500 (-0.0 x4),
// 0x993510 (0, 1, 0, 0), 0x9936d8 (1.0, 1/64, -1.0).
#pragma once

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kObjectConstantsSpec;

namespace ObjectConstants {

inline constexpr u32 kBlockBytes = 0x160; ///< everything the function reads or writes in the block (+0x158 is last)

/// The reconstruction itself (System V, as the guest calls it).
SSPC_SYSV_ABI void Native(const float* world, const float* previous, const float* view_projection, u64 object,
                          const float* extra, s32 value, u32 flags, u8* block);

using Fn = SSPC_SYSV_ABI void (*)(const float*, const float*, const float*, u64, const float*, s32, u32, u8*);

/// Runs `original` and the reconstruction on copies of `block` and compares them bit for bit; `a`/`b` receive the
/// copies. True when equal.
bool Compare(Fn original, const float* world, const float* previous, const float* view_projection, u64 object,
             const float* extra, s32 value, u32 flags, const u8* block, u8* a, u8* b);

/// A recorded case (native_engine_verify.h) replayed against `original`: true when the reconstruction matches.
bool Replay(Fn original, const std::string& bytes, std::string& report);

} // namespace ObjectConstants
} // namespace SecondSon::NativeEngine::Reconstructed
