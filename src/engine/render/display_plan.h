// SecondSonPC — the game's display, screen and scene sizes for a window or display of any size and shape
// (docs/RESOLUTION_IMPLEMENTATION.md). Plain arithmetic: no game state, checked by --selftest.
//
// The engine has three sizes (docs/RESOLUTION_RESEARCH.md):
//   display  its VideoOut buffers and the pixels the HUD is drawn in (1920x1080 on PS4, 3840x2160 on PS4 Pro)
//   screen   the HUD's layout space and the source of every camera's aspect ratio (1920x1080 on both consoles);
//            the HUD is laid out in it and scaled to the display
//   scene    the 3D scene's targets (1920x1080 on PS4, 3200x1800 checkerboard on PS4 Pro), scaled to the display
// The camera keeps its vertical field of view and takes the screen's aspect: a wider screen sees more at the sides
// (Hor+), a narrower one less, nothing is stretched.
#pragma once

#include <string>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Engine::Render {

struct Size {
    u32 width = 0;
    u32 height = 0;
    bool operator==(const Size&) const = default;
};

/// [Display] aspectRatio: the shape of the game's picture. Auto: the window's (or the display's in full screen), within
/// 4:3 .. 32:9 - a taller window shows 16:9 with bars above and below, a wider one 32:9 with bars at the sides.
enum class AspectMode : u8 { Auto, R16x9, R16x10, R21x9, R32x9, R4x3 };
std::string_view AspectModeName(AspectMode m);              ///< "auto", "16:9", ...
bool ParseAspectMode(std::string_view s, AspectMode& out);  ///< false: not one of the names

/// [Graphics] renderScale: the 3D scene against the display, per side. Auto (0): the scene at most 1080 lines tall, as
/// the PS4 renders it (a larger display gets the scene scaled up by the game, the HUD stays sharp); otherwise percent.
constexpr u32 kRenderScaleAuto = 0;
constexpr u32 kRenderScaleMin = 50;
constexpr u32 kRenderScaleMax = 100;
/// "auto" -> kRenderScaleAuto, "50".."100" -> the percent; anything else -> kRenderScaleAuto.
u32 ParseRenderScale(std::string_view s);

struct DisplayPlan {
    Size output;   ///< where the presenter shows the frame: the window's client area or the display in full screen
    Size display;  ///< the game's display (VideoOut buffers, HUD pixels)
    Size screen;   ///< the game's screen size (HUD layout, camera aspect): 1080 tall
    Size scene;    ///< the 3D scene's size at start (render scale applied, within kMaxScenePixels)
    double aspect = 16.0 / 9.0; ///< display width / height
    bool boxed = false;          ///< the frame does not fill the output: bars (letterbox / pillarbox)
    bool scene_capped = false;   ///< the render scale asked for more than kMaxScenePixels
    bool display_raised = false; ///< the output is smaller than kMinDisplayHeight: the game draws larger, shown smaller
    u32 extra_direct_mib = 0;    ///< direct memory the game needs on top of the retail budget for these sizes
    bool Native() const { return display == Size{1920, 1080} && screen == Size{1920, 1080} && scene == display; }
};

/// The display height below which the game still draws at this height, as on the PS4 (a 16:9 window up to 1080p gets
/// exactly the console's 1920x1080; the presenter shows the frame smaller).
constexpr u32 kMinDisplayHeight = 1080;
/// The largest 3D scene verified to run (docs/RESOLUTION_TESTS.md): 2880x2160 and 3840x1080 ran, 5120x1440 and 3840x2160
/// lost the GPU device in the world (the pixel count, not the width; a 3840x2160 display with a smaller scene runs). A
/// larger render scale gives this many pixels, in the display's shape.
constexpr u64 kMaxScenePixels = 2880ull * 2160ull;
/// The largest display (VideoOut buffer) accepted.
constexpr Size kMaxDisplay{7680, 4320};

/// The plan for an output of `output` pixels. `fixed`: [Display] resolution when not "auto" (the game's display size;
/// the presenter scales it into the output), {0, 0} for auto. `render_scale`: kRenderScaleAuto or 50..100.
DisplayPlan PlanDisplay(Size output, AspectMode aspect, Size fixed, u32 render_scale);

/// The scene size for `display` at `render_scale` (kRenderScaleAuto or 50..100), rounded to 16 x 8 pixels, within
/// kMaxScenePixels; `capped` (optional) tells whether the cap applied.
Size SceneSizeFor(Size display, u32 render_scale, bool* capped = nullptr);

/// "WxH" -> size; false when it is not two numbers within 320x240 .. kMaxDisplay.
bool ParseSize(std::string_view s, Size& out);

/// One line for the log / the settings window: "display 2560x1080 (21:9), screen 2560x1080, scene 2560x1080".
std::string Describe(const DisplayPlan& p);

} // namespace SecondSon::Engine::Render
