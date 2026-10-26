// SecondSonPC — the launcher's drawing surface: Direct2D on the launcher's window (hardware, WARP when there is no
// GPU device), DirectWrite text laid out as CSS lays it out (line height, letter spacing, the weights Chrome picks
// for Segoe UI), the mockup's SVG icon paths, gradients and box shadows. Coordinates are CSS pixels (DIPs); the
// window's DPI scales them. Main thread only. Own code.
#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"

struct HWND__;

namespace SecondSon::Launcher {

struct Color {
    float r = 0, g = 0, b = 0, a = 1;
};
/// 0xRRGGBB with an alpha 0..1.
constexpr Color Rgb(u32 rgb, float a = 1.0f) {
    return {static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f,
            static_cast<float>(rgb & 0xFF) / 255.0f, a};
}
inline Color WithAlpha(Color c, float a) {
    c.a *= a;
    return c;
}
Color Mix(Color a, Color b, float t);

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    float R() const {
        return x + w;
    }
    float B() const {
        return y + h;
    }
    bool Contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    Rect Inset(float d) const {
        return {x + d, y + d, w - 2 * d, h - 2 * d};
    }
    Rect Inset(float dx, float dy) const {
        return {x + dx, y + dy, w - 2 * dx, h - 2 * dy};
    }
};

/// A CSS font: size (px), weight (100..900; matched to Segoe UI's faces as Chrome matches them), monospace (Cascadia
/// Mono, Consolas), letter spacing in em, tabular figures.
/// `display`: the mockup's --font-display ("Segoe UI Variable Display" where Windows has it, as --font's "Segoe UI
/// Variable Text"; Segoe UI otherwise - Windows 10).
struct Font {
    float size = 14.0f;
    int weight = 400;
    bool mono = false;
    float tracking = 0.0f;
    bool tabular = false;
    bool italic = false;
    bool display = false;
};

enum class Align : u8 { Left, Center, Right };

/// A run of a text with another weight / colour (the mockup's <b>, <i>): [begin, end) in UTF-8 bytes.
struct Span {
    u32 begin = 0, end = 0;
    int weight = 0;     // 0: as the text
    bool color = false; // true: `rgb` instead of the text's colour
    Color rgb;
    bool italic = false;
};

struct TextBox {
    float w = 0, h = 0;
    u32 lines = 0;
    float last_line_w = 0;
};

/// A decoded image (WIC), drawn by the canvas that made it.
struct Image;

struct Stop {
    float at = 0; // 0..1
    Color color;
};

class Canvas {
public:
    Canvas();
    ~Canvas();
    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    /// On a window; or offscreen (pixels; a bitmap SavePng writes - the launcher's screenshots).
    bool Init(HWND__* hwnd, std::string& error);
    bool InitOffscreen(u32 width_px, u32 height_px, float scale, std::string& error);
    void Resize(u32 width_px, u32 height_px);
    void SetScale(float scale); // DPI / 96
    float Scale() const;

    bool Begin(Color clear);
    /// false: the device was lost - everything is recreated, draw again.
    bool End();
    bool SavePng(const std::wstring& path, std::string& error);

    void FillRect(Rect r, Color c, float radius = 0.0f);
    /// Each corner its own radius (top-left, top-right, bottom-right, bottom-left).
    void FillRect4(Rect r, Color c, float tl, float tr, float br, float bl);
    /// A CSS border: `width` inside `r`.
    void Border(Rect r, Color c, float width, float radius = 0.0f);
    void Line(float x0, float y0, float x1, float y1, Color c, float width);
    void FillCircle(float cx, float cy, float radius, Color c);
    void StrokeCircle(float cx, float cy, float radius, Color c, float width);
    /// CSS linear-gradient(`angle_deg`, stops) over `r` (0deg: to top, 90deg: to right).
    void LinearGradient(Rect r, float angle_deg, std::span<const Stop> stops, float radius = 0.0f);
    /// CSS radial-gradient(rx% ry% at cx% cy%, stops) over `r` (fractions of the box).
    void RadialGradient(Rect r, float rx, float ry, float cx, float cy, std::span<const Stop> stops);
    /// Thin parallel lines (the mockup's static rain): `angle_deg` as CSS, a line `line` px wide every `period` px,
    /// faded out towards the bottom by `fade` (CSS mask 100 % -> 50 % at 60 % -> 0).
    void Hatch(Rect r, float angle_deg, float period, float offset, float line, Color c, bool fade);
    /// CSS box-shadow (outset): offset, blur, spread, colour, of a box `r` with `radius`.
    void Shadow(Rect r, float radius, float dx, float dy, float blur, float spread, Color c);

    /// An SVG path in a `view` x `view` viewBox (the mockup's icons) drawn into `box`: stroked (`stroke` > 0, in
    /// viewBox units; round caps and joins unless `butt`) and / or filled.
    void Icon(std::string_view path_data, Rect box, Color c, float stroke = 1.8f, bool fill = false,
              float view = 24.0f, bool butt = false);

    std::shared_ptr<Image> LoadPicture(const std::wstring& path, std::string& error);
    /// `background-size: cover` at `background-position: px% py%`.
    void ImageCover(const Image& image, Rect r, float px, float py, float opacity = 1.0f);

    /// Text in `box`: wrapped to its width when `wrap`, else one line (ellipsis when it does not fit). `line_h`: the
    /// CSS line height in px; the first line's box starts at box.y.
    void Text(std::string_view utf8, Rect box, const Font& font, Color c, float line_h, Align align = Align::Left,
              bool wrap = true, std::span<const Span> spans = {});
    TextBox Measure(std::string_view utf8, const Font& font, float line_h, float max_w, bool wrap = true,
                    std::span<const Span> spans = {});
    /// CSS `line-height: normal` of the font (ascent + descent + line gap).
    float NormalLineHeight(const Font& font) const;

    void PushClip(Rect r);
    void PopClip();
    void PushOpacity(float alpha);
    void PopOpacity();
    void PushTranslate(float dx, float dy);
    void PopTranslate();

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/// The mockup's 24x24 icons by name ("home", "display" ... "save"; "" if unknown).
std::string_view IconPath(std::string_view name);

} // namespace SecondSon::Launcher
