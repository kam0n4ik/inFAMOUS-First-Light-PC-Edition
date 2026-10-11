// SecondSonPC — the input device in use (see device_tracker.h).
#include "input/device_tracker.h"

#include <cmath>

namespace SecondSon::Input {

void DeviceTracker::Use(Device device, u64 now_ns, bool analog) {
    if (device == active_) {
        return;
    }
    if (analog && switched_once_ && now_ns >= switched_ns_ && now_ns - switched_ns_ < kHoldNs) {
        return;
    }
    active_ = device;
    switched_ns_ = now_ns;
    switched_once_ = true;
    ++switches_;
}

void DeviceTracker::KeyOrMouseButton(u64 now_ns) {
    Use(Device::KeyboardMouse, now_ns, false);
}

void DeviceTracker::MouseMotion(double dx, double dy, u64 now_ns) {
    if (now_ns < mouse_window_ns_ || now_ns - mouse_window_ns_ > kMouseWindowNs) {
        mouse_window_ns_ = now_ns;
        mouse_sum_ = 0.0;
    }
    mouse_sum_ += std::abs(dx) + std::abs(dy);
    if (mouse_sum_ >= kMouseCounts) {
        Use(Device::KeyboardMouse, now_ns, true);
    }
}

void DeviceTracker::PadButton(Device pad, u64 now_ns) {
    Use(pad, now_ns, false);
}

void DeviceTracker::PadAxis(Device pad, double deflection, u64 now_ns) {
    if (deflection >= kStickThreshold) {
        Use(pad, now_ns, true);
    }
}

PromptStyle ResolvePrompts(std::string_view setting, Device active) {
    if (setting == "playstation") {
        return PromptStyle::PlayStation;
    }
    if (setting == "xbox") {
        return PromptStyle::Xbox;
    }
    if (setting == "keyboard") {
        return PromptStyle::Keyboard;
    }
    switch (active) {
    case Device::KeyboardMouse:
        return PromptStyle::Keyboard;
    case Device::PlayStation:
        return PromptStyle::PlayStation;
    case Device::Xbox:
    case Device::OtherGamepad:
        return PromptStyle::Xbox;
    }
    return PromptStyle::Keyboard;
}

} // namespace SecondSon::Input
