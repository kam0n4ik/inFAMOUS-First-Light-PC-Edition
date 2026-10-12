// SecondSonPC — what SecondSonPC's menu draws, independent of who draws it: rectangles, lines, circles, triangles and
// UTF-8 text in window client pixels. The Win32 overlay renders it now (ui/overlay_win32.*); a pass of SecondSonPC's
// own presenter can render the same list later (P4.6 contract, docs/PC_SETTINGS_IMPLEMENTATION.md §9). Own code.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Ui {

struct Color {
    u8 r = 0, g = 0, b = 0, a = 255;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    bool Contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
    float Right() const {
        return x + w;
    }
    float Bottom() const {
        return y + h;
    }
};

enum class Align : u8 { Left, Center, Right };
enum class FontWeight : u8 { Regular, Semibold };

struct DrawCmd {
    enum class Kind : u8 {
        FillRect,       // rect, radius, color
        StrokeRect,     // rect, radius, width, color
        Line,           // p[0] - p[1], width, color
        FillCircle,     // rect = bounding box
        StrokeCircle,   // rect = bounding box, width
        FillTriangle,   // p[0..2]
        StrokeTriangle, // p[0..2], width
        Text,           // rect = box, text, size, align, weight, wrap (else one line, vertically centred, ellipsis)
        PushClip,       // rect
        PopClip,
    };
    Kind kind = Kind::FillRect;
    Rect rect;
    float radius = 0.0f;
    float width = 1.0f;
    Color color;
    float p[6] = {};
    std::string text;
    float size = 16.0f;
    Align align = Align::Left;
    FontWeight weight = FontWeight::Regular;
    bool wrap = false;
};

class DrawList {
public:
    void Reset(float width, float height) {
        cmds_.clear();
        width_ = width;
        height_ = height;
        surface_ = {0, 0, width, height};
        dim_alpha_ = 0;
    }
    float Width() const {
        return width_;
    }
    float Height() const {
        return height_;
    }
    /// The part of the client area the commands cover (the overlay sizes its surface to it).
    void SetSurface(Rect surface) {
        surface_ = surface;
    }
    Rect Surface() const {
        return surface_;
    }
    /// The rest of the client area darkened behind the menu (0..255).
    void SetDim(u8 alpha) {
        dim_alpha_ = alpha;
    }
    u8 Dim() const {
        return dim_alpha_;
    }

    void FillRect(Rect r, Color c, float radius = 0.0f) {
        Add({.kind = DrawCmd::Kind::FillRect, .rect = r, .radius = radius, .color = c});
    }
    void StrokeRect(Rect r, Color c, float width, float radius = 0.0f) {
        Add({.kind = DrawCmd::Kind::StrokeRect, .rect = r, .radius = radius, .width = width, .color = c});
    }
    void Line(float x0, float y0, float x1, float y1, Color c, float width) {
        Add({.kind = DrawCmd::Kind::Line, .width = width, .color = c, .p = {x0, y0, x1, y1}});
    }
    void FillCircle(float cx, float cy, float r, Color c) {
        Add({.kind = DrawCmd::Kind::FillCircle, .rect = {cx - r, cy - r, 2 * r, 2 * r}, .color = c});
    }
    void StrokeCircle(float cx, float cy, float r, Color c, float width) {
        Add({.kind = DrawCmd::Kind::StrokeCircle, .rect = {cx - r, cy - r, 2 * r, 2 * r}, .width = width, .color = c});
    }
    void FillTriangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c) {
        Add({.kind = DrawCmd::Kind::FillTriangle, .color = c, .p = {x0, y0, x1, y1, x2, y2}});
    }
    void StrokeTriangle(float x0, float y0, float x1, float y1, float x2, float y2, Color c, float width) {
        Add({.kind = DrawCmd::Kind::StrokeTriangle, .width = width, .color = c, .p = {x0, y0, x1, y1, x2, y2}});
    }
    void Text(Rect box, std::string_view text, float size, Color c, Align align = Align::Left,
              FontWeight weight = FontWeight::Regular, bool wrap = false) {
        Add({.kind = DrawCmd::Kind::Text, .rect = box, .color = c, .text = std::string(text), .size = size,
             .align = align, .weight = weight, .wrap = wrap});
    }
    void PushClip(Rect r) {
        Add({.kind = DrawCmd::Kind::PushClip, .rect = r});
    }
    void PopClip() {
        Add({.kind = DrawCmd::Kind::PopClip});
    }

    std::span<const DrawCmd> Commands() const {
        return cmds_;
    }

private:
    void Add(DrawCmd cmd) {
        cmds_.push_back(std::move(cmd));
    }

    std::vector<DrawCmd> cmds_;
    float width_ = 0, height_ = 0;
    Rect surface_;
    u8 dim_alpha_ = 0;
};

/// Text width for layout (the renderer's font metrics; the self-tests use an estimate).
class TextMeasure {
public:
    virtual ~TextMeasure() = default;
    virtual float Width(std::string_view utf8, float size, FontWeight weight) = 0;
};

/// An estimate without a font (tests, a renderer that is not up yet): 0.55 em per character.
class EstimateMeasure final : public TextMeasure {
public:
    float Width(std::string_view utf8, float size, FontWeight weight) override {
        size_t chars = 0;
        for (const char c : utf8) {
            chars += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
        }
        return static_cast<float>(chars) * size * (weight == FontWeight::Semibold ? 0.58f : 0.55f);
    }
};

} // namespace SecondSon::Ui
