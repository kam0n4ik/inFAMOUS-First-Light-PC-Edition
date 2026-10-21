// SecondSonPC R6-Core — snapshot reuse in the CUE commit 0x14ad0 (SSPC_R6_CUE_DEDUP=1, off by default;
// docs/NATIVE_RENDER_R6_CORE.md §6).
//
// The commit copies a dirty stage's shadow into a new snapshot of the CUE's linear allocator (0x123d0: cur +0x20,
// start +0x28, end +0x30) and points the shader's user data at it (0x626400). The shadow is often byte-equal to the
// snapshot the same stage got a few draws before, but the pointer is new, so the command processor walks the
// resource table again and the draw cannot join a run. With the flag the native commit (recomp_14ad0 under
// SSPC_NATIVE_ENGINE=native, cue_commit) reuses that earlier snapshot instead:
//   - the earlier snapshot P of (CUE, stage) is reused only when the allocator's start is the one P was taken under
//     and the one at the commit's entry, and start <= P, P + n <= cur at the commit's entry (P lies in what the chunk
//     instance held before this commit: the allocator sets cur = start on every new chunk or reset, 0x123d0 /
//     0x12390, and only grows cur after that; the commit's own allocations may still be unwritten), and the n bytes
//     at P equal the shadow now (memcmp at the commit);
//   - then the new snapshot stays allocated (the arena is used as the game uses it) but is not written, and each
//     user-data pointer into it is moved to the same offset in P (0x626400 is the only callee of 0x14ad0 that gets a
//     snapshot pointer - checked offline, r6_core_tests.cpp).
// Lifetime: P and the new snapshot are in the same chunk instance, which the GPU reads to the frame's end; the
// allocator's users write a snapshot once, when they take it [A: 0x14ad0], so equal bytes at the commit stay equal
// while the chunk lives. The command processor's stage-memo verification checks the reuses where they are consumed.
#pragma once

#include "base/abi.h"

namespace SecondSon::NativeEngine::R6::CueDedup {

/// SSPC_R6_CUE_DEDUP=1, read once.
bool Enabled();
/// Called by the cue_commit install: the module base and recomp_set_redirect.
void Prepare(u64 module_base, void (*redirect)(u64 from, u64 to));
/// One native commit of `cue` by `impl` (recomp_14ad0) with the snapshot calls redirected.
void Commit(SSPC_SYSV_ABI void (*impl)(u64), u64 cue);

struct Stats {
    u64 commits, snapshots, reused, bytes_saved, miss_cold, miss_chunk, miss_bytes, moved_pointers, other_copies;
};
Stats Totals();

} // namespace SecondSon::NativeEngine::R6::CueDedup
