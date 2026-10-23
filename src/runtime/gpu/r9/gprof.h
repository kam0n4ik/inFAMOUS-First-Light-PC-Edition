// SecondSonPC R9 - the gameplay profiler (SSPC_R9_GPROF=1|2, off by default; docs/R9_GAMEPLAY_PERFORMANCE.md §2).
//
// What it measures while the game is played (fights, flight, effects - not a still scene): every thread's CPU time per
// tick, the game's and SecondSonPC's code that takes it, counters of the subsystems around the frame (file reads,
// guest memory maps, write-tracking faults) and the moments the player or a scripted run marked. Per frame through the
// frame log's clock: tools/r9/gprof.py joins it with gpu_frames.csv (and r8_cp_profile.csv when that runs).
//
//   mode 1   a sampler thread wakes every SSPC_R9_GPROF_US (1000; mode 2: 2000) us and reads each thread's cycle counter
//            (QueryThreadCycleTime: the TSC ticks the thread ran - not suspended, not preempted, not waiting); the
//            threads are found again every second (names from SetThreadDescription). No thread is stopped.
//   mode 2   also a sample of each thread that ran at least SSPC_R9_GPROF_MIN_PCT (5) % of the tick: suspended, its
//            instruction pointer, the nearest return address into the eboot on its stack (when it is in SecondSonPC's,
//            the driver's or a system's code), the nearest one into SecondSonPC's code (when it is in the driver's or a
//            system's) and the eboot's frame-pointer chain (the game keeps frame pointers), then resumed. Every 10th
//            tick the watched threads that did not run (Game:Main, the job workers, the GPU pipeline's threads) are
//            sampled the same way: what they wait on. The report weights a sample by the cycles its thread ran in that tick, so a function's share is
//            CPU time, not wall time. Nothing is allocated, logged or locked while a thread is suspended.
//
// Output: logs\r9_gprof.bin (streamed by a writer thread every half second - any session length; tools/r9/gprof.py
// reads it), the totals in the log at the end. The profiler's own cost (its thread's cycles, the time threads were
// suspended) is in the file and the log; docs/R9_GAMEPLAY_PERFORMANCE.md §2 measures it on the stand.
#pragma once

#include <atomic>
#include <filesystem>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Gpu::Gprof {

/// SSPC_R9_GPROF (read once, at static initialization): 0 off, 1 thread times + counters + marks, 2 also code samples.
extern const u8 g_mode;
inline bool On() {
    return g_mode != 0;
}

/// Cumulative counters (any thread, relaxed): the file's COUNTERS records carry their values every tick.
enum class Ctr : u8 {
    FileReadBytes, ///< the game's file reads (sceKernelRead/Pread/_read): bytes
    FileReadNs,    ///< ... their time
    FileReads,     ///< ... calls
    GuestMaps,     ///< guest memory mapped / unmapped (the backend's memory listener)
    GuestUnmaps,
    Marks,         ///< marks so far
    // polled from the backend each tick (SetPoll), not added
    TrackerFaults, ///< the page tracker's write faults (the guest's first write into a tracked 64 KiB block)
    TrackerArms,   ///< pages armed for write tracking
    GameFlips,     ///< FrameLog: flips the game requested
    CpFlips,       ///< ... flip markers the command processor reached
    Presents,      ///< ... flips presented
    Count,
};
inline constexpr u32 kCounters = static_cast<u32>(Ctr::Count);
/// The first counter SetPoll fills.
inline constexpr u32 kFirstPolled = static_cast<u32>(Ctr::TrackerFaults);

namespace detail {
extern std::atomic<u64> g_counters[kCounters];
}
inline void Add(Ctr c, u64 v) {
    detail::g_counters[static_cast<u32>(c)].fetch_add(v, std::memory_order_relaxed);
}

/// Values the profiler does not own, read each tick on its thread: `fn` writes counters kFirstPolled..kCounters-1.
using PollFn = void (*)(void* ctx, u64* polled);
void SetPoll(PollFn fn, void* ctx);

struct StartInfo {
    std::filesystem::path logs_dir;
    u64 guest_lo = 0, guest_hi = 0; ///< the eboot's code (Engine::GuestImageBase/End)
    u64 origin_tsc = 0;             ///< the frame log's time zero (FrameLog::StartTsc)
};
/// Starts the sampler and the writer (when On()). The native backend, once the frame log started.
void Start(const StartInfo& info);
/// Stops both, writes the last records and closes the file; logs the totals. The native backend at its stop.
void Stop();
/// A marked moment: a hotkey (F5 calm, F6 fight, F7 traversal, F9 effects, F12 stutter seen) or a scripted run's
/// `mark:<label>@<s>` (--press). Any thread; dropped when off.
void Mark(std::string_view label);

} // namespace SecondSon::Gpu::Gprof
