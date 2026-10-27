// SecondSonPC — the hitch log: every slow frame of every session, when it happened and what it waited for. On whenever
// the native backend runs (SSPC_HITCH_LOG=0: off); a frame is slow when it was shown SSPC_HITCH_MS (default 50) ms or
// more after the previous one. A thread of its own reads the frame log (FrameLog) and samples the process every 50 ms;
// nothing is added to the game's or the GPU thread's hot path but relaxed counters.
//
// Files: logs\sessions\<date_time>\ (the newest 30 sessions are kept), written while the game runs (a crash keeps them):
//   hitches.txt   one line per slow frame: the local time, the session's seconds (gpu_frames.csv's time base), the
//                 frame, its length, the cause, where its time went - the game's own CPU work, the backend (the game
//                 waiting for it, the command processor's longer latency), the GPU's longer latency, presentation - and
//                 the backend's work in it (shader translations, pipelines, readback waits, recording, uploads) and the
//                 context (file reads, page faults, CPU taken by other programs, memory, VRAM); F12 marks
//   hitches.csv   the same, every value in its column
//   seconds.csv   per second: FPS, the slowest frame, slow frames, CPU (this process and other programs), page faults,
//                 memory, VRAM, file reads, guest memory maps, write-tracking faults, shaders and pipelines made
//   summary.txt   at the backend's stop: FPS, frame time percentiles, 1% low, slow frames by cause, the worst ones, the
//                 marks (also in SecondSonPC.log); gpu_frames.csv is copied next to it
#pragma once

#include <atomic>
#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Gpu::HitchLog {

namespace detail {
extern std::atomic<u64> g_file_bytes, g_file_ns, g_file_reads, g_guest_maps;
}

/// A game thread read a file (sceKernelRead/Pread/_read): bytes read, time.
inline void FileRead(u64 bytes, u64 ns) {
    detail::g_file_bytes.fetch_add(bytes, std::memory_order_relaxed);
    detail::g_file_ns.fetch_add(ns, std::memory_order_relaxed);
    detail::g_file_reads.fetch_add(1, std::memory_order_relaxed);
}
/// A guest memory map/unmap (the backend's memory listener).
inline void GuestMap() {
    detail::g_guest_maps.fetch_add(1, std::memory_order_relaxed);
}

/// The player saw a stutter (F12): a line in hitches.txt with the slow frames of the 5 seconds before it. Any thread.
void Mark();

/// Write-tracking faults so far (the backend's page tracker), read on the log's thread.
using FaultsFn = u64 (*)(void* ctx);
struct StartInfo {
    std::filesystem::path logs_dir;
    FaultsFn faults = nullptr;
    void* faults_ctx = nullptr;
};
/// Starts the log's thread (unless SSPC_HITCH_LOG=0). The native backend, after FrameLog::Start.
void Start(const StartInfo& info);
/// Takes the last frames, writes summary.txt, copies gpu_frames.csv (written already) and stops the thread.
void Stop();

} // namespace SecondSon::Gpu::HitchLog
