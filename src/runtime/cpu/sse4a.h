// SecondSonPC — AMD SSE4a for CPUs without it (Phase 3, P3.4; docs/PHASE3_P34_EXCEPTIONS_CPU.md).
//
// The game's code was built for the PS4's Jaguar, which has SSE4a; no Intel CPU does. Audit
// (tools/cpu_audit.py): eboot.bin has 96 `extrq xmm, xmm` (73 of them 4 bytes long, 23 with a REX prefix,
// 5 bytes) and no insertq/movntss/movntsd; libc.prx and libSceFios2.prx have none. The CPU patcher
// (fs_patcher.h) rewrites them at load time: a form of 5 bytes or more becomes a jump to a trampoline that
// does the operation with general registers; movntss/movntsd become movss/movsd in place (the same store
// without the non-temporal hint); a 4-byte form, too short for a jump, is executed by the exception
// dispatcher from its illegal-instruction exception (EmulateInContext). Nothing is written into code that
// runs.
//
// Semantics: as AMD's APM, and where it leaves them undefined as measured on Zen 3 (Ryzen 5 5500): the
// upper 64 bits of the destination are zeroed by all four forms; bits 255:128 of the ymm register are
// kept (legacy SSE encoding). A length of 0 means 64.
#pragma once

#include <optional>

#include "base/abi.h"

struct _CONTEXT;

namespace SecondSon::Cpu::Sse4a {

enum class Op : u8 { ExtrqImm, ExtrqReg, InsertqImm, InsertqReg, Movntss, Movntsd };

struct Insn {
    Op op;
    u8 dst;       // xmm register number (0-15); for movnt*: the stored register
    u8 src;       // xmm register number (reg forms, insertq imm)
    u8 len;       // imm forms: length field (0 = 64)
    u8 idx;       // imm forms: index field
    u8 length;    // bytes of the instruction
    u8 opcode_at; // movnt*: offset of the 0x2B opcode byte (rewritten to 0x11)
};

/// The SSE4a instruction at `code`, or nullopt.
std::optional<Insn> Decode(const u8* code, u64 available);

/// The reference semantics on 128-bit values {low, high}: `dst` is updated (extrq/insertq only).
void Execute(const Insn& insn, u64 dst[2], const u64 src[2]);

/// Trampoline of an extrq/insertq form at `site` placed at address `at` in [out, out + capacity): the
/// operation with rax/rcx/rdx/r8 saved below the red zone and the flags kept, then a jump to
/// site + length. Returns its size, 0 if it does not fit or the form has no trampoline.
u64 EncodeTrampoline(const Insn& insn, u64 site, u64 at, u8* out, u64 capacity);

/// Executes extrq/insertq on the xmm registers of a thread context and moves rip past it.
bool EmulateInContext(const Insn& insn, _CONTEXT* context);

/// CPUID: the host executes SSE4a itself.
bool HostHasSse4a();

} // namespace SecondSon::Cpu::Sse4a
