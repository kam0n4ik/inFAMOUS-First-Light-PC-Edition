// SecondSonPC — FrameBreak FB-3 (docs/FRAMEBREAK.md): a read-only probe of the game's clock, to check that the game's
// time runs at the same rate whatever the frame rate.
//
// The game's clock (eboot 0x12a160, called once a frame by the main loop 0x29c8d0 with the object at eboot+0xe16290):
// +0x00 float dt = min(TSC ticks since the last frame / sceKernelGetTscFrequency, 1/30 s) (0 if negative); four
// clocks, each with a scale (+0x30/+0x20/+0x10/+0x40, products of 28 time factors), accumulate dt * scale as whole
// ticks of 1/1440 s (+0x04, +0x14, +0x24, +0x34, s32) plus a remainder in seconds (+0x0c, +0x1c, +0x2c, +0x3c, float).
// SSPC_FB_CLOCK_PROBE=1: a thread reads these every 100 ms (no write, no hook) into <logs>\game_clock.csv
// (t_s,dt_ms,c0,c1,c2,c3: wall seconds since the probe started, the last frame's dt, each clock's game seconds);
// tools/framebreak/clock_rate.py turns a window of it into game seconds per wall second.
#pragma once

#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Engine::ClockProbe {

/// Starts the probe if SSPC_FB_CLOCK_PROBE=1 (after the eboot is loaded). It runs until the process exits.
void StartFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir);

} // namespace SecondSon::Engine::ClockProbe
