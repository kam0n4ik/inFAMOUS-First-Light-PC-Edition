// SecondSonPC — TEMPORARY bridge for the native input (Phase 2, P2.1).
//  * The shadPS4 window hands every SDL event to SecondSonPC's keyboard/mouse layer first (patch
//    0006). Keyboard and mouse events never reach shadPS4's input handler, so its keyboard layout
//    (runtime\sspc\user\input_config) no longer drives any controller.
//  * Real controllers stay on shadPS4's SDL controller layer (Input::GameControllers: opening and
//    hot-plug, button mapping, gyro integration, touch pad, light bar, rumble) exactly as before;
//    the native libScePad reads them through Pad::Host until SecondSonPC has its own controller
//    backend (Phase 3).
#include <SDL3/SDL_events.h>

#include "app/config.h"
#include "base/log.h"
#include "bridge/shadps4/loader_host.h"
#include "common/elf_info.h"
#include "common/singleton.h"
#include "core/libraries/kernel/time.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "input/input_handler.h"
#include "input/keyboard_mouse.h"
#include "runtime/pad/pad.h"
#include "sdl_window.h"

namespace SecondSon::Bridge {

namespace {

::Input::GameController* Controller(s32 slot) {
    return (*Common::Singleton<::Input::GameControllers>::Instance())[static_cast<size_t>(slot)];
}

class PadHostImpl final : public Pad::Host {
public:
    u64 ProcessTimeUs() override {
        return Libraries::Kernel::sceKernelGetProcessTime();
    }

    bool ReadPhysical(s32 slot, Pad::PhysicalState& out) override {
        auto* c = Controller(slot);
        if (c->m_sdl_gamepad == nullptr) {
            return false; // the keyboard-only "controller" of slot 1 is not a real one
        }
        const ::Input::State s = c->ReadState();
        if (!s.connected) {
            return false;
        }
        // Same conversion as the BRIDGE libScePad (core/libraries/pad/pad.cpp, ProcessStates).
        using ::Input::Axis;
        const auto axis = [&s](Axis a) { return static_cast<u8>(s.axes[static_cast<int>(a)]); };
        out.buttons = static_cast<u32>(s.buttonsState);
        out.lx = axis(Axis::LeftX);
        out.ly = axis(Axis::LeftY);
        out.rx = axis(Axis::RightX);
        out.ry = axis(Axis::RightY);
        out.l2 = axis(Axis::TriggerLeft);
        out.r2 = axis(Axis::TriggerRight);
        out.acceleration = {static_cast<float>(s.acceleration.x * 0.098),
                            static_cast<float>(s.acceleration.y * 0.098),
                            static_cast<float>(s.acceleration.z * 0.098)};
        out.angular_velocity = {s.angularVelocity.x, s.angularVelocity.y, s.angularVelocity.z};
        out.orientation = {s.orientation.x, s.orientation.y, s.orientation.z, s.orientation.w};
        out.touch_num = (s.touchpad[0].state ? 1 : 0) + (s.touchpad[1].state ? 1 : 0);
        const auto touch = [](const ::Input::TouchpadEntry& t) {
            return Pad::PhysicalState::Touch{t.x, t.y, t.ID};
        };
        if (!s.touchpad[0].state && s.touchpad[1].state) {
            out.touch[0] = touch(s.touchpad[1]);
        } else {
            out.touch[0] = touch(s.touchpad[0]);
            out.touch[1] = touch(s.touchpad[1]);
        }
        out.time_since_touch = static_cast<u32>(s.touch_time_since_held_down);
        out.connected_count = s.connected_count;
        return true;
    }

    void SetLightBar(s32 slot, u8 r, u8 g, u8 b) override {
        Controller(slot)->SetLightBarRGB(r, g, b);
    }

    void SetVibration(s32 slot, u8 large_motor, u8 small_motor) override {
        Controller(slot)->SetVibration(small_motor, large_motor);
    }

    void ResetOrientation(s32 slot) override {
        Controller(slot)->ResetOrientation();
    }

    bool OverlayHasInput() override {
        return ImGui::Core::IsGamepadInputCaptured();
    }

    bool ReportsTouchTime() override {
        return Common::ElfInfo::Instance().FirmwareVer() > Common::ElfInfo::FW_350;
    }
};

bool InterceptWindowEvent(const SDL_Event& event) {
    const auto r = Input::Player1().OnEvent(event);
    if (r.hotkey == Input::KeyboardMouse::Hotkey::ToggleFps) {
        SDL_Event fps{};
        fps.type = SDL_EVENT_TOGGLE_SIMPLE_FPS; // the bridge overlay's FPS counter
        SDL_PushEvent(&fps);
    }
    return r.consumed;
}

} // namespace

void StartControllers() {
    // What libScePad's RegisterLib starts in shadPS4: opens the connected controllers; the first
    // check logs in the user of slot 1 even without any controller (the keyboard player).
    Common::Singleton<::Input::GameControllers>::Instance()->TryOpenSDLControllers();
}

void InstallNativeInput(const App::Config& cfg, SDL_Window* window, bool test_mode) {
    static PadHostImpl host;
    Pad::SetHost(&host);
    Pad::SetInputLog(cfg.input_log);
    Input::Player1().Configure(cfg.input);
    Input::Player1().AttachWindow(window, test_mode);
    if (test_mode) {
        Input::Player1().AcceptOnlyScriptedInput(); // --press pushes everything the run needs
    }
    Frontend::SetEventInterceptor(&InterceptWindowEvent);
    SSPC_LOG(Input, Info,
             "native libScePad: keyboard/mouse (SecondSonPC) + real controllers (bridge SDL layer); "
             "window keyboard/mouse events go to SecondSonPC only");
}

} // namespace SecondSon::Bridge
