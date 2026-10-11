// SecondSonPC — keyboard and mouse as the DualShock 4 of player 1 (Phase 2, P2.1). Native code:
// SDL3 events in (main thread; the bridge window hands them over, patch 0006), the state of a
// DualShock 4 out (libScePad, NATIVE, reads it on the game thread once per scePadReadState).
#pragma once

#include <array>
#include <mutex>

#include "base/abi.h"
#include "input/motion.h"
#include "input/settings.h"

union SDL_Event;
struct SDL_Window;

namespace SecondSon::Input {

/// libScePad button bits (OrbisPadButtonDataOffset).
namespace Button {
constexpr u32 L3 = 0x2;
constexpr u32 R3 = 0x4;
constexpr u32 Options = 0x8;
constexpr u32 Up = 0x10;
constexpr u32 Right = 0x20;
constexpr u32 Down = 0x40;
constexpr u32 Left = 0x80;
constexpr u32 L2 = 0x100;
constexpr u32 R2 = 0x200;
constexpr u32 L1 = 0x400;
constexpr u32 R1 = 0x800;
constexpr u32 Triangle = 0x1000;
constexpr u32 Circle = 0x2000;
constexpr u32 Cross = 0x4000;
constexpr u32 Square = 0x8000;
constexpr u32 TouchPad = 0x100000;
} // namespace Button

struct Touch {
    bool down = false;
    u16 x = 0;
    u16 y = 0;
    u8 id = 0;
};

/// What a reading was made of ([Debug] inputLog).
struct ReadInfo {
    double mouse_dx = 0.0, mouse_dy = 0.0; // mouse counts this reading used up
    double dt = 0.0;                        // s since the previous reading
    bool repeated = false;                  // a second read within kMinReadIntervalUs
    bool captured = false;
    bool motion_mode = false;
};

/// One reading of the virtual controller.
struct PadState {
    u32 buttons = 0;
    u8 lx = 128, ly = 128, rx = 128, ry = 128;
    u8 l2 = 0, r2 = 0;
    MotionState motion;
    bool touch_active = false; // a virtual finger is on the touch pad: it wins over a real one
    std::array<Touch, 2> touch{};
    ReadInfo info;
};

class KeyboardMouse {
public:
    enum class Hotkey : u8 { None, ToggleFps };
    struct EventResult {
        bool consumed = false; // the bridge window does not see the event
        Hotkey hotkey = Hotkey::None;
    };

    void Configure(const Settings& settings);
    /// Main thread: new settings while playing (the settings menu). Configure, then the cursor follows
    /// capture_mouse (released at once when it is turned off).
    void ApplySettings(const Settings& settings);
    /// Main thread: SecondSonPC's settings menu opened/closed. Open: nothing stays pressed, the cursor is given back
    /// (the menu takes every keyboard/mouse event meanwhile). Closed: nothing pressed, the cursor is captured again
    /// if it was before and the window has focus.
    void SetMenuOpen(bool open);
    /// The window the mouse is captured in: captured (hidden, kept inside the window, raw
    /// movement) whenever the window gets focus; Esc gives it back (pause menu) and takes it again,
    /// a click takes it too; focus loss (Alt+Tab) gives it back until the window has focus again.
    /// `test_mode` (automated runs): mouse events count as captured, the system cursor is never
    /// grabbed and focus changes are ignored, so injected input is not disturbed.
    void AttachWindow(SDL_Window* window, bool test_mode);
    /// Automated runs (--press): keyboard and mouse events reach the game only when they carry
    /// kScriptedInputId; the real keyboard and mouse are ignored (counted, logged once), so
    /// someone touching the mouse during a measured run does not change what the game is sent.
    void AcceptOnlyScriptedInput();
    /// Main thread: every SDL event of the game window.
    EventResult OnEvent(const SDL_Event& event);
    /// Game thread: the controller now; mouse movement since the previous read is used up.
    PadState Read(u64 now_us);
    /// scePadOpen: the motion pose returns to rest.
    void ResetOrientation();

    /// libScePad touch pad coordinates (the bridge reports x 0..1919, y 0..941).
    static constexpr u16 kTouchWidth = 1920;
    static constexpr u16 kTouchHeight = 942;
    /// Mouse speed (counts/s) that gives full right-stick deflection at sensitivity 1.
    static constexpr double kFullStickCountsPerSecond = 1500.0;
    /// Motion mode: controller rotation per mouse count at sensitivity 1 (1000 counts ~ 86 deg).
    static constexpr double kMotionRadiansPerCount = 0.0015;
    /// Touch drag: touch pad units per mouse count.
    static constexpr double kTouchUnitsPerCount = 2.0;
    /// TouchTwoFingers: where the two fingers rest (a quarter of the width from each side, half the height).
    static constexpr u16 kTwoFingersLeftX = kTouchWidth / 4;
    static constexpr u16 kTwoFingersRightX = kTouchWidth * 3 / 4;
    /// `which` of the keyboard/mouse events --press pushes (no real device has this id).
    static constexpr u32 kScriptedInputId = 0x53535043; // "SSPC"

private:
    bool Pressed(Action a) const;
    void ClearInputs();             // mutex held
    void SetCapture(bool captured); // main thread, mutex not held
    bool WindowHasFocus() const;    // main thread
    u8 NextTouchId();

    mutable std::mutex mutex_;
    Settings settings_ = DefaultSettings();
    std::array<u32, 512> key_actions_{}; // scancode -> action bits
    std::array<u32, 8> mouse_actions_{}; // SDL mouse button -> action bits
    std::array<u32, 4> wheel_actions_{}; // wheel up/down/left/right -> action bits
    std::array<bool, 512> key_down_{};
    u32 mouse_down_ = 0; // bit = SDL mouse button
    u32 wheel_pending_ = 0; // bit = wheel direction: notches since the last reading
    u32 wheel_latched_ = 0; // the notches this reading presses (one reading each)
    bool menu_open_ = false;
    std::array<u64, kActionCount> action_seq_{}; // order of presses (last of opposite keys wins)
    u64 seq_ = 0;

    SDL_Window* window_ = nullptr;
    bool test_mode_ = false;
    bool scripted_only_ = false;    // main thread: real keyboard/mouse events are dropped
    u64 ignored_real_events_ = 0;   // main thread: how many (the first one is logged)
    bool captured_ = false;
    bool want_capture_ = true; // playing: captured whenever the window has focus; Esc toggles it
    u64 focus_gained_ns_ = 0;  // a click right after the window got focus only focuses it
    u8 swallowed_button_ = 0;  // the click that captured the mouse: its release is not a press
    double mouse_dx_ = 0.0, mouse_dy_ = 0.0;

    bool motion_mode_ = false;
    VirtualMotion motion_;

    bool drag_ = false;
    bool tap_ = false;
    double drag_x_ = 0.0, drag_y_ = 0.0;
    u8 finger_id_ = 0;
    bool two_fingers_ = false;
    std::array<u8, 2> two_finger_ids_{};
    u8 next_touch_id_ = 1;

    bool has_last_ = false;
    u64 last_read_us_ = 0;
    PadState last_;
};

/// The keyboard/mouse controller (player 1).
KeyboardMouse& Player1();

} // namespace SecondSon::Input
