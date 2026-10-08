// SecondSonPC — virtual DualShock 4 motion sensors (see motion.h for the conventions).
#include "input/motion.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace SecondSon::Input {

namespace {

double Approach(double value, double target, double max_step) {
    if (value < target) {
        return std::min(value + max_step, target);
    }
    return std::max(value - max_step, target);
}

Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

} // namespace

Quat Multiply(const Quat& a, const Quat& b) {
    // Hamilton product, the convention of the bridge's gyro integration (q' = q * omega / 2).
    return {
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

Quat Conjugate(const Quat& q) {
    return {-q.x, -q.y, -q.z, q.w};
}

Quat AxisAngle(float ax, float ay, float az, double angle) {
    const auto s = static_cast<float>(std::sin(angle / 2.0));
    return {ax * s, ay * s, az * s, static_cast<float>(std::cos(angle / 2.0))};
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 c = Cross(u, v);
    const Vec3 t{2.0f * c.x, 2.0f * c.y, 2.0f * c.z};
    const Vec3 c2 = Cross(u, t);
    return {v.x + q.w * t.x + c2.x, v.y + q.w * t.y + c2.y, v.z + q.w * t.z + c2.z};
}

void VirtualMotion::SetSideways(bool on, bool right_side_up) {
    sideways_ = on;
    // +roll about +Z (towards the player) turns the right side up.
    target_roll_ = on ? (right_side_up ? 1.0 : -1.0) * std::numbers::pi / 2.0 : 0.0;
}

void VirtualMotion::Turn(double yaw, double pitch) {
    constexpr double kMaxYaw = 0.9 * std::numbers::pi; // never wraps, so the way back is unambiguous
    yaw_ = std::clamp(yaw_ + yaw, -kMaxYaw, kMaxYaw);
    pitch_ = std::clamp(pitch_ + pitch, -kMaxPitch, kMaxPitch);
}

void VirtualMotion::SetShake(bool on) {
    shake_ = on;
}

void VirtualMotion::Reset() {
    yaw_ = pitch_ = roll_ = 0.0;
    target_roll_ = 0.0;
    sideways_ = false;
    shake_ = false;
    shake_amount_ = 0.0;
    shake_time_ = 0.0;
    last_q_ = {};
    last_time_ = -1.0;
}

MotionState VirtualMotion::Step(double now_s) {
    const double dt = last_time_ < 0.0 ? 0.0 : std::clamp(now_s - last_time_, 0.0, 0.1);
    const double max_step = kTurnRate * dt;
    roll_ = Approach(roll_, target_roll_, max_step);
    if (!sideways_) {
        yaw_ = Approach(yaw_, 0.0, max_step);
        pitch_ = Approach(pitch_, 0.0, max_step);
    }
    // The shake starts and stops within kShakeRamp, so there is no jump in the pose.
    shake_amount_ = Approach(shake_amount_, shake_ ? 1.0 : 0.0, dt / kShakeRamp);
    shake_time_ = shake_amount_ > 0.0 ? shake_time_ + dt : 0.0;
    const double wave = shake_amount_ * std::sin(2.0 * std::numbers::pi * kShakeHz * shake_time_);
    // Roll about the controller's own axis, then pitch (with the wrist's rocking while shaking)
    // and yaw about the fixed reference axes.
    const double pitch = pitch_ + kShakeAngle * wave;
    const Quat q = Multiply(Multiply(AxisAngle(0, 1, 0, yaw_), AxisAngle(1, 0, 0, pitch)),
                            AxisAngle(0, 0, 1, roll_));

    MotionState s;
    s.orientation = q;
    if (dt > 0.0) {
        // Rotation over dt in the controller's frame: q = last * d, d = exp(omega * dt / 2).
        Quat d = Multiply(Conjugate(last_q_), q);
        if (d.w < 0.0f) {
            d = {-d.x, -d.y, -d.z, -d.w};
        }
        const double len = std::sqrt(double(d.x) * d.x + double(d.y) * d.y + double(d.z) * d.z);
        if (len > 1e-9) {
            const double scale = 2.0 * std::atan2(len, double(d.w)) / (len * dt);
            s.angular_velocity = {static_cast<float>(d.x * scale), static_cast<float>(d.y * scale),
                                  static_cast<float>(d.z * scale)};
        }
    }
    // An accelerometer reads the reaction to gravity (up) plus the hand's own acceleration.
    const Vec3 up{0.0f, kGravity * (1.0f + kShakeG * static_cast<float>(wave)), 0.0f};
    s.acceleration = Rotate(Conjugate(q), up);

    const auto moved = [](double v) { return std::abs(v) > 1e-6; };
    const auto& w = s.angular_velocity;
    s.active = sideways_ || shake_ || shake_amount_ > 0.0 || moved(roll_) || moved(yaw_) ||
               moved(pitch_) || moved(w.x) || moved(w.y) || moved(w.z);
    last_q_ = q;
    last_time_ = now_s;
    return s;
}

} // namespace SecondSon::Input
