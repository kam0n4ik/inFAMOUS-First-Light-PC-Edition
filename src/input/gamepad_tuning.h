// SecondSonPC — PC options of a real controller ([Gamepad] in SecondSonPC.toml): right stick (camera) sensitivity and
// inversion, rumble strength. Applied by libScePad (src/runtime/pad) to what the controller reports and to what the
// game asks of its motors. Neutral values change nothing (bit for bit).
#pragma once

#include "base/abi.h"

namespace SecondSon::Input {

struct GamepadTuning {
    double camera_sensitivity = 1.0; // right stick deflection multiplier
    bool invert_camera_x = false;
    bool invert_camera_y = false;
    u32 vibration_percent = 100; // 0 = off

    bool NeutralStick() const {
        return camera_sensitivity == 1.0 && !invert_camera_x && !invert_camera_y;
    }
};

/// The right stick of a real controller (0..255, centre 127.5): the deflection scaled about the centre, its
/// direction kept (clamped into the square the stick reports), then mirrored per axis.
void TuneRightStick(u8& x, u8& y, const GamepadTuning& tuning);

/// Motor strength the game asks for, scaled by `percent` (0..100).
u8 ScaleMotor(u8 strength, u32 percent);

} // namespace SecondSon::Input
