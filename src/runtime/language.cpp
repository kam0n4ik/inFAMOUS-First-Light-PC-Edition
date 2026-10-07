// SecondSonPC — system language table.
#include "runtime/language.h"

#include <array>
#include <cstring>
#include <string>

#include "base/log.h"

namespace SecondSon::Runtime {

namespace {

// Index == PS4 language id. The game_tag column is the game's own table (eboot.bin 0xd7b500,
// char*[23]); the game accepts ids 0..22 (cmp eax, 0x16 at 0x15d6d0).
constexpr std::array<LanguageInfo, 23> kLanguages{{
    {0, "ja-JP", "japanese", "Japanese"},
    {1, "en-US", "english", "English (US)"},
    {2, "fr-FR", "french", "French"},
    {3, "es-ES", "spanish", "Spanish"},
    {4, "de-DE", "german", "German"},
    {5, "it-IT", "italian", "Italian"},
    {6, "nl-NL", "dutch", "Dutch"},
    {7, "pt-PT", "portuguese", "Portuguese"},
    {8, "ru-RU", "russian", "Russian"},
    {9, "ko-KR", "korean", "Korean"},
    {10, "zh-TW", "chinese", "Chinese (Traditional)"},
    {11, "zh-CN", "chinese_s", "Chinese (Simplified)"},
    {12, "fi-FI", "finnish", "Finnish"},
    {13, "sv-SE", "swedish", "Swedish"},
    {14, "da-DK", "danish", "Danish"},
    {15, "nb-NO", "norwegian", "Norwegian"},
    {16, "pl-PL", "polish", "Polish"},
    {17, "pt-BR", "brazilian", "Portuguese (Brazil)"},
    {18, "en-GB", "british", "English (UK)"},
    {19, "tr-TR", "turkish", "Turkish"},
    {20, "es-419", "latino", "Spanish (Latin America)"},
    {21, "ar-AE", "arabic", "Arabic"},
    {22, "fr-CA", "quebecois", "French (Canada)"},
}};

bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() && _strnicmp(a.data(), b.data(), a.size()) == 0;
}

} // namespace

std::span<const LanguageInfo> Languages() {
    return kLanguages;
}

const LanguageInfo* FindLanguageByLocale(std::string_view locale) {
    for (const auto& language : kLanguages) {
        if (EqualsNoCase(language.locale, locale)) {
            return &language;
        }
    }
    return nullptr;
}

const LanguageInfo* FindLanguageById(int id) {
    if (id < 0 || id >= static_cast<int>(kLanguages.size())) {
        return nullptr;
    }
    return &kLanguages[static_cast<size_t>(id)];
}

bool ReportLanguageFiles(const LanguageInfo& language, const std::filesystem::path& game_dir) {
    const std::string tag{language.game_tag};
    const auto cache = game_dir / "art" / "cache";
    struct Item {
        std::filesystem::path path;
        bool mandatory;
    };
    const Item items[] = {
        {cache / ("lang_" + tag + "_text.xpps"), true},
        {cache / ("lang_" + tag + "_text_p.xpps"), false},
        {cache / ("lang_" + tag + "_audio.xpps"), false},
        {cache / ("lang_" + tag + "_audio_p.xpps"), false},
        {cache / "psarc" / (tag + "_a.psarc"), false},
        {cache / "psarc" / (tag + "_b.psarc"), false},
        {cache / "psarc" / (tag + "_a_p.psarc"), false},
        {cache / "psarc" / (tag + "_b_p.psarc"), false},
    };
    bool ok = true;
    for (const auto& item : items) {
        std::error_code ec;
        const bool exists = std::filesystem::is_regular_file(item.path, ec);
        if (exists) {
            SSPC_LOG(Language, Info, "  present: {} ({} bytes)", item.path.string(),
                     std::filesystem::file_size(item.path, ec));
        } else if (item.mandatory) {
            SSPC_LOG(Language, Error, "  MISSING (required): {}", item.path.string());
            ok = false;
        } else {
            SSPC_LOG(Language, Warning, "  missing: {} (not shipped in this game dump)",
                     item.path.string());
        }
    }
    return ok;
}

} // namespace SecondSon::Runtime
