// SecondSonPC — keyboard and mouse as the DualShock 4 of player 1 (see keyboard_mouse.h).
#include "input/keyboard_mouse.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numbers>
#include <string>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <windows.h>

#include "base/log.h"
#include "runtime/gpu/diag_request.h"
#include "runtime/gpu/r9/gprof.h"

namespace SecondSon::Input {

namespace {

constexpr u32 Bit(Action a) {
    return 1u << static_cast<u32>(a);
}
static_assert(kActionCount <= 32, "action bits are a u32");

/// Never read twice within this time: a second read in the same frame reports the same state
/// instead of a mouse speed measured over a few microseconds.
constexpr u64 kMinReadIntervalUs = 2000;

u8 Axis(double v) {
    return static_cast<u8>(std::clamp(std::lround(127.5 + v * 127.5), 0L, 255L));
}

u64 SteadyMicroseconds() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

/// The program that took the focus away (focus-loss log): its executable name and process id only
/// (window titles of other programs can be private: chats, documents).
std::string ForegroundWindowName() {
    HWND w = GetForegroundWindow();
    if (w == nullptr) {
        return "no window";
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid == GetCurrentProcessId()) {
        return fmt::format("SecondSonPC itself, process {}", pid);
    }
    std::string exe = "?";
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); p != nullptr) {
        wchar_t path[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &size)) {
            const wchar_t* name = path;
            for (const wchar_t* c = path; *c != L'\0'; ++c) {
                if (*c == L'\\' || *c == L'/') {
                    name = c + 1;
                }
            }
            char utf8[MAX_PATH * 3] = {};
            WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, sizeof(utf8) - 1, nullptr, nullptr);
            exe = utf8;
        }
        CloseHandle(p);
    }
    return fmt::format("{}, process {}", exe, pid);
}

struct ButtonOf {
    Action action;
    u32 bit;
};
constexpr ButtonOf kButtons[] = {
    {Action::Cross, Button::Cross},       {Action::Circle, Button::Circle},
    {Action::Square, Button::Square},     {Action::Triangle, Button::Triangle},
    {Action::L1, Button::L1},             {Action::R1, Button::R1},
    {Action::L2, Button::L2},             {Action::R2, Button::R2},
    {Action::L3, Button::L3},             {Action::R3, Button::R3},
    {Action::Options, Button::Options},   {Action::TouchPad, Button::TouchPad},
    {Action::DpadUp, Button::Up},         {Action::DpadDown, Button::Down},
    {Action::DpadLeft, Button::Left},     {Action::DpadRight, Button::Right},
};

} // namespace

void KeyboardMouse::Configure(const Settings& settings) {
    std::scoped_lock lock{mutex_};
    settings_ = settings;
    ++layout_generation_;
    key_actions_.fill(0);
    mouse_actions_.fill(0);
    wheel_actions_.fill(0);
    for (size_t a = 0; a < kActionCount; ++a) {
        for (const auto& in : settings_.bindings[a]) {
            if (in.kind == InputCode::Kind::Key && in.code < key_actions_.size()) {
                key_actions_[in.code] |= 1u << a;
            } else if (in.kind == InputCode::Kind::MouseButton && in.code < mouse_actions_.size()) {
                mouse_actions_[in.code] |= 1u << a;
            } else if (in.kind == InputCode::Kind::MouseWheel && in.code < wheel_actions_.size()) {
                wheel_actions_[in.code] |= 1u << a;
            }
        }
    }
}

void KeyboardMouse::ApplySettings(const Settings& settings) {
    Configure(settings);
    bool release;
    {
        std::scoped_lock lock{mutex_};
        release = captured_ && !settings_.capture_mouse && !test_mode_;
    }
    if (release) {
        SetCapture(false);
    }
}

