// SecondSonPC — the host of the in-game settings menu (docs/PC_SETTINGS_IMPLEMENTATION.md §7): window events in
// (before the keyboard/mouse controller), hotkeys (F1 menu, F10 FPS counter, F11 full screen) and the controller's
// menu button, routing to the menu while it is open (the game then reads an "intercepted" pad and the cursor is free),
// the active device, the Win32 overlay, and the appliers SecondSonPC owns (window, keyboard/mouse, real controllers).
// Independent of the bridge and of the GPU backend. Main thread. Own code.
#pragma once

#include <string>

#include "base/abi.h"

union SDL_Event;
struct SDL_Window;

namespace SecondSon::Ui {

enum class EventRoute : u8 {
    Normal,     // not the menu's: the keyboard/mouse controller and the bridge as before
    Consumed,   // the menu's: nobody else sees it
    BridgeOnly, // the bridge must see it (window, controller state) but the keyboard/mouse controller must not
};

class MenuController {
public:
    struct Options {
        SDL_Window* window = nullptr;
        bool test_mode = false;     // automated runs: the menu and its hotkeys are off (input stays scripted)
        u32 capabilities = 0;       // Settings::Capability bits of this run
        std::string backend_label;  // shown in the Graphics section
        bool confirm_is_circle = false;
    };
    /// Main thread, once the window exists. Registers the appliers, applies the start-up placement.
    void Install(const Options& options);
    bool Installed() const {
        return installed_;
    }
    /// Main thread: every event of the game window, first.
    EventRoute OnEvent(const SDL_Event& event);
    bool IsOpen() const;
    void Open();
    void Close();

private:
    bool installed_ = false;
};

MenuController& Menu();

} // namespace SecondSon::Ui
