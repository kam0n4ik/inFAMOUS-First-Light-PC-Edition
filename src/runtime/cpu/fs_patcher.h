// SecondSonPC — guest FS-relative instructions on Windows (Phase 3, P3.3;
// docs/PHASE3_INDEPENDENT_RUNTIME.md §5). On the PS4 `fs:` addresses the thread's TCB; on Windows the
// FS base of 64-bit code is 0, so `mov rax, qword ptr fs:[0]` would read address 0. Every such
// instruction of a registered guest module is replaced by a `jmp rel32` to a trampoline in the
// module's trampoline area that loads the TCB from SecondSonPC's TLS slot (tcb_slot.h) and does the
// original operation on it, then jumps back.
//
// Two paths, one owner (this file; the bridge's core/cpu_patches.cpp is not used for these modules):
//  * load time (PatchModuleStatically): every FS instruction found by decoding the module's functions
//    (.eh_frame boundaries) is patched before the module runs — no other thread can execute it then,
//    so no live-code race exists for these sites;
//  * run time (the vectored handler, InstallFsPatcher): an FS instruction the static pass did not reach
//    faults on first use (read of address = displacement); it is patched while other threads may run
//    it, with the protocol of SSPC-002's fix re-audited for this patcher: int3 into the first byte,
//    the rest of the jump, then its first byte, each step published to every processor running the
//    process (FlushProcessWriteBuffers). A thread that meets the int3 (or whose int3 the kernel reports
//    as a privileged instruction because the jump replaced it meanwhile) waits for the patch and
//    re-executes the instruction. Only this patcher's own sites are claimed; any other breakpoint,
//    privileged instruction or access violation goes on to the next handler.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/abi.h"

struct _EXCEPTION_POINTERS;

