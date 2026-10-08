// SecondSonPC — motion sensors of the virtual DualShock 4 (keyboard/mouse player). The controller
// has a pose (yaw, pitch, roll about fixed reference axes); orientation, gravity and angular
// velocity are all derived from that one pose, so they always agree with each other the way a real
// controller's sensors do. Conventions are those libScePad reports for a real controller through
// the bridge (SDL axes: +X right, +Y up, +Z towards the player; gyro in rad/s in the controller's
// frame; acceleration in g with gravity read as +Y at rest; orientation integrated from the gyro
// starting at identity = flat, facing the screen).
#pragma once

#include "base/abi.h"

namespace SecondSon::Input {

struct Vec3 {
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct Quat {
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
};

/// 1 g as libScePad reports it for a real controller on the bridge (SDL m/s^2 x 0.098, computed
/// the way the bridge does: float 9.81 times double 0.098, stored as float).
constexpr float kGravity = static_cast<float>(9.81f * 0.098);

struct MotionState {
    bool active = false; // away from rest (sideways, returning, shaking): this state wins over a
                         // physical controller's sensors
    Quat orientation;
    Vec3 acceleration{0.0f, kGravity, 0.0f};
    Vec3 angular_velocity;
};

class VirtualMotion {
public:
    /// Motion mode: the controller turns sideways (roll 90 deg, right or left side up); off: back to
    /// rest. Both move at kTurnRate, the way a hand turns a controller.
    void SetSideways(bool on, bool right_side_up);
    /// Motion mode: turns the controller by these angles (radians) about the reference vertical
    /// (yaw, positive = to the left) and horizontal (pitch, positive = front up) axes.
    void Turn(double yaw, double pitch);
    void SetShake(bool on);
    /// scePadResetOrientation / scePadOpen: the current pose becomes the reference.
    void Reset();
    /// Advances the pose to `now_s` (seconds, any monotonic clock) and returns the sensors.
    MotionState Step(double now_s);

    bool Sideways() const {
        return sideways_;
    }

    static constexpr double kTurnRate = 6.283185307179586; // rad/s: a quarter turn in 0.25 s
    static constexpr double kMaxPitch = 1.3;                // ~75 deg
    // Shaking a spray can: the hand moves up and down while the wrist rocks the can.
    static constexpr double kShakeHz = 5.0;
    static constexpr float kShakeG = 2.5f;        // peak vertical acceleration of the hand, in g
    static constexpr double kShakeAngle = 0.35;    // wrist rocking amplitude (pitch), rad (~20 deg)
    static constexpr double kShakeRamp = 0.1;      // s to start / stop shaking

private:
    double yaw_ = 0.0, pitch_ = 0.0, roll_ = 0.0;
    double target_roll_ = 0.0;
    bool sideways_ = false;
    bool shake_ = false;
    double shake_amount_ = 0.0; // 0..1, ramps with kShakeRamp
    double shake_time_ = 0.0;   // s since the shake started (phase of the wave)
    double last_time_ = -1.0;
    Quat last_q_;
};

// Quaternion helpers (also used by the self-test).
Quat Multiply(const Quat& a, const Quat& b);
Quat Conjugate(const Quat& q);
Quat AxisAngle(float ax, float ay, float az, double angle);
/// Rotates v by q (q * v * q^-1).
Vec3 Rotate(const Quat& q, const Vec3& v);

} // namespace SecondSon::Input
