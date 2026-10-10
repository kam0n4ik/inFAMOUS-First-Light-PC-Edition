// SecondSonPC — SecondSonPC's command processor on a recorded trace, without the game and without the bridge
// (Phase 4, P4.2; docs/PHASE4_P42_COMMAND_PROCESSOR.md §7–§8). --gpu-replay <trace> [--gpu-replay-bench <n>].
//
// The trace (cp_validation.h; recorded by --gpu-trace) holds the bridge's registers when the capture started, every
// submission in the order the bridge took them, the indirect buffers read, and the bridge's state before every draw
// and dispatch. The replay starts from that state, processes the submissions itself (Mode::Shadow: the trace has no
// memory to write) and at every checkpoint compares its registers — as the bridge would hold them — with the
// recorded ones: hashes at every checkpoint, every register where the trace holds a full snapshot (and resynchronises
// there, so one difference is reported once). The benchmark passes time the same work: the whole replay, every
// packet by opcode (P50/P95/P99), the decoder alone, register writes with and without the change tracking.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Gpu::Cp {

class TraceReader;

struct ReplayResult {
    u64 submissions = 0, packets = 0, draws = 0, dispatches = 0, frames = 0;
    u64 checkpoints = 0, hash_mismatches = 0, compute_mismatches = 0, extra_mismatches = 0, unreached = 0;
    u64 full_checks = 0, full_diff_regs = 0, faults = 0;
    std::vector<std::string> details; ///< the first differences, with the packet and register
    std::string error;                ///< the trace could not be replayed at all
    bool Matched() const {
        return error.empty() && checkpoints != 0 && hash_mismatches == 0 && compute_mismatches == 0 &&
               extra_mismatches == 0 && unreached == 0 && full_diff_regs == 0 && faults == 0;
    }
};

/// Replays `trace` and compares it with the bridge's recorded state.
bool Replay(const TraceReader& trace, ReplayResult& out);

/// --gpu-replay: loads, replays, compares (and times `bench_passes` passes); report <logs_dir>\gpu_replay.txt.
/// 0 when the replay matched the bridge at every checkpoint.
int RunReplay(const std::filesystem::path& trace, const std::filesystem::path& logs_dir, u32 bench_passes);

} // namespace SecondSon::Gpu::Cp
