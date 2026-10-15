// SecondSonPC — the in-game settings menu as a model (see settings_menu.h).
#include "ui/settings_menu.h"

#include <algorithm>
#include <cmath>
#include <span>

#include <fmt/format.h>

#include "input/binding_rules.h"
#include "settings/registry.h"

namespace SecondSon::Ui {

namespace {

using Settings::Category;
using Settings::Desc;
using Settings::Type;
using Settings::Value;

// The game's menu look (its settings screen, measured on a 1920x1080 frame): a dark see-through panel with a thin
// white frame, a yellow title in capitals, white section names in capitals, white items, the chosen item yellow.
constexpr Color kPanel{14, 14, 14, 228};
constexpr Color kFrame{235, 235, 235, 255};
constexpr Color kSidebar{0, 0, 0, 70};
constexpr Color kLine{255, 255, 255, 46};
constexpr Color kRowFocus{255, 255, 255, 20};
constexpr Color kRowHover{255, 255, 255, 10};
constexpr Color kAccent{245, 184, 0, 255};
constexpr Color kText{240, 240, 240, 255};
constexpr Color kText2{168, 168, 168, 255};
constexpr Color kDisabled{100, 100, 100, 255};
constexpr Color kWarn{255, 107, 94, 255};
constexpr Color kAmber{245, 184, 0, 255};
constexpr Color kTrackOff{80, 80, 80, 255};
constexpr Color kWhite{255, 255, 255, 255};
constexpr Color kKeycap{38, 38, 38, 255};
constexpr Color kKeycapLine{120, 120, 120, 255};
constexpr Color kGlyphBg{38, 38, 38, 255};
constexpr Color kShade{0, 0, 0, 150};

/// Capitals as the game writes its titles (ASCII and Russian; other text unchanged).
std::string Caps(std::string_view utf8) {
    std::string out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size(); ++i) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c >= 'a' && c <= 'z') {
            out += static_cast<char>(c - 32);
        } else if ((c == 0xD0 || c == 0xD1) && i + 1 < utf8.size()) {
            const auto d = static_cast<unsigned char>(utf8[i + 1]);
            if (c == 0xD0 && d >= 0xB0 && d <= 0xBF) { // а..п
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d - 0x20);
            } else if (c == 0xD1 && d >= 0x80 && d <= 0x8F) { // р..я
                out += static_cast<char>(0xD0);
                out += static_cast<char>(d + 0x20);
            } else if (c == 0xD1 && d == 0x91) { // ё
                out += static_cast<char>(0xD0);
                out += static_cast<char>(0x81);
            } else {
                out += static_cast<char>(c);
                out += static_cast<char>(d);
            }
            ++i;
        } else {
            out += static_cast<char>(c);
        }
    }
    return out;
}

constexpr Category kOrder[] = {Category::Display,  Category::Graphics, Category::Upscaling, Category::Audio,     Category::Controls,
                               Category::Keyboard, Category::Gamepad,  Category::Interface, Category::Shaders,
                               Category::Game};
constexpr u64 kConfirmCommandNs = 5'000'000'000;

using A = Input::Action;
constexpr A kMoveActions[] = {A::MoveForward, A::MoveBack, A::MoveLeft, A::MoveRight, A::Walk};
constexpr A kButtonActions[] = {A::Cross,  A::Circle,  A::Square, A::Triangle, A::L1,       A::R1,
                                A::L2,     A::R2,      A::L3,     A::R3,       A::Options,  A::DpadUp,
                                A::DpadDown, A::DpadLeft, A::DpadRight};
constexpr A kTouchActions[] = {A::TouchPad, A::TouchTwoFingers, A::TouchDrag, A::MotionMode, A::Shake};
constexpr A kPcActions[] = {A::ReleaseMouse};
struct Group {
    std::string_view en, ru;
    std::span<const A> actions;
};
const Group kGroups[] = {
    {"Movement", "Движение", kMoveActions},
    {"Controller buttons", "Кнопки контроллера", kButtonActions},
    {"Touch pad and motion", "Сенсорная панель и наклон", kTouchActions},
    {"SecondSonPC", "SecondSonPC", kPcActions},
};

constexpr std::string_view kDisplayIds[] = {"display.fullscreen", "display.fullscreenMode", "display.width",
                                            "display.height", "display.monitor"};

bool IsDisplayId(std::string_view id) {
    return std::ranges::find(kDisplayIds, id) != std::end(kDisplayIds);
}

const Desc* BindingDesc(Input::Action a) {
    return Settings::Find(fmt::format("keyboard.{}", Input::ActionName(a)));
}

enum class Hint : u8 { Navigate, Adjust, Accept, Back, Reset, Clear, Tabs };

/// A button symbol of the footer: a keycap, a PlayStation shape or an Xbox letter, drawn from primitives (no font
/// glyphs, no system icons). Returns its width.
float DrawGlyph(DrawList& out, TextMeasure& measure, Hint hint, const MenuEnv& env, float x, float cy, float s) {
    const auto keycap = [&](std::string_view label) {
        const float size = 14.0f * s;
        const float w = std::max(28.0f * s, measure.Width(label, size, FontWeight::Semibold) + 16.0f * s);
        const Rect r{x, cy - 14.0f * s, w, 28.0f * s};
        out.FillRect(r, kKeycap, 6.0f * s);
        out.StrokeRect(r, kKeycapLine, std::max(1.0f, s), 6.0f * s);
        out.Text(r, label, size, kText, Align::Center, FontWeight::Semibold);
        return w;
    };
    const float r = 14.0f * s;
    const float gx = x + r;
    switch (env.prompts) {
    case Input::PromptStyle::Keyboard: {
        constexpr std::string_view kKeys[] = {"↑↓", "←→", "Enter", "Esc", "Del", "Backspace", "Tab"};
        return keycap(kKeys[static_cast<size_t>(hint)]);
    }
    case Input::PromptStyle::PlayStation: {
        enum Shape { Cross, Circle, Square, Triangle };
        Shape shape;
        switch (hint) {
        case Hint::Navigate:
            return keycap("↑↓");
        case Hint::Adjust:
            return keycap("←→");
        case Hint::Tabs:
            return keycap("L1 R1");
        case Hint::Accept:
            shape = env.confirm_is_circle ? Circle : Cross;
            break;
        case Hint::Back:
            shape = env.confirm_is_circle ? Cross : Circle;
            break;
        case Hint::Reset:
            shape = Square;
            break;
        case Hint::Clear:
        default:
            shape = Triangle;
            break;
        }
        out.FillCircle(gx, cy, r, kGlyphBg);
        const float k = 6.5f * s, w = 2.2f * s;
        switch (shape) {
        case Cross:
            out.Line(gx - k, cy - k, gx + k, cy + k, Color{124, 170, 234}, w);
            out.Line(gx - k, cy + k, gx + k, cy - k, Color{124, 170, 234}, w);
            break;
        case Circle:
            out.StrokeCircle(gx, cy, k, Color{232, 98, 98}, w);
            break;
        case Square:
            out.StrokeRect({gx - k, cy - k, 2 * k, 2 * k}, Color{214, 126, 190}, w);
            break;
        case Triangle:
            out.StrokeTriangle(gx, cy - k, gx + k, cy + k * 0.8f, gx - k, cy + k * 0.8f, Color{92, 200, 160}, w);
            break;
        }
        return 2 * r;
    }
    case Input::PromptStyle::Xbox: {
        struct Letter {
            std::string_view text;
            Color color;
        };
        Letter letter;
        switch (hint) {
        case Hint::Navigate:
            return keycap("↑↓");
        case Hint::Adjust:
            return keycap("←→");
        case Hint::Tabs:
            return keycap("LB RB");
        case Hint::Accept:
            letter = {"A", Color{94, 186, 73}};
            break;
        case Hint::Back:
            letter = {"B", Color{222, 74, 63}};
            break;
        case Hint::Reset:
            letter = {"X", Color{63, 129, 222}};
            break;
        case Hint::Clear:
        default:
            letter = {"Y", Color{240, 190, 40}};
            break;
        }
        out.FillCircle(gx, cy, r, letter.color);
        out.Text({x, cy - r, 2 * r, 2 * r}, letter.text, 15.0f * s, Color{20, 22, 26}, Align::Center,
                 FontWeight::Semibold);
        return 2 * r;
    }
    }
    return 0.0f;
}

std::string Replace(std::string s, std::string_view from, std::string_view to) {
    for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size())) {
        s.replace(at, from.size(), to);
    }
    return s;
}

} // namespace

SettingsMenu::SettingsMenu(Settings::Service& service) : service_{service} {}

void SettingsMenu::Open() {
    open_ = true;
    close_request_ = false;
    capture_.reset();
    modal_ = Modal::None;
    dragging_row_ = -1;
    hover_row_ = -1;
    status_.clear();
    RebuildRows();
    if (focus_ >= rows_.size() || !Focusable(focus_)) {
        focus_ = 0;
        MoveFocus(0);
    }
}

void SettingsMenu::Close() {
    if (modal_ == Modal::ConfirmDisplay) {
        RevertDisplay(); // closed without keeping them: the safe side (the picture may be gone)
    }
    open_ = false;
    capture_.reset();
    modal_ = Modal::None;
    dragging_row_ = -1;
}

bool SettingsMenu::TakeCloseRequest() {
    return std::exchange(close_request_, false);
}

void SettingsMenu::SetEnv(MenuEnv env) {
    env_ = std::move(env);
    if (GameStyle() != game_style_) {
        game_style_ = GameStyle();
        RebuildRows();
        focus_ = 0;
        MoveFocus(0);
        game_scroll_ = 0;
    }
}

void SettingsMenu::SyncCategory() {
    if (game_style_ && focus_ < rows_.size()) {
        category_ = rows_[focus_].cat;
    }
}

