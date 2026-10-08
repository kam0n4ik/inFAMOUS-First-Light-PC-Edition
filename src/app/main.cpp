// SecondSonPC — entry point.
//   SecondSonPC.exe [--lang <locale>] [--loader sspc|shadps4] [options]
//     --lang <locale>          overrides [Language] system for this run
//     --loader sspc|shadps4    sspc: SecondSonPC's own Loader (Phase 1);
//                              shadps4: Phase 0 path on shadPS4's Core::Linker (in-binary oracle)
//     --system-name <name>     overrides [System] name (reproduces SSPC-001 when non-empty)
//   diagnostics:
//     --no-counter             disables the import call counter
//     --call-order <N>         records the first N import calls -> logs\call_order.txt
//     --reloc-snapshot <file>  writes every relocation slot at the first ParamGetInt call
//     --bridge <group>         sends a NATIVE group back to BRIDGE (fs, system, ...)
//     --exports all|native|observers|none, --no-bind <name>   which exports are bound
//     --stock-bootstrap        shadPS4's own startup path inside this binary
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
        } else if (arg == "--call-order" && has_value) {
            config->call_order = static_cast<u32>(std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--reloc-snapshot" && has_value) {
            config->reloc_snapshot = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--system-name" && has_value) {
            config->system_name = argv[++i];
        } else {
            return Fail(std::string("unknown argument: ") + std::string(arg));
        }
    }

    if (!Log::Init(config->logs_dir / "SecondSonPC.log", config->log_level)) {
        return Fail("cannot open " + (config->logs_dir / "SecondSonPC.log").string());
    }
    Base::InstallCrashLogger();
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
    if (!config->system_name.empty()) {
        SSPC_LOG(SystemService, Warning,
                 "non-empty SystemName: known crash after the settings save (docs/KNOWN_ISSUES.md "
                 "SSPC-001, reproducer tools/repro_sspc001.ps1)");
    }
    SSPC_LOG(Trace, Info, "import counter {}, call order {}, bind natives {}, bind observers {}",
             config->import_counter ? "on" : "off", config->call_order, config->bind_natives,
             config->bind_observers);
    for (const auto& g : config->bridged_groups) {
        SSPC_LOG(App, Warning, "DIAGNOSTIC: --bridge {}", g);
    }

    if (stock_bootstrap) {
        Bridge::RunStockForDiagnostics(*config);
    }
    Bridge::RunGame(*config);
}
