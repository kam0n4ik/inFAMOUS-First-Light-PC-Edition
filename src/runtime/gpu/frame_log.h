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
    // shader preparation (docs/SHADER_PRECOMPILATION.md): what a frame waited for before its draws could be recorded
    u64 shaders = 0, shader_ns = 0;     ///< translations made (GCN -> SPIR-V) and their time
    u64 shader_disk = 0;                ///< translations taken from the disk cache instead
    u64 pipelines = 0, pipeline_ns = 0; ///< pipelines created (graphics + compute) and the driver's time for them
    u64 module_ns = 0;                  ///< vkCreateShaderModule and the auxiliary stages' emission
    // P4.10: the processor's time split further; the parallel command processor (SSPC_P410_PARALLEL_CP=1)
    u64 callbacks_ns = 0;     ///< of cp_ns: the processor's callbacks other than draws/dispatches (fences, DMA, ...)
    u64 par_wait_ns = 0;      ///< of cp_ns: the GPU thread waiting for the recording thread (drains, a full queue)
    u64 par_busy_ns = 0;      ///< the recording thread executing records (draws/dispatches: record_ns is of it)
    u64 par_idle_ns = 0;      ///< the recording thread waiting for records
    u64 par_drains = 0;       ///< drains that waited
    u64 par_records = 0;      ///< records pushed
    u64 par_depth_sum = 0;    ///< queue depth at each push (mean: / records)
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
/// Seconds since the log started (the origin of the CSV's times); -1 before it started.
double Seconds();
/// The TSC at the log's start (0 before it started).
u64 StartTsc();

/// P4.8 (the Performance Inspector streams the rows while the game runs): the rows are a ring of the last kCap frames;
/// a row's fields as they are now (TSC values, 0 when not reached yet).
struct RowView {
    u64 game_submit = 0, flip_enter = 0, game_flip = 0, done_enter = 0, done_exit = 0;
    u32 submits = 0;
    u64 cp_flip = 0, gpu_value = 0, gpu_done = 0, last_unblock = 0, present = 0;
    u64 idle_wait_ns = 0, idle_waits = 0;
    GpuThreadTotals totals;
};
struct Progress {
    u32 game = 0, cp = 0, done = 0, present = 0; ///< flips requested / flip markers reached / completed / presented
};
Progress GetProgress();
/// Frame `n`'s row; false when the log has not started or the ring holds a later frame there.
bool Read(u32 n, RowView& out);
/// The CSV (once the backend's threads stopped). false: not started or not written.
bool Write(const std::filesystem::path& file);

} // namespace SecondSon::Gpu::FrameLog
