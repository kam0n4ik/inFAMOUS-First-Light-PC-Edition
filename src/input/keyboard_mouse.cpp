// SecondSonPC — keyboard and mouse as the DualShock 4 of player 1 (see keyboard_mouse.h).
#include "input/keyboard_mouse.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>
#include <windows.h>

#include "base/log.h"

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
    key_actions_.fill(0);
    mouse_actions_.fill(0);
    for (size_t a = 0; a < kActionCount; ++a) {
        for (const auto& in : settings_.bindings[a]) {
            if (in.kind == InputCode::Kind::Key && in.code < key_actions_.size()) {
                key_actions_[in.code] |= 1u << a;
            } else if (in.kind == InputCode::Kind::MouseButton && in.code < mouse_actions_.size()) {
                mouse_actions_[in.code] |= 1u << a;
            }
        }
    }
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
        } else if (in.code < 32 && (mouse_down_ & (1u << in.code)) != 0) {
            return true;
        }
    }
    return false;
}

void KeyboardMouse::ClearInputs() {
    key_down_.fill(false);
    mouse_down_ = 0;
    swallowed_button_ = 0;
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
                if ((actions & Bit(Action::ReleaseMouse)) != 0 && !test_mode_) {
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
            if (down && ((!captured_ && settings_.capture_mouse) || focus_click)) {
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
    case SDL_EVENT_MOUSE_WHEEL:
        result.consumed = true;
        break;
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

    PadState s;
    for (const auto& b : kButtons) {
        if (Pressed(b.action)) {
            s.buttons |= b.bit;
        }
    }
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

    if (motion_mode_) {
        const double k = kMotionRadiansPerCount * settings_.motion_sensitivity;
        motion_.Turn(-mdx * k, -mdy * k); // mouse right = turn right, mouse up = front up
    } else if (drag_) {
        drag_x_ = std::clamp(drag_x_ + mdx * kTouchUnitsPerCount, 0.0, kTouchWidth - 1.0);
        drag_y_ = std::clamp(drag_y_ + mdy * kTouchUnitsPerCount, 0.0, kTouchHeight - 1.0);
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

    if (drag_) {
        s.touch_active = true;
        s.touch[0] = {true, static_cast<u16>(drag_x_), static_cast<u16>(drag_y_), finger_id_};
    } else if (tap_) {
        s.touch_active = true;
        s.touch[0] = {true, kTouchWidth / 2, kTouchHeight / 2, finger_id_};
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

KeyboardMouse& Player1() {
    static KeyboardMouse instance;
    return instance;
}

} // namespace SecondSon::Input