void SettingsMenu::SelectCategory(Category category) {
    category_ = category;
    capture_.reset();
    hover_row_ = -1;
    dragging_row_ = -1;
    scroll_ = 0;
    slot_ = 0;
    RebuildRows();
    focus_ = 0;
    MoveFocus(0);
}

void SettingsMenu::RebuildRows() {
    rows_.clear();
    if (game_style_) {
        // the game page: every section in one list, as the game's Settings screen lists its own
        for (const auto c : kOrder) {
            rows_.push_back({.kind = Row::Kind::Header, .cat = c, .section = true,
                             .en = Settings::CategoryLabel(c, false), .ru = Settings::CategoryLabel(c, true)});
            AppendCategory(c);
        }
    } else {
        AppendCategory(category_);
    }
    if (focus_ >= rows_.size()) {
        focus_ = rows_.size() - 1;
    }
}

void SettingsMenu::AppendCategory(Category category_) {
    const size_t first = rows_.size();
    const auto info = [this](std::string_view en, std::string_view ru, std::string tag) {
        rows_.push_back({.kind = Row::Kind::Info, .en = en, .ru = ru, .tag = std::move(tag)});
    };
    if (category_ == Category::Graphics) {
        info("Graphics backend", "Графический бэкенд", "info.backend");
    }
    if (category_ == Category::Gamepad) {
        info("Active input device", "Активное устройство ввода", "info.device");
    }
    if (category_ == Category::Keyboard) {
        for (const auto& g : kGroups) {
            rows_.push_back({.kind = Row::Kind::Header, .en = g.en, .ru = g.ru});
            for (const auto a : g.actions) {
                rows_.push_back({.kind = Row::Kind::Setting, .desc = BindingDesc(a)});
            }
        }
        info("SecondSonPC hotkeys", "Горячие клавиши SecondSonPC", "info.hotkeys");
    } else {
        for (const auto& d : Settings::All()) {
            if (d.category != category_ || !d.own_row) {
                continue;
            }
            rows_.push_back({.kind = Row::Kind::Setting, .desc = &d});
            if (d.id == "display.fullscreenMode") {
                rows_.push_back({.kind = Row::Kind::WindowSize, .en = "Window size", .ru = "Размер окна",
                                 .tag = "window.size"});
            }
        }
    }
    if (category_ == Category::Display) {
        info("Game render resolution", "Разрешение рендеринга игры", "info.render");
    }
    if (category_ == Category::Shaders) {
        info("Cache size", "Размер кэша", "info.cache.size");
        info("In the cache", "В кэше", "info.cache.content");
        info("This run", "В этом запуске", "info.cache.run");
        rows_.push_back({.kind = Row::Kind::Command, .en = "Clear the shader cache", .ru = "Очистить кэш шейдеров",
                         .tag = "cmd.clear_shaders"});
        rows_.push_back({.kind = Row::Kind::Command, .en = "Clear the Vulkan pipeline cache",
                         .ru = "Очистить Vulkan pipeline cache", .tag = "cmd.clear_pipelines"});
    }
    rows_.push_back({.kind = Row::Kind::ResetCategory, .en = "Reset this section", .ru = "Сбросить раздел",
                     .tag = "reset.category"});
    for (size_t i = first; i < rows_.size(); ++i) {
        rows_[i].cat = category_;
    }
}

bool SettingsMenu::Focusable(size_t row) const {
    return row < rows_.size() && rows_[row].kind != Row::Kind::Header;
}

void SettingsMenu::MoveFocus(int delta) {
    if (rows_.empty()) {
        return;
    }
    const int dir = delta < 0 ? -1 : 1;
    int steps = std::abs(delta);
    int at = static_cast<int>(focus_);
    if (steps == 0) {
        // settle on a focusable row from here
        while (at < static_cast<int>(rows_.size()) && !Focusable(at)) {
            ++at;
        }
        focus_ = static_cast<size_t>(std::min(at, static_cast<int>(rows_.size()) - 1));
        return;
    }
    int last_ok = at;
    while (steps > 0) {
        at += dir;
        if (at < 0 || at >= static_cast<int>(rows_.size())) {
            break;
        }
        if (Focusable(at)) {
            last_ok = at;
            --steps;
        }
    }
    if (static_cast<size_t>(last_ok) != focus_) {
        focus_ = static_cast<size_t>(last_ok);
        slot_ = 0;
    }
}

std::string SettingsMenu::TakeCommand() {
    return std::exchange(command_, {});
}

std::string SettingsMenu::FocusedId() const {
    if (focus_ >= rows_.size()) {
        return {};
    }
    const auto& r = rows_[focus_];
    return r.kind == Row::Kind::Setting && r.desc != nullptr ? std::string(r.desc->id) : r.tag;
}

bool SettingsMenu::Focus(std::string_view id) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        const auto& r = rows_[i];
        if ((r.kind == Row::Kind::Setting && r.desc != nullptr && r.desc->id == id) || (!r.tag.empty() && r.tag == id)) {
            focus_ = i;
            slot_ = 0;
            return true;
        }
    }
    return false;
}

void SettingsMenu::BeginDisplayChange() {
    if (modal_ == Modal::ConfirmDisplay) {
        return;
    }
    pending_display_.clear();
    for (const auto id : kDisplayIds) {
        if (const Desc* d = Settings::Find(id)) {
            pending_display_.emplace_back(d, service_.Get(*d));
        }
    }
}

void SettingsMenu::EndDisplayChange(bool changed) {
    if (!changed || modal_ == Modal::ConfirmDisplay) {
        return;
    }
    display_before_ = pending_display_;
    modal_ = Modal::ConfirmDisplay;
    modal_button_ = 0;
    modal_deadline_ns_ = env_.now_ns + kConfirmNs;
}

void SettingsMenu::RevertDisplay() {
    if (!display_before_.empty()) {
        service_.SetMany(display_before_);
    }
    display_before_.clear();
    modal_ = Modal::None;
    status_ = std::string(T("Display settings restored.", "Параметры экрана возвращены."));
}

void SettingsMenu::SetValue(const Desc& desc, const Value& value) {
    const auto avail = service_.Available(desc);
    if (!avail.available) {
        status_ = std::string(T(avail.why_en, avail.why_ru));
        return;
    }
    const bool display = IsDisplayId(desc.id);
    if (display) {
        BeginDisplayChange();
    }
    std::string error;
    const auto result = service_.Set(desc, value, &error);
    if (display) {
        EndDisplayChange(result == Settings::SetResult::Ok);
    }
    if (result == Settings::SetResult::Invalid || result == Settings::SetResult::Unavailable) {
        status_ = error;
    } else if (result == Settings::SetResult::Ok && desc.apply == Settings::Apply::Restart) {
        status_ = std::string(T("Takes effect after a restart.", "Вступит в силу после перезапуска."));
    }
}

void SettingsMenu::SetWindowSize(u32 w, u32 h) {
    const Desc* dw = Settings::Find("display.width");
    const Desc* dh = Settings::Find("display.height");
    if (dw == nullptr || dh == nullptr || !service_.Available(*dw).available) {
        return;
    }
    BeginDisplayChange();
    const std::pair<const Desc*, Value> changes[] = {{dw, Value{static_cast<s64>(w)}}, {dh, Value{static_cast<s64>(h)}}};
    std::string error;
    const auto result = service_.SetMany(changes, &error);
    EndDisplayChange(result == Settings::SetResult::Ok);
    if (result == Settings::SetResult::Invalid) {
        status_ = error;
    }
}

void SettingsMenu::StepWindowSize(int dir) {
    auto sizes = env_.window_sizes;
    const auto cfg = service_.Current();
    const std::pair<u32, u32> now{cfg.window_width, cfg.window_height};
    if (std::ranges::find(sizes, now) == sizes.end()) {
        sizes.push_back(now);
        std::ranges::sort(sizes);
    }
    const auto it = std::ranges::find(sizes, now);
    const auto idx = static_cast<int>(it - sizes.begin()) + dir;
    if (idx < 0 || idx >= static_cast<int>(sizes.size())) {
        return;
    }
    SetWindowSize(sizes[idx].first, sizes[idx].second);
}

std::vector<std::string> SettingsMenu::TextOptions(const Desc& desc) const {
    std::vector<std::string> options{""};
    const auto& extra = desc.id == "display.monitor" ? env_.displays : env_.audio_devices;
    options.insert(options.end(), extra.begin(), extra.end());
    const auto current = std::get<std::string>(service_.Get(desc));
    if (std::ranges::find(options, current) == options.end()) {
        options.push_back(current); // a device that is gone now stays selectable
    }
    return options;
}

void SettingsMenu::StepText(const Desc& desc, int dir) {
    const auto options = TextOptions(desc);
    const auto current = std::get<std::string>(service_.Get(desc));
    const auto it = std::ranges::find(options, current);
    const int n = static_cast<int>(options.size());
    const int idx = ((static_cast<int>(it - options.begin()) + dir) % n + n) % n;
    SetValue(desc, Value{options[idx]});
}

void SettingsMenu::Adjust(int dir) {
    SyncCategory();
    if (focus_ >= rows_.size()) {
        return;
    }
    const auto& row = rows_[focus_];
    if (row.kind == Row::Kind::WindowSize) {
        StepWindowSize(dir);
        return;
    }
    if (row.kind != Row::Kind::Setting || row.desc == nullptr) {
        return;
    }
    const Desc& d = *row.desc;
    if (d.type == Type::Binding) {
        slot_ = std::clamp(slot_ + dir, 0, 1);
        return;
    }
    if (!service_.Available(d).available) {
        const auto a = service_.Available(d);
        status_ = std::string(T(a.why_en, a.why_ru));
        return;
    }
    const Value v = service_.Get(d);
    switch (d.type) {
    case Type::Bool:
        SetValue(d, Value{dir > 0});
        break;
    case Type::Int: {
        const double x = std::clamp(static_cast<double>(std::get<s64>(v)) + dir * d.step, d.ui_min, d.ui_max);
        SetValue(d, Value{static_cast<s64>(std::lround(x))});
        break;
    }
    case Type::Float: {
        double x = std::get<double>(v) + dir * d.step;
        x = std::round(std::round(x / d.step) * d.step * 1e6) / 1e6; // 0.35, not 0.35000000000000003
        SetValue(d, Value{std::clamp(x, d.ui_min, d.ui_max)});
        break;
    }
    case Type::Choice: {
        const auto& cur = std::get<std::string>(v);
        const int n = static_cast<int>(d.choices.size());
        int idx = 0;
        for (int i = 0; i < n; ++i) {
            if (d.choices[i].value == cur) {
                idx = i;
            }
        }
        idx = ((idx + dir) % n + n) % n;
        SetValue(d, Value{std::string(d.choices[idx].value)});
        break;
    }
    case Type::Text:
        StepText(d, dir);
        break;
    case Type::Binding:
        break;
    }
}

