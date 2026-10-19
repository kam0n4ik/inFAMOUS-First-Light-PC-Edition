// SecondSonPC — reconstruction of eboot 0x1f0870, "FrameScratch::Alloc" (our name; no symbols in the eboot).
// Second function of Native Engine Reconstruction (docs/NATIVE_ENGINE_FUNCTIONS.md): shared memory and atomics.
//
// What it does [A: disassembly, docs/native-reconstruction/RENDER_ENGINE_ARCHITECTURE.md §6; E: bit-exact offline
// comparison with the eboot's code; in the game every call of a 10 s run went to the reconstruction]: the lock-free
// bump allocator of the frame's scratch memory (instance constants of the shadow and G-buffer draws, particles, UI -
// 19 call sites). System V: (rdi scratch, esi size, edx alignment) -> rax address or 0.
//
//   size 0                     -> 0
//   primary arena  +0x38 cursor (CAS), +0x40 end: p = align_up(cursor, alignment); cursor = p + size;
//                  p != 0 and p + size <= end   -> p
//   otherwise the cursor stays advanced (the original does not roll it back) and the fallback arena is used:
//   fallback arena +0x68 cursor (CAS), +0x70 end: q = align_up(cursor, alignment); cursor = q + size;
//                  q + size <= end -> q, else 0
//   align_up(x, a) = (x + a - 1) & -a in 64-bit arithmetic with a zero-extended from 32 bits (a = 0 gives 0).
//   Each end is read once, before its CAS loop; the CAS is `lock cmpxchg` (sequentially consistent).
#pragma once

#include <string>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kFrameScratchAllocSpec;

namespace FrameScratch {

inline constexpr u32 kObjectBytes = 0x78; ///< the allocator's fields end at +0x78

SSPC_SYSV_ABI u64 Native(u64 scratch, u32 size, u32 alignment);

using Fn = SSPC_SYSV_ABI u64 (*)(u64, u32, u32);

/// Runs `original` and the reconstruction on copies of the allocator object (`a`, `b`: kObjectBytes each, any
/// address) and compares results and objects bit for bit.
bool Compare(Fn original, const u8* object, u32 size, u32 alignment, u8* a, u8* b, u64& result_a, u64& result_b);

bool Replay(Fn original, const std::string& bytes, std::string& report);

/// The implementation the dispatcher, the comparisons and the self-check use: the hand-written reconstruction
/// (Native) by default; the static recompilation PoC (SSPC_NATIVE_ENGINE_RECOMP) puts the machine-translated one here
/// before the install, so the same self-check, verify and guarded modes judge it.
void UseImplementation(Fn fn, const char* source);
Fn Implementation();
const char* ImplementationSource();

} // namespace FrameScratch
} // namespace SecondSon::NativeEngine::Reconstructed
