// SecondSonPC — PC options of a real controller (see gamepad_tuning.h).
#include "input/gamepad_tuning.h"

#include <algorithm>
#include <cmath>

namespace SecondSon::Input {

void TuneRightStick(u8& x, u8& y, const GamepadTuning& tuning) {
    if (tuning.NeutralStick()) {
        return;
    }
    // centre 128 (where a resting stick reports): 128..255 -> 0..1, 0..128 -> -1..0
    const auto in = [](u8 v) { return v >= 128 ? (v - 128) / 127.0 : (v - 128) / 128.0; };
    double vx = in(x);
    double vy = in(y);
    if (tuning.camera_sensitivity != 1.0) {
        vx *= tuning.camera_sensitivity;
        vy *= tuning.camera_sensitivity;
        const double over = std::max(std::abs(vx), std::abs(vy));
        if (over > 1.0) { // keep the direction: both axes shrink together
            vx /= over;
            vy /= over;
        }
    }
    const auto axis = [](double v) {
        return static_cast<u8>(std::clamp(std::lround(128.0 + v * (v >= 0 ? 127.0 : 128.0)), 0L, 255L));
    };
    x = axis(vx);
    y = axis(vy);
    if (tuning.invert_camera_x) {
        x = static_cast<u8>(255 - x);
    }
    if (tuning.invert_camera_y) {
        y = static_cast<u8>(255 - y);
    }
}

u8 ScaleMotor(u8 strength, u32 percent) {
    return static_cast<u8>(static_cast<u32>(strength) * std::min(percent, 100u) / 100u);
}

} // namespace SecondSon::Input
