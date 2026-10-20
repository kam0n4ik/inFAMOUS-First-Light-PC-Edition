// SecondSonPC — the CUE commit, eboot 0x14ad0 (our name "CUE::Commit"; no symbols in the eboot): verification and
// replacement of its static recompilation (recomp_14ad0). Native Engine R5-D (docs/NATIVE_RECOMP_R5_D.md §4).
//
// What it does [A: P-code and disassembly; E: bit-exact offline comparison with the eboot's code]: for each stage
// whose shadow is dirty (byte flags at cue + 0xc1c9 ...) it allocates a snapshot (0x123d0), copies the stage's shadow
// into it (memcpy), points the shader's user data at it (0x626400) and writes single values (0x626520), prefetches
// (0x623630), binds the stage's shader through Gnm (0x625100 ... 0x625aa0), writes a register range (0x624cb0, with a
// descriptor 0x638540 builds), then clears the flags. It writes no absolute address; it writes the CUE (the flags,
// two 16-byte shadow entries) and, through its callees, the snapshot arena and the command buffer.
//
// There is no hand-written reconstruction: the implementation is the recompiled function (SSPC_NATIVE_ENGINE_RECOMP
// with recomp_14ad0 and recomp_set_redirect); without it the install is refused and the game's code runs.
//
// Verification without double side effects: the original runs as a clone whose 18 calls with side effects go to
// recorders (guest_code_clone.h CodeRedirect), the implementation with the same recorders (recomp_set_redirect), each
// on its own copy of the CUE; the allocator hands out a per-thread arena, the memcpy copies into it, nothing reaches
// the command buffer or the CUE. Compared: the recorded calls (target, the argument registers the callee reads, a
// pointer into the CUE copy as its offset, a stack pointer as the 32 bytes it points at - the stack is zeroed before
// each run), both CUE copies, the arena. The pure builders it calls (0x61e0c0, 0x61e330, 0x639250, 0x6393a0,
// 0x639450, 0x6394e0) run for real in both: they write only their arguments [A]. Then the one real call runs.
// A shadow run copies 0xc710 bytes twice, so verify and guarded check every Nth call (SSPC_NATIVE_ENGINE_SAMPLE).
#pragma once

#include <iterator>
#include <string>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kCueCommitSpec; ///< 0x14ad0, key cue_commit

namespace CueCommit {

using Fn = SSPC_SYSV_ABI void (*)(u64 cue);

/// The callees whose calls are recorded instead of made in a shadow run, and the argument registers each reads
/// [A: read-before-write over its P-code; 0x9843a0 memcpy (dst, src, n), 0x984360 __stack_chk_fail ()].
struct Callee {
    u64 rva;
    u32 arity;
};
inline constexpr Callee kRecorded[] = {
    {0x123d0, 2},  {0x9843a0, 3}, {0x984360, 0}, {0x626400, 4}, {0x626520, 5}, {0x623630, 3},
    {0x624cb0, 3}, {0x625100, 2}, {0x6251c0, 2}, {0x625220, 3}, {0x625300, 3}, {0x625370, 3},
    {0x6253e0, 3}, {0x625510, 3}, {0x6256a0, 3}, {0x625a40, 2}, {0x625aa0, 2}, {0x638540, 5},
};
inline constexpr u32 kRecordedCount = static_cast<u32>(std::size(kRecorded));
inline constexpr u64 kCueBytes = 0xc710;    ///< the CUE as 0x14ad0 reads it (highest: the byte at + 0xc70e)
inline constexpr u64 kCopyBytes = 0x20000;  ///< a copy's buffer: the 14-bit shadow index reaches + 0x1489c
inline constexpr u64 kArenaBytes = 0x18000; ///< snapshots of one shadow run (7 stages x 0x1800 + 9 dwords)

struct Call {
    u64 target; ///< index into kRecorded (64-bit: no padding, recordings are compared with memcmp)
    u64 a[6];
    u8 content[6][32];
};
inline constexpr u32 kMaxCalls = 128; ///< 99 call sites, no loop
struct Recording {
    u32 calls = 0;
    bool overflow = false;
    Call call[kMaxCalls];
};

/// Runs `fn` on `cue` (a copy of kCopyBytes) with the 18 calls recorded into `out` (`recording_original`: the clone).
void RunRecorded(Fn fn, bool recording_original, u64 cue, Recording& out);
bool SameRecordings(const Recording& a, const Recording& b);

/// For tests: prepares like the install does (the recording clone).
bool Prepare(u64 module_base, std::string& error);
Fn RecordingOriginal();

void UseImplementation(Fn fn, const char* source, void (*redirect)(u64 from, u64 to));
Fn Implementation();

} // namespace CueCommit
} // namespace SecondSon::NativeEngine::Reconstructed