void SettingsMenu::Activate() {
    SyncCategory();
    if (focus_ >= rows_.size()) {
        return;
    }
    const auto& row = rows_[focus_];
    switch (row.kind) {
    case Row::Kind::WindowSize:
        StepWindowSize(1);
        return;
    case Row::Kind::ResetCategory:
        modal_ = Modal::ConfirmReset;
        modal_button_ = 1; // "cancel" first: a reset is not a single key away
        return;
    case Row::Kind::Header:
    case Row::Kind::Info:
        return;
    case Row::Kind::Command:
        if (armed_ == row.tag && env_.now_ns - armed_ns_ < kConfirmCommandNs) {
            command_ = row.tag;
            armed_.clear();
            status_ = std::string(T("Done: the cache is cleared at the next start.",
                                    "Готово: кэш будет очищен при следующем запуске."));
        } else {
            armed_ = row.tag;
            armed_ns_ = env_.now_ns;
            status_ = std::string(T("Press once more to confirm.", "Нажмите ещё раз для подтверждения."));
        }
        return;
    case Row::Kind::Setting:
        break;
    }
    if (row.desc == nullptr) {
        return;
    }
    const Desc& d = *row.desc;
    if (d.type == Type::Binding) {
        capture_ = Capture{static_cast<Input::Action>(d.action), static_cast<size_t>(slot_)};
        status_.clear();
        return;
    }
    if (d.type == Type::Bool) {
        if (service_.Available(d).available) {
            SetValue(d, Value{!std::get<bool>(service_.Get(d))});
        } else {
            Adjust(1); // reports why
        }
        return;
    }
    if (d.type == Type::Choice || d.type == Type::Text) {
        Adjust(1);
    }
}

void SettingsMenu::ResetFocused() {
    SyncCategory();
    if (focus_ >= rows_.size()) {
        return;
    }
    const auto& row = rows_[focus_];
    if (row.kind == Row::Kind::WindowSize) {
        const Desc* dw = Settings::Find("display.width");
        const Desc* dh = Settings::Find("display.height");
        if (dw != nullptr && dh != nullptr) {
            SetWindowSize(static_cast<u32>(std::get<s64>(service_.Default(*dw))),
                          static_cast<u32>(std::get<s64>(service_.Default(*dh))));
        }
        return;
    }
    if (row.kind == Row::Kind::Setting && row.desc != nullptr && !row.desc->table.empty()) {
        SetValue(*row.desc, service_.Default(*row.desc));
    }
}

void SettingsMenu::ModalAccept(int button) {
    if (modal_ == Modal::ConfirmDisplay) {
        if (button == 0) {
            modal_ = Modal::None;
            display_before_.clear();
            status_ = std::string(T("Display settings kept.", "Параметры экрана сохранены."));
        } else {
            RevertDisplay();
        }
    } else if (modal_ == Modal::ConfirmReset) {
        modal_ = Modal::None;
        if (button == 0) {
            const bool display = category_ == Category::Display;
            if (display) {
                BeginDisplayChange();
            }
            const auto before = service_.CurrentOverrides().size();
            service_.ResetCategory(category_);
            if (display) {
                EndDisplayChange(service_.CurrentOverrides().size() != before);
            }
            status_ = fmt::format("{} «{}»", T("Reset:", "Сброшено:"), Settings::CategoryLabel(category_, env_.ru));
        }
    }
}

void SettingsMenu::OnNav(Nav nav) {
    if (!open_) {
        return;
    }
    if (modal_ != Modal::None) {
        switch (nav) {
        case Nav::Left:
        case Nav::Up:
            modal_button_ = 0;
            break;
        case Nav::Right:
        case Nav::Down:
            modal_button_ = 1;
            break;
        case Nav::Accept:
            ModalAccept(modal_button_);
            break;
        case Nav::Back:
            ModalAccept(1);
            break;
        default:
            break;
        }
        return;
    }
    if (capture_) {
        if (nav == Nav::Back) {
            capture_.reset();
            status_ = std::string(T("Not changed.", "Без изменений."));
        }
        return;
    }
    switch (nav) {
    case Nav::Up:
        MoveFocus(-1);
        break;
    case Nav::Down:
        MoveFocus(1);
        break;
    case Nav::PageUp:
        MoveFocus(-std::max(1, visible_rows_ - 1));
        break;
    case Nav::PageDown:
        MoveFocus(std::max(1, visible_rows_ - 1));
        break;
    case Nav::Left:
        Adjust(-1);
        break;
    case Nav::Right:
        Adjust(1);
        break;
    case Nav::Accept:
        Activate();
        break;
    case Nav::Back:
        close_request_ = true;
        break;
    case Nav::PrevTab:
    case Nav::NextTab: {
        const auto n = static_cast<int>(std::size(kOrder));
        int idx = 0;
        for (int i = 0; i < n; ++i) {
            if (kOrder[i] == category_) {
                idx = i;
            }
        }
        idx = ((idx + (nav == Nav::NextTab ? 1 : -1)) % n + n) % n;
        if (game_style_) {
            // the game page: to the next / previous section's first item
            SyncCategory();
            for (int i = 0; i < n; ++i) {
                if (kOrder[i] == category_) {
                    idx = ((i + (nav == Nav::NextTab ? 1 : -1)) % n + n) % n;
                }
            }
            for (size_t r = 0; r < rows_.size(); ++r) {
                if (rows_[r].section && rows_[r].cat == kOrder[idx]) {
                    focus_ = r;
                    MoveFocus(0);
                    slot_ = 0;
                    break;
                }
            }
            break;
        }
        SelectCategory(kOrder[idx]);
        break;
    }
    case Nav::Reset:
        ResetFocused();
        break;
    case Nav::Clear:
        if (focus_ < rows_.size() && rows_[focus_].desc != nullptr && rows_[focus_].desc->type == Type::Binding) {
            const Desc& d = *rows_[focus_].desc;
            auto input = service_.Current().input;
            Input::ClearBinding(input, static_cast<Input::Action>(d.action), static_cast<size_t>(slot_));
            SetValue(d, Value{input.bindings[static_cast<size_t>(d.action)]});
        }
        break;
    }
}

void SettingsMenu::OnCaptureInput(Input::InputCode input) {
    if (!capture_) {
        return;
    }
    const auto [action, slot] = *capture_;
    capture_.reset();
    if (Input::IsReservedInput(input)) {
        status_ = fmt::format("{}: {}", BindingText(input),
                              T("a SecondSonPC hotkey (F10 FPS, F11 full screen) - not assigned",
                                "горячая клавиша SecondSonPC (F10 FPS, F11 полный экран) — не назначена"));
        return;
    }
    auto settings = service_.Current().input;
    Input::AssignBinding(settings, action, slot, input);
    const Desc* d = BindingDesc(action);
    if (d == nullptr) {
        return;
    }
    SetValue(*d, Value{settings.bindings[static_cast<size_t>(action)]});
    std::string others;
    for (const auto other : Input::ActionsUsing(settings, input)) {
        if (other != action && !Input::MayShareInput(other, action)) {
            others += fmt::format("{}«{}»", others.empty() ? "" : ", ", Settings::ActionLabel(other, env_.ru));
        }
    }
    status_ = others.empty()
                  ? fmt::format("{} — {}", Settings::ActionLabel(action, env_.ru), BindingText(input))
                  : fmt::format("{} {} {}", BindingText(input), T("is also assigned to", "также назначена на"), others);
}

void SettingsMenu::Tick(u64 now_ns) {
    env_.now_ns = now_ns;
    if (modal_ == Modal::ConfirmDisplay && now_ns >= modal_deadline_ns_) {
        RevertDisplay();
    }
}

bool SettingsMenu::NeedsTicks() const {
    return open_ && modal_ == Modal::ConfirmDisplay;
}

void SettingsMenu::OnMouseMove(float x, float y) {
    hover_row_ = -1;
    for (const auto& h : hits_) {
        if (h.kind == Hit::Kind::Row && h.rect.Contains(x, y)) {
            hover_row_ = h.index;
        }
    }
    // the game page: the item under the pointer is the chosen one (as the game's own menus with a cursor)
    if (game_style_ && hover_row_ >= 0 && modal_ == Modal::None && !capture_ && dragging_row_ < 0 &&
        Focusable(static_cast<size_t>(hover_row_)) && focus_ != static_cast<size_t>(hover_row_)) {
        focus_ = static_cast<size_t>(hover_row_);
        slot_ = 0;
    }
    if (dragging_row_ >= 0) {
        SetSliderFromX(static_cast<size_t>(dragging_row_), x);
    }
}

