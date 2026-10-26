// SecondSonPC — the launcher: the window SecondSonPC.exe opens when it is started without arguments (the HTML
// mockup, 1:1). Home: the art, Play, the launch profile; the sections: the game's PC settings (the registry,
// the user settings file through its own Settings::Service, saved per section), the key bindings, the experiments.
// Play starts the game (SecondSonPC.exe --no-launcher) with play.cmd's preset or play_experimental.cmd's and the
// saved settings. Main thread only. Own code.
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "launcher/ui.h"

namespace SecondSon::Launcher {

struct AppOptions {
    std::filesystem::path settings_file; // the user settings file ("" : %LOCALAPPDATA%\SecondSonPC\settings.toml)
    bool read_only = false;              // screenshots: nothing is saved (settings, launcher.toml), nothing started
    std::string page;                    // the section shown first ("home", "display", ... "advanced")
    std::string tab;                     // controls: "kbm" | "pad"
    bool open_experiments = false;       // advanced: the experiments shown open
    float scroll = 0;                    // screenshots: the section scrolled down by this many pixels
    bool no_art = false;                 // screenshots: the home page without the game's art (README)
    std::vector<std::string> game_args;  // passed to the game after --no-launcher
};

enum class WindowRequest : u8 { None, Minimize, Maximize, Close };

class LauncherApp {
public:
    explicit LauncherApp(AppOptions options);
    ~LauncherApp();
    LauncherApp(const LauncherApp&) = delete;
    LauncherApp& operator=(const LauncherApp&) = delete;

    /// The PC, the settings, the launch profile. false + `error`: nothing to show (the window is not opened).
    bool Init(std::string& error);
    /// One frame of the whole window (`width` x `height` DIPs).
    void Frame(Ui& ui, float width, float height, bool maximized);
    WindowRequest TakeRequest();
    /// The title bar's free part: the window is dragged from it (WM_NCHITTEST).
    bool CaptionAt(float x, float y, float width) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/// SecondSonPC.exe without arguments, or with --launcher [game arguments]: the launcher's window until it is closed.
/// --launcher-shot <png> [--launcher-page <section>] [--launcher-tab kbm|pad] [--launcher-experiments]
/// [--launcher-scroll <px>] [--launcher-no-art] [--settings <file>]: one frame offscreen at 1100 x 720 into a PNG (comparison with the mockup), nothing saved.
int Run(int argc, char* argv[]);

} // namespace SecondSon::Launcher
