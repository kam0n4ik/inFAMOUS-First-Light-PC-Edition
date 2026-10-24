// SecondSonPC — the game's display, screen and scene sizes (see display_plan.h).
#include "engine/render/display_plan.h"

#include <algorithm>
#include <charconv>
#include <cmath>

#include <fmt/format.h>

namespace SecondSon::Engine::Render {

namespace {

constexpr double kMinAspect = 4.0 / 3.0;
constexpr double kMaxAspect = 32.0 / 9.0;

double AspectOf(AspectMode m) {
    switch (m) {
    case AspectMode::R16x10: return 16.0 / 10.0;
    case AspectMode::R21x9: return 64.0 / 27.0; // 2560x1080, what "21:9" displays are
    case AspectMode::R32x9: return 32.0 / 9.0;
    case AspectMode::R4x3: return 4.0 / 3.0;
    case AspectMode::R16x9:
    case AspectMode::Auto: break;
    }
    return 16.0 / 9.0;
}

u32 Even(double v) {
    return std::max<u32>(2, static_cast<u32>(std::lround(v / 2.0)) * 2);
}

/// The largest `aspect` rectangle inside `box`.
Size Fit(Size box, double aspect) {
    const double box_aspect = static_cast<double>(box.width) / box.height;
    if (box_aspect > aspect) {
        return {Even(box.height * aspect), box.height & ~1u};
    }
    return {box.width & ~1u, Even(box.width / aspect)};
}

std::string_view RatioLabel(double a) {
    struct Named {
        double aspect;
        std::string_view name;
    };
    constexpr Named kNames[] = {{4.0 / 3.0, "4:3"},   {16.0 / 10.0, "16:10"}, {16.0 / 9.0, "16:9"},
                                {64.0 / 27.0, "21:9"}, {43.0 / 18.0, "21:9"},  {32.0 / 9.0, "32:9"}};
    for (const Named& n : kNames) {
        if (std::abs(a - n.aspect) < 0.01) {
            return n.name;
        }
    }
    return {};
}

} // namespace

std::string_view AspectModeName(AspectMode m) {
    switch (m) {
    case AspectMode::Auto: return "auto";
    case AspectMode::R16x9: return "16:9";
    case AspectMode::R16x10: return "16:10";
    case AspectMode::R21x9: return "21:9";
    case AspectMode::R32x9: return "32:9";
    case AspectMode::R4x3: return "4:3";
    }
    return "auto";
}

bool ParseAspectMode(std::string_view s, AspectMode& out) {
    for (const AspectMode m : {AspectMode::Auto, AspectMode::R16x9, AspectMode::R16x10, AspectMode::R21x9,
                               AspectMode::R32x9, AspectMode::R4x3}) {
        if (s == AspectModeName(m)) {
            out = m;
            return true;
        }
    }
    return false;
}

u32 ParseRenderScale(std::string_view s) {
    u32 v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc{} || r.ptr != s.data() + s.size() || v < kRenderScaleMin || v > kRenderScaleMax) {
        return kRenderScaleAuto;
    }
    return v;
}

bool ParseSize(std::string_view s, Size& out) {
    const size_t x = s.find('x');
    if (x == std::string_view::npos) {
        return false;
    }
    u32 w = 0, h = 0;
    const auto a = std::from_chars(s.data(), s.data() + x, w);
    const auto b = std::from_chars(s.data() + x + 1, s.data() + s.size(), h);
    if (a.ec != std::errc{} || a.ptr != s.data() + x || b.ec != std::errc{} || b.ptr != s.data() + s.size() ||
        w < 320 || h < 240 || w > kMaxDisplay.width || h > kMaxDisplay.height) {
        return false;
    }
    out = {w, h};
    return true;
}

