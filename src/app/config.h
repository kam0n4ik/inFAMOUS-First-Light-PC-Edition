// SecondSonPC — SecondSonPC.toml.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "base/abi.h"
#include "base/log.h"

namespace SecondSon::App {

struct Config {
    std::filesystem::path root;
    std::filesystem::path file;

    // [Game]
    std::filesystem::path game_dir;

    // [Language]
    std::string language_locale; // "ru-RU"
    int language_id = 1;         // PS4 id, 8 = Russian

    // [System]
    // sceSystemServiceParamGetString(SystemName). Empty by default: a non-empty name makes the game
    // take a path that crashes in the Phase 0 bridge (docs/KNOWN_ISSUES.md, SSPC-001).
    std::string system_name;
    bool enter_button_cross = true;

    // [Paths]
    std::filesystem::path user_data_dir; // /savedata0 lives under here (never the shadPS4 install)
    std::filesystem::path logs_dir;

    // [Display]
    u32 window_width = 1280;
    u32 window_height = 720;
    bool fullscreen = false;
    u32 vblank_hz = 60;

    // [Input] (Phase 3, parsed and validated now)
    double mouse_sensitivity = 1.0;
    bool invert_y = false;

    // [Log]
    Log::Level log_level = Log::Level::Info;
    std::string bridge_log_filter = "*:info"; // temporary shadPS4 core log (Phase 0-4)

    // [Trace]
    bool import_counter = true;
    u32 dump_interval_s = 10;

    // Diagnostics (command line only): which SecondSonPC exports are bound.
    bool bind_natives = true;
    bool bind_observers = true;
    std::vector<std::string> no_bind; // export names excluded with --no-bind <name>
};

/// Parses <root>\SecondSonPC.toml. On failure returns nullopt and fills `errors`.
std::optional<Config> LoadConfig(const std::filesystem::path& root, std::vector<std::string>& errors);

} // namespace SecondSon::App
