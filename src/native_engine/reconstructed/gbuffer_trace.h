// SecondSonPC — call-trace recording for the geometry pass reconstructions (R6-G; docs/NATIVE_RENDER_R6_GBUFFER.md §3).
// Design taken from R5-E's draft (docs/NATIVE_RECOMP_R5_E_GBUFFER.md §7), finished and generalised.
//
// The geometry pass functions (0x1a2030 the mesh pass loop, 0x1a1770 its instanced draw, 0x1a1960 the mesh draw) are
// orchestrators: almost everything they do is calls (CUE setters, Gnm packet writers, the material bind, the commit,
// the draws) whose side effects (DCB packets, ring allocations, CUE state) must happen exactly once. A reconstruction
// is therefore judged by its calls, as in R5-C/R5-D, without running anything twice against the game's state:
//   - the original runs as a clone (guest_code_clone.h) whose calls to the recorded callees go to recorder thunks;
//     the reconstruction runs with its callee table (hand-written) or recomp_set_redirects (recompiled) aimed at the
//     same thunks; both on the same inputs, each with its own copy of the memory the function itself writes;
//   - a recorded callee is not called: the thunk stores the argument registers, the stack arguments, the bytes the
//     callee's pointer arguments point at (per callee spec) and returns what the callee's effect says (a fixed
//     sequence, an allocation from the run's arena, the real callee for pure ones ...);
//   - the two traces (and the copies) are compared; pointers into a copy, an arena or the stack are compared as
//     "region + offset" / "stack", everything else as values.
// The callees that only compute into memory the function owns (V# builders, the stage table) are not redirected:
// they run for real in both runs (their bodies are pinned by hash in the reconstruction's spec).
//
// The thunk keeps every register of the call except RAX (and what the effect sets: RDX, XMM0), as an original whose
// recorded callee returned - code after a call does not read the caller-saved registers (System V).
#pragma once

#include <string>

#include "base/abi.h"

namespace SecondSon::NativeEngine::Reconstructed::GTrace {

inline constexpr u32 kMaxCallees = 48;

/// What a callee's pointer argument points at: the argument register (0..5 = RDI..R9) or stack argument (6 + k),
/// the count register (-1: one unit) and unit bytes; more than `max` bytes marks the trace (overflow, not compared).
struct Snap {
    s8 ptr = -1;
    s8 count = -1;
    u16 unit = 0;
    u32 max = 0;
};

/// The registers the thunk saved (RDI RSI RDX RCX R8 R9 R10 R11, then XMM0-15). An effect may change gpr[2] (RDX)
/// and xmm[0] (XMM0) to return them; the thunk restores all of them from here.
struct CallIn {
    u64 gpr[8];
    alignas(16) u8 xmm[16][16];
};

struct Trace;
struct CalleeSpec;
/// A recorded callee's behaviour in a recording run: RAX (and RDX/XMM0 through `in`), memory it writes that the
/// function reads back. nullptr: returns 0.
using EffectFn = u64 (*)(Trace& t, const CalleeSpec& callee, CallIn& in, const u64* stack);

inline constexpr u32 kSnaps = 4; ///< pointer arguments snapshotted per call

struct CalleeSpec {
    u64 rva;
    const char* name;
    u8 gpr;    ///< argument registers compared
    u8 widths; ///< bit i: argument register i is 32-bit (only its low half compared, for a hand-written caller)
    u8 xmm;    ///< XMM argument registers compared
    u8 stack;  ///< stack arguments (qwords) compared
    Snap snap[kSnaps];
    EffectFn effect = nullptr;
    bool record = true; ///< false: the effect runs, nothing is recorded (a pure callee the caller needs, e.g. memmove)
    u8 ignore = 0;       ///< bit i: argument register i below `gpr` is not an argument (the callee never reads it)
    u8 stack_widths = 0; ///< bit k: stack argument k is 32-bit (low half compared, for a hand-written caller)
};

struct Call {
    u32 callee;
    u32 flags; ///< 1: a snapshot exceeded its max
    u32 snap_off[kSnaps];
    u32 snap_len[kSnaps];
    u64 gpr[8];
    u64 stack[4];
    alignas(16) u8 xmm[8][16];
};

struct Region {
    u64 base = 0;
    u64 size = 0;
};
inline constexpr u32 kMaxRegions = 6;

/// One recording run's trace (owned by the verifying thread; the buffers are its own).
struct Trace {
    const CalleeSpec* callees = nullptr;
    u32 ncallees = 0;
    Call* calls = nullptr;
    u32 max_calls = 0;
    u8* snap = nullptr;
    u32 snap_cap = 0;
    u8* arena = nullptr; ///< the effects' allocations (registered as a region by Begin when set)
    u64 arena_cap = 0;
    Region regions[kMaxRegions];
    u32 nregions = 0;
    u64 user[4] = {}; ///< for the effects (e.g. the shadow context they write)
    // per run
    u32 ncalls = 0;
    u32 snap_used = 0;
    u64 arena_used = 0;
    u64 sequence = 0;
    u64 stack_lo = 0, stack_hi = 0; ///< the stack window of the run (pointers into it compare as "stack")
    bool overflow = false;
    bool stack_check_failed = false;

    /// Starts a run: counters cleared, the effects' sequence seeded, the stack window (64 KB) below `stack_top` (an
    /// address in the frame of the function that makes the run: every frame of the run is below it).
    void Begin(u64 seed, u64 stack_top);
    /// 16-aligned arena allocation (0 and overflow when full).
    u64 Allocate(u64 bytes, u64 alignment);
    void AddRegion(u64 base, u64 size);
};

/// The thunk of callee `index` of the active trace's table.
u64 ThunkAddress(u32 index);

/// The calling thread's active trace (a thunk reached without one returns 0 and counts it: OrphanCalls()).
void SetActive(Trace* t);
Trace* Active();
u64 OrphanCalls();

/// Compares two runs: "" when equal, else the first difference: the calls' order, their declared argument registers
/// (exact: whole - a recompiled function against the clone; otherwise the 32-bit ones in `widths` by their low half),
/// XMM and stack arguments, the bytes their pointer arguments point at, the effects' allocations.
std::string Compare(const Trace& a, const Trace& b, bool exact);

/// A pointer value as both runs see it: "stack", "region k + offset", or the value.
u64 Normal(const Trace& t, u64 v);

inline u64 Mix(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    return x ^ (x >> 33);
}

/// The effects' deterministic values: both runs draw the same sequence.
inline u64 Next(Trace& t) {
    t.sequence = Mix(t.sequence + 0x9e3779b97f4a7c15ull);
    return t.sequence;
}

} // namespace SecondSon::NativeEngine::Reconstructed::GTrace
