// SecondSonPC — the PC Edition's replacements in what the game reads (see content_patch.h).
#include "runtime/filesystem/content_patch.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#include "base/log.h"

namespace SecondSon::Filesystem::ContentPatch {

namespace {

struct Replacement {
    std::string_view key;
    std::string_view ru, en;
};

// u+F08E: the game font's "yes" button (cross; u_button_press_yes is "Нажмите " / "Press ")
constexpr Replacement kReplacements[] = {
    {"u_options_opt_hdr", "Дополнительные настройки", "Additional Settings"},
    {"u_options_val_hdr_unavailable", "Нажмите \xEF\x82\x8E", "Press \xEF\x82\x8E"},
};

bool IsLanguagePatchTable(std::string_view guest, bool& russian) {
    const auto slash = guest.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? guest : guest.substr(slash + 1);
    if (!name.starts_with("lang_") || !name.ends_with("_text_p.xpps")) {
        return false;
    }
    russian = name == "lang_russian_text_p.xpps";
    return true;
}

} // namespace

std::vector<Edit> ForLanguageTable(const std::string& bytes, bool russian) {
    std::vector<Edit> edits;
    for (const auto& r : kReplacements) {
        const std::string key = std::string(r.key) + '\0';
        const size_t at = bytes.find(key);
        if (at == std::string::npos || bytes.find(key, at + 1) != std::string::npos) {
            continue; // not this table's, or not one entry
        }
        // the entry: key\0 text\0 then zero padding up to the next entry's key
        size_t end = bytes.find('\0', at + key.size());
        if (end == std::string::npos) {
            continue;
        }
        while (end < bytes.size() && bytes[end] == '\0') {
            ++end;
        }
        const std::string_view text = russian ? r.ru : r.en;
        const size_t room = end - at;
        if (text.size() + 2 > room) {
            SSPC_LOG(Filesystem, Warning, "language table: \"{}\" does not fit the entry {} ({} bytes)", text, r.key,
                     room);
            continue;
        }
        Edit e;
        e.offset = at;
        e.bytes.assign(room, '\0');
        e.bytes.replace(1, text.size(), text); // an empty key name, then the text
        edits.push_back(std::move(e));
    }
    return edits;
}

namespace {
LanguageTableHook g_table_hook;

bool IsLanguageTable(std::string_view guest) {
    const auto slash = guest.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? guest : guest.substr(slash + 1);
    return name.starts_with("lang_") && name.ends_with("_text.xpps");
}
} // namespace

void SetLanguageTableHook(LanguageTableHook hook) {
    g_table_hook = hook;
}

FilePatch ForFile(std::string_view guest, const std::filesystem::path& host) {
    bool russian = false;
    const bool patch_table = IsLanguagePatchTable(guest, russian);
    const bool table = !patch_table && IsLanguageTable(guest) && g_table_hook.open != nullptr;
    if (!patch_table && !table) {
        return {};
    }
    std::ifstream in(host, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 16 || bytes.compare(0, 4, "KCAP") != 0) {
        return {};
    }
    if (table) {
        return g_table_hook.open(guest, bytes);
    }
    FilePatch patch;
    patch.edits = ForLanguageTable(bytes, russian);
    SSPC_LOG(Filesystem, Info,
             "{}: {} of the game's strings replaced as read (PC Edition: the Settings screen's \"Additional "
             "settings\" item; the file is unchanged)",
             guest, patch.edits.size());
    return patch;
}

void Observe(const FilePatch& patch, std::string_view guest, u64 offset, u64 address, u64 n) {
    const u64 b = std::max(offset, patch.watch_offset);
    const u64 e = std::min(offset + n, patch.watch_offset + patch.watch_size);
    if (b < e && g_table_hook.read != nullptr) {
        g_table_hook.read(guest, b, address + (b - offset), e - b);
    }
}

void Apply(const std::vector<Edit>& edits, u64 offset, u8* buf, u64 n) {
    for (const auto& e : edits) {
        const u64 b = std::max(offset, e.offset), end = std::min(offset + n, e.offset + e.bytes.size());
        if (b < end) {
            std::memcpy(buf + (b - offset), e.bytes.data() + (b - e.offset), end - b);
        }
    }
}

} // namespace SecondSon::Filesystem::ContentPatch
