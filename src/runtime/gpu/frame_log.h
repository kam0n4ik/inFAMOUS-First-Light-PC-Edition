// SecondSonPC — the native backend's frame log (Phase 4, P4.6; docs/PHASE4_P46_PERFORMANCE.md §1, §5). On whenever the
// native backend runs: one row per flip, each field written by one thread (TSC samples and the GPU thread's running
// totals at the frame's flip marker) — no lock, no allocation and no log on the hot path. At the backend's stop:
// logs\gpu_frames.csv; tools\perf\frame_stats.py turns a time window of it into FPS, 1% low, frame time percentiles and
// the frame's critical path (the game's submission, the command processor, the GPU, the vblank).
//
// Rows pair by order: the n-th flip the game requests (SubmitAndFlip), the n-th flip marker the command processor
// reaches and the n-th EOP flip VideoOut presents are one frame.
#pragma once

#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Gpu::FrameLog {

/// The GPU thread's running totals, sampled at each flip marker (ns unless named; per-frame values are differences).
struct GpuThreadTotals {
    u64 cp_ns = 0;            ///< busy: the processor's rounds (packets, recording, its waits), polls and flushes
    u64 record_ns = 0;        ///< of it: recording draws and dispatches
    u64 sync_ns = 0;          ///< of it: the processor waiting for the GPU (Synchronize)
    u64 blocked_label_ns = 0; ///< idle, a WAIT_REG_MEM blocked on a VideoOut buffer label (a flip still to be shown)
    u64 blocked_other_ns = 0; ///< idle, a WAIT_REG_MEM blocked on other memory (flags the game's CPU writes)
    u64 empty_ns = 0;         ///< idle with nothing to process (waiting for the game's submissions)
    u64 gpu_busy_ns = 0;      ///< the GPU executing batches (timestamps of the completed ones)
    u64 draws = 0, dispatches = 0, batches = 0, blocks = 0; ///< blocks: WAIT_REG_MEM that blocked
};

/// The backend starts: time zero and the rows' storage.
void Start();
/// A graphics submission of the game (counted; the first one after the previous flip is the frame's first).
void GameSubmit();
/// sceGnmSubmitAndFlip* entered (before it waits for the submission lock): the game has the frame ready.
void GameFlipEnter();
/// ... and handed on (the flip requested, its command buffers submitted).
void GameFlip();
/// sceGnmSubmitDone entered / left (it waits for the submission lock like every submission).
void GameSubmitDone(u64 enter_tsc, u64 exit_tsc);
/// A game thread waited `ns` for the submission lock (sceGnmSubmitDone's: until the GPU is idle) - any submit call.
void GameIdleWait(u64 ns);
/// The GPU thread at a flip marker, the frame's last batch submitted; `gpu_value`: the timeline value it completes at;
/// `last_unblock_tsc`: when the last WAIT_REG_MEM that had blocked was met (the game's last flag before the frame end).
void CpFlip(u64 gpu_value, const GpuThreadTotals& totals, u64 last_unblock_tsc);
/// The GPU thread saw the timeline reach `completed` (its end-of-pipe labels are written now).
void GpuCompleted(u64 completed);
/// The present thread showed the next EOP flip.
void Presented();
/// The CSV (once the backend's threads stopped). false: not started or not written.
bool Write(const std::filesystem::path& file);

} // namespace SecondSon::Gpu::FrameLog
