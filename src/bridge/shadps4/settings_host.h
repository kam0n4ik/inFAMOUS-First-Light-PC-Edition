// SecondSonPC — TEMPORARY bridge side of the PC settings (Phase 4): what only the bridge can apply while it owns the
// presenter and the audio output (FSR/RCAS of the final frame, the FPS counter overlay, the master volume), and the
// capabilities of this run. The settings, the menu and its overlay are SecondSonPC's (src/settings, src/ui).
#pragma once

#include "app/config.h"

struct SDL_Window;

namespace SecondSon::Bridge {

/// [Graphics] into the bridge's settings before its presenter is created (it reads them once, at creation).
void ApplyPcBridgeSettings(const App::Config& cfg);

/// After the native input: capabilities, the bridge's appliers, the menu host. `native_gpu`: --gpu-backend sspc
/// (the bridge's presenter is not started: FSR/RCAS and its FPS counter are unavailable).
void InstallSettingsHost(const App::Config& cfg, SDL_Window* window, bool test_mode, bool native_gpu);

/// SecondSonPC's GPU backend: the upscaling settings applied - the upscaler checked (DLSS on the device, FSR's DLL),
/// the game's render size asked for (switched at its next frame), its own TAA off while an upscaler jitters, frame
/// generation switched (`at_start`: the presenter was created with it). At start and on every menu change.
void ApplyUpscaling(bool at_start);

} // namespace SecondSon::Bridge