void SettingsMenu::SetSliderFromX(size_t row, float x) {
    if (row >= rows_.size() || rows_[row].desc == nullptr) {
        return;
    }
    const Desc& d = *rows_[row].desc;
    for (const auto& h : hits_) {
        if (h.kind == Hit::Kind::Row && h.index == static_cast<int>(row) && h.part == Hit::Part::Track) {
            const double f = std::clamp((x - h.rect.x) / std::max(1.0f, h.rect.w), 0.0f, 1.0f);
            double v = d.ui_min + f * (d.ui_max - d.ui_min);
            v = std::round(std::round(v / d.step) * d.step * 1e6) / 1e6;
            v = std::clamp(v, d.ui_min, d.ui_max);
            if (d.type == Type::Int) {
                SetValue(d, Value{static_cast<s64>(std::lround(v))});
            } else {
                SetValue(d, Value{v});
            }
            return;
        }
    }
}

void SettingsMenu::OnMouseButton(float x, float y, bool down) {
    if (!down) {
        dragging_row_ = -1;
        return;
    }
    const Hit* hit = nullptr;
    for (const auto& h : hits_) {
        if (h.rect.Contains(x, y)) {
            hit = &h; // the last one drawn is on top
        }
    }
    if (hit == nullptr) {
        return;
    }
    if (modal_ != Modal::None) {
        if (hit->kind == Hit::Kind::ModalButton) {
            ModalAccept(hit->index);
        }
        return;
    }
    if (capture_) {
        return; // the host sends clicks as inputs to bind while capturing
    }
    switch (hit->kind) {
    case Hit::Kind::Close:
        close_request_ = true;
        return;
    case Hit::Kind::Category:
        SelectCategory(kOrder[hit->index]);
        return;
    case Hit::Kind::ModalButton:
        return;
    case Hit::Kind::Row:
        break;
    }
    if (!Focusable(static_cast<size_t>(hit->index))) {
        return;
    }
    if (focus_ != static_cast<size_t>(hit->index)) {
        focus_ = static_cast<size_t>(hit->index);
        slot_ = 0;
    }
    switch (hit->part) {
    case Hit::Part::Prev:
        Adjust(-1);
        break;
    case Hit::Part::Next:
        Adjust(1);
        break;
    case Hit::Part::Value:
    case Hit::Part::Button:
        Activate();
        break;
    case Hit::Part::Track:
        dragging_row_ = hit->index;
        SetSliderFromX(focus_, x);
        break;
    case Hit::Part::Slot0:
    case Hit::Part::Slot1:
        slot_ = hit->part == Hit::Part::Slot0 ? 0 : 1;
        Activate();
        break;
    case Hit::Part::Body:
        break;
    }
}

void SettingsMenu::OnWheel(float notches) {
    if (modal_ != Modal::None || notches == 0.0f) {
        return;
    }
    if (game_style_) {
        MoveFocus(notches > 0 ? -1 : 1); // as the game's lists: a notch is a step up / down
        return;
    }
    scroll_ -= static_cast<int>(std::lround(notches * 3.0f));
    scroll_ = std::max(0, scroll_);
    // keep the focus in view so the next Build does not scroll back
    if (static_cast<int>(focus_) < scroll_) {
        focus_ = static_cast<size_t>(scroll_);
        MoveFocus(0);
    } else if (static_cast<int>(focus_) >= scroll_ + visible_rows_) {
        focus_ = static_cast<size_t>(std::max(0, scroll_ + visible_rows_ - 1));
        while (focus_ > 0 && !Focusable(focus_)) {
            --focus_;
        }
    }
}

std::string SettingsMenu::BindingText(const Input::InputCode& input) const {
    std::string name = Input::InputName(input);
    if (!env_.ru) {
        return name;
    }
    static constexpr std::pair<std::string_view, std::string_view> kRu[] = {
        {"Mouse Left", "ЛКМ"},           {"Mouse Right", "ПКМ"},           {"Mouse Middle", "СКМ"},
        {"Mouse X1", "Мышь X1"},         {"Mouse X2", "Мышь X2"},          {"Wheel Up", "Колесо вверх"},
        {"Wheel Down", "Колесо вниз"},   {"Wheel Left", "Колесо влево"},   {"Wheel Right", "Колесо вправо"},
        {"Space", "Пробел"},             {"Left Shift", "Левый Shift"},    {"Right Shift", "Правый Shift"},
        {"Left Ctrl", "Левый Ctrl"},     {"Right Ctrl", "Правый Ctrl"},    {"Left Alt", "Левый Alt"},
        {"Right Alt", "Правый Alt"},     {"Return", "Enter"},              {"Escape", "Esc"},
        {"Up", "↑"},                     {"Down", "↓"},                    {"Left", "←"},
        {"Right", "→"},
    };
    for (const auto& [en, ru] : kRu) {
        if (name == en) {
            return std::string(ru);
        }
    }
    return name;
}

std::string SettingsMenu::ValueText(const Desc& desc, const Value& value) const {
    switch (desc.type) {
    case Type::Bool:
        return std::string(std::get<bool>(value) ? T("On", "Вкл.") : T("Off", "Выкл."));
    case Type::Int: {
        const auto x = std::get<s64>(value);
        return desc.unit.empty() ? std::to_string(x) : fmt::format("{} {}", x, desc.unit);
    }
    case Type::Float: {
        std::string s = fmt::format("{:.{}f}", std::get<double>(value), desc.step < 0.1 ? 2 : 1);
        if (env_.ru) {
            s = Replace(s, ".", ",");
        }
        return desc.unit == "x" ? s + "×" : s;
    }
    case Type::Choice: {
        const auto label = Settings::ChoiceLabel(desc, std::get<std::string>(value), env_.ru);
        return label.empty() ? std::get<std::string>(value) : std::string(label);
    }
    case Type::Text: {
        const auto& s = std::get<std::string>(value);
        if (!s.empty()) {
            return s;
        }
        return std::string(desc.id == "display.monitor" ? T("Primary display", "Основной монитор")
                                                        : T("System default", "По умолчанию"));
    }
    case Type::Binding:
        break;
    }
    return {};
}

