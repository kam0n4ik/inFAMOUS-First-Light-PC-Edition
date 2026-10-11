// SecondSonPC — the in-game settings menu as a model (docs/PC_SETTINGS_IMPLEMENTATION.md §6): categories, rows built
// from the settings registry, focus and navigation from the keyboard, a controller or the mouse, editing (toggles,
// choices, sliders, key capture), confirmation of display changes, conflicts, and the layout it draws into a
// DrawList. It knows nothing of SDL, Win32 or the GPU: the host (ui/menu_controller.*) feeds it input and renders the
// list. Main thread only. Own code.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "input/device_tracker.h"
#include "input/settings.h"
#include "settings/settings_service.h"
#include "ui/draw_list.h"

namespace SecondSon::Ui {

enum class Nav : u8 {
    Up,
    Down,
    Left,
    Right,
    Accept,
    Back,    // closes a dialog or the menu
    PrevTab, // previous category
    NextTab,
    Reset,   // the focused setting (or key) back to its default
    Clear,   // a key binding slot emptied
    PageUp,
    PageDown,
};

/// What the host knows that the menu shows; set before input and Build.
struct MenuEnv {
    float width = 1920.0f; // client area in pixels
    float height = 1080.0f;
    bool ru = true;
    Input::PromptStyle prompts = Input::PromptStyle::Keyboard;
    bool confirm_is_circle = false;         // [System] enterButton = circle: ○ confirms, ✕ goes back
    std::string device_label;               // the active input device
    std::string backend_label;              // the GPU backend of this run
    std::vector<std::string> displays;      // display names
    std::vector<std::string> audio_devices; // playback device names
    std::vector<std::pair<u32, u32>> window_sizes;
    u64 now_ns = 0;
};

class SettingsMenu {
public:
    static constexpr u64 kConfirmNs = 15'000'000'000; // display changes are reverted unless kept in this time

    explicit SettingsMenu(Settings::Service& service);

    void Open();
    void Close();
    bool IsOpen() const {
        return open_;
    }
    /// The user asked to close (Back at the top, the close button): taken once by the host.
    bool TakeCloseRequest();

    void SetEnv(MenuEnv env);
    void OnNav(Nav nav);
    void OnMouseMove(float x, float y);
    void OnMouseButton(float x, float y, bool down);
    void OnWheel(float notches);
    /// Waiting for a key or mouse button to bind: the host sends them to OnCaptureInput (Esc: Nav::Back cancels).
    bool Capturing() const {
        return capture_.has_value();
    }
    void OnCaptureInput(Input::InputCode input);
    void Tick(u64 now_ns);
    bool NeedsTicks() const; // a countdown is running
    void Build(TextMeasure& measure, DrawList& out);

    Settings::Category CurrentCategory() const {
        return category_;
    }
    void SelectCategory(Settings::Category category);
    /// The focused row: a setting id, or "window.size", "reset.category", "info.*".
    std::string FocusedId() const;
    int FocusedSlot() const {
        return slot_;
    }
    bool ConfirmPending() const {
        return modal_ == Modal::ConfirmDisplay;
    }
    bool ResetPending() const {
        return modal_ == Modal::ConfirmReset;
    }
    const std::string& Status() const {
        return status_;
    }
    /// Focus a row by id in the current category (tests, the host).
    bool Focus(std::string_view id);

private:
    struct Row {
        enum class Kind : u8 { Header, Setting, WindowSize, Info, ResetCategory };
        Kind kind = Kind::Setting;
        const Settings::Desc* desc = nullptr;
        std::string_view en, ru; // Header/Info/ResetCategory label
        std::string tag;         // FocusedId of non-setting rows
    };
    enum class Modal : u8 { None, ConfirmDisplay, ConfirmReset };
    struct Capture {
        Input::Action action;
        size_t slot;
    };
    struct Hit {
        enum class Kind : u8 { Category, Row, Close, ModalButton };
        enum class Part : u8 { Body, Prev, Next, Value, Track, Slot0, Slot1, Button };
        Kind kind;
        Rect rect;
        int index = 0;
        Part part = Part::Body;
    };

    std::string_view T(std::string_view en, std::string_view ru) const {
        return env_.ru ? ru : en;
    }
    void RebuildRows();
    bool Focusable(size_t row) const;
    void MoveFocus(int delta);
    void Adjust(int dir);           // Left/Right on the focused row
    void Activate();                // Accept on the focused row
    void ResetFocused();
    void SetValue(const Settings::Desc& desc, const Settings::Value& value);
    void SetWindowSize(u32 w, u32 h);
    void StepWindowSize(int dir);
    void StepText(const Settings::Desc& desc, int dir);
    void SetSliderFromX(size_t row, float x);
    std::vector<std::string> TextOptions(const Settings::Desc& desc) const;
    void BeginDisplayChange();      // remember the display settings before a change
    void EndDisplayChange(bool changed);
    void RevertDisplay();
    void ModalAccept(int button);   // 0 = keep / yes, 1 = revert / no
    std::string ValueText(const Settings::Desc& desc, const Settings::Value& value) const;
    std::string BindingText(const Input::InputCode& input) const;

    Settings::Service& service_;
    MenuEnv env_;
    bool open_ = false;
    bool close_request_ = false;
    Settings::Category category_ = Settings::Category::Display;
    std::vector<Row> rows_;
    size_t focus_ = 0;
    int slot_ = 0;
    int scroll_ = 0;         // first visible row
    int visible_rows_ = 8;   // from the last Build
    int hover_row_ = -1;
    int dragging_row_ = -1;  // a slider being dragged with the mouse
    std::optional<Capture> capture_;
    Modal modal_ = Modal::None;
    int modal_button_ = 0;
    u64 modal_deadline_ns_ = 0;
    std::vector<std::pair<const Settings::Desc*, Settings::Value>> display_before_;
    std::vector<std::pair<const Settings::Desc*, Settings::Value>> pending_display_;
    std::string status_;
    std::vector<Hit> hits_;
    Rect slider_track_; // the dragged slider's track (from the last Build)
    float track_x_ = 0, track_w_ = 1;
};

} // namespace SecondSon::Ui
