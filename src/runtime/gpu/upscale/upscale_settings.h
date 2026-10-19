// SecondSonPC — the upscaling and frame generation settings and status (docs/UPSCALING_FRAME_GENERATION.md): no
// Vulkan here, for the loader, the settings menu and the GPU backend alike (upscalers.h has the upscalers).
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Gpu::Upscale {

enum class Backend : u8 { Off, Dlss, Fsr };
/// The usual per-side scale factors: Native (DLAA / FSR Native AA) 1.0, Quality 1.5, Balanced 1.72, Performance 2.0,
/// Ultra Performance 3.0.
enum class Quality : u8 { Native, Quality, Balanced, Performance, UltraPerformance };
enum class FrameGen : u8 { Off, Fsr, Dlss };

float ScaleOf(Quality q);
const char* BackendName(Backend b);
const char* QualityName(Quality q);

/// What the player chose (the settings menu / SecondSonPC.toml; SSPC_UPSCALER etc. override for automated runs). Read
/// by the GPU's recording thread each frame; written by the settings.
struct Settings {
    Backend backend = Backend::Off;
    Quality quality = Quality::Quality;
    float sharpness = 0.5f; ///< 0..1
    bool jitter = true;     ///< sub-pixel jitter of the scene's geometry (off: diagnostics only)
    FrameGen frame_gen = FrameGen::Off;
    u32 fg_multiplier = 2; ///< frames shown per rendered frame (2..4)
    bool reflex = true;
};
/// The settings menu's / SecondSonPC.toml's values ([Graphics] upscaler, upscaleQuality, upscaleSharpness,
/// frameGeneration, frameGenMultiplier, reflex) as the runtime's settings.
void ApplyConfig(std::string_view upscaler, std::string_view quality, u32 sharpness_percent,
                 std::string_view frame_generation, u32 fg_multiplier, bool reflex);
void SetSettings(const Settings& s);
Settings GetSettings();
/// GetSettings().backend != Backend::Off without the settings' lock (the recording thread asks at every draw).
bool BackendOn();
/// SSPC_UPSCALER=off|dlss|fsr, SSPC_UPSCALE_QUALITY=native|quality|balanced|performance|ultra, SSPC_UPSCALE_SHARPNESS=
/// 0..1, SSPC_UPSCALE_JITTER=0 over the current settings (automated runs; logged).
void ApplyEnvironmentOverrides();
/// DLSS on NVIDIA (Vulkan: VK_NVX_binary_import) starts the driver's CUDA part, which reserves ~48 GiB of address space
/// below 1 TiB - where the guest's memory manager reserves every free byte. Before the guest's address space is taken:
/// everything free below 0xF000000000 is held for a moment, CUDA is initialized (it takes a window under 1 TiB, above
/// what the game maps from 0x200000000), the hold is released. false (logged): not NVIDIA / no nvcuda.dll / cuInit
/// failed - DLSS will be unavailable, nothing else changes.
bool ReserveCudaWindow();
/// Names as SecondSonPC.toml / the menu store them ("off", "dlss", "fsr"; "native", "quality", ...).
bool ParseBackend(std::string_view s, Backend& out);
bool ParseQuality(std::string_view s, Quality& out);
/// The render size the game was started with (its scene targets); the display size is the game's 1920x1080.
void SetRenderSize(u32 width, u32 height);
void GetRenderSize(u32& width, u32& height);

/// What runs, for the settings menu and the logs (any thread).
struct Status {
    bool dlss_available = false, fsr_available = false;
    bool fsr_fg_available = false, dlss_fg_available = false; ///< frame generation (found when the backend started)
    std::string fsr_fg_why, dlss_fg_why;
    std::string dlss_why, fsr_why; ///< why one is not available
    Backend active = Backend::Off; ///< the upscaler of the last frame (Off: none ran)
    std::string reason;            ///< why none ran, if one was chosen
    u32 render_w = 0, render_h = 0, out_w = 0, out_h = 0;
    float gpu_ms = 0.0f; ///< the upscaler's GPU time (last measured)
    u64 frames = 0;      ///< frames upscaled
};
Status GetStatus();
void UpdateStatus(const std::function<void(Status&)>& fn);

} // namespace SecondSon::Gpu::Upscale