void SettingsMenu::Build(TextMeasure& measure, DrawList& out) {
    const float W = env_.width, H = env_.height;
    out.Reset(W, H);
    hits_.clear();
    if (!open_) {
        return;
    }
    if (game_style_) {
        BuildGame(measure, out);
        return;
    }
    const auto cfg = service_.Current();
    float s = H / 1080.0f * static_cast<float>(cfg.pc.ui_scale) / 100.0f;
    s = std::clamp(s, 0.5f, 4.0f);
    // the panel fits the window whatever the scale
    s = std::min({s, (W - 16.0f) / 1360.0f * 1.0f / 0.94f, (H - 16.0f) / 860.0f / 0.92f});
    s = std::max(s, 0.45f);
    const float pw = std::min(1360.0f * s, W - 16.0f), ph = std::min(860.0f * s, H - 16.0f);
    const Rect panel{std::floor((W - pw) / 2), std::floor((H - ph) / 2), std::floor(pw), std::floor(ph)};
    out.SetSurface(panel);
    out.SetDim(150);
    out.FillRect(panel, kPanel);
    out.StrokeRect(panel, kFrame, std::max(1.0f, 1.5f * s));
    out.FillRect({panel.x - 3 * s, panel.y - 3 * s, 7 * s, 7 * s}, kFrame); // the corner mark of the game's panels

    // ---- header
    const float hh = 96.0f * s;
    out.Text({panel.x + 32 * s, panel.y + 6 * s, pw * 0.6f, hh}, Caps(T("PC settings", "ПК-настройки")), 54.0f * s,
             kAccent, Align::Left, FontWeight::Title);
    const Rect close{panel.Right() - 58 * s, panel.y + 18 * s, 40 * s, 40 * s};
    out.FillRect(close, kKeycap, 8 * s);
    out.Line(close.x + 13 * s, close.y + 13 * s, close.Right() - 13 * s, close.Bottom() - 13 * s, kText, 2 * s);
    out.Line(close.x + 13 * s, close.Bottom() - 13 * s, close.Right() - 13 * s, close.y + 13 * s, kText, 2 * s);
    hits_.push_back({.kind = Hit::Kind::Close, .rect = close});
    out.Text({panel.x + pw * 0.45f, panel.y, close.x - 18 * s - (panel.x + pw * 0.45f), hh},
             fmt::format("{}: {}", T("Device", "Устройство"), env_.device_label), 15.0f * s, kText2, Align::Right);
    out.Line(panel.x, panel.y + hh, panel.Right(), panel.y + hh, kLine, std::max(1.0f, s));

    // ---- footer
    const float fh = 60.0f * s;
    const float fy = panel.Bottom() - fh;
    out.Line(panel.x, fy, panel.Right(), fy, kLine, std::max(1.0f, s));

    // ---- sidebar
    const float sw = 330.0f * s;
    const Rect side{panel.x, panel.y + hh, sw, fy - panel.y - hh};
    out.FillRect({side.x + 1, side.y + 1, side.w, side.h - 1}, kSidebar);
    for (size_t i = 0; i < std::size(kOrder); ++i) {
        const Rect item{side.x + 12 * s, side.y + 12 * s + static_cast<float>(i) * 50 * s, sw - 24 * s, 44 * s};
        const bool selected = kOrder[i] == category_;
        if (selected) {
            out.FillRect(item, kRowFocus);
        }
        out.Text({item.x + 18 * s, item.y, item.w - 24 * s, item.h},
                 Caps(Settings::CategoryLabel(kOrder[i], env_.ru)), 21.0f * s, selected ? kAccent : kText,
                 Align::Left, FontWeight::Title);
        hits_.push_back({.kind = Hit::Kind::Category, .rect = item, .index = static_cast<int>(i)});
    }

    // ---- content
    const float cx0 = side.Right(), cw0 = panel.Right() - cx0;
    const float dh = 104.0f * s; // description area
    const float dy = fy - dh;
    const float top = panel.y + hh + 10 * s;
    const float row_h = 54.0f * s;
    visible_rows_ = std::max(1, static_cast<int>((dy - top - 6 * s) / row_h));
    const int nrows = static_cast<int>(rows_.size());
    if (static_cast<int>(focus_) < scroll_) {
        scroll_ = static_cast<int>(focus_);
    }
    if (static_cast<int>(focus_) >= scroll_ + visible_rows_) {
        scroll_ = static_cast<int>(focus_) - visible_rows_ + 1;
    }
    scroll_ = std::clamp(scroll_, 0, std::max(0, nrows - visible_rows_));
    // a header right above the focused row stays visible with it
    if (scroll_ > 0 && rows_[static_cast<size_t>(scroll_) - 1].kind == Row::Kind::Header &&
        static_cast<int>(focus_) == scroll_) {
        --scroll_;
    }

    const auto conflicts = Input::FindConflicts(cfg.input);
    const auto in_conflict = [&conflicts](Input::Action a, const Input::InputCode& in) {
        return std::ranges::any_of(conflicts, [&](const Input::BindingConflict& c) {
            return (c.first == a || c.second == a) && c.input == in;
        });
    };

    const Rect list{cx0, top, cw0, dy - top};
    out.PushClip(list);
    for (int ri = scroll_; ri < std::min(nrows, scroll_ + visible_rows_); ++ri) {
        const auto& row = rows_[static_cast<size_t>(ri)];
        const float y = top + static_cast<float>(ri - scroll_) * row_h;
        const Rect rr{cx0 + 16 * s, y, cw0 - 32 * s, row_h - 4 * s};
        const float cy = rr.y + rr.h / 2;
        if (row.kind == Row::Kind::Header) {
            out.Text({rr.x + 12 * s, rr.y + 10 * s, rr.w, rr.h - 10 * s}, Caps(T(row.en, row.ru)), 22.0f * s, kText,
                     Align::Left, FontWeight::Title);
            continue;
        }
        const bool focused = static_cast<size_t>(ri) == focus_;
        if (focused) {
            out.FillRect(rr, kRowFocus);
        } else if (ri == hover_row_) {
            out.FillRect(rr, kRowHover);
        }
        hits_.push_back({.kind = Hit::Kind::Row, .rect = rr, .index = ri, .part = Hit::Part::Body});

        const float ctrl_w = std::min(440.0f * s, rr.w * 0.46f);
        const float ctrl_x = rr.Right() - ctrl_w - 14 * s;
        const Rect ctrl{ctrl_x, rr.y, ctrl_w, rr.h};
        float label_right = ctrl_x - 12 * s;

        const Desc* d = row.desc;
        const bool available = d == nullptr || service_.Available(*d).available;
        bool dimmed = !available;
        if (d != nullptr && !d->requires_on.empty()) {
            if (const Desc* dep = Settings::Find(d->requires_on)) {
                const Value dv = service_.Get(*dep);
                dimmed = dimmed || (std::holds_alternative<bool>(dv) && !std::get<bool>(dv));
            }
        }
        // restart tag and the "changed" dot
        if (d != nullptr && service_.NeedsRestart(*d)) {
            const auto tag = T("after restart", "после перезапуска");
            const float tw = measure.Width(tag, 13.0f * s, FontWeight::Semibold) + 16 * s;
            const Rect tr{label_right - tw, cy - 12 * s, tw, 24 * s};
            out.StrokeRect(tr, kAmber, std::max(1.0f, s), 12 * s);
            out.Text(tr, tag, 13.0f * s, kAmber, Align::Center, FontWeight::Semibold);
            label_right = tr.x - 8 * s;
        }
        if (d != nullptr && service_.IsOverridden(*d)) {
            out.FillCircle(rr.x + 14 * s, cy, 3.5f * s, kAccent);
        }
        std::string_view label;
        if (d != nullptr) {
            label = env_.ru ? d->ru : d->en;
        } else if (row.kind == Row::Kind::ResetCategory) {
            label = T(row.en, row.ru);
        } else {
            label = T(row.en, row.ru);
        }
        std::string label_text(label);
        if (row.kind == Row::Kind::ResetCategory) {
            label_text = fmt::format("{} «{}»", label, Settings::CategoryLabel(category_, env_.ru));
        }
        out.Text({rr.x + 26 * s, rr.y, label_right - (rr.x + 26 * s), rr.h}, label_text, 21.0f * s,
                 dimmed ? kDisabled : focused ? kAccent : kText);

        const auto arrows = [&](std::string_view value_text, bool enabled) {
            const Color c = enabled ? kText : kDisabled;
            const float a = 6 * s;
            out.FillTriangle(ctrl.x + 14 * s, cy, ctrl.x + 14 * s + a * 1.4f, cy - a, ctrl.x + 14 * s + a * 1.4f,
                             cy + a, enabled ? kText2 : kDisabled);
            out.FillTriangle(ctrl.Right() - 14 * s, cy, ctrl.Right() - 14 * s - a * 1.4f, cy - a,
                             ctrl.Right() - 14 * s - a * 1.4f, cy + a, enabled ? kText2 : kDisabled);
            out.Text({ctrl.x + 36 * s, rr.y, ctrl.w - 72 * s, rr.h}, value_text, 17.0f * s, c, Align::Center);
            hits_.push_back({.kind = Hit::Kind::Row, .rect = {ctrl.x, rr.y, 40 * s, rr.h}, .index = ri,
                             .part = Hit::Part::Prev});
            hits_.push_back({.kind = Hit::Kind::Row, .rect = {ctrl.Right() - 40 * s, rr.y, 40 * s, rr.h},
                             .index = ri, .part = Hit::Part::Next});
            hits_.push_back({.kind = Hit::Kind::Row, .rect = {ctrl.x + 40 * s, rr.y, ctrl.w - 80 * s, rr.h},
                             .index = ri, .part = Hit::Part::Value});
        };

        switch (row.kind) {
        case Row::Kind::Header:
            break;
        case Row::Kind::Info: {
            std::string text;
            if (row.tag == "info.backend") {
                text = env_.backend_label;
            } else if (row.tag == "info.device") {
                text = env_.device_label;
            } else if (row.tag == "info.hotkeys") {
                text = std::string(T("Esc: Settings · F10 FPS · F11 full screen",
                                     "Esc → «Настройки» · F10 FPS · F11 полный экран"));
            } else if (row.tag == "info.render") {
                text = std::string(T("1920 × 1080 (the window scales the frame)", "1920 × 1080 (окно масштабирует кадр)"));
            } else if (row.tag == "info.cache.size") {
                text = env_.shader_cache_size;
            } else if (row.tag == "info.cache.content") {
                text = env_.shader_cache_content;
            } else if (row.tag == "info.cache.run") {
                text = env_.shader_cache_run;
            }
            out.Text(ctrl, text, 16.0f * s, kText2, Align::Right);
            break;
        }
        case Row::Kind::Command: {
            const Rect b{ctrl.x + ctrl.w * 0.35f, rr.y + 8 * s, ctrl.w * 0.65f, rr.h - 16 * s};
            const bool armed = armed_ == row.tag && env_.now_ns - armed_ns_ < kConfirmCommandNs;
            out.StrokeRect(b, focused || armed ? kAccent : kKeycapLine, std::max(1.0f, 1.5f * s), 8 * s);
            out.Text(b, armed ? T("Confirm", "Подтвердить") : T("Clear", "Очистить"), 16.0f * s, kText, Align::Center,
                     FontWeight::Semibold);
            hits_.push_back({.kind = Hit::Kind::Row, .rect = b, .index = ri, .part = Hit::Part::Button});
            break;
        }
        case Row::Kind::ResetCategory: {
            const Rect b{ctrl.x + ctrl.w * 0.35f, rr.y + 8 * s, ctrl.w * 0.65f, rr.h - 16 * s};
            out.StrokeRect(b, focused ? kAccent : kKeycapLine, std::max(1.0f, 1.5f * s), 8 * s);
            out.Text(b, T("Reset", "Сбросить"), 16.0f * s, kText, Align::Center, FontWeight::Semibold);
            hits_.push_back({.kind = Hit::Kind::Row, .rect = b, .index = ri, .part = Hit::Part::Button});
            break;
        }
        case Row::Kind::WindowSize: {
            const bool ok = service_.Available(*Settings::Find("display.width")).available;
            arrows(fmt::format("{} × {}", cfg.window_width, cfg.window_height), ok);
            break;
        }
        case Row::Kind::Setting: {
            if (d == nullptr) {
                break;
            }
            if (!available) {
                const auto tag = T("Unavailable", "Недоступно");
                const float tw = measure.Width(tag, 14.0f * s, FontWeight::Semibold) + 22 * s;
                const Rect tr{ctrl.Right() - tw, cy - 14 * s, tw, 28 * s};
                out.FillRect(tr, kKeycap, 14 * s);
                out.Text(tr, tag, 14.0f * s, kDisabled, Align::Center, FontWeight::Semibold);
                hits_.push_back({.kind = Hit::Kind::Row, .rect = ctrl, .index = ri, .part = Hit::Part::Value});
                break;
            }
            const Value v = service_.Get(*d);
            switch (d->type) {
            case Type::Bool: {
                const bool on = std::get<bool>(v);
                const Rect pill{ctrl.Right() - 60 * s, cy - 15 * s, 60 * s, 30 * s};
                out.FillRect(pill, on ? kAccent : kTrackOff, 15 * s);
                out.FillCircle(on ? pill.Right() - 15 * s : pill.x + 15 * s, cy, 11 * s, kWhite);
                out.Text({ctrl.x, rr.y, ctrl.w - 72 * s, rr.h}, ValueText(*d, v), 16.0f * s,
                         dimmed ? kDisabled : kText2, Align::Right);
                hits_.push_back({.kind = Hit::Kind::Row, .rect = ctrl, .index = ri, .part = Hit::Part::Value});
                break;
            }
            case Type::Int:
            case Type::Float: {
                const double x = std::holds_alternative<s64>(v) ? static_cast<double>(std::get<s64>(v))
                                                                : std::get<double>(v);
                const double f = std::clamp((x - d->ui_min) / std::max(1e-9, d->ui_max - d->ui_min), 0.0, 1.0);
                const float tx = ctrl.x + 8 * s, tw = ctrl.w - 118 * s;
                out.FillRect({tx, cy - 3 * s, tw, 6 * s}, kTrackOff, 3 * s);
                out.FillRect({tx, cy - 3 * s, static_cast<float>(tw * f), 6 * s}, dimmed ? kDisabled : kAccent,
                             3 * s);
                out.FillCircle(tx + static_cast<float>(tw * f), cy, 10 * s, kWhite);
                out.Text({ctrl.Right() - 100 * s, rr.y, 100 * s, rr.h}, ValueText(*d, v), 17.0f * s,
                         dimmed ? kDisabled : kText, Align::Right);
                hits_.push_back({.kind = Hit::Kind::Row, .rect = {tx, rr.y, tw, rr.h}, .index = ri,
                                 .part = Hit::Part::Track});
                break;
            }
            case Type::Choice:
            case Type::Text:
                arrows(ValueText(*d, v), !dimmed);
                break;
            case Type::Binding: {
                const auto& b = std::get<Settings::Bindings>(v);
                const auto action = static_cast<Input::Action>(d->action);
                const float slot_w = (ctrl.w - 12 * s) / 2;
                for (int i = 0; i < 2; ++i) {
                    const Rect sr{ctrl.x + static_cast<float>(i) * (slot_w + 12 * s), rr.y + 7 * s, slot_w,
                                  rr.h - 14 * s};
                    const bool capturing = capture_ && capture_->action == action &&
                                           capture_->slot == static_cast<size_t>(i);
                    const bool has = static_cast<size_t>(i) < b.size();
                    const bool conflict = has && in_conflict(action, b[static_cast<size_t>(i)]);
                    out.FillRect(sr, capturing ? kRowFocus : kKeycap, 6 * s);
                    const Color line = capturing                       ? kAccent
                                       : conflict                      ? kWarn
                                       : (focused && slot_ == i)       ? kAccent
                                                                       : kKeycapLine;
                    out.StrokeRect(sr, line, (focused && slot_ == i) || capturing || conflict ? 2 * s : s, 6 * s);
                    std::string text = capturing ? std::string(T("Press a key…", "Нажмите клавишу…"))
                                       : has      ? BindingText(b[static_cast<size_t>(i)])
                                                  : std::string("—");
                    if (has && i == 1 && b.size() > 2) {
                        text += fmt::format(" +{}", b.size() - 2);
                    }
                    out.Text(sr, text, 16.0f * s, has || capturing ? kText : kDisabled, Align::Center);
                    hits_.push_back({.kind = Hit::Kind::Row, .rect = sr, .index = ri,
                                     .part = i == 0 ? Hit::Part::Slot0 : Hit::Part::Slot1});
                }
                break;
            }
            }
            break;
        }
        }
    }
    out.PopClip();
    if (nrows > visible_rows_) {
        const float track_h = dy - top - 8 * s;
        const float bar_h = std::max(24 * s, track_h * static_cast<float>(visible_rows_) / static_cast<float>(nrows));
        const float bar_y = top + (track_h - bar_h) * static_cast<float>(scroll_) /
                                      static_cast<float>(std::max(1, nrows - visible_rows_));
        out.FillRect({panel.Right() - 8 * s, bar_y, 4 * s, bar_h}, kKeycapLine, 2 * s);
    }

    // ---- description of the focused row
    out.Line(cx0 + 16 * s, dy, panel.Right() - 16 * s, dy, kLine, std::max(1.0f, s));
    const Rect desc_box{cx0 + 28 * s, dy + 12 * s, cw0 - 56 * s, dh - 20 * s};
    std::string help;
    Color help_color = kText2;
    if (focus_ < rows_.size()) {
        const auto& row = rows_[focus_];
        if (capture_) {
            help = fmt::format("{} «{}». {}", T("Press a key or a mouse button for", "Нажмите клавишу или кнопку мыши для"),
                               Settings::ActionLabel(capture_->action, env_.ru),
                               T("Esc cancels.", "Esc — отмена."));
            help_color = kText;
        } else if (row.desc != nullptr) {
            const auto a = service_.Available(*row.desc);
            if (!a.available) {
                help = fmt::format("{} {}", T("Unavailable.", "Недоступно."), T(a.why_en, a.why_ru));
                help_color = kAmber;
            } else {
                help = std::string(T(row.desc->help_en, row.desc->help_ru));
                if (!row.desc->requires_on.empty()) {
                    if (const Desc* dep = Settings::Find(row.desc->requires_on)) {
                        help += fmt::format("{}{} «{}».", help.empty() ? "" : " ",
                                            T("Matters while on:", "Действует, когда включено"),
                                            env_.ru ? dep->ru : dep->en);
                    }
                }
                if (row.desc->type == Type::Binding) {
                    help += fmt::format("{}{}", help.empty() ? "" : " ",
                                        T("Enter: assign · Backspace: clear the slot · Del: default (the way back to "
                                          "Esc, which cancels an assignment).",
                                          "Enter — назначить · Backspace — очистить ячейку · Del — по умолчанию (так "
                                          "возвращается Esc: при назначении он отменяет)."));
                }
            }
        } else if (row.kind == Row::Kind::WindowSize) {
            help = std::string(T("The window's size when not in full screen. The game renders 1920×1080; the window "
                                 "only scales the finished frame.",
                                 "Размер окна вне полноэкранного режима. Игра рисует 1920×1080; окно только "
                                 "масштабирует готовый кадр."));
        } else if (row.kind == Row::Kind::ResetCategory) {
            help = std::string(T("Every setting of this section back to SecondSonPC.toml's value.",
                                 "Все настройки раздела вернутся к значениям из SecondSonPC.toml."));
        } else if (row.tag == "cmd.clear_shaders") {
            help = std::string(T("The translated shaders, the list of known pipelines and the Vulkan pipeline cache "
                                 "are deleted at the next start: that start and the first visit of every place compile "
                                 "again.",
                                 "Переведённые шейдеры, список известных конвейеров и Vulkan pipeline cache удаляются "
                                 "при следующем запуске: этот запуск и первое посещение каждого места компилируют "
                                 "заново."));
        } else if (row.tag == "cmd.clear_pipelines") {
            help = std::string(T("Only the driver's compiled pipelines (vulkan.bin) are deleted at the next start; the "
                                 "translated shaders and the list of pipelines stay (they are compiled again at the "
                                 "start).",
                                 "При следующем запуске удаляются только скомпилированные драйвером конвейеры "
                                 "(vulkan.bin); переведённые шейдеры и список конвейеров остаются (компилируются "
                                 "заново при запуске)."));
        } else if (row.tag.starts_with("info.cache.")) {
            help = std::string(T("The native backend's persistent shader cache ([ShaderCache] dir).",
                                 "Постоянный кэш шейдеров собственного бэкенда ([ShaderCache] dir)."));
        } else if (row.tag == "info.backend") {
            help = std::string(T("Chosen at start (--gpu-backend). Settings that need the other backend are marked "
                                 "unavailable.",
                                 "Выбирается при запуске (--gpu-backend). Настройки, которым нужен другой бэкенд, "
                                 "отмечены как недоступные."));
        }
    }
    out.Text(desc_box, help, 15.5f * s, help_color, Align::Left, FontWeight::Regular, true);

    // ---- footer: hints, then status / restart note
    struct HintItem {
        Hint hint;
        std::string_view en, ru;
    };
    std::vector<HintItem> hints;
    if (modal_ != Modal::None) {
        hints = {{Hint::Adjust, "Choose", "Выбор"}, {Hint::Accept, "Confirm", "Подтвердить"}, {Hint::Back, "Cancel", "Отмена"}};
    } else if (capture_) {
        hints = {{Hint::Back, "Cancel", "Отмена"}};
    } else {
        hints = {{Hint::Navigate, "Select", "Выбор"}, {Hint::Adjust, "Change", "Изменить"},
                 {Hint::Accept, "Apply", "Выбрать"},  {Hint::Reset, "Default", "По умолчанию"},
                 {Hint::Tabs, "Section", "Раздел"},   {Hint::Back, "Back", "Назад"}};
    }
    float hx = panel.x + 24 * s;
    const float hcy = fy + fh / 2;
    for (const auto& h : hints) {
        hx += DrawGlyph(out, measure, h.hint, env_, hx, hcy, s) + 8 * s;
        const auto text = T(h.en, h.ru);
        const float tw = measure.Width(text, 15.0f * s, FontWeight::Regular);
        out.Text({hx, fy, tw + 4 * s, fh}, text, 15.0f * s, kText2);
        hx += tw + 24 * s;
    }
    std::string note = status_;
    Color note_color = kText2;
    if (note.empty() && !env_.save_note.empty()) {
        note = env_.save_note;
        note_color = kWarn;
    } else if (note.empty() && !conflicts.empty()) {
        note = fmt::format("{}: {}", T("Key conflicts", "Конфликтов назначений"), conflicts.size());
        note_color = kWarn;
    } else if (note.empty() && service_.AnyNeedsRestart()) {
        note = std::string(T("Some changes take effect after a restart.", "Часть изменений вступит в силу после перезапуска."));
        note_color = kAmber;
    }
    if (!note.empty() && hx < panel.Right() - 120 * s) {
        out.Text({hx, fy, panel.Right() - 24 * s - hx, fh}, note, 15.0f * s, note_color, Align::Right);
    }

    // ---- dialogs
    if (modal_ != Modal::None) {
        out.FillRect(panel, kShade);
        const float mw = std::min(660 * s, pw - 40 * s), mh = 250 * s;
        const Rect box{panel.x + (pw - mw) / 2, panel.y + (ph - mh) / 2, mw, mh};
        out.FillRect(box, Color{14, 14, 14, 245});
        out.StrokeRect(box, kFrame, std::max(1.0f, 1.5f * s));
        std::string title, text;
        std::string_view yes, no;
        if (modal_ == Modal::ConfirmDisplay) {
            const u64 left_ns = modal_deadline_ns_ > env_.now_ns ? modal_deadline_ns_ - env_.now_ns : 0;
            const auto left_s = (left_ns + 999'999'999) / 1'000'000'000;
            title = std::string(T("Keep these display settings?", "Сохранить параметры экрана?"));
            text = fmt::format("{} {} {}.", T("If the picture is wrong, do nothing: they are restored in",
                                              "Если изображение неправильное, ничего не нажимайте: параметры вернутся через"),
                               left_s, T("s", "с"));
            yes = T("Keep", "Сохранить");
            no = T("Restore", "Вернуть");
        } else {
            title = fmt::format("{} «{}»?", T("Reset", "Сбросить раздел"), Settings::CategoryLabel(category_, env_.ru));
            text = std::string(T("Every setting of this section goes back to SecondSonPC.toml's value.",
                                 "Все настройки раздела вернутся к значениям из SecondSonPC.toml."));
            yes = T("Reset", "Сбросить");
            no = T("Cancel", "Отмена");
        }
        out.Text({box.x + 28 * s, box.y + 20 * s, box.w - 56 * s, 40 * s}, Caps(title), 26.0f * s, kAccent,
                 Align::Left, FontWeight::Title);
        out.Text({box.x + 28 * s, box.y + 70 * s, box.w - 56 * s, 80 * s}, text, 16.0f * s, kText2, Align::Left,
                 FontWeight::Regular, true);
        const float bw = 170 * s, bh = 46 * s, by = box.Bottom() - bh - 24 * s;
        for (int i = 0; i < 2; ++i) {
            const Rect b{box.Right() - 28 * s - bw * static_cast<float>(2 - i) - 14 * s * static_cast<float>(1 - i),
                         by, bw, bh};
            const bool sel = modal_button_ == i;
            out.FillRect(b, kKeycap);
            out.StrokeRect(b, sel ? kAccent : kKeycapLine, std::max(1.0f, (sel ? 2.0f : 1.0f) * s));
            out.Text(b, i == 0 ? yes : no, 19.0f * s, sel ? kAccent : kText, Align::Center, FontWeight::Semibold);
            hits_.push_back({.kind = Hit::Kind::ModalButton, .rect = b, .index = i});
        }
    }
}


