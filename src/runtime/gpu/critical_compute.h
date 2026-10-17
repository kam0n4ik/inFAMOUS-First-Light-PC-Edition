// SecondSonPC — Compute Critical Path (FrameBreak CC-2): the game's threads blocked in the engine's own wait for its
// async compute queue (0x1695e0, Engine::Render::Trace's replacement of it counts them here). The native backend's
// command processor serves the compute queues first while one waits (SSPC_CC_CRITICAL=1): the work the game is blocked
// on goes to the GPU before more of the graphics frame - only then, and the graphics queue keeps a slice in every round.
#pragma once

#include <atomic>

#include "base/abi.h"

namespace SecondSon::Gpu::CriticalCompute {

/// Game threads inside the wait now, past their first check of the label (the work they wait for is not done).
inline std::atomic<u32> g_waiting{0};
/// Waits that blocked, since the start.
inline std::atomic<u64> g_blocked_waits{0};

inline void Begin() {
    g_waiting.fetch_add(1, std::memory_order_relaxed);
    g_blocked_waits.fetch_add(1, std::memory_order_relaxed);
}
inline void End() {
    g_waiting.fetch_sub(1, std::memory_order_relaxed);
}
inline bool Waiting() {
    return g_waiting.load(std::memory_order_relaxed) != 0;
}

} // namespace SecondSon::Gpu::CriticalCompute
