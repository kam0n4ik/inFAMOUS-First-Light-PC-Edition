// SecondSonPC R7-G — which System V arguments a guest function reads (its argument contract, from its own code).
//
// Why: a recorded callee compares only the argument registers its spec declares (gbuffer_trace.h). A register the
// callee reads but the spec does not declare is invisible to verification - 0x451440 reads R9 (the caller's mesh
// instance, left there by 0x1a2030's record building), the first native pass did not set it and verified equal in the
// recording runs, then crashed on the real callee. The contract therefore comes from the callee's body, not from the
// call site.
//
// Analysis: every path from the entry (direct jumps followed, a direct call contributes its target's own reads of the
// registers not yet written; after a call the caller-saved registers count as written); a register is an argument
// when some path reads it before writing it (a 32-bit write counts as a full write; an 8/16-bit write defines only
// later reads of that low part; zero idioms are not reads; the first source of an AVX scalar merge such as
// vcvtsi2ss xmm0, xmm0, edx supplies only upper lanes and is not counted). Stack arguments: reads of [rsp + x] / [rbp + x] above the return address while the stack
// pointer is tracked. Indirect calls / jumps end the path (counted in `unknown`).
#pragma once

#include "base/abi.h"

namespace SecondSon::NativeEngine::R7G {

struct ArgUse {
    u8 gpr = 0;     ///< bit i: RDI RSI RDX RCX R8 R9 read before written
    u8 xmm = 0;     ///< bit i: XMM0..XMM7
    u8 stack = 0;   ///< bit k: stack argument k (qword at entry rsp + 8 + 8k)
    u32 instructions = 0;
    u32 unknown = 0; ///< indirect calls / jumps met (their targets' reads are not included)
    bool ok = false; ///< false: undecodable code met
    u64 where[14] = {}; ///< the first read found of GPR 0..5 / XMM 0..7 (the instruction, or the call that reads it)
};

/// The arguments of the function at `address` (mapped code). `max_depth`: direct calls followed this deep.
ArgUse ArgumentsRead(u64 address, u32 max_depth = 4);

} // namespace SecondSon::NativeEngine::R7G
