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

    // [Debug] — diagnostics of the temporary Vulkan bridge (Phase 0-4) and of the input
    bool vulkan_validation = false;
    bool vulkan_crash_diagnostic = false;
    bool input_log = false; // keyboard/mouse -> libScePad -> the game's stick filter, once a second
    bool crash_dump = true; // minidump of the first execute fault -> logs\crash (SSPC-002)

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
    std::vector<std::string> native_groups;  // --native <group>: EXPERIMENTAL group enabled (BRIDGE by default);
                                             // --native memory: SecondSonPC's memory manager (P2.3.4)
    bool memory_commit_on_demand = false;    // --memory-commit on-demand: with --native memory, physical memory
                                             // charged to the system commit when handed out (E1), not at start
    u32 memory_map_s = 0;                    // --memory-map <s>: guest memory maps -> logs\memory_map.txt
    u32 call_order = 0;                      // --call-order N: record the first N import calls
    bool selftest = false;                   // --selftest: NATIVE vs BRIDGE subsystem test, no game
    bool fault_test = false;                 // --fault-test: crash recorder test, no game, no bridge
    bool fault_recorder = true;              // --no-fault-recorder: no first-chance handler (A/B diagnostics)
    bool frame_log = false;                  // --frame-log: presented frame times -> logs\frametimes.csv
    bool track_stats = false;                // --track-stats: GPU write tracking statistics (P2.5 diagnostics)
    std::filesystem::path reloc_snapshot;    // --reloc-snapshot <file>: at the first ParamGetInt
    std::string test_presses;                // --press key@sec[,key@sec...]: automated-run input
    // [ShaderCache]: translated shaders and pipelines kept between runs (bridge, patch 0013) in
    // <dir>\<game>\<generation>-<device>; --shader-cache <dir> / --no-shader-cache for test runs.
    bool shader_cache = true;
    std::filesystem::path shader_cache_dir;
    bool clear_shader_cache = false;         // --clear-shader-cache: delete <dir>\<game> and exit
};

/// Parses <root>\SecondSonPC.toml. On failure returns nullopt and fills `errors`.
std::optional<Config> LoadConfig(const std::filesystem::path& root, std::vector<std::string>& errors);

} // namespace SecondSon::App
