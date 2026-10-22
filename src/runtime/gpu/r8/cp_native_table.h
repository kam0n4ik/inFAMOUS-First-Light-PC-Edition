// SecondSonPC R8-A2 — the snapshot table: what the native CUE commit knows about each stage snapshot it wrote, for the
// command processor's thread (docs/NATIVE_ENGINE_R8_CP_OPTIMIZATION.md §3).
//
//   game thread (R7::Resources::Commit, after recomp_14ad0)   Publish(snapshot address -> version, extent, pointers)
//        │  a lock-free direct-mapped table; each entry written under its own sequence lock, before the DCB that
//        │  points at the snapshot is submitted (the commit writes both; the submission orders them for thread 1)
//        ▼
//   thread 1 (ParallelCp, at a draw)                           Find(the stage's user data) -> the entry, or nothing
//
// The table is a hint only (docs/NATIVE_ENGINE_R8_CORE.md §5.2 rule 1): an entry names the snapshot its address held
// when the native commit wrote it. A snapshot written at that address later by a writer the commit does not see (the
// dispatch-draw commit 0x140d0, a commit run as the game's code, another frame reusing the arena chunk) leaves the old
// entry in place - a consumer that reuses work by it checks the bytes it relies on itself.
#pragma once

#include <array>
#include <span>
#include <string>

#include "base/abi.h"

namespace SecondSon::Gpu::R8 {

/// One stage snapshot of a native CUE commit.
struct SnapshotInfo {
    u64 base = 0;    ///< the snapshot (the commit's copy of the stage's shadow in the frame's ring)
    u64 cue = 0;
    u64 version = 0; ///< R7 binding version of (cue, stage); 0: tracking does not vouch for it
    u32 dwords = 0;
    u8 stage = 0;    ///< the CUE stage (Gnm numbering: 1 PS, 2 VS)
    u8 refs = 0;     ///< user-data SGPR pairs the commit pointed into the snapshot
    std::array<u8, 7> ref_sgpr{};
    std::array<u16, 7> ref_offset{}; ///< dwords from the base
};

/// The table is filled (SSPC_R8_CP_DIAG=1, or an R8 CP mechanism that consumes it; read once). Off: Publish is never
/// called and Find finds nothing.
bool TableOn();
/// Tests: the table on without the environment (before its first use).
void EnableForTests();

/// A native commit's stage snapshot (game threads).
void Publish(const SnapshotInfo& s);

/// The entry of the snapshot at `base` (any thread; false: none, or one being rewritten right now).
bool Lookup(u64 base, SnapshotInfo& out);

/// The snapshot a stage's 16 user-data registers point at: the first register pair holding the base of a published
/// snapshot of `cue_stage` whose every pointer pair the commit wrote is in `user_data` as written. `pointer_mask`: the
/// registers holding those pointers (their values differ from one snapshot to the next by construction).
bool Find(std::span<const u32, 16> user_data, u32 cue_stage, SnapshotInfo& out, u32& pointer_mask);

struct TableStats {
    u64 published = 0, lookups = 0, found = 0, torn = 0;
};
TableStats Stats();

} // namespace SecondSon::Gpu::R8