namespace SecondSon::Cpu {

/// The operation of an FS-relative instruction this patcher can rewrite: `op reg, fs:[disp]` (load
/// forms) or `mov fs:[disp], reg` (store), 32 or 64 bits, no base or index register.
enum class FsOp : u8 { Load, Store, Xor, Cmp, Add, Sub, And, Or };

struct FsInstruction {
    FsOp op;
    u8 reg;     // general purpose register number (0 = rax ... 15 = r15)
    u8 width;   // operand size in bytes: 4 or 8
    u8 length;  // bytes of the original instruction
    s32 disp;   // displacement from the TCB (signed: static TLS lies below it)
};

/// Decodes the instruction at `code` (at most `available` bytes). nullopt: not an FS-relative
/// instruction of a supported form. `fs_relative` (if given) is set when the instruction does use
/// the FS segment — true with nullopt means a form this patcher cannot rewrite.
std::optional<FsInstruction> DecodeFs(const u8* code, u64 available, bool* fs_relative = nullptr);

/// Writes the trampoline of `insn` (at `site`, `slot` = the TCB slot) into [out, out + capacity)
/// placed at address `at`, and returns its size, 0 if it does not fit. It ends with a jump to
/// site + insn.length. Exposed for the self-test.
u64 EncodeTrampoline(const FsInstruction& insn, u64 site, u32 slot, u64 at, u8* out, u64 capacity);

/// This patcher can own the guest's code (since P3.4 always: it emulates SSE4a too). `why_not` (a note,
/// despite the name kept for its callers): what it does not reproduce on this CPU — on Intel, AMD's
/// rcpps/rsqrtps precision.
bool CpuNeedsOnlyFsPatches(std::string* why_not = nullptr);

/// Makes the patcher ready for its exceptions. `own_handler`: registers its own vectored exception
/// handler (first in the chain); without it SecondSonPC's exception dispatcher (exceptions.h, P3.4)
/// calls the Handle* functions below. Call once, after InitTcbSlot, before any registered module runs.
/// false: the handler could not be added.
bool InstallFsPatcher(bool own_handler = true);
bool FsPatcherInstalled();

/// The patcher's part of exception handling (true: resolved, resume the context). An access violation
/// at an FS-relative instruction of a registered module (patched now, or found patched); ...
bool HandlePatcherAccessViolation(_EXCEPTION_POINTERS* info);
/// ... the int3 of a site being patched (misreported: reported as a privileged instruction) ...
bool HandlePatcherTrap(_EXCEPTION_POINTERS* info, bool misreported);
/// ... an instruction the host CPU lacks, at a site of a registered module (emulated in place).
bool HandleIllegalInstruction(_EXCEPTION_POINTERS* info);

/// A guest module: code [code, code + code_size), trampolines [trampoline, + trampoline_size)
/// (read/write/execute, within ±2 GiB of every code byte). Before the module runs. false: table full
/// or bad ranges.
bool RegisterModule(u64 code, u64 code_size, u64 trampoline, u64 trampoline_size, std::string_view name);

/// Native Engine Reconstruction (src/native_engine): `bytes` (rounded up to 64) of the trampoline area of the
/// registered module whose code holds `address`, taken from the area's top - the patcher's own trampolines grow from
/// its bottom and keep at least 4 MiB. For code that must reach the module with rel32 (the guest's reservation leaves
/// no free memory within ±2 GiB of it). The memory stays read/write/execute and is never given back. 0: no module
/// holds `address` (e.g. the bridge's CPU patcher runs instead) or no room.
u64 AllocateModuleCode(u64 address, u64 bytes);

struct StaticResult {
    u64 functions = 0;       // decoded functions
    u64 patched = 0;         // FS instructions rewritten
    u64 unsupported = 0;     // FS instructions of a form this patcher cannot rewrite
    u64 undecodable = 0;     // functions whose decoding did not end exactly at their end (data in the range)
    u64 left_to_runtime = 0; // FS instructions found past the trusted part of such a function
    u64 red_zone_functions = 0; // functions that keep data below rsp (marked for the exception dispatcher)
    // SSE4a (P3.4), only while it is emulated (SetSse4aEmulation):
    u64 sse4a_trampolines = 0;     // extrq/insertq of 5+ bytes rewritten to jump to a trampoline
    u64 sse4a_inplace = 0;         // movntss/movntsd rewritten to movss/movsd
    u64 sse4a_short = 0;           // too short for a jump: executed from the illegal-instruction exception
    u64 sse4a_left_to_runtime = 0; // past the trusted part of a function: the same exception, decoded then
};

/// A function's code [begin, end): the range of its .eh_frame FDE.
struct FunctionRange {
    u64 begin;
    u64 end;
};

/// Load time: decodes each function inside [segment, segment + size) and patches its FS instructions.
/// Instruction boundaries are trusted to the function's end if its decoding ends exactly there; if it
/// does not (data in the range — a switch jump table), only up to the last unconditional transfer
/// (ret/jmp/ud2/int3/hlt) before decoding stopped; sites beyond are left to the run-time path. The
/// module must be registered and must not be running.
StaticResult PatchModuleStatically(u64 segment, u64 size, std::span<const FunctionRange> functions);

/// P3.4: only marks the functions that keep data below rsp (Exceptions::MarkRedZoneFunction) — when the
/// bridge's patcher owns the code and PatchModuleStatically does not run. Returns how many were marked.
u64 AnalyzeRedZone(std::span<const FunctionRange> functions);

struct Stats {
    u64 modules;
    u64 static_sites;     // patched at load time
    u64 runtime_sites;    // patched by the handler
    u64 runtime_repeats;  // faults at a site another thread had already patched
    u64 breakpoints;      // threads sent back to a site being patched
    u64 misreported;      // ... of them reported by the kernel as a privileged instruction
    u64 unsupported;      // FS instructions that could not be patched (left to fault)
    u64 trampoline_bytes; // used in all trampoline areas
    u64 first_unsupported; // address of the first one the handler met (0: none); logged at exit
    u64 sse4a_trampolines; // SSE4a sites (P3.4): rewritten to trampolines ...
    u64 sse4a_inplace;     // ... movnt* rewritten in place ...
    u64 sse4a_short;       // ... executed from their exception ...
    u64 sse4a_emulated;    // ... executions through that exception
};
Stats GetStats();

/// P3.4: SSE4a is emulated when the host has none, or `forced` (--cpu-emulate sse4a, diagnostics: the
/// short sites are made to trap on an AMD CPU as they would on an Intel one). Before any module is patched.
void SetSse4aEmulation(bool forced);
bool Sse4aEmulated();

/// The instruction sets the game's code uses besides x86-64 and SSE4a (tools/cpu_audit.py): SSE2-SSE4.2,
/// POPCNT, MOVBE, AVX (with the system saving ymm), F16C, BMI1, LZCNT. false: `missing` lists what the CPU lacks.
bool HostMeetsGameRequirements(std::string* missing);

namespace Test {
/// Whether the handler would take a breakpoint (`misreported`: privileged instruction) at `address`
/// as one of this patcher's sites — without raising it (the self-test cannot let a foreign trap reach
/// the bridge's handler, which ends the process on it).
bool ClaimsTrap(u64 address, bool misreported);
} // namespace Test

} // namespace SecondSon::Cpu
