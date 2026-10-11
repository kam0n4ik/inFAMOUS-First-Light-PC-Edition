// SecondSonPC — keyboard/mouse settings and the action layout ([Input], [Keyboard] in
// SecondSonPC.toml). Native code; the physical names are SDL3 scancode names (layout independent:
// "W" is the key left of "E" on any keyboard layout, Russian included).
#pragma once

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Input {

/// What a key or mouse button does. The config key of each action is ActionName().
enum class Action : u8 {
    // DualShock 4 buttons
    Cross,
    Circle,
    Square,
    Triangle,
    L1,
    R1,
    L2,
    R2,
    L3,
    R3,
    Options,
    TouchPad,
    DpadUp,
    DpadDown,
    DpadLeft,
    DpadRight,
    // left stick
    MoveForward,
    MoveBack,
    MoveLeft,
    MoveRight,
    Walk, // left stick at half deflection while held
    // SecondSonPC
    ReleaseMouse, // gives the cursor back (the press itself is not sent to the game)
    MotionMode,   // toggles "controller held sideways": the mouse moves the controller (gyro)
    Shake,        // shakes the controller while held
    TouchDrag,    // while held the mouse drags a finger on the touch pad
    TouchTwoFingers, // while held: two fingers on the touch pad and its click (the game's two-finger click)
    Count
};
constexpr size_t kActionCount = static_cast<size_t>(Action::Count);

/// A physical input: an SDL scancode, a mouse button (SDL button number, 1 = left) or a mouse wheel direction
/// (MouseWheel: 0 up, 1 down, 2 left, 3 right; a notch counts as a press of one controller reading).
struct InputCode {
    enum class Kind : u8 { Key, MouseButton, MouseWheel };
    Kind kind = Kind::Key;
    u16 code = 0;
    bool operator==(const InputCode&) const = default;
};

struct Settings {
    std::array<std::vector<InputCode>, kActionCount> bindings;
    double mouse_sensitivity = 1.0; // camera speed for a given mouse speed
    bool invert_y = false;
    double mouse_deadzone = 0.10; // right-stick deflection a moving mouse starts from (game dead zone)
    double motion_sensitivity = 1.0; // controller rotation per mouse count in motion mode
    bool sideways_right_up = true;   // motion mode: right side of the controller up (else left)
    bool capture_mouse = true;       // false: the mouse is never captured (keyboard only)
};

std::string_view ActionName(Action action);

/// Default layout (docs/INPUT_MAPPING.md).
Settings DefaultSettings();

/// "Space", "W", "Left Shift", "Return"/"Enter", "Escape"/"Esc", "Mouse Left", "Mouse X1", "Wheel Up", ...
/// Case-insensitive.
bool ParseInput(std::string_view name, InputCode& out);
std::string InputName(InputCode input);

} // namespace SecondSon::Input
