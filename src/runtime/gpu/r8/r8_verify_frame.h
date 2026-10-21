// SecondSonPC R8 - the deterministic mode for frame comparison (docs/NATIVE_ENGINE_R8_VERIFICATION.md §5). A
// verification mode only: off by default, never for play.
//
// Why: the stand's input (--press) is timed by the wall clock and the game's clock 0x12a160 measures every frame's dt
// by the TSC (docs/FRAMEBREAK.md FB-3), so at a different frame rate flip N shows another moment of the game (R7 CORE
// §8). Here the game's time and the scripted input both follow the game's frames:
//
// SSPC_R8_VERIFY_DET=1
//   - every frame's dt is the clock's own clamp (1/30 s; 1/15 under the game's flag): an entry probe on 0x12a160 (the
//     object's +0x1b0 "last TSC" set to 0 before the clock runs, so the measured dt is over the clamp; the function
//     then stores the TSC as always). The game advances by its frames, not by wall time.
//   - <logs>\r8_det_frames.csv: per game frame its index, the next flip at the clock's call, wall ms, dt, the four
//     clocks (whole ticks of 1/1440 s) and the world frame (below). At most kMaxRows rows (bounded on long sessions).
// SSPC_R8_VERIFY_DET_ANCHOR=<seconds> (wall, from the script's start): the scripted events before it keep their wall
//   time (the menus, the load); the events at or after it are due at world frame round((t - anchor) * 30), counted
//   from the world start: the first frame after the anchor at which clock SSPC_R8_VERIFY_DET_CLOCK (0..3, default 0)
//   advanced. Each such frame waits (at its clock call, bounded) until the script has pushed every event due by then
//   and the window's queue took them: the frame an event reaches is the same in every run.
//   Without an anchor every event keeps its wall time (the dt is still fixed).
#pragma once

#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Gpu::R8::Verify::Frame {

inline constexpr u64 kMaxRows = 400000;

/// SSPC_R8_VERIFY_DET=1 (read once).
bool Enabled();
/// Installs the clock probe (after the eboot is loaded, before guest code runs). false: not enabled or not installed
/// (the reason in the log).
bool InstallFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir);

/// The anchor in seconds of the script (< 0: none, or the mode is off - every event by wall time).
double Anchor();
/// The scripted input (bootstrap's --press thread) starts; `events_after_anchor`: how many of its events are at or
/// after the anchor.
void ScriptStart(u32 events_after_anchor);
/// An event at `seconds` of the script and at or after the anchor: blocks until its world frame has begun.
void WaitWorldFrame(double seconds);
/// An event after the anchor is pushed and the window's queue took it; `next_seconds`: the script's next event (< 0:
/// none). The frames waiting for it go on unless the next event is due in the same world frame.
void Delivered(double next_seconds);

/// Game frames seen (clock calls), and the current world frame (-1 before the world start).
u64 GameFrames();
s64 WorldFrame();

} // namespace SecondSon::Gpu::R8::Verify::Frame
