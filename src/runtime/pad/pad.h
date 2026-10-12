// SecondSonPC — libScePad (NATIVE since Phase 2, P2.1): the seven functions the game imports.
// Player 1's controller is the keyboard/mouse (src/input) merged with a real controller in the
// same slot (DualShock 4, DualSense, Xbox, ... read through the bridge's SDL controller layer until
// SecondSonPC has its own, Phase 3). Behaviour of the functions follows the Phase 0 oracle
// (third_party/shadPS4/src/core/libraries/pad/pad.cpp @ 0fe263a).
#pragma once

#include <array>
#include <span>

#include "base/abi.h"
#include "input/gamepad_tuning.h"
#include "input/motion.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Pad {

constexpr s32 kSlots = 4; // player slots 1..4 = indices 0..3

/// A real controller as libScePad reports it (units already converted: g, rad/s, touch pad
/// coordinates).
struct PhysicalState {
    u32 buttons = 0;
    u8 lx = 128, ly = 128, rx = 128, ry = 128;
    u8 l2 = 0, r2 = 0;
    Input::Quat orientation;
    Input::Vec3 acceleration;
    Input::Vec3 angular_velocity;
    u8 touch_num = 0;
    struct Touch {
        u16 x = 0, y = 0;
        u8 id = 0;
    };
    std::array<Touch, 2> touch{};
    u32 time_since_touch = 0; // us the touch pad has been held
    u8 connected_count = 0;
};

/// What the native libScePad needs from the bridge (real controllers, clock, overlay).
class Host {
public:
    virtual ~Host() = default;
    /// libkernel process time in microseconds (pad timestamps; the clock the game reads).
    virtual u64 ProcessTimeUs() = 0;
    /// false: no real controller in this slot.
    virtual bool ReadPhysical(s32 slot, PhysicalState& out) = 0;
    virtual void SetLightBar(s32 slot, u8 r, u8 g, u8 b) = 0;
    virtual void SetVibration(s32 slot, u8 large_motor, u8 small_motor) = 0;
    virtual void ResetOrientation(s32 slot) = 0;
    /// The bridge overlay navigates with the controller: the game gets "intercepted" data.
    virtual bool OverlayHasInput() = 0;
    /// touchData.time_since_touch_held_down is filled for games built for firmware > 3.50.
    virtual bool ReportsTouchTime() = 0;
};
void SetHost(Host* host);
/// [Gamepad] PC options of the real controllers (right stick sensitivity/inversion, rumble strength). Any thread.
void SetGamepadTuning(const Input::GamepadTuning& tuning);
/// SecondSonPC's settings menu is open (or a controller button that closed it is still held): the game reads
/// "intercepted" pads, as when the bridge overlay navigates. Any thread.
void SetMenuBlocksInput(bool blocked);
/// [Debug] inputLog: once a second while player 1's right stick or mouse is active, what
/// scePadReadState hands the game and what the game's stick filter made of it.
void SetInputLog(bool on);

std::span<const Runtime::Export> PadExports();

} // namespace SecondSon::Pad
