// SecondSonPC — the game's internal render resolution (Upscaling, docs/UPSCALING_FRAME_GENERATION.md).
//
// The engine keeps two sizes in its graphics device object (VideoOut init 0x1eff00, called from 0x1ef980):
//   +0x0 / +0x4  display size: the VideoOut buffers and the UI (1920x1080 on PS4, 3840x2160 on PS4 Pro)
//   +0xC / +0x10 render size: the 3D scene's targets (1920x1080 on PS4, 3200x1800 checkerboard on PS4 Pro)
// and derives every scene-sized target (half, quarter, the light grid in 20-pixel tiles) from the render size. The
// PS4 build writes both as immediates (`mov dword [rbx+0xC], 0x780` / `mov dword [rbx+0x10], 0x438` at 0x1eff47);
// a lower render size is written into those two instructions before the game runs, so the scene is really rendered
// at fewer pixels and the game's own scene-to-display scaling (the PS4 Pro path) brings it to the display size,
// where the HUD is drawn. A temporal upscaler replaces that scaling (native backend).
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::Render {

struct RenderSize {
    u32 width = 1920;
    u32 height = 1080;
};

/// The display size of the PS4 build (VideoOut buffers, UI).
constexpr RenderSize kDisplaySize{1920, 1080};

/// A render size for `scale` (display / render per side, >= 1): rounded to 16 pixels in width and 8 in height, at
/// least 640x360 (the engine's quarter-size targets have a floor of 480x270).
RenderSize RenderSizeForScale(float scale);

/// The loader's window before the guest runs: writes `size` into the game's render size (SSPC_RENDER_RES=WxH wins).
/// 1920x1080 leaves the code untouched. Returns false and logs why when the code is not the analysed one.
bool InstallRenderResolution(u64 eboot_base, RenderSize size);

/// The render size the game was started with (1920x1080 when not patched).
RenderSize CurrentRenderSize();

/// The game's temporal anti-aliasing (SMAA T2x) off while `off` is set: its camera jitter and two-frame blend would
/// take the place of the upscaler's own jitter and smear its input. CameraJitter 0x472810 runs every frame: while the
/// T2x flag (0xda07f9) is set it moves the main camera's (0x7f2d2b8) frustum window by +-0.5/width, -+0.5/height
/// in turn (Camera::SetFrustumWindow 0x581a50); pass 32's setup (0x1929c0) blends the frame with the previous one when
/// the flag is set. An entry probe runs it, then clears the flag and gives the camera its centred window again
/// (Camera::ResetFrustumWindow 0x581a10) - the scene unjittered, the resolve a single frame (SMAA 1x).
bool InstallTemporalAaControl(u64 eboot_base);
void SetTemporalAaOff(bool off);

} // namespace SecondSon::Engine::Render