bool KeyboardMouse::WindowHasFocus() const {
    SDL_Window* window = window_; // set before the first event, on this (main) thread
    return window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

void KeyboardMouse::SetMenuOpen(bool open) {
    bool capture = false;
    {
        std::scoped_lock lock{mutex_};
        ClearInputs();
        if (open && motion_mode_) {
            // the controller is not left held sideways: after the menu the mouse turns the camera again
            motion_mode_ = false;
            motion_.SetSideways(false, settings_.sideways_right_up);
        }
        // back to one of the game's menus (the PC settings open from its settings screen): the cursor stays free
        capture = !open && !test_mode_ && want_capture_ && settings_.capture_mouse && !game_menu_;
    }
    if (open) {
        SetCapture(false);
    } else if (capture && WindowHasFocus()) {
        SetCapture(true);
    }
    SSPC_LOG(Input, Info, "settings menu {}: keyboard/mouse {}", open ? "open" : "closed",
             open ? "go to the menu" : "back to the game");
}

void KeyboardMouse::SetGameMenuOpen(bool open) {
    bool capture = false;
    {
        std::scoped_lock lock{mutex_};
        if (game_menu_ == open) {
            return;
        }
        game_menu_ = open;
        menu_mouse_ = 0;
        menu_queue_.clear();
        menu_latched_ = 0;
        menu_gap_ = false;
        mouse_down_ = 0; // a button held into or out of the menu is not a press on the other side
        swallowed_button_ = 0;
        if (!open) {
            want_capture_ = true; // back in the game: the mouse turns the camera again
        }
        capture = !open && !test_mode_ && settings_.capture_mouse;
    }
    if (test_mode_) {
        return;
    }
    if (open) {
        SetCapture(false);
    } else if (capture && WindowHasFocus()) {
        SetCapture(true);
    }
    SSPC_LOG(Input, Info, "game menu {}: {}", open ? "shown" : "gone",
             open ? "cursor free, left button = cross, right = circle, wheel = d-pad up/down"
                  : "the mouse is the game's again");
}

void KeyboardMouse::MenuPress(u32 buttons) {
    std::scoped_lock lock{mutex_};
    if (game_menu_ && buttons != 0 && menu_queue_.size() < 32) {
        menu_queue_.push_back(buttons);
    }
}

size_t KeyboardMouse::MenuPressesQueued() const {
    std::scoped_lock lock{mutex_};
    return menu_queue_.size() + (menu_latched_ != 0 ? 1 : 0);
}

Settings KeyboardMouse::Layout() const {
    std::scoped_lock lock{mutex_};
    return settings_;
}

u64 KeyboardMouse::LayoutGeneration() const {
    std::scoped_lock lock{mutex_};
    return layout_generation_;
}

u16 KeyboardMouse::KeyFor(Action a) const {
    std::scoped_lock lock{mutex_};
    for (const auto& in : settings_.bindings[static_cast<size_t>(a)]) {
        if (in.kind == InputCode::Kind::Key) {
            return in.code;
        }
    }
    return 0;
}

bool KeyboardMouse::KeyTriggers(u16 scancode, Action a) const {
    std::scoped_lock lock{mutex_};
    return scancode < key_actions_.size() && (key_actions_[scancode] & (1u << static_cast<size_t>(a))) != 0;
}

void KeyboardMouse::AttachWindow(SDL_Window* window, bool test_mode) {
    {
        std::scoped_lock lock{mutex_};
        window_ = window;
        test_mode_ = test_mode;
        captured_ = test_mode; // logical capture only: SetCapture is never called in test mode
    }
    SSPC_LOG(Input, Info, "keyboard/mouse controller attached ({})",
             test_mode ? "test mode: mouse counts as captured, system cursor never grabbed"
                       : "the window captures the mouse while it has focus; Esc gives it back and "
                         "takes it again, a click takes it too");
}

void KeyboardMouse::AcceptOnlyScriptedInput() {
    scripted_only_ = true; // before the first event, on the main thread
    SSPC_LOG(Input, Info, "automated run: only the scripted (--press) keyboard/mouse input reaches the game; "
                          "the real keyboard and mouse are ignored");
}

bool KeyboardMouse::Pressed(Action a) const {
    for (const auto& in : settings_.bindings[static_cast<size_t>(a)]) {
        if (in.kind == InputCode::Kind::Key) {
            if (in.code < key_down_.size() && key_down_[in.code]) {
                return true;
            }
        } else if (in.kind == InputCode::Kind::MouseWheel) {
            if (in.code < 32 && (wheel_latched_ & (1u << in.code)) != 0) {
                return true;
            }
        } else if (in.code < 32 && (mouse_down_ & (1u << in.code)) != 0) {
            return true;
        }
    }
    return false;
}

void KeyboardMouse::ClearInputs() {
    key_down_.fill(false);
    mouse_down_ = 0;
    wheel_pending_ = wheel_latched_ = 0;
    swallowed_button_ = 0;
    menu_mouse_ = 0;
    menu_queue_.clear();
    menu_latched_ = 0;
    menu_gap_ = false;
    mouse_dx_ = mouse_dy_ = 0.0;
}

void KeyboardMouse::SetCapture(bool captured) {
    SDL_Window* window;
    {
        std::scoped_lock lock{mutex_};
        if (test_mode_ || captured_ == captured || (captured && !settings_.capture_mouse)) {
            return;
        }
        captured_ = captured;
        mouse_dx_ = mouse_dy_ = 0.0;
        window = window_; // none in the self-test: the logical state only
    }
    // Relative mode hides the cursor and reports raw movement; the grab also keeps the cursor
    // inside the window, so a click can never land on another window and take the focus away.
    if (window != nullptr && !SDL_SetWindowRelativeMouseMode(window, captured)) {
        SSPC_LOG(Input, Error, "mouse {} failed: {}", captured ? "capture" : "release", SDL_GetError());
    }
    if (window != nullptr && !SDL_SetWindowMouseGrab(window, captured)) {
        SSPC_LOG(Input, Error, "mouse {} failed: {}", captured ? "grab" : "ungrab", SDL_GetError());
    }
    SSPC_LOG(Input, Info, "mouse {}", captured ? "captured" : "released");
}

u8 KeyboardMouse::NextTouchId() {
    const u8 id = next_touch_id_;
    next_touch_id_ = next_touch_id_ == 127 ? 1 : next_touch_id_ + 1; // the bridge's id sequence
    return id;
}

KeyboardMouse::EventResult KeyboardMouse::OnEvent(const SDL_Event& e) {
    EventResult result;
    if (scripted_only_) {
        u32 which = kScriptedInputId; // not a keyboard/mouse event: handled as always
        switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            which = e.key.which;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            which = e.button.which;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            which = e.motion.which;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            which = e.wheel.which;
            break;
        default:
            break;
        }
        if (which != kScriptedInputId) {
            if (ignored_real_events_++ == 0) {
                SSPC_LOG(Input, Warning, "automated run: real keyboard/mouse input ignored (event 0x{:x})",
                         static_cast<u32>(e.type));
            }
            result.consumed = true; // the bridge window does not see it either
            return result;
        }
    }
    switch (e.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        result.consumed = true;
        const bool down = e.type == SDL_EVENT_KEY_DOWN;
        if (down && e.key.repeat) {
            break;
        }
        const auto sc = static_cast<size_t>(e.key.scancode);
        if (down && sc == SDL_SCANCODE_F10) {
            result.hotkey = Hotkey::ToggleFps;
            break;
        }
        // R9 gameplay profiler (SSPC_R9_GPROF): the player marks what is happening now
        if (down && Gpu::Gprof::On()) {
            const char* mark = sc == SDL_SCANCODE_F5    ? "calm"
                               : sc == SDL_SCANCODE_F6  ? "fight"
                               : sc == SDL_SCANCODE_F7  ? "traversal"
                               : sc == SDL_SCANCODE_F9  ? "effects"
                               : sc == SDL_SCANCODE_F12 ? "stutter"
                                                        : nullptr;
            if (mark != nullptr) {
                Gpu::Gprof::Mark(mark);
                break;
            }
        }
        // DIAGNOSTIC (SSPC_DIAG_TEX_KEY=1): F8 dumps the textures of the next frame (runtime/gpu/diag_request.h)
        static const bool tex_key = [] {
            char v[4] = {};
            return GetEnvironmentVariableA("SSPC_DIAG_TEX_KEY", v, sizeof(v)) == 1 && v[0] == '1';
        }();
        if (down && sc == SDL_SCANCODE_F8 && tex_key) {
            Gpu::Diag::g_texture_dump_request.fetch_add(1, std::memory_order_relaxed);
            SSPC_LOG(Input, Warning, "DIAGNOSTIC: F8 - the next frame's textures are dumped");
            break;
        }
        if (down && sc == SDL_SCANCODE_F11) {
            SDL_Window* window = window_; // set before the first event, on this (main) thread
            if (window != nullptr) {
                SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
            }
            break;
        }
        if (sc >= key_down_.size()) {
            break;
        }
        bool toggle_capture = false, capture = false;
        {
            std::scoped_lock lock{mutex_};
            key_down_[sc] = down;
            const u32 actions = key_actions_[sc];
            if (down) {
                for (size_t a = 0; a < kActionCount; ++a) {
                    if ((actions & (1u << a)) != 0) {
                        action_seq_[a] = ++seq_;
                    }
                }
                constexpr u32 kMove = Bit(Action::MoveForward) | Bit(Action::MoveBack) |
                                      Bit(Action::MoveLeft) | Bit(Action::MoveRight);
                const bool toggle = (actions & Bit(Action::MotionMode)) != 0;
                // Walking ends the motion mode: the mouse goes back to the camera after graffiti
                // even when G was not pressed again (the character never walks while spraying).
                if (toggle || (motion_mode_ && (actions & kMove) != 0)) {
                    motion_mode_ = !motion_mode_;
                    motion_.SetSideways(motion_mode_, settings_.sideways_right_up);
                    mouse_dx_ = mouse_dy_ = 0.0;
                    SSPC_LOG(Input, Info, "motion mode {} (controller {}){}", motion_mode_ ? "on" : "off",
                             motion_mode_ ? (settings_.sideways_right_up ? "sideways, right side up"
                                                                         : "sideways, left side up")
                                          : "back to rest",
                             toggle ? "" : ": a movement key, the mouse turns the camera again");
                }
                // Esc gives the cursor back (the pause menu opens with it) and takes it again
                // (the menu closes).
                if ((actions & Bit(Action::ReleaseMouse)) != 0 && !test_mode_ && !game_menu_) {
                    toggle_capture = true;
                    want_capture_ = !captured_;
                    capture = want_capture_;
                }
            }
        }
        if (toggle_capture) {
            SetCapture(capture);
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        result.consumed = true;
        const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const u8 button = e.button.button;
        if (button >= 32) {
            break;
        }
        bool capture = false;
        {
            std::scoped_lock lock{mutex_};
            // A click that only brings the window to the front (it captured the mouse on focus).
            constexpr u64 kFocusClickNs = 300'000'000;
            const bool focus_click =
                down && focus_gained_ns_ != 0 && e.button.timestamp >= focus_gained_ns_ &&
                e.button.timestamp - focus_gained_ns_ < kFocusClickNs;
            focus_gained_ns_ = 0;
            if (game_menu_) {
                // the game's menu: right = circle (back); the left button is the menu controller's (the item
                // under the cursor, MenuPress); nothing else, and the cursor stays free
                const u32 bit = button == SDL_BUTTON_RIGHT ? Button::Circle : 0;
                menu_mouse_ = down ? (menu_mouse_ | bit) : (menu_mouse_ & ~bit);
            } else if (down && ((!captured_ && settings_.capture_mouse) || focus_click)) {
                // The click that captures the mouse is not a press for the game.
                swallowed_button_ = button;
                capture = !captured_;
                want_capture_ = true;
            } else if (!down && swallowed_button_ == button) {
                swallowed_button_ = 0;
            } else {
                if (down) {
                    mouse_down_ |= 1u << button;
                    const u32 actions = button < mouse_actions_.size() ? mouse_actions_[button] : 0;
                    for (size_t a = 0; a < kActionCount; ++a) {
                        if ((actions & (1u << a)) != 0) {
                            action_seq_[a] = ++seq_;
                        }
                    }
                } else {
                    mouse_down_ &= ~(1u << button);
                }
            }
        }
        if (capture) {
            SetCapture(true);
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        result.consumed = true;
        std::scoped_lock lock{mutex_};
        if (captured_) {
            mouse_dx_ += e.motion.xrel;
            mouse_dy_ += e.motion.yrel;
        }
        break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
        // A notch is a press of the bound action for one controller reading (never lost between readings).
        result.consumed = true;
        std::scoped_lock lock{mutex_};
        if (game_menu_) {
            // the game's menu: a notch is one d-pad press (up / down)
            const float wy = e.wheel.y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f);
            if (wy != 0.0f && menu_queue_.size() < 32) {
                menu_queue_.push_back(wy > 0.0f ? Button::Up : Button::Down);
            }
            break;
        }
        if (!captured_ && settings_.capture_mouse) {
            break; // the cursor is free (pause, another window's turn): the wheel is not the game's
        }
        const float flip = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
        const float wy = e.wheel.y * flip, wx = e.wheel.x * flip;
        u32 dirs = 0;
        dirs |= wy > 0.0f ? 1u << 0 : 0u;
        dirs |= wy < 0.0f ? 1u << 1 : 0u;
        dirs |= wx < 0.0f ? 1u << 2 : 0u;
        dirs |= wx > 0.0f ? 1u << 3 : 0u;
        for (size_t w = 0; w < wheel_actions_.size(); ++w) {
            if (wheel_actions_[w] == 0) {
                dirs &= ~(1u << w); // an unbound direction is not kept for a later reading
            }
        }
        wheel_pending_ |= dirs;
        for (size_t w = 0; w < wheel_actions_.size(); ++w) {
            if ((dirs & (1u << w)) == 0) {
                continue;
            }
            for (size_t a = 0; a < kActionCount; ++a) {
                if ((wheel_actions_[w] & (1u << a)) != 0) {
                    action_seq_[a] = ++seq_;
                }
            }
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED: {
        // Back in the game (start, Alt+Tab back): the mouse is captured again unless Esc gave it
        // back on purpose.
        bool capture;
        {
            std::scoped_lock lock{mutex_};
            capture = !test_mode_ && want_capture_ && settings_.capture_mouse;
            focus_gained_ns_ = capture ? e.window.timestamp : 0;
        }
        if (capture) {
            SetCapture(true);
        }
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST: {
        // Alt+Tab: nothing stays pressed, no movement piles up, the cursor is given back.
        bool test;
        {
            std::scoped_lock lock{mutex_};
            test = test_mode_;
            if (!test) {
                ClearInputs();
                // The controller is not left held sideways (graffiti): back in the game the mouse turns the camera.
                if (motion_mode_) {
                    motion_mode_ = false;
                    motion_.SetSideways(false, settings_.sideways_right_up);
                    SSPC_LOG(Input, Info, "motion mode off: the window lost focus");
                }
            }
        }
        if (!test) {
            SetCapture(false);
            SSPC_LOG(Input, Info, "window lost focus: keys released (now in front: {})",
                     ForegroundWindowName());
        }
        break;
    }
    default:
        break;
    }
    return result;
}

PadState KeyboardMouse::Read(u64 now_us) {
    std::scoped_lock lock{mutex_};
    if (has_last_ && now_us >= last_read_us_ && now_us - last_read_us_ < kMinReadIntervalUs) {
        PadState again = last_;
        again.info.repeated = true;
        return again;
    }
    const double dt =
        has_last_ ? std::clamp((now_us - last_read_us_) / 1e6, 0.002, 0.1) : 1.0 / 60.0;
    // Wheel notches since the previous reading press their actions for this reading only.
    wheel_latched_ = wheel_pending_;
    wheel_pending_ = 0;
    // a queued game-menu press lasts one reading, the reading after it is released (a new press for the game)
    menu_latched_ = 0;
    if (menu_gap_) {
        menu_gap_ = false;
    } else if (!menu_queue_.empty()) {
        menu_latched_ = menu_queue_.front();
        menu_queue_.pop_front();
        menu_gap_ = true;
    }

    PadState s;
    for (const auto& b : kButtons) {
        if (Pressed(b.action)) {
            s.buttons |= b.bit;
        }
    }
    s.buttons |= menu_mouse_ | menu_latched_;
    s.l2 = Pressed(Action::L2) ? 255 : 0;
    s.r2 = Pressed(Action::R2) ? 255 : 0;

    // Left stick: of two opposite keys the one pressed last wins; diagonals on the unit circle.
    const auto axis = [this](Action minus, Action plus) {
        const bool m = Pressed(minus), p = Pressed(plus);
        if (m && p) {
            return action_seq_[static_cast<size_t>(plus)] > action_seq_[static_cast<size_t>(minus)]
                       ? 1.0
                       : -1.0;
        }
        return m ? -1.0 : (p ? 1.0 : 0.0);
    };
    double lx = axis(Action::MoveLeft, Action::MoveRight);
    double ly = axis(Action::MoveForward, Action::MoveBack);
    if (lx != 0.0 && ly != 0.0) {
        lx *= std::numbers::sqrt2 / 2.0;
        ly *= std::numbers::sqrt2 / 2.0;
    }
    if (Pressed(Action::Walk)) {
        lx *= 0.5;
        ly *= 0.5;
    }
    s.lx = Axis(lx);
    s.ly = Axis(ly);

    const double mdx = mouse_dx_, mdy = mouse_dy_;
    mouse_dx_ = mouse_dy_ = 0.0;
    s.info = {mdx, mdy, dt, false, captured_, motion_mode_};

    // Touch pad: a held TouchDrag puts a finger down at the centre and the mouse drags it; the
    // TouchPad button touches the centre while held (a real click presses a finger on the pad).
    const bool drag = Pressed(Action::TouchDrag);
    if (drag && !drag_) {
        drag_x_ = kTouchWidth / 2.0;
        drag_y_ = kTouchHeight / 2.0;
        finger_id_ = NextTouchId();
    }
    drag_ = drag;
    const bool tap = Pressed(Action::TouchPad);
    if (tap && !tap_ && !drag_) {
        finger_id_ = NextTouchId();
    }
    tap_ = tap;
    // TouchTwoFingers: two fingers and the click while held - the game's "two-finger click" (logical 0x20000:
    // absorbing with two fingers, pushing a car; docs/PHASE4_PC_INPUT_UI_RESEARCH.md §4.1).
    const bool two = Pressed(Action::TouchTwoFingers);
    if (two && !two_fingers_) {
        two_finger_ids_[0] = NextTouchId();
        two_finger_ids_[1] = NextTouchId();
    }
    two_fingers_ = two;

    if (motion_mode_) {
        const double k = kMotionRadiansPerCount * settings_.motion_sensitivity;
        motion_.Turn(-mdx * k, -mdy * k); // mouse right = turn right, mouse up = front up
    } else if (drag_) {
        drag_x_ = std::clamp(drag_x_ + mdx * kTouchUnitsPerCount, 0.0, kTouchWidth - 1.0);
        drag_y_ = std::clamp(drag_y_ + mdy * kTouchUnitsPerCount, 0.0, kTouchHeight - 1.0);
    } else if (const u64 marked = camera_direct_us_.load(std::memory_order_relaxed);
               marked != 0 && SteadyMicroseconds() - marked < kDirectCameraHoldUs) {
        // the camera takes the counts as angles (TakeCameraCounts): the stick stays at rest
        camera_dx_ += mdx;
        camera_dy_ += mdy;
    } else {
        // Right stick = mouse speed over this read: the camera turns by an amount proportional to
        // the distance the mouse moved, whatever the frame rate; no smoothing, no delay.
        const double full = kFullStickCountsPerSecond / settings_.mouse_sensitivity;
        double sx = mdx / dt / full;
        double sy = mdy / dt / full * (settings_.invert_y ? -1.0 : 1.0);
        const double mag = std::hypot(sx, sy);
        if (mag > 1e-9) {
            // Start past the game's dead zone, so slow movements still turn the camera.
            const double dz = settings_.mouse_deadzone;
            const double out = dz + (1.0 - dz) * std::min(mag, 1.0);
            sx *= out / mag;
            sy *= out / mag;
        }
        s.rx = Axis(sx);
        s.ry = Axis(sy);
    }

    motion_.SetShake(Pressed(Action::Shake));
    s.motion = motion_.Step(now_us / 1e6);

    if (two_fingers_) {
        s.buttons |= Button::TouchPad;
        s.touch_active = true;
        s.touch[0] = {true, kTwoFingersLeftX, kTouchHeight / 2, two_finger_ids_[0]};
        s.touch[1] = {true, kTwoFingersRightX, kTouchHeight / 2, two_finger_ids_[1]};
    } else if (drag_) {
        s.touch_active = true;
        s.touch[0] = {true, static_cast<u16>(drag_x_), static_cast<u16>(drag_y_), finger_id_};
    } else if (tap_) {
        s.touch_active = true;
        s.touch[0] = {true, kTouchWidth / 2, kTouchHeight / 2, finger_id_};
    }

    if (s.buttons != last_.buttons) {
        // every press and release the game is sent (two buttons at once, a missed press: the order is in the log)
        static constexpr std::pair<u32, const char*> kNames[] = {
            {Button::Cross, "cross"}, {Button::Circle, "circle"}, {Button::Square, "square"},
            {Button::Triangle, "triangle"}, {Button::L1, "L1"}, {Button::R1, "R1"}, {Button::L2, "L2"},
            {Button::R2, "R2"}, {Button::L3, "L3"}, {Button::R3, "R3"}, {Button::Options, "options"},
            {Button::TouchPad, "touchpad"}, {Button::Up, "up"}, {Button::Down, "down"}, {Button::Left, "left"},
            {Button::Right, "right"}};
        std::string change;
        for (const auto& [bit, name] : kNames) {
            if ((s.buttons & bit) != (last_.buttons & bit)) {
                change += fmt::format(" {}{}", (s.buttons & bit) != 0 ? '+' : '-', name);
            }
        }
        SSPC_LOG(Input, Info, "keyboard/mouse buttons:{} (now 0x{:x})", change, s.buttons);
    }
    last_ = s;
    last_read_us_ = now_us;
    has_last_ = true;
    return s;
}

void KeyboardMouse::ResetOrientation() {
    std::scoped_lock lock{mutex_};
    motion_.Reset();
    motion_mode_ = false;
}

void KeyboardMouse::TakeCameraCounts(double& dx, double& dy) {
    std::scoped_lock lock{mutex_};
    const u64 now = SteadyMicroseconds();
    const u64 previous = camera_direct_us_.exchange(now, std::memory_order_relaxed);
    const bool fresh = previous != 0 && now - previous < kDirectCameraHoldUs;
    dx = fresh ? camera_dx_ : 0.0;
    dy = fresh ? camera_dy_ : 0.0;
    camera_dx_ = camera_dy_ = 0.0;
    if (!motion_mode_ && !drag_) { // (the counts since the last reading as well: one frame less of delay)
        dx += mouse_dx_;
        dy += mouse_dy_;
        mouse_dx_ = mouse_dy_ = 0.0;
    }
    dx *= settings_.mouse_sensitivity;
    dy *= settings_.mouse_sensitivity * (settings_.invert_y ? -1.0 : 1.0);
}

KeyboardMouse& Player1() {
    static KeyboardMouse instance;
    return instance;
}

} // namespace SecondSon::Input