// ---- the game page: "Additional settings" drawn as the game draws its Settings screen
//
// Measured on the game's 1920x1080 frame (its Settings screen, 2026-10-15/16): the panel 315,92 742x895 (a thin white
// frame, a white square at its top-left corner, a white tab over its top-right end, a fine grid over a dark see-through
// fill); the title at x 347 (yellow slab serif capitals, ~66 px); section names at x 387 (white slab capitals, ~34 px);
// items at x 415 and values ending at x 984 (a condensed sans, ~28 px; the chosen item and its value yellow); rows
// 35 px apart, a section's first item 36 px under its name, the next section's name 64 px under its last item; the
// list shows frame y 225..985; volume-like values are ten dashes 26x7 every 30 px from x 690; the scroll bar at x
// 1026..1040 (its thumb grey 79). The game's own screen stays under it: its controller picture and "(O) Back".
void SettingsMenu::BuildGame(TextMeasure& measure, DrawList& out) {
    constexpr Color kYellow{245, 184, 0, 255};
    constexpr Color kWhiteText{240, 240, 240, 255};
    constexpr Color kGreyText{120, 120, 120, 255};
    constexpr Color kDashOff{62, 62, 62, 255};
    const Rect P = env_.game_panel;
    const float k = P.w / 742.0f;
    const auto X = [&](float fx) { return P.x + (fx - 315.0f) * k; };
    const auto Y = [&](float fy) { return P.y + (fy - 92.0f) * k; };
    const auto F = [&](float fx, float fy, float fw, float fh) { return Rect{X(fx), Y(fy), fw * k, fh * k}; };
    const auto cfg = service_.Current();

    out.SetSurface({X(305), Y(80), 790 * k, 920 * k});
    out.SetDim(0);
    // the panel: an opaque copy of the game's (its own list is under it)
    out.FillRect(F(316, 93, 740, 893), Color{40, 40, 41, 250});
    for (float gx = 316; gx < 1056; gx += 8) {
        out.Line(X(gx), Y(93), X(gx), Y(986), Color{255, 255, 255, 7}, std::max(1.0f, k));
    }
    for (float gy = 93; gy < 986; gy += 8) {
        out.Line(X(316), Y(gy), X(1056), Y(gy), Color{255, 255, 255, 7}, std::max(1.0f, k));
    }
    out.StrokeRect(F(315, 92, 742, 895), kWhiteText, std::max(1.0f, 1.5f * k));
    out.FillRect(F(311, 88, 8, 8), kWhiteText);
    out.FillRect(F(925, 85, 132, 7), kWhiteText);

    // the title, narrowed to the panel when long
    const std::string title = Caps(T("Additional settings", "Дополнительные настройки"));
    float title_size = 66.0f * k;
    while (title_size > 30.0f * k && measure.Width(title, title_size, FontWeight::GameSlab) > 690.0f * k) {
        title_size -= 2.0f * k;
    }
    out.Text(F(347, 118, 700, 84), title, title_size, kYellow, Align::Left, FontWeight::GameSlab);

    // ---- the list: each row's centre in list coordinates
    const int n = static_cast<int>(rows_.size());
    std::vector<float> cy(rows_.size());
    float y = 0;
    for (int i = 0; i < n; ++i) {
        const auto& r = rows_[static_cast<size_t>(i)];
        if (i == 0) {
            y = 0;
        } else if (r.kind == Row::Kind::Header) {
            y += rows_[static_cast<size_t>(i) - 1].kind == Row::Kind::Header ? 40.0f : 64.0f;
        } else {
            y += rows_[static_cast<size_t>(i) - 1].kind == Row::Kind::Header ? 36.0f : 35.0f;
        }
        cy[static_cast<size_t>(i)] = y;
    }
    const float list_top = 225, list_bottom = 985, first_y = 260;
    const float content_end = n > 0 ? cy.back() + 30 : 0;
    const float max_scroll = std::max(0.0f, content_end - (list_bottom - first_y));
    if (focus_ < rows_.size()) {
        // the chosen item in view, with its section's name when it is the first item
        float want_top = cy[focus_] - 40;
        if (focus_ > 0 && rows_[focus_ - 1].kind == Row::Kind::Header) {
            want_top = cy[focus_ - 1] - 40;
        }
        const float want_bottom = cy[focus_] + 40;
        if (want_top - game_scroll_ < 0) {
            game_scroll_ = want_top;
        }
        if (want_bottom - game_scroll_ > list_bottom - first_y) {
            game_scroll_ = want_bottom - (list_bottom - first_y);
        }
    }
    game_scroll_ = std::clamp(game_scroll_, 0.0f, max_scroll);

    out.PushClip({X(316), Y(list_top), 740 * k, (list_bottom - list_top) * k});
    for (int i = 0; i < n; ++i) {
        const auto& row = rows_[static_cast<size_t>(i)];
        const float fy = first_y + cy[static_cast<size_t>(i)] - game_scroll_;
        if (fy < list_top - 40 || fy > list_bottom + 40) {
            continue;
        }
        if (row.kind == Row::Kind::Header) {
            const std::string name = Caps(T(row.en, row.ru));
            float size = (row.section ? 34.0f : 28.0f) * k;
            while (size > 20.0f * k && measure.Width(name, size, FontWeight::GameSlab) > 620.0f * k) {
                size -= 1.0f * k; // a long section name fits the panel as the game's do
            }
            out.Text(F(387, fy - 24, 630, 48), name, size, row.section ? kWhiteText : kGreyText, Align::Left,
                     FontWeight::GameSlab);
            continue;
        }
        const bool focused = static_cast<size_t>(i) == focus_;
        const Desc* d = row.desc;
        const bool available = d == nullptr || service_.Available(*d).available;
        bool dimmed = !available;
        if (d != nullptr && !d->requires_on.empty()) {
            if (const Desc* dep = Settings::Find(d->requires_on)) {
                const Value dv = service_.Get(*dep);
                dimmed = dimmed || (std::holds_alternative<bool>(dv) && !std::get<bool>(dv));
            }
        }
        const Color color = focused ? kYellow : dimmed ? kGreyText : kWhiteText;
        std::string label = d != nullptr ? std::string(env_.ru ? d->ru : d->en) : std::string(T(row.en, row.ru));
        if (row.kind == Row::Kind::ResetCategory) {
            label = fmt::format("{} «{}»", label, Settings::CategoryLabel(row.cat, env_.ru));
        }
        if (d != nullptr && service_.NeedsRestart(*d)) {
            label += " *"; // takes effect after a restart (the description says so)
        }
        const Rect body = F(387, fy - 17.5f, 613, 35);
        hits_.push_back({.kind = Hit::Kind::Row, .rect = body, .index = i, .part = Hit::Part::Body});
        constexpr float kItem = 27.0f;
        // the label ends where its value begins (measured below): drawn last so it knows the room it has
        float label_right = 974; // frame x
        const auto label_text = [&] {
            out.Text(F(415, fy - 20, std::max(120.0f, label_right - 415), 40), label, kItem * k, color, Align::Left,
                     FontWeight::GameItem);
        };

        // the value, right-aligned at x 984 (choices: a click on its left half steps back, on its right half on)
        const auto value_text = [&](const std::string& text, Color c, float right = 984) {
            const float vw = std::min(420.0f * k, measure.Width(text, kItem * k, FontWeight::GameItem)) / k;
            out.Text(F(right - vw - 4, fy - 20, vw + 4, 40), text, kItem * k, c, Align::Right, FontWeight::GameItem);
            label_right = std::min(label_right, right - vw - 18);
        };
        // the game's cross button glyph (its "Press (X)")
        const auto cross_glyph = [&](Color c) {
            const float gx = X(969), gy = Y(fy), r = 11.0f * k;
            out.FillCircle(gx, gy, r, c);
            const float a = 4.5f * k;
            out.Line(gx - a, gy - a, gx + a, gy + a, Color{40, 40, 41, 255}, std::max(1.0f, 2.6f * k));
            out.Line(gx - a, gy + a, gx + a, gy - a, Color{40, 40, 41, 255}, std::max(1.0f, 2.6f * k));
        };
        const auto step_hits = [&] {
            hits_.push_back({.kind = Hit::Kind::Row, .rect = F(690, fy - 17.5f, 147, 35), .index = i,
                             .part = Hit::Part::Prev});
            hits_.push_back({.kind = Hit::Kind::Row, .rect = F(837, fy - 17.5f, 163, 35), .index = i,
                             .part = Hit::Part::Next});
        };
        switch (row.kind) {
        case Row::Kind::Header:
            break;
        case Row::Kind::Info: {
            std::string text;
            if (row.tag == "info.backend") {
                text = env_.backend_label;
            } else if (row.tag == "info.device") {
                text = env_.device_label;
            } else if (row.tag == "info.hotkeys") {
                text = std::string(T("F10 FPS · F11 full screen", "F10 FPS · F11 полный экран"));
            } else if (row.tag == "info.render") {
                text = "1920 × 1080";
            } else if (row.tag == "info.cache.size") {
                text = env_.shader_cache_size;
            } else if (row.tag == "info.cache.content") {
                text = env_.shader_cache_content;
            } else if (row.tag == "info.cache.run") {
                text = env_.shader_cache_run;
            }
            value_text(text, focused ? kYellow : kWhiteText);
            break;
        }
        case Row::Kind::Command:
        case Row::Kind::ResetCategory: {
            const bool armed = armed_ == row.tag && env_.now_ns - armed_ns_ < kConfirmCommandNs;
            value_text(armed ? std::string(T("Press again", "Нажмите ещё раз")) : std::string(T("Press", "Нажмите")),
                       color, 951);
            cross_glyph(color);
            hits_.push_back({.kind = Hit::Kind::Row, .rect = F(690, fy - 17.5f, 310, 35), .index = i,
                             .part = Hit::Part::Button});
            break;
        }
        case Row::Kind::WindowSize:
            value_text(fmt::format("{} × {}", cfg.window_width, cfg.window_height), color);
            step_hits();
            break;
        case Row::Kind::Setting: {
            if (d == nullptr) {
                break;
            }
            if (!available) {
                value_text(std::string(T("Unavailable", "Недоступно")), focused ? kYellow : kGreyText);
                break;
            }
            const Value v = service_.Get(*d);
            switch (d->type) {
            case Type::Bool:
                value_text(std::get<bool>(v) ? std::string(T("On", "Вкл.")) : std::string(T("Off", "Выкл.")), color);
                hits_.push_back({.kind = Hit::Kind::Row, .rect = F(690, fy - 17.5f, 310, 35), .index = i,
                                 .part = Hit::Part::Value});
                break;
            case Type::Int:
            case Type::Float: {
                // as the game's volume: ten dashes, as many lit as the value
                const double x = std::holds_alternative<s64>(v) ? static_cast<double>(std::get<s64>(v))
                                                                : std::get<double>(v);
                const double f = std::clamp((x - d->ui_min) / std::max(1e-9, d->ui_max - d->ui_min), 0.0, 1.0);
                const int lit = static_cast<int>(std::lround(f * 10.0));
                label_right = 676;
                for (int j = 0; j < 10; ++j) {
                    out.FillRect(F(690 + 30.0f * static_cast<float>(j), fy - 4, 26, 7),
                                 j < lit ? (focused ? kYellow : (dimmed ? kGreyText : kWhiteText)) : kDashOff);
                }
                hits_.push_back({.kind = Hit::Kind::Row, .rect = F(690, fy - 17.5f, 296, 35), .index = i,
                                 .part = Hit::Part::Track});
                break;
            }
            case Type::Choice:
            case Type::Text:
                value_text(ValueText(*d, v), color);
                step_hits();
                break;
            case Type::Binding: {
                const auto& b = std::get<Settings::Bindings>(v);
                const auto action = static_cast<Input::Action>(d->action);
                const bool capturing = capture_ && capture_->action == action;
                std::string text;
                if (capturing) {
                    text = std::string(T("Press a key…", "Нажмите клавишу…"));
                } else {
                    for (size_t j = 0; j < std::min<size_t>(b.size(), 2); ++j) {
                        const std::string one = BindingText(b[j]);
                        text += (j == 0 ? "" : " / ") +
                                (focused && slot_ == static_cast<int>(j) ? "[" + one + "]" : one);
                    }
                    if (text.empty()) {
                        text = "—";
                    }
                }
                value_text(text, capturing ? kYellow : color);
                hits_.push_back({.kind = Hit::Kind::Row, .rect = F(690, fy - 17.5f, 155, 35), .index = i,
                                 .part = Hit::Part::Slot0});
                hits_.push_back({.kind = Hit::Kind::Row, .rect = F(845, fy - 17.5f, 155, 35), .index = i,
                                 .part = Hit::Part::Slot1});
                break;
            }
            }
            break;
        }
        }
        label_text();
    }
    out.PopClip();

    // the scroll bar
    if (max_scroll > 0) {
        const float track = list_bottom - list_top;
        const float thumb = std::max(40.0f, track * (list_bottom - first_y) / (content_end + 1));
        const float ty = list_top + (track - thumb) * (game_scroll_ / max_scroll);
        out.FillRect(F(1028, list_top, 9, track), Color{30, 30, 30, 255});
        out.FillRect(F(1028, ty, 9, thumb), Color{79, 79, 79, 255});
    }

    // ---- the chosen item's description: right of the panel, under the game's controller picture
    std::string help;
    if (focus_ < rows_.size()) {
        const auto& row = rows_[focus_];
        if (capture_) {
            help = fmt::format("{} «{}». {}", T("Press a key or a mouse button for", "Нажмите клавишу или кнопку мыши для"),
                               Settings::ActionLabel(capture_->action, env_.ru), T("Esc cancels.", "Esc — отмена."));
        } else if (row.desc != nullptr) {
            const auto a = service_.Available(*row.desc);
            help = a.available ? std::string(T(row.desc->help_en, row.desc->help_ru))
                               : fmt::format("{} {}", T("Unavailable.", "Недоступно."), T(a.why_en, a.why_ru));
            if (service_.NeedsRestart(*row.desc)) {
                help += std::string(help.empty() ? "" : " ") +
                        std::string(T("* Takes effect after a restart.", "* Вступит в силу после перезапуска."));
            }
        } else if (row.kind == Row::Kind::WindowSize) {
            help = std::string(T("The window's size when not in full screen.", "Размер окна вне полноэкранного режима."));
        } else if (row.kind == Row::Kind::ResetCategory) {
            help = std::string(T("Every setting of this section back to its default.",
                                 "Все настройки раздела вернутся к значениям по умолчанию."));
        }
    }
    if (!status_.empty()) {
        help = status_ + (help.empty() ? "" : "\n") + help;
    } else if (!env_.save_note.empty()) {
        help = env_.save_note + (help.empty() ? "" : "\n") + help;
    }
    if (!help.empty()) {
        out.SetSurface({X(305), Y(80), (1820 - 305) * k, 920 * k});
        out.Text(F(1108, 700, 690, 200), help, 24.0f * k, Color{220, 220, 220, 255}, Align::Left,
                 FontWeight::GameItem, true);
    }

    // ---- dialogs: a smaller panel of the same kind over the list
    if (modal_ != Modal::None) {
        out.FillRect(F(316, 93, 740, 893), Color{0, 0, 0, 150});
        const Rect box = F(355, 380, 664, 300);
        out.FillRect(box, Color{40, 40, 41, 252});
        out.StrokeRect(box, kWhiteText, std::max(1.0f, 1.5f * k));
        std::string mtitle, text;
        std::string_view yes, no;
        if (modal_ == Modal::ConfirmDisplay) {
            const u64 left_ns = modal_deadline_ns_ > env_.now_ns ? modal_deadline_ns_ - env_.now_ns : 0;
            const auto left_s = (left_ns + 999'999'999) / 1'000'000'000;
            mtitle = std::string(T("Keep these display settings?", "Сохранить параметры экрана?"));
            text = fmt::format("{} {} {}.", T("If the picture is wrong, do nothing: they are restored in",
                                              "Если изображение неправильное, ничего не нажимайте: параметры вернутся через"),
                               left_s, T("s", "с"));
            yes = T("Keep", "Сохранить");
            no = T("Restore", "Вернуть");
        } else {
            mtitle = fmt::format("{} «{}»?", T("Reset", "Сбросить раздел"), Settings::CategoryLabel(category_, env_.ru));
            text = std::string(T("Every setting of this section goes back to its default.",
                                 "Все настройки раздела вернутся к значениям по умолчанию."));
            yes = T("Reset", "Сбросить");
            no = T("Cancel", "Отмена");
        }
        out.Text(F(380, 395, 620, 60), Caps(mtitle), 34.0f * k, kYellow, Align::Left, FontWeight::GameSlab);
        out.Text(F(380, 460, 620, 110), text, 26.0f * k, kWhiteText, Align::Left, FontWeight::GameItem, true);
        for (int i = 0; i < 2; ++i) {
            const Rect b = F(415, 590.0f + 35.0f * static_cast<float>(i), 400, 35);
            out.Text(b, i == 0 ? yes : no, 28.0f * k, modal_button_ == i ? kYellow : kWhiteText, Align::Left,
                     FontWeight::GameItem);
            hits_.push_back({.kind = Hit::Kind::ModalButton, .rect = b, .index = i});
        }
    }
}

} // namespace SecondSon::Ui
