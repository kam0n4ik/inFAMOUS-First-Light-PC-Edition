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
//
// Any display size and shape (docs/RESOLUTION_IMPLEMENTATION.md): the loader also writes the display size (0x1eff55,
// the same instructions' neighbours) and the screen size (0x1f03f8: the HUD's layout space and every camera's aspect)
// from the plan main.cpp made (display_plan.h); the game's PS4 Pro path then does the rest (its HUD scaled from the
// screen to the display, the scene scaled to the display).
#pragma once

#include <string_view>

#include "base/abi.h"
#include "engine/render/display_plan.h"

namespace SecondSon::Engine::Render {

struct RenderSize {
    u32 width = 1920;
    u32 height = 1080;
};

/// The display size of the PS4 build (VideoOut buffers, UI).
constexpr RenderSize kDisplaySize{1920, 1080};

/// The plan the game starts with (main.cpp, before the bridge starts; the PS4's 1920x1080 unless set) and the render
/// scale ([Graphics] renderScale: kRenderScaleAuto or 50..100; it changes while playing).
void SetDisplayPlan(const DisplayPlan& plan);
const DisplayPlan& GetDisplayPlan();
void SetRenderScale(u32 render_scale);
/// The scene size the render scale asks for now (without an upscaler), within the size the game started with.
RenderSize SceneSizeForRenderScale();

/// A render size for `scale` (display / render per side, >= 1): rounded to 16 pixels in width and 8 in height, at
/// least 640x360 (the engine's quarter-size targets have a floor of 480x270).
RenderSize RenderSizeForScale(float scale);

/// The loader's window before the guest runs: writes the plan's display, screen and (largest) scene size into the
/// game's code (SSPC_RENDER_RES=WxH: the scene's start size, SSPC_DISPLAY_RES / SSPC_SCREEN_RES: diagnostics). The
/// PS4's sizes leave the code untouched. Returns false and logs why when the code is not the analysed one.
bool InstallRenderResolution(u64 eboot_base);

/// The scene's render size now (the size the game started with, then the last switch).
RenderSize CurrentRenderSize();
/// The game's display size (its VideoOut buffers and UI): kDisplaySize unless the loader changed it.
RenderSize DisplaySize();

/// The HUD's distance from the picture's edges ([Display] hudSafeArea 85..100 %, as the PS4's Display Area the game
/// reads with sceSystemServiceGetDisplaySafeAreaInfo) and the part of a wide picture it uses ([Display] hudArea "full" |
/// "21:9" | "16:9"). The game's margins (0x1d0ccc8 / 0x1d0cccc, screen units) are written at every frame start (needs
/// InstallRenderSwitch); 100 % and "full" are the game's own 16 / 16.
void SetHudLayout(u32 safe_area_percent, std::string_view area);
/// The safe area ratio sceSystemServiceGetDisplaySafeAreaInfo reports (0.85..1).
float HudSafeAreaRatio();

/// [Graphics] fov 80..130 %: the player camera's vertical field of view. An entry probe on Camera::SetFov 0x580580
/// scales the angle when the call comes from the player camera's update (0x14ddd0, return address 0x14f517); the
/// cut-scene and the other cameras keep theirs. Install in the loader's window.
bool InstallFovControl(u64 eboot_base);
void SetFovPercent(u32 percent);

/// The render size changed while the game runs (the PS4 Pro's mode switch path): an entry probe on FrameBegin
/// 0x1f0be0 writes a requested size into the device and its size-class table and re-describes the display and scene
/// targets (0x300b00, 0x18e210) before the frame starts. Their memory is the one allocated at start: sizes up to the
/// started one. Install in the loader's window (before the guest runs), after InstallRenderResolution.
bool InstallRenderSwitch(u64 eboot_base);
/// Asks for `size` (clamped to the started size) at the game's next frame. false: the switch is not installed.
bool RequestRenderSize(RenderSize size);

/// The game's temporal anti-aliasing (SMAA T2x) off while `off` is set: its camera jitter and two-frame blend would
/// take the place of the upscaler's own jitter and smear its input. CameraJitter 0x472810 runs every frame: while the
/// T2x flag (0xda07f9) is set it moves the main camera's (0x7f2d2b8) frustum window by +-0.5/width, -+0.5/height
/// in turn (Camera::SetFrustumWindow 0x581a50); pass 32's setup (0x1929c0) blends the frame with the previous one when
/// the flag is set. An entry probe runs it, then clears the flag and gives the camera its centred window again
/// (Camera::ResetFrustumWindow 0x581a10) - the scene unjittered, the resolve a single frame (SMAA 1x).
bool InstallTemporalAaControl(u64 eboot_base);
void SetTemporalAaOff(bool off);

} // namespace SecondSon::Engine::Render
