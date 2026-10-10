// SecondSonPC — the flip timeline (Phase 4, P4.2; SSPC-015, docs/PHASE4_P42_COMMAND_PROCESSOR.md §8). Off unless
// --frame-pacing; then each hook (patch 0029: GnmDriver, Liverpool, VideoOut) is one TSC sample stored in the event's
// own array (no lock, no log). At the exit: logs\frame_pacing.csv (one row per frame) and frame_pacing.txt (what the
// frames waited for: the game's submit blocked by the previous frame, the GPU thread's work and its CPU time — below
// its wall time means the thread was preempted —, the flip queue, the vblank ticks, the present intervals' 16.7/33.3 ms
// bins).
#pragma once

#include <atomic>
#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Gpu::FramePacing {

extern std::atomic<bool> g_enabled;

inline bool Enabled() {
    return g_enabled.load(std::memory_order_acquire); // the state exists once this reads true
}

enum class Event : u8 {
    SubmitEnter,     ///< sceGnmSubmitAndFlip* called (the game's thread)
    SubmitUnblocked, ///< ... past the wait for the previous frame to drain
    Submitted,       ///< ... handed to the GPU thread
    SubmitDone,      ///< sceGnmSubmitDone
    CpFlip,          ///< the GPU thread reached the frame's flip marker (also its CPU time and core)
    CpLabelWait,     ///< the GPU thread starts waiting for a VideoOut label (a flip to complete)
    CpLabelWaitEnd,
    FlipQueued,      ///< the frame prepared and queued for presentation (the GPU thread)
    Vblank,          ///< a PresentThread tick
    FlipTaken,       ///< the tick took a frame
    Presented,       ///< presented and its labels released
    Count,
};

void Enable(const std::filesystem::path& dir);

/// Any thread: the event now.
void Mark(Event event);

} // namespace SecondSon::Gpu::FramePacing
