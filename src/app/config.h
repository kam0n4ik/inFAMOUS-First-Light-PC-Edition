// SecondSonPC — SecondSonPC.toml.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "base/abi.h"
#include "base/log.h"
#include "input/settings.h"

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
    // sceSystemServiceParamGetString(SystemName). Empty by default (the Phase 0 value). The early
    // "non-empty name crashes" finding was SSPC-001, the commit-limit failure (docs/KNOWN_ISSUES.md).
    std::string system_name;
    bool enter_button_cross = true;

    // [Console] — the emulated PS4; the game sees these (sceKernelIsNeoMode, memory budget).
    bool neo_mode = false;     // PS4 Pro
    bool dev_kit = false;      // development kit memory budget
    u32 extra_dmem_mib = 0;    // direct memory added to the retail budget
    u32 extra_fmem_mib = 0;    // flexible memory added to the retail budget

    // [Paths]
    std::filesystem::path user_data_dir; // /savedata0 lives under here (never the shadPS4 install)
    std::filesystem::path logs_dir;

    // [Display]
    u32 window_width = 1280;
    u32 window_height = 720;
    bool fullscreen = false;
    u32 vblank_hz = 60;
    std::string present_mode = "Mailbox"; // Vulkan present mode, spelled as the bridge expects
    bool exclusive_fullscreen = false;    // false: borderless window over the desktop
    bool show_fps = false;                // FPS counter of the bridge overlay (F10 toggles)

    // [Audio]
    u32 volume = 100;         // 0..100, applied on top of the game's own volume
    std::string audio_device; // output device name; empty = system default

    // [Debug] — diagnostics of the temporary Vulkan bridge (Phase 0-4)
    bool vulkan_validation = false;
    bool vulkan_crash_diagnostic = false;

    // [Input] + [Keyboard]: the keyboard/mouse controller of player 1 (docs/INPUT_MAPPING.md)
    Input::Settings input = Input::DefaultSettings();

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

    // Phase 1 migration (command line only).
    // --loader: SecondSonPC's own Loader (sspc) or the Phase 0 path on shadPS4's Core::Linker,
    // kept as the in-binary oracle until Phase 1 is complete.
    enum class LoaderBackend { Sspc, Shadps4 };
    LoaderBackend loader = LoaderBackend::Sspc; // default since Phase 1 milestone 1 (docs/MIGRATION.md)
    std::vector<std::string> bridged_groups; // --bridge <group>: NATIVE group back to BRIDGE
    u32 call_order = 0;                      // --call-order N: record the first N import calls
    bool selftest = false;                   // --selftest: NATIVE vs BRIDGE subsystem test, no game
    std::filesystem::path reloc_snapshot;    // --reloc-snapshot <file>: at the first ParamGetInt
    std::string test_presses;                // --press key@sec[,key@sec...]: automated-run input
};

/// Parses <root>\SecondSonPC.toml. On failure returns nullopt and fills `errors`.
std::optional<Config> LoadConfig(const std::filesystem::path& root, std::vector<std::string>& errors);

} // namespace SecondSon::App
