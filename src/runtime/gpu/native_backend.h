// SecondSonPC — SecondSonPC's own GPU backend in the game process (Phase 4, P4.4; --gpu-backend sspc;
// docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §B). The one owner of the guest GPU in this mode: it claims every
// Resource (backend.h), registers the one memory listener, installs the GnmDriver host and starts VideoOut's present
// thread. Its GPU thread owns the command processor (P4.2, Mode::Isolated), the shader frontend (P4.3) and the Vulkan
// executor; submissions from the game's threads are queued to it. Chosen once at start-up, never switched while the
// game runs; the bridge's renderer, presenter and VideoOut are not started in this mode (their libraries have no BRIDGE
// imports left).
#pragma once

#include <filesystem>
#include <string>

#include "base/abi.h"

namespace SecondSon::Gpu::Native {

struct Options {
    void* sdl_window = nullptr;          ///< the game window (SDL_Window*)
    s32 device_index = -1;
    bool validation = false;             ///< --gpu-native-validate
    bool diagnostics = false;            ///< --gpu-native-diag (command journal, checkpoints, markers, names)
    s32 sdk_version = 0;                 ///< the game's compiled SDK version (GnmDriver's init sequence)
    u64 capture_flip = 0;                ///< --gpu-capture-native-frame N
    std::filesystem::path logs_dir;      ///< reports, captures
};

/// Brings the backend up (device, caches, threads, ownership). false with `error`: nothing claimed, nothing running.
bool Start(const Options& options, std::string& error);
/// Stops the threads, waits for the device, releases the resources (the process's exit step). Idempotent.
void Stop();
bool Running();
/// A summary of what the backend did (also written to <logs>\gpu_native.txt at Stop).
std::string Report();

} // namespace SecondSon::Gpu::Native
