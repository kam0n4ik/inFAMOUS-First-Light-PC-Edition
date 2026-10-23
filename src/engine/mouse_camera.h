// SecondSonPC — the mouse turns the game's camera directly (PC-style aiming: an angle per mouse count, no stick
// emulation, dead zone, acceleration or speed limit).
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::MouseCamera {

/// Call before any guest code runs (the loader's window after the eboot is loaded). The follow camera's integrator
/// (0x14d6f0) gets an entry probe: while it runs, the mouse turns its angles by 0.001 rad per count (times the mouse
/// sensitivity, the game's own inversion options and invert_y; the pitch within the camera's limits) and the right
/// stick rests; other cameras (no integrator call for 250 ms) get the mouse as the right stick again.
/// SSPC_MOUSE_CAMERA=0: always the stick; =observe: the stick, the integrator's turns logged (diagnostics).
/// SSPC_MOUSE_PROBE=1 / SSPC_MOUSE_WATCH=1 (diagnostics): the pad's stick getters wrapped / hardware watchpoints on the
/// pad object (SSPC_MOUSE_WATCH_AT: the 4 offsets), who reads what logged every 5 s.
void Install(u64 eboot_base);

} // namespace SecondSon::Engine::MouseCamera
