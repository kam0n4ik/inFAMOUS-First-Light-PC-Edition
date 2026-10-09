// SecondSonPC — entry point.
//   SecondSonPC.exe [--lang <locale>] [--loader sspc|shadps4] [options]
//     --lang <locale>          overrides [Language] system for this run
//     --loader sspc|shadps4    sspc: SecondSonPC's own Loader (Phase 1);
//                              shadps4: Phase 0 path on shadPS4's Core::Linker (in-binary oracle)
//     --system-name <name>     overrides [System] name
//     --user-data <dir>        overrides [Paths] userData (test runs on a copy of the saves)
//   diagnostics:
//     --no-counter             disables the import call counter
//     --call-order <N>         records the first N import calls -> logs\call_order.txt
//     --reloc-snapshot <file>  writes every relocation slot at the first ParamGetInt call
//     --selftest               NATIVE vs BRIDGE subsystem self-test -> logs\selftest.txt, no game
//     --fault-test             first-chance crash recorder test -> logs\faulttest.txt, no game
//     --no-fault-recorder      no first-chance crash recorder (A/B performance diagnostics)
//     --frame-log              presented frame times -> logs\frametimes.csv (tools\perf_runs.ps1)
//     --vram-log <seconds>     video memory budget/usage samples (SSPC-001)
//     --press <key@sec,...>    automated runs: key presses (SDL key names, e.g. n = cross in the
//                              bridge's default keyboard layout) pushed into the window's event
//                              queue <sec> seconds after the window opens; no window focus needed
//     --bridge <group>         sends a NATIVE group back to BRIDGE (fs, system, ...)
//     --native <group>         enables an EXPERIMENTAL group, BRIDGE by default (kernel-dmem: direct
//                              memory, P2.3.1; --loader sspc only)
//     --memory-map <seconds>   the guest's virtual and direct memory maps -> logs\memory_map.txt
//     --exports all|native|observers|none, --no-bind <name>   which exports are bound
//     --stock-bootstrap        shadPS4's own startup path inside this binary
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include "app/config.h"
#include "app/paths.h"
#include "base/crash.h"
#include "base/log.h"
#include "base/vram_monitor.h"
#include "bridge/shadps4/bootstrap.h"
#include "runtime/language.h"
#include "runtime/system/system_service.h"

namespace {

int Fail(const std::string& message) {
    std::fprintf(stderr, "SecondSonPC: %s\n", message.c_str());
    MessageBoxA(nullptr, message.c_str(), "SecondSonPC", MB_ICONERROR | MB_OK);
    return 1;
}

} // namespace

