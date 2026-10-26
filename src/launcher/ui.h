// SecondSonPC — the launcher's widgets: the HTML mockup's (styles.css) buttons, selects,
// switches, sliders, segmented controls, chips and keycaps as immediate-mode calls on a Canvas, with their sizes,
// colours and transitions as the CSS has them; hover, press, keyboard focus (Tab, Enter / Space, arrows), layers
// (a dialog or an open list takes the input), the cursor. Main thread only. Own code.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "launcher/canvas.h"

namespace SecondSon::Launcher {

/// The mockup's :root tokens.
namespace Theme {
constexpr Color kDesk = Rgb(0x08090b);
constexpr Color kBg = Rgb(0x0f1013);
constexpr Color kSurface = Rgb(0x15161a);
constexpr Color kSurface2 = Rgb(0x1b1c21);
constexpr Color kSurface3 = Rgb(0x23252b);
constexpr Color kBorder = Rgb(0x2a2c33);
constexpr Color kBorderStrong = Rgb(0x3a3d46);
constexpr Color kText = Rgb(0xf2f2f4);
constexpr Color kMuted = Rgb(0xa4a6ae);
constexpr Color kDim = Rgb(0x74767f);
constexpr Color kAccent = Rgb(0xe5336c);
constexpr Color kAccentHover = Rgb(0xf5487f);
constexpr Color kAccentPress = Rgb(0xc92659);
constexpr Color kAccentWeak = Rgb(0xe5336c, 0.14f);
constexpr Color kAccentLine = Rgb(0xe5336c, 0.45f);
constexpr Color kAmber = Rgb(0xf0a73a);
constexpr Color kAmberWeak = Rgb(0xf0a73a, 0.13f);
constexpr Color kOk = Rgb(0x45c98b);
constexpr Color kDanger = Rgb(0xff5b5b);
constexpr Color kWhite = Rgb(0xffffff);
constexpr float kRadiusS = 6, kRadius = 8, kRadiusL = 12;
constexpr float kLine = 1.45f; // body line-height
} // namespace Theme

/// A font of the mockup's: Segoe UI (Variable) `size` px, `weight`.
inline Font F(float size, int weight = 400, float tracking = 0.0f) {
    return Font{.size = size, .weight = weight, .tracking = tracking};
}
inline Font Fd(float size, int weight = 400, float tracking = 0.0f) { // --font-display
    return Font{.size = size, .weight = weight, .tracking = tracking, .display = true};
}

enum class Cursor : u8 { Arrow, Hand, NotAllowed };

struct KeyEvent {
    u32 vk = 0;
    u32 scan = 0; // the keyboard's scan code (with 0xE000 for an extended key)
    bool shift = false, ctrl = false, alt = false;
};

/// What the window gave this frame.
struct InputState {
    float mx = -1e6f, my = -1e6f; // the pointer (DIPs), off the window: far away
    bool down = false;            // the left button
    bool pressed = false, released = false;
    u32 other_pressed = 0; // bits: 1 right, 2 middle, 4 X1, 8 X2 (the key capture)
    float wheel = 0;       // notches, + away from the user
    std::vector<KeyEvent> keys;
};

u64 Id(std::string_view s, u64 seed = 0);
inline u64 Id(u64 parent, u64 child) {
    return (parent * 0x9E3779B97F4A7C15ull) ^ (child + 0x7F4A7C15ull + (parent << 6) + (parent >> 2));
}

class Ui {
public:
    explicit Ui(Canvas& canvas) : c(canvas) {}

    Canvas& c;
    InputState in;
    double now = 0; // seconds
    float dt = 0;
    bool raw_keys = false; // a key capture: Tab is a key like the others this frame (no focus move)

    void BeginFrame(double now_s);
    void EndFrame();
    /// Whether anything still moves (a transition): the window then draws the next frame at once.
    bool Animating() const {
        return animating_;
    }
    void KeepAnimating() {
        animating_ = true;
    }
    Cursor CursorShape() const {
        return cursor_;
    }

    // ---- layers: input goes to the top one only (0: the page, 1+: a dialog, the open list)
    void SetInputLayer(int layer) {
        input_layer_ = layer;
    }
    int InputLayer() const {
        return input_layer_;
    }
    void SetLayer(int layer) {
        layer_ = layer;
    }
    int Layer() const {
        return layer_;
    }
    bool InputHere() const {
        return layer_ == input_layer_ && !popup_.open;
    }

    // ---- interaction
    struct Hit {
        bool hover = false;   // the pointer over it (and it takes the input)
        bool held = false;    // pressed on it, still down
        bool clicked = false; // released on it, or Enter / Space while focused
        bool focused = false;
        bool focus_ring = false; // focused by the keyboard: draw the outline
    };
    /// A box that can be hovered, pressed and (if `focusable`) reached with Tab.
    Hit Interact(u64 id, Rect r, bool enabled = true, bool focusable = true, Cursor cursor = Cursor::Hand);
    bool PointerIn(Rect r) const;
    /// Hit tests inside a scrolled page see only its visible part.
    void PushHitClip(Rect r);
    void PopHitClip();
    void SetCursor(Cursor c) {
        cursor_ = c;
    }
    u64 Focus() const {
        return focus_;
    }
    void SetFocus(u64 id, bool ring = false);
    bool FocusRingOn() const {
        return keyboard_focus_;
    }
    /// A key for the focused widget this frame (arrows, Home / End); consumed.
    bool TakeKey(u32 vk);
    /// A key nobody took yet (consumed when true).
    bool TakeAnyKey(u32 vk) {
        return TakeKey(vk);
    }

