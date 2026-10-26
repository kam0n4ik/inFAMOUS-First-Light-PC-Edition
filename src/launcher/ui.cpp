// SecondSonPC — the launcher's widgets (ui.h). Sizes and colours: the mockup's styles.css. Own code.
#include "launcher/ui.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

namespace SecondSon::Launcher {

namespace {

using namespace Theme;

/// CSS `ease`: cubic-bezier(0.25, 0.1, 0.25, 1) at time fraction `t`.
float Ease(float t) {
    if (t <= 0) {
        return 0;
    }
    if (t >= 1) {
        return 1;
    }
    constexpr float x1 = 0.25f, y1 = 0.1f, x2 = 0.25f, y2 = 1.0f;
    const auto bx = [](float s) { return 3 * (1 - s) * (1 - s) * s * x1 + 3 * (1 - s) * s * s * x2 + s * s * s; };
    const auto by = [](float s) { return 3 * (1 - s) * (1 - s) * s * y1 + 3 * (1 - s) * s * s * y2 + s * s * s; };
    float lo = 0, hi = 1, s = t;
    for (int i = 0; i < 24; ++i) {
        s = (lo + hi) / 2;
        (bx(s) < t ? lo : hi) = s;
    }
    return by(s);
}

/// One line of text vertically centred in `r` (CSS: a line box of `line_h` centred by flex).
void TextMid(Ui& ui, std::string_view s, Rect r, const Font& f, Color c, Align a = Align::Left) {
    const float lh = f.size * kLine;
    ui.c.Text(s, {r.x, r.y + (r.h - lh) / 2, r.w, lh}, f, c, lh, a, false);
}

float TextW(Ui& ui, std::string_view s, const Font& f) {
    return ui.c.Measure(s, f, f.size * kLine, 1e5f, false).w;
}

} // namespace

u64 Id(std::string_view s, u64 seed) {
    u64 h = 1469598103934665603ull ^ seed;
    for (const char ch : s) {
        h = (h ^ static_cast<unsigned char>(ch)) * 1099511628211ull;
    }
    return h;
}

// ------------------------------------------------------------------ Ui

void Ui::BeginFrame(double now_s) {
    dt = now == 0 ? 0.0f : static_cast<float>(std::min(0.1, now_s - now));
    now = now_s;
    animating_ = false;
    cursor_ = Cursor::Arrow;
    hot_ = 0;
    prev_focus_order_.swap(focus_order_);
    focus_order_.clear();
    hit_clips_.clear();
    layer_ = 0;
    tab_pending_ = false;
    for (auto it = in.keys.begin(); it != in.keys.end();) {
        if (it->vk == VK_TAB && !it->ctrl && !it->alt && !popup_.open && !raw_keys) {
            tab_pending_ = true;
            shift_tab_ = it->shift;
            it = in.keys.erase(it);
        } else {
            ++it;
        }
    }
    if (in.pressed && !popup_.open) {
        // a click drops the keyboard's focus ring (a widget that is clicked takes the focus itself)
        keyboard_focus_ = false;
    }
}

void Ui::EndFrame() {
    if (in.released || !in.down) {
        active_ = 0;
    }
    if (tab_pending_ && !focus_order_.empty()) {
        const auto it = std::ranges::find(focus_order_, focus_);
        size_t i = 0;
        if (it == focus_order_.end()) {
            i = shift_tab_ ? focus_order_.size() - 1 : 0;
        } else {
            const size_t at = static_cast<size_t>(it - focus_order_.begin());
            i = shift_tab_ ? (at + focus_order_.size() - 1) % focus_order_.size() : (at + 1) % focus_order_.size();
        }
        focus_ = focus_order_[i];
        keyboard_focus_ = true;
        animating_ = true; // redraw with the ring
    }
    if (focus_ != 0 && std::ranges::find(focus_order_, focus_) == focus_order_.end() && !popup_.open) {
        focus_ = 0; // the widget is gone (another page)
    }
    in.pressed = in.released = false;
    in.other_pressed = 0;
    in.wheel = 0;
    in.keys.clear();
}

bool Ui::PointerIn(Rect r) const {
    if (!r.Contains(in.mx, in.my)) {
        return false;
    }
    return hit_clips_.empty() || hit_clips_.back().Contains(in.mx, in.my);
}

void Ui::PushHitClip(Rect r) {
    if (!hit_clips_.empty()) {
        const Rect& p = hit_clips_.back();
        const float x0 = std::max(r.x, p.x), y0 = std::max(r.y, p.y);
        const float x1 = std::min(r.R(), p.R()), y1 = std::min(r.B(), p.B());
        r = {x0, y0, std::max(0.0f, x1 - x0), std::max(0.0f, y1 - y0)};
    }
    hit_clips_.push_back(r);
}

void Ui::PopHitClip() {
    if (!hit_clips_.empty()) {
        hit_clips_.pop_back();
    }
}

void Ui::SetFocus(u64 id, bool ring) {
    focus_ = id;
    keyboard_focus_ = ring;
}

Ui::Hit Ui::Interact(u64 id, Rect r, bool enabled, bool focusable, Cursor cursor) {
    Hit h;
    if (!InputHere()) {
        return h;
    }
    if (focusable && enabled) {
        focus_order_.push_back(id);
    }
    const bool inside = PointerIn(r);
    if (inside) {
        hot_ = id;
        cursor_ = enabled ? cursor : Cursor::NotAllowed;
    }
    if (!enabled) {
        if (active_ == id) {
            active_ = 0;
        }
        return h;
    }
    h.hover = inside && (active_ == 0 || active_ == id);
    if (inside && in.pressed) {
        active_ = id;
        if (focusable) {
            focus_ = id;
        }
    }
    if (active_ == id) {
        h.held = in.down || in.pressed;
        if (in.released && inside) {
            h.clicked = true;
        }
    }
    if (focus_ == id) {
        h.focused = true;
        h.focus_ring = keyboard_focus_;
        for (auto it = in.keys.begin(); it != in.keys.end(); ++it) {
            if ((it->vk == VK_RETURN || it->vk == VK_SPACE) && !it->ctrl && !it->alt) {
                h.clicked = true;
                in.keys.erase(it);
                break;
            }
        }
    }
    return h;
}

bool Ui::TakeKey(u32 vk) {
    for (auto it = in.keys.begin(); it != in.keys.end(); ++it) {
        if (it->vk == vk) {
            in.keys.erase(it);
            return true;
        }
    }
    return false;
}

float Ui::Anim(u64 id, float target, float seconds) {
    auto [it, fresh] = anims_.try_emplace(id);
    AnimState& a = it->second;
    if (fresh) {
        a.from = a.to = a.value = target;
        return target;
    }
    if (a.to != target) {
        a.from = a.value;
        a.to = target;
        a.start = now;
        a.seconds = seconds;
    }
    if (a.value != a.to) {
        const float t = a.seconds <= 0 ? 1.0f : static_cast<float>((now - a.start) / a.seconds);
        a.value = t >= 1.0f ? a.to : a.from + (a.to - a.from) * Ease(t);
        if (a.value != a.to) {
            animating_ = true;
        }
    }
    return a.value;
}

void Ui::SnapAnim(u64 id, float value) {
    AnimState& a = anims_[id];
    a.from = a.to = a.value = value;
}

void Ui::FocusRing(Rect r, float radius, float offset, Color col, float width) {
    const Rect o{r.x - offset - width, r.y - offset - width, r.w + 2 * (offset + width), r.h + 2 * (offset + width)};
    c.Border(o, col, width, radius > 0 ? radius + offset + width : 0);
}

void Ui::OpenPopup(u64 owner, Rect anchor, std::vector<PopupOption> options, int current) {
    popup_.open = true;
    popup_.owner = owner;
    popup_.anchor = anchor;
    popup_.options = std::move(options);
    popup_.current = current;
    popup_.hovered = current;
    popup_.scroll = -1; // (placed at the first draw)
    popup_.fresh = true;
    animating_ = true;
}

void Ui::ClosePopup() {
    popup_.open = false;
    animating_ = true;
}

int Ui::TakePopupChoice(u64 owner) {
    const auto it = chosen_.find(owner);
    if (it == chosen_.end()) {
        return -1;
    }
    const int v = it->second;
    chosen_.erase(it);
    return v;
}

void Ui::DrawPopup(Rect bounds) {
    if (!popup_.open) {
        return;
    }
    Popup& p = popup_;
    constexpr float kItemH = 30, kPad = 4;
    const int n = static_cast<int>(p.options.size());
    const float full = n * kItemH + 2 * kPad + 2;
    const float below = bounds.B() - p.anchor.B() - 8, above = p.anchor.y - bounds.y - 8;
    const bool down = below >= std::min(full, 240.0f) || below >= above;
    const float h = std::min(full, std::max(kItemH * 3 + 2 * kPad + 2, down ? below : above));
    const Rect box{p.anchor.x, down ? p.anchor.B() + 4 : p.anchor.y - 4 - h, p.anchor.w, h};
    const float view = h - 2 * kPad - 2, content = n * kItemH;
    const float max_scroll = std::max(0.0f, content - view);
    if (p.scroll < 0) {
        p.scroll = std::clamp((p.current + 0.5f) * kItemH - view / 2, 0.0f, max_scroll);
    }
    // keys: arrows move, Enter chooses, Esc closes
    const auto step = [&](int dir) {
        for (int i = p.hovered + dir; i >= 0 && i < n; i += dir) {
            if (!p.options[i].disabled) {
                p.hovered = i;
                p.scroll = std::clamp(p.scroll, (i + 1) * kItemH - view, static_cast<float>(i) * kItemH);
                break;
            }
        }
    };
    for (const auto& k : in.keys) {
        if (k.vk == VK_DOWN) {
            step(1);
        } else if (k.vk == VK_UP) {
            step(-1);
        } else if (k.vk == VK_RETURN || k.vk == VK_SPACE) {
            if (p.hovered >= 0 && p.hovered < n && !p.options[p.hovered].disabled) {
                chosen_[p.owner] = p.hovered;
            }
            p.open = false;
        } else if (k.vk == VK_ESCAPE || k.vk == VK_TAB) {
            p.open = false;
        }
    }
    in.keys.clear();
    if (box.Contains(in.mx, in.my) && in.wheel != 0) {
        p.scroll = std::clamp(p.scroll - in.wheel * 3 * kItemH, 0.0f, max_scroll);
        in.wheel = 0;
    }
    // the box (a dark list in the mockup's option colours: #1b1c21, disabled #6c6e76)
    c.Shadow(box, kRadius, 0, 8, 24, 0, Rgb(0, 0.5f));
    c.FillRect(box, kSurface2, kRadius);
    c.Border(box, kBorderStrong, 1, kRadius);
    const Rect list{box.x + 1, box.y + 1 + kPad, box.w - 2, view};
    c.PushClip(list);
    int hover_now = -1;
    for (int i = 0; i < n; ++i) {
        const Rect item{list.x + kPad, list.y + i * kItemH - p.scroll, list.w - 2 * kPad, kItemH};
        if (item.B() < list.y || item.y > list.B()) {
            continue;
        }
        const auto& o = p.options[i];
        if (item.Contains(in.mx, in.my) && list.Contains(in.mx, in.my)) {
            hover_now = i;
            cursor_ = o.disabled ? Cursor::Arrow : Cursor::Hand;
        }
        if (i == p.hovered && !o.disabled) {
            c.FillRect(item, kSurface3, kRadiusS);
        }
        if (i == p.current) {
            c.FillRect({item.x, item.y + 7, 3, item.h - 14}, kAccent, 1.5f);
        }
        TextMid(*this, o.label, {item.x + 10, item.y, item.w - 20, item.h}, F(14), o.disabled ? Rgb(0x6c6e76) : kText);
    }
    c.PopClip();
    if (max_scroll > 0) {
        const float th = std::max(24.0f, view * view / content);
        const float ty = list.y + (view - th) * (p.scroll / max_scroll);
        c.FillRect({box.R() - 7, ty, 4, th}, kBorderStrong, 2);
    }
    if (hover_now >= 0 && !p.options[hover_now].disabled) {
        p.hovered = hover_now;
    }
    if (in.released && !p.fresh && hover_now >= 0 && !p.options[hover_now].disabled) {
        chosen_[p.owner] = hover_now;
        p.open = false;
    } else if (in.pressed && !p.fresh && !box.Contains(in.mx, in.my)) {
        p.open = false; // a click outside closes it and does nothing else
    }
    if (!in.down) {
        p.fresh = false; // (the click that opened it is over)
    }
    in.pressed = in.released = false;
    if (!p.open) {
        animating_ = true;
    }
}

// ------------------------------------------------------------------ buttons

float ButtonWidth(Ui& ui, std::string_view label, std::string_view icon, const ButtonStyle& s) {
    const float fs = s.font_size > 0 ? s.font_size : s.small ? 13.0f : 14.0f;
    const float pl = s.pad_l >= 0 ? s.pad_l : s.small ? 12.0f : 16.0f;
    const float pr = s.pad_r >= 0 ? s.pad_r : pl;
    float w = label.empty() ? 0.0f : TextW(ui, label, F(fs, s.weight, s.tracking));
    if (!icon.empty()) {
        w += s.icon + (label.empty() ? 0 : 8);
    }
    return w + pl + pr + 2;
}

bool Button(Ui& ui, u64 id, Rect r, std::string_view label, std::string_view icon, const ButtonStyle& s,
            bool enabled) {
    const Ui::Hit h = ui.Interact(id, r, enabled);
    const bool hover = h.hover, press = h.held && h.hover;
    Color bg = s.bg.a > 0 ? s.bg : kSurface2, border = kBorderStrong, fg = kText;
    switch (s.kind) {
    case ButtonKind::Normal:
        if (hover) {
            bg = press ? Rgb(0x1a1b20) : kSurface3;
            border = Rgb(0x4b4f59);
        }
        break;
    case ButtonKind::Ghost:
        bg = {0, 0, 0, 0};
        if (hover) {
            bg = press ? Rgb(0x1a1b20) : kSurface3;
            border = Rgb(0x4b4f59);
        }
        break;
    case ButtonKind::Primary:
        bg = border = kAccent;
        fg = kWhite;
        if (hover) {
            bg = press ? kAccentPress : kAccentHover;
            border = kAccentHover;
        }
        break;
    case ButtonKind::Danger:
        bg = {0, 0, 0, 0};
        border = Rgb(0xff5b5b, 0.45f);
        fg = Rgb(0xff8a8a);
        if (hover) {
            bg = Rgb(0xff5b5b, 0.1f);
            border = kDanger;
        }
        break;
    }
    if (!enabled) {
        ui.c.PushOpacity(0.42f);
    }
    if (s.shadow_alpha > 0) {
        const bool sh = hover && enabled;
        ui.c.Shadow(r, s.radius, 0, 6, sh ? s.shadow_hover_blur : s.shadow_blur, 0,
                    Rgb(0xe5336c, sh ? s.shadow_hover_alpha : s.shadow_alpha));
    }
    ui.c.FillRect(r, bg, s.radius);
    ui.c.Border(r, border, 1, s.radius);
    const float fs = s.font_size > 0 ? s.font_size : s.small ? 13.0f : 14.0f;
    const Font f = F(fs, s.weight, s.tracking);
    const float tw = label.empty() ? 0.0f : TextW(ui, label, f);
    const float cw = tw + (icon.empty() ? 0.0f : s.icon + (label.empty() ? 0 : 8));
    // (justify-content: center within the padding box)
    const float pl = s.pad_l >= 0 ? s.pad_l : s.small ? 12.0f : 16.0f;
    const float pr = s.pad_r >= 0 ? s.pad_r : pl;
    float x = r.x + 1 + pl + (r.w - 2 - pl - pr - cw) / 2;
    if (!icon.empty()) {
        const bool filled = icon == "play";
        ui.c.Icon(IconPath(icon), {x, r.y + (r.h - s.icon) / 2, s.icon, s.icon}, fg, filled ? 0.0f : 1.8f, filled);
        x += s.icon + 8;
    }
    if (!label.empty()) {
        TextMid(ui, label, {x, r.y, tw + 4, r.h}, f, fg);
    }
    if (!enabled) {
        ui.c.PopOpacity();
    }
    if (h.focus_ring) {
        ui.FocusRing(r, s.radius, 2);
    }
    return h.clicked;
}

// ------------------------------------------------------------------ switch

bool Switch(Ui& ui, u64 id, Rect r, bool on, bool enabled) {
    const Ui::Hit h = ui.Interact(id, r, enabled);
    const float t = ui.Anim(Id(id, 1), on ? 1.0f : 0.0f, 0.12f);
    Color bg = kSurface3, border = kBorderStrong;
    if (on) {
        bg = border = kAccent;
        if (h.hover) {
            bg = border = kAccentHover;
        }
    } else if (h.hover) {
        border = Rgb(0x555a65);
    }
    ui.c.FillRect(r, bg, r.h / 2);
    ui.c.Border(r, border, 1, r.h / 2);
    ui.c.FillCircle(r.x + 1 + 3 + 8 + 18 * t, r.y + 1 + 3 + 8, 8, Mix(kMuted, kWhite, t));
    if (h.focus_ring) {
        ui.FocusRing(r, r.h / 2, 2);
    }
    return h.clicked;
}

// ------------------------------------------------------------------ select

int Select(Ui& ui, u64 id, Rect r, std::span<const SelectOption> options, int current, bool enabled, Color bg) {
    int result = ui.TakePopupChoice(id);
    if (result == current) {
        result = -1;
    }
    const bool open = ui.PopupOpenFor(id);
    const Ui::Hit h = ui.Interact(id, r, enabled);
    const bool hover = (h.hover || open) && enabled;
    ui.c.FillRect(r, hover ? kSurface3 : bg, kRadius);
    ui.c.Border(r, hover ? Rgb(0x4b4f59) : kBorderStrong, 1, kRadius);
    const std::string_view label =
        current >= 0 && current < static_cast<int>(options.size()) ? std::string_view(options[current].label) : "";
    TextMid(ui, label, {r.x + 12, r.y, r.w - 12 - 34 - 1, r.h}, F(14), kText);
    ui.c.Icon(IconPath("select-chev"), {r.R() - 1 - 11 - 12, r.y + (r.h - 12) / 2, 12, 12}, kMuted, 1.6f, false, 12);
    if (h.focus_ring) {
        ui.FocusRing(r, kRadius, 2);
    }
    if (!enabled) {
        return result;
    }
    const auto open_list = [&] {
        std::vector<Ui::PopupOption> list;
        list.reserve(options.size());
        for (const auto& o : options) {
            list.push_back({o.label, o.disabled});
        }
        ui.OpenPopup(id, r, std::move(list), current);
    };
    if (h.hover && ui.in.pressed) {
        open_list(); // (a select opens on the press)
    } else if (h.clicked && !ui.in.released) {
        open_list(); // Enter / Space
    } else if (h.focused) {
        const auto move = [&](int dir) {
            for (int i = current + dir; i >= 0 && i < static_cast<int>(options.size()); i += dir) {
                if (!options[i].disabled) {
                    return i;
                }
            }
            return -1;
        };
        if (ui.TakeKey(VK_DOWN)) {
            result = move(1);
        } else if (ui.TakeKey(VK_UP)) {
            result = move(-1);
        }
    }
    return result;
}

// ------------------------------------------------------------------ range

bool Range(Ui& ui, u64 id, Rect r, double& value, double min, double max, double step, bool enabled) {
    const Ui::Hit h = ui.Interact(id, r, enabled);
    const double old = value;
    const auto snap = [&](double v) {
        v = std::clamp(v, min, max);
        v = min + std::round((v - min) / step) * step;
        return std::round(std::clamp(v, min, max) * 100.0) / 100.0;
    };
    if (enabled && h.held) {
        const float f = std::clamp((ui.in.mx - r.x - 8) / std::max(1.0f, r.w - 16), 0.0f, 1.0f);
        value = snap(min + (max - min) * f);
        ui.SetCursor(Cursor::Hand);
    }
    if (enabled && h.focused) {
        if (ui.TakeKey(VK_RIGHT) || ui.TakeKey(VK_UP)) {
            value = snap(value + step);
        } else if (ui.TakeKey(VK_LEFT) || ui.TakeKey(VK_DOWN)) {
            value = snap(value - step);
        } else if (ui.TakeKey(VK_HOME)) {
            value = min;
        } else if (ui.TakeKey(VK_END)) {
            value = max;
        }
    }
    const float pct = max > min ? static_cast<float>((value - min) / (max - min)) : 0.0f;
    const float ty = r.y + r.h / 2 - 2;
    // the track: linear-gradient(to right, accent pct, #3a3d46 pct) over its whole width
    ui.c.FillRect({r.x, ty, r.w, 4}, Rgb(0x3a3d46), 2);
    if (pct > 0) {
        ui.c.PushClip({r.x, ty - 1, r.w * pct, 6});
        ui.c.FillRect({r.x, ty, r.w, 4}, kAccent, 2);
        ui.c.PopClip();
    }
    const float cx = r.x + 8 + (r.w - 16) * pct, cy = r.y + r.h / 2;
    if (h.focus_ring) {
        ui.c.FillCircle(cx, cy, 8 + 3, kAccentLine);
        ui.c.FillCircle(cx, cy, 8 + 1, kWhite);
    } else {
        ui.c.FillCircle(cx, cy, 8 + 1, Rgb(0, 0.3f));
    }
    ui.c.FillCircle(cx, cy, 8, kWhite);
    return value != old;
}

// ------------------------------------------------------------------ segmented

int Segmented(Ui& ui, u64 id, Rect r, std::span<const std::string> labels, int current, bool enabled) {
    int result = -1;
    ui.c.FillRect(r, kSurface2, kRadius);
    ui.c.Border(r, kBorderStrong, 1, kRadius);
    const int n = static_cast<int>(labels.size());
    const float inner = r.w - 2 - 6, w = (inner - 3 * (n - 1)) / n;
    for (int i = 0; i < n; ++i) {
        const Rect seg{r.x + 1 + 3 + i * (w + 3), r.y + 1 + 3, w, r.h - 2 - 6};
        const u64 sid = Id(id, static_cast<u64>(i) + 1);
        const Ui::Hit h = ui.Interact(sid, seg, enabled);
        const bool checked = i == current;
        Color fg = kMuted, bg{0, 0, 0, 0};
        if (h.hover) {
            fg = kText;
            bg = kSurface3;
        }
        if (checked) {
            fg = kWhite;
            bg = kAccent;
        }
        ui.c.FillRect(seg, bg, kRadiusS);
        TextMid(ui, labels[i], seg, F(13), fg, Align::Center);
        if (h.focus_ring) {
            ui.FocusRing(seg, kRadiusS, 1, kAccentHover);
        }
        if (h.clicked && !checked) {
            result = i;
        }
        if (h.focused) {
            if (ui.TakeKey(VK_RIGHT) || ui.TakeKey(VK_DOWN)) {
                result = (current + 1) % n;
                ui.SetFocus(Id(id, static_cast<u64>(result) + 1), true);
            } else if (ui.TakeKey(VK_LEFT) || ui.TakeKey(VK_UP)) {
                result = (current + n - 1) % n;
                ui.SetFocus(Id(id, static_cast<u64>(result) + 1), true);
            }
        }
    }
    return result;
}

// ------------------------------------------------------------------ chips, keycaps

namespace {

Font ChipFont(ChipKind k) {
    return F(11, k == ChipKind::Example ? 500 : 600, 0.01f);
}

} // namespace

float ChipWidth(Ui& ui, std::string_view text, ChipKind kind) {
    return TextW(ui, text, ChipFont(kind)) + 16 + 2;
}

float Chip(Ui& ui, float x, float y, std::string_view text, ChipKind kind) {
    Color fg = kMuted, border = kBorderStrong, bg{0, 0, 0, 0};
    switch (kind) {
    case ChipKind::Plain:
    case ChipKind::Example:
        break;
    case ChipKind::Dev:
        fg = kAmber;
        border = Rgb(0xf0a73a, 0.5f);
        bg = kAmberWeak;
        break;
    case ChipKind::Na:
        fg = kDim;
        break;
    case ChipKind::Exp:
        fg = kAccentHover;
        border = kAccentLine;
        bg = kAccentWeak;
        break;
    case ChipKind::Preset:
        fg = kOk;
        border = Rgb(0x45c98b, 0.45f);
        break;
    case ChipKind::Changed:
        fg = kText;
        bg = kSurface3;
        break;
    }
    const Font f = ChipFont(kind);
    const float w = TextW(ui, text, f) + 16 + 2;
    const Rect r{x, y, w, 20};
    ui.c.FillRect(r, bg, 10);
    ui.c.Border(r, border, 1, 10);
    ui.c.Text(text, {x + 9, y + 2, w, 16}, f, fg, 16, Align::Left, false);
    return w;
}

float KeycapWidth(Ui& ui, std::string_view text) {
    return std::max(30.0f, TextW(ui, text, F(12.5f, 600)) + 16 + 2);
}

float Keycap(Ui& ui, float x, float y, std::string_view text, bool accent, Color fg) {
    const float w = KeycapWidth(ui, text);
    const Color border = accent ? kAccent : kBorderStrong;
    ui.c.FillRect({x, y, w, 26}, border, kRadiusS);
    ui.c.FillRect({x + 1, y + 1, w - 2, 26 - 3}, kSurface2, kRadiusS - 1);
    // (inline-grid, place-items: center: the text in the content box above the 2 px bottom border)
    TextMid(ui, text, {x + 1, y + 1, w - 2, 26 - 3}, F(12.5f, 600), accent ? kAccentHover : fg, Align::Center);
    return w;
}

// ------------------------------------------------------------------ scrolling

void ScrollTo(ScrollState& s, float y, bool instant) {
    s.target = std::clamp(y, 0.0f, std::max(0.0f, s.content - s.view));
    if (instant) {
        s.offset = s.target;
    }
}

void ScrollInput(Ui& ui, ScrollState& s, Rect view) {
    s.view = view.h;
    const float max_scroll = std::max(0.0f, s.content - s.view);
    if (ui.InputHere() && ui.PointerIn(view) && ui.in.wheel != 0) {
        s.target = std::clamp(s.target - ui.in.wheel * 100.0f, 0.0f, max_scroll);
        ui.in.wheel = 0;
    }
    if (ui.InputHere() && ui.Focus() == 0) {
        if (ui.TakeKey(VK_NEXT)) {
            s.target = std::clamp(s.target + s.view * 0.875f, 0.0f, max_scroll);
        } else if (ui.TakeKey(VK_PRIOR)) {
            s.target = std::clamp(s.target - s.view * 0.875f, 0.0f, max_scroll);
        } else if (ui.TakeKey(VK_HOME)) {
            s.target = 0;
        } else if (ui.TakeKey(VK_END)) {
            s.target = max_scroll;
        }
    }
    s.target = std::clamp(s.target, 0.0f, max_scroll);
    if (s.dragging) {
        s.offset = s.target;
    } else if (std::abs(s.target - s.offset) > 0.25f) {
        s.offset += (s.target - s.offset) * std::min(1.0f, ui.dt * 16.0f);
        ui.KeepAnimating();
    } else {
        s.offset = s.target;
    }
}

void ScrollBar(Ui& ui, u64 id, ScrollState& s, Rect view, float content_h, float gutter) {
    s.content = content_h;
    s.view = view.h;
    const float max_scroll = std::max(0.0f, s.content - s.view);
    if (max_scroll <= 0 || gutter <= 0) {
        s.dragging = false;
        return;
    }
    const float th = std::max(32.0f, view.h * view.h / s.content);
    const float ty = view.y + (view.h - th) * (s.offset / max_scroll);
    const Rect track{view.R() - gutter, view.y, gutter, view.h};
    const Rect thumb{track.x + (gutter - 8) / 2, ty + 2, 8, th - 4};
    const Ui::Hit h = ui.Interact(id, {track.x, ty, gutter, th}, true, false, Cursor::Arrow);
    if (h.held && ui.in.pressed) {
        s.dragging = true;
        s.drag_from = ui.in.my;
        s.drag_offset = s.offset;
    }
    if (s.dragging) {
        if (!ui.in.down) {
            s.dragging = false;
        } else {
            s.target = std::clamp(s.drag_offset + (ui.in.my - s.drag_from) * max_scroll / std::max(1.0f, view.h - th),
                                  0.0f, max_scroll);
            s.offset = s.target;
        }
    }
    ui.c.FillRect(thumb, h.hover || s.dragging ? Rgb(0x4b4f59) : kBorderStrong, 4);
}

} // namespace SecondSon::Launcher