Size SceneSizeFor(Size display, u32 render_scale, bool* capped) {
    double s = 1.0;
    if (render_scale == kRenderScaleAuto) {
        s = std::min(1.0, 1080.0 / display.height);
    } else {
        s = std::clamp<u32>(render_scale, kRenderScaleMin, kRenderScaleMax) / 100.0;
    }
    double w = display.width * s, h = display.height * s;
    const bool over = w * h > static_cast<double>(kMaxScenePixels) * 1.0001;
    if (over) {
        const double k = std::sqrt(static_cast<double>(kMaxScenePixels) / (w * h));
        w *= k;
        h *= k;
    }
    if (capped != nullptr) {
        *capped = over;
    }
    // (16 x 8 as the upscalers' sizes; a full-size side keeps the display's own size)
    auto round_down = [](double v, u32 step) { return std::max<u32>(step, static_cast<u32>(v / step) * step); };
    Size scene{round_down(w + 0.5, 16), round_down(h + 0.5, 8)};
    if (std::abs(w - display.width) < 1.0) {
        scene.width = display.width;
    }
    if (std::abs(h - display.height) < 1.0) {
        scene.height = display.height;
    }
    scene.width = std::clamp<u32>(scene.width, std::min<u32>(640, display.width), display.width);
    scene.height = std::clamp<u32>(scene.height, std::min<u32>(360, display.height), display.height);
    return scene;
}

DisplayPlan PlanDisplay(Size output, AspectMode aspect, Size fixed, u32 render_scale) {
    DisplayPlan p;
    p.output = {std::max<u32>(output.width, 320), std::max<u32>(output.height, 240)};
    const Size box = fixed.width != 0 ? fixed : p.output;
    double a = AspectOf(aspect);
    if (aspect == AspectMode::Auto) {
        a = static_cast<double>(box.width) / box.height;
        if (a < kMinAspect - 1e-6) {
            a = 16.0 / 9.0; // portrait or nearly square: the console's picture, bars above and below
        } else if (a > kMaxAspect + 1e-6) {
            a = kMaxAspect;
        }
        if (std::abs(a - 16.0 / 9.0) < 0.004) {
            a = 16.0 / 9.0; // 1366x768 and the like: the console's own shape, not a few columns more
        }
    }
    Size display = Fit(box, a);
    if (display.height < kMinDisplayHeight) {
        p.display_raised = true;
        display = {Even(kMinDisplayHeight * a), kMinDisplayHeight};
    }
    if (display.width > kMaxDisplay.width || display.height > kMaxDisplay.height) {
        display = Fit(kMaxDisplay, a);
    }
    p.display = display;
    p.aspect = static_cast<double>(display.width) / display.height;
    p.screen = {Even(1080.0 * p.aspect), 1080};
    if (std::abs(p.aspect - 16.0 / 9.0) < 1e-3) {
        p.screen = {1920, 1080};
    }
    p.scene = SceneSizeFor(display, render_scale, &p.scene_capped);
    const double out_aspect = static_cast<double>(p.output.width) / p.output.height;
    p.boxed = std::abs(out_aspect - p.aspect) > 0.01;
    // direct memory: the render targets grow with the display and the scene; the retail budget leaves room for about
    // 2.4x the console's pixels (3440x1440 ran without more), 4K needed +1 GiB (docs/RESOLUTION_RESEARCH.md)
    const double px = std::max<double>(static_cast<double>(display.width) * display.height,
                                       static_cast<double>(p.scene.width) * p.scene.height);
    const double growth = px / (1920.0 * 1080.0) - 1.0;
    if (growth > 0.05) {
        const u32 mib = static_cast<u32>(std::ceil(growth * 384.0 / 256.0)) * 256;
        p.extra_direct_mib = std::min<u32>(mib, 4096);
    }
    return p;
}

std::string Describe(const DisplayPlan& p) {
    const std::string_view label = RatioLabel(p.aspect);
    return fmt::format("output {}x{}, display {}x{} ({}{}{:.3f}){}, screen {}x{}, scene {}x{}{}{}", p.output.width,
                       p.output.height, p.display.width, p.display.height, label, label.empty() ? "" : " = ",
                       p.aspect, p.boxed ? " with bars" : "", p.screen.width, p.screen.height, p.scene.width,
                       p.scene.height, p.scene_capped ? " (capped)" : "",
                       p.extra_direct_mib != 0 ? fmt::format(", +{} MiB direct memory", p.extra_direct_mib) : "");
}

} // namespace SecondSon::Engine::Render