    // ---- transitions: a value that eases to `target` in `seconds` (CSS ease)
    float Anim(u64 id, float target, float seconds);
    void SnapAnim(u64 id, float value);

    // ---- outlines (:focus-visible)
    void FocusRing(Rect r, float radius, float offset, Color c = Theme::kAccentHover, float width = 2.0f);

    // ---- the open list of a select: one at a time, drawn last (DrawPopup)
    struct PopupOption {
        std::string label;
        bool disabled = false;
    };
    void OpenPopup(u64 owner, Rect anchor, std::vector<PopupOption> options, int current);
    bool PopupOpenFor(u64 owner) const {
        return popup_.open && popup_.owner == owner;
    }
    bool PopupOpen() const {
        return popup_.open;
    }
    void ClosePopup();
    /// The choice made in the list of `owner` since the last call (-1: none).
    int TakePopupChoice(u64 owner);
    /// The open list, on top of everything, inside `bounds` (the window's client area).
    void DrawPopup(Rect bounds);

private:
    struct AnimState {
        float from = 0, to = 0, value = 0;
        double start = 0;
        float seconds = 0;
    };
    struct Popup {
        bool open = false;
        u64 owner = 0;
        Rect anchor;
        std::vector<PopupOption> options;
        int current = -1, hovered = -1;
        float scroll = 0;
        bool fresh = false; // opened this frame: its click does not choose
    };

    int layer_ = 0, input_layer_ = 0;
    u64 hot_ = 0, active_ = 0, focus_ = 0;
    bool keyboard_focus_ = false;
    bool animating_ = false;
    bool tab_pending_ = false, shift_tab_ = false;
    Cursor cursor_ = Cursor::Arrow;
    std::vector<u64> focus_order_, prev_focus_order_;
    std::vector<Rect> hit_clips_;
    std::unordered_map<u64, AnimState> anims_;
    std::unordered_map<u64, int> chosen_;
    Popup popup_;
};

// ---- widgets (each draws itself and returns what changed)

enum class ButtonKind : u8 { Normal, Primary, Ghost, Danger };

struct ButtonStyle {
    ButtonKind kind = ButtonKind::Normal;
    bool small = false;   // .btn.small: 30 px, 0 12 px, 13 px
    float height = 0;     // 0: 34 (30 small)
    float pad_l = -1, pad_r = -1;
    float font_size = 0;  // 0: 14 (13 small)
    int weight = 500;
    float tracking = 0;
    float radius = Theme::kRadius;
    float icon = 16;
    Color bg{0, 0, 0, 0}; // a.k.a. .btn-tall's translucent background (a > 0)
    float shadow_alpha = 0, shadow_hover_alpha = 0, shadow_blur = 0, shadow_hover_blur = 0; // .btn-play
};

float ButtonWidth(Ui& ui, std::string_view label, std::string_view icon, const ButtonStyle& s = {});
bool Button(Ui& ui, u64 id, Rect r, std::string_view label, std::string_view icon = {}, const ButtonStyle& s = {},
            bool enabled = true);

/// .switch (42 x 24). Returns true when clicked.
bool Switch(Ui& ui, u64 id, Rect r, bool on, bool enabled = true);

struct SelectOption {
    std::string label;
    bool disabled = false;
};
/// .select: the current option's label, the list opens below. Returns the chosen index (-1: no change).
int Select(Ui& ui, u64 id, Rect r, std::span<const SelectOption> options, int current, bool enabled = true,
           Color bg = Theme::kSurface2);

/// .range (the track, the thumb): the value in [min, max] by `step`. Returns true when it changed.
bool Range(Ui& ui, u64 id, Rect r, double& value, double min, double max, double step, bool enabled = true);

/// .segmented: equal parts. Returns the chosen index (-1: no change).
int Segmented(Ui& ui, u64 id, Rect r, std::span<const std::string> labels, int current, bool enabled = true);
constexpr float kSegmentedH = 3 + 3 + 2 + 10 + 13 * Theme::kLine;

enum class ChipKind : u8 { Plain, Dev, Na, Exp, Preset, Changed, Example };
float ChipWidth(Ui& ui, std::string_view text, ChipKind kind = ChipKind::Plain);
/// A chip at (x, y): 20 px high. Returns its width.
float Chip(Ui& ui, float x, float y, std::string_view text, ChipKind kind);

float KeycapWidth(Ui& ui, std::string_view text);
/// .keycap at (x, y), 26 px high (`accent`: the capturing row's first key; `fg`: the text, .dimmed's in the pad
/// table). Returns its width.
float Keycap(Ui& ui, float x, float y, std::string_view text, bool accent = false, Color fg = Theme::kText);

/// A scrolled region: offset (eased to the target), the content height of the last frame, a scrollbar.
struct ScrollState {
    float offset = 0, target = 0, content = 0, view = 0;
    bool dragging = false;
    float drag_from = 0, drag_offset = 0;
};
/// Wheel and keys over `view`; call before drawing the content at y - s.offset.
void ScrollInput(Ui& ui, ScrollState& s, Rect view);
/// After the content: its height, the scrollbar in the gutter at the right of `view`.
void ScrollBar(Ui& ui, u64 id, ScrollState& s, Rect view, float content_h, float gutter);
void ScrollTo(ScrollState& s, float y, bool instant = false);

} // namespace SecondSon::Launcher
