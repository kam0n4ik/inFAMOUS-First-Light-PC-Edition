// SecondSonPC — reconstruction of eboot 0x11e5d0 (our name "R::SetDrawConstants"; no symbols in the eboot). Native
// Engine R5-E (docs/NATIVE_RECOMP_R5_E_GBUFFER.md).
//
// What it does [A: disassembly; RENDER_ENGINE_ARCHITECTURE.md §4.3; E: bit-exact offline comparison with the eboot]:
// the compare-and-set of the 1 KiB draw-constant block of a render context (R + 0xda18, the mesh pass passes it as
// `block`), `count` floats from `src` into the block's float slots `offset`, `offset + 1`, ...; a slot is written
// only when its value differs, and then the block's dirty byte (block + 0x404 = R + 0xde1c) is set to 1. 226 callers
// (the mesh pass 0x1a2030, 0x1742f0, 0x450460, 0x451290, ...).
//
//   System V: (rdi block, si offset (u16), rdx src, ecx count (s32))
//
//   count <= 0           nothing
//   per slot i, in order: new = src[i], old = block[offset + i] (both 4 bytes)
//     vucomiss old, new: equal OR unordered (a NaN on either side)  -> nothing
//     otherwise                                                    -> block[+0x404] = 1, block[offset + i] = new
//
// The comparison is the CPU's own vucomiss on the thread's MXCSR (DAZ makes denormals equal to zero, -0 == +0, a NaN
// never "differs"): the reconstruction uses the same instruction. Per slot the read, compare and writes happen in
// the original's order, so a `src` that overlaps the block behaves the same.
//
// Verification: both run on shadow copies of the written span (the slots and the dirty byte, which may lie inside
// it); a call whose `src` overlaps that span, or whose span exceeds 8 KiB, runs the original only (skipped).
#pragma once

#include <string>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kDrawConstantsSetSpec; ///< 0x11e5d0, key draw_constants_set

namespace DrawConstantsSet {

inline constexpr u64 kDirtyOffset = 0x404;
inline constexpr u64 kMaxSpan = 0x2000;

using Fn = SSPC_SYSV_ABI void (*)(u64 block, u32 offset, u64 src, u32 count);

SSPC_SYSV_ABI void Native(u64 block, u32 offset, u64 src, u32 count);

bool Replay(const std::string& bytes, std::string& report);
void UseImplementation(Fn fn, const char* source);
Fn Implementation();
const char* ImplementationSource();

} // namespace DrawConstantsSet
} // namespace SecondSon::NativeEngine::Reconstructed