int main(int argc, char* argv[]) {
    using namespace SecondSon;
    SetConsoleOutputCP(CP_UTF8);

    std::vector<std::string> errors;
    auto config = App::LoadConfig(App::RootDir(), errors);
    if (!config) {
        std::string message = "SecondSonPC.toml (" + (App::RootDir() / "SecondSonPC.toml").string() + "):";
        for (const auto& e : errors) {
            message += "\n  " + e;
        }
        return Fail(message);
    }
    bool stock_bootstrap = false;
    u32 vram_log_seconds = 0; // --vram-log <seconds>: video memory budget/usage samples (SSPC-001)
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--no-counter") {
            config->import_counter = false;
        } else if (arg == "--stock-bootstrap") {
            stock_bootstrap = true; // diagnostics only
        } else if (arg == "--no-bind" && has_value) {
            config->no_bind.emplace_back(argv[++i]); // diagnostics only
        } else if (arg == "--exports" && has_value) {
            const std::string_view mode = argv[++i]; // diagnostics only
            if (mode != "all" && mode != "native" && mode != "observers" && mode != "none") {
                return Fail("--exports must be all|native|observers|none");
            }
            config->bind_natives = mode == "all" || mode == "native";
            config->bind_observers = mode == "all" || mode == "observers";
        } else if (arg == "--lang" && has_value) {
            const auto* lang = Runtime::FindLanguageByLocale(argv[++i]);
            if (lang == nullptr) {
                return Fail(std::string("--lang: unknown locale ") + argv[i]);
            }
            config->language_id = lang->id;
            config->language_locale = std::string(lang->locale);
        } else if (arg == "--loader" && has_value) {
            const std::string_view backend = argv[++i];
            if (backend == "sspc") {
                config->loader = App::Config::LoaderBackend::Sspc;
            } else if (backend == "shadps4") {
                config->loader = App::Config::LoaderBackend::Shadps4;
            } else {
                return Fail("--loader must be sspc|shadps4");
            }
        } else if (arg == "--bridge" && has_value) {
            config->bridged_groups.emplace_back(argv[++i]);
        } else if (arg == "--native" && has_value) {
            config->native_groups.emplace_back(argv[++i]); // experimental groups only
        } else if (arg == "--memory-map" && has_value) {
            config->memory_map_s = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--call-order" && has_value) {
            config->call_order = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--reloc-snapshot" && has_value) {
            config->reloc_snapshot = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--system-name" && has_value) {
            config->system_name = argv[++i];
        } else if (arg == "--user-data" && has_value) {
            config->user_data_dir = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--fault-test") {
            config->fault_test = true;
        } else if (arg == "--no-fault-recorder") {
            config->fault_recorder = false; // diagnostics only
        } else if (arg == "--frame-log") {
            config->frame_log = true; // diagnostics only
        } else if (arg == "--selftest") {
            config->selftest = true;
            config->import_counter = false;
        } else if (arg == "--vram-log" && has_value) {
            vram_log_seconds = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--press" && has_value) {
            config->test_presses = argv[++i];
        } else {
            return Fail(std::string("unknown argument: ") + std::string(arg));
        }
    }

    if (config->selftest && config->loader != App::Config::LoaderBackend::Sspc) {
        return Fail("--selftest compares SecondSonPC's subsystems with shadPS4's; it needs --loader sspc");
    }
    if (!config->native_groups.empty() && config->loader != App::Config::LoaderBackend::Sspc) {
        return Fail("--native enables SecondSonPC code on its own loader; it needs --loader sspc");
    }
    for (const auto& g : config->native_groups) {
        if (std::find(config->bridged_groups.begin(), config->bridged_groups.end(), g) !=
            config->bridged_groups.end()) {
            return Fail("--native " + g + " and --bridge " + g + " contradict each other");
        }
    }
    if (!Log::Init(config->logs_dir / "SecondSonPC.log", config->log_level)) {
        return Fail("cannot open " + (config->logs_dir / "SecondSonPC.log").string());
    }
    Base::InstallCrashLogger(config->logs_dir / "crash", config->crash_dump,
                             config->fault_recorder || config->fault_test);
    if (config->fault_test) {
        return Base::RunFaultSelfTest(config->logs_dir); // before any bridge code: no other handlers
    }
    Base::StartVramLog(vram_log_seconds);
    const bool own_loader = config->loader == App::Config::LoaderBackend::Sspc;
    SSPC_LOG(App, Info, "SecondSonPC Phase 1 — loader: {}",
             own_loader ? "SecondSonPC (sspc)" : "Phase 0 path on the shadPS4 core (shadps4, oracle)");
    SSPC_LOG(App, Info, "root {}, config {}", config->root.string(), config->file.string());
    SSPC_LOG(App, Info, "game {}", config->game_dir.string());
    SSPC_LOG(App, Info, "user data {}, logs {}", config->user_data_dir.string(), config->logs_dir.string());

    const auto* language = Runtime::FindLanguageById(config->language_id);
    SSPC_LOG(Language, Info, "system language {} = PS4 id {} ({}), game tag \"{}\"", language->locale,
             language->id, language->display, language->game_tag);
    if (!Runtime::ReportLanguageFiles(*language, config->game_dir)) {
        SSPC_LOG(Language, Error, "language files for {} are missing in the game folder", language->locale);
    }

    SystemService::Configure(SystemService::Settings{
        .language_id = config->language_id,
        .system_name = config->system_name,
        .enter_button_cross = config->enter_button_cross,
    });
    SSPC_LOG(SystemService, Info, "SystemName \"{}\", enter button {}", config->system_name,
             config->enter_button_cross ? "cross" : "circle");
    SSPC_LOG(Trace, Info, "import counter {}, call order {}, bind natives {}, bind observers {}",
             config->import_counter ? "on" : "off", config->call_order, config->bind_natives,
             config->bind_observers);
    for (const auto& g : config->bridged_groups) {
        SSPC_LOG(App, Warning, "DIAGNOSTIC: --bridge {}", g);
    }
    for (const auto& g : config->native_groups) {
        SSPC_LOG(App, Warning, "EXPERIMENTAL: --native {}", g);
    }

    if (stock_bootstrap) {
        Bridge::RunStockForDiagnostics(*config);
    }
    Bridge::RunGame(*config);
}
