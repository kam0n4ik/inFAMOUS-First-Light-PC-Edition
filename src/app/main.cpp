// SecondSonPC — entry point.
//   SecondSonPC.exe [--lang <locale>]      (--lang overrides [Language] system for this run)
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

#include "app/config.h"
#include "app/paths.h"
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
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--lang" && i + 1 < argc) {
            const auto* lang = Runtime::FindLanguageByLocale(argv[++i]);
            if (lang == nullptr) {
                return Fail(std::string("--lang: unknown locale ") + argv[i]);
            }
            config->language_id = lang->id;
            config->language_locale = std::string(lang->locale);
        } else {
            return Fail(std::string("unknown argument: ") + std::string(arg));
        }
    }

    if (!Log::Init(config->logs_dir / "SecondSonPC.log", config->log_level)) {
        return Fail("cannot open " + (config->logs_dir / "SecondSonPC.log").string());
    }
    SSPC_LOG(App, Info, "SecondSonPC Phase 0 (bootstrap on the shadPS4 core — temporary)");
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

    Bridge::RunGame(*config);
}
