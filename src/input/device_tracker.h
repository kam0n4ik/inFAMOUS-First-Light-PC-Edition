// SecondSonPC — the input device the player uses now (keyboard/mouse or a controller of some kind), for the button
// hints of SecondSonPC's menu and its "active device" line (docs/PHASE4_PC_INPUT_UI_RESEARCH.md §12). Keys and
// buttons switch at once; mouse movement and sticks only past a threshold and not within kHoldNs of the last
// switch, so jitter of a resting stick or a nudged mouse does not flip the hints. Native code, no SDL calls.
#pragma once

#include <string_view>

#include "base/abi.h"

namespace SecondSon::Input {

enum class Device : u8 { KeyboardMouse, PlayStation, Xbox, OtherGamepad };
enum class PromptStyle : u8 { Keyboard, PlayStation, Xbox };

class DeviceTracker {
public:
    static constexpr u64 kHoldNs = 1'500'000'000;     // analog input does not switch sooner than this
    static constexpr double kStickThreshold = 0.5;    // stick deflection (0..1) that counts as controller use
    static constexpr double kMouseCounts = 40.0;      // mouse movement within kMouseWindowNs that counts
    static constexpr u64 kMouseWindowNs = 100'000'000;

    void KeyOrMouseButton(u64 now_ns);
    void MouseMotion(double dx, double dy, u64 now_ns);
    void PadButton(Device pad, u64 now_ns);
    void PadAxis(Device pad, double deflection, u64 now_ns);

    Device Active() const {
        return active_;
    }
    u32 Switches() const {
        return switches_;
    }

private:
    void Use(Device device, u64 now_ns, bool analog);

    Device active_ = Device::KeyboardMouse;
    u64 switched_ns_ = 0;
    bool switched_once_ = false;
    u32 switches_ = 0;
    double mouse_sum_ = 0.0;
    u64 mouse_window_ns_ = 0;
};

/// [Interface] prompts ("auto" | "playstation" | "xbox" | "keyboard") for the active device. "auto": the device's
/// own (a controller that is neither PlayStation nor Xbox: Xbox positions, as SDL names them).
PromptStyle ResolvePrompts(std::string_view setting, Device active);

} // namespace SecondSon::Input
