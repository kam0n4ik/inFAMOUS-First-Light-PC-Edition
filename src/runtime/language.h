// SecondSonPC — system language (PS4 SCE_SYSTEM_PARAM_LANG_* ids) and Second Son file naming.
#pragma once

#include <filesystem>
#include <span>
#include <string_view>

namespace SecondSon::Runtime {

struct LanguageInfo {
    int id;                    // PS4 system language id returned by sceSystemServiceParamGetInt(1)
    std::string_view locale;   // SecondSonPC.toml [Language] system
    std::string_view game_tag; // Second Son table at eboot 0xd7b500: lang_<tag>_text.xpps, <tag>_a.psarc
    std::string_view display;
};

std::span<const LanguageInfo> Languages();
const LanguageInfo* FindLanguageByLocale(std::string_view locale); // case-insensitive
const LanguageInfo* FindLanguageById(int id);

/// Logs which localization files of this language exist in the game folder (read-only check).
/// Returns false when the mandatory text file is missing.
bool ReportLanguageFiles(const LanguageInfo& language, const std::filesystem::path& game_dir);

} // namespace SecondSon::Runtime
