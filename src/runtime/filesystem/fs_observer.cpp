// SecondSonPC — Filesystem observers (Phase 0): sceKernelOpen / sceKernelStat.
#include "runtime/filesystem/fs_observer.h"

#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include "base/log.h"
#include "runtime/language.h"

namespace SecondSon::Filesystem {

namespace {

typedef s32(SSPC_SYSV_ABI* OpenFn)(const char*, s32, u16);
typedef s32(SSPC_SYSV_ABI* StatFn)(const char*, void*);

std::mutex g_seen_mutex;
std::unordered_set<std::string> g_seen_language_files;

/// Returns the language tag if `path` is one of the game's per-language files, else empty.
std::string_view LanguageTagOf(std::string_view path) {
    const auto slash = path.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    std::string_view rest = name;
    if (rest.starts_with("lang_")) {
        rest.remove_prefix(5); // lang_<tag>_text.xpps / lang_<tag>_audio_p.xpps
    }
    for (const auto& language : Runtime::Languages()) {
        const auto& tag = language.game_tag;
        if (rest.size() > tag.size() && rest.starts_with(tag) && rest[tag.size()] == '_') {
            const auto suffix = rest.substr(tag.size());
            const bool lang_xpps = name.starts_with("lang_") &&
                                   (suffix.starts_with("_text") || suffix.starts_with("_audio"));
            const bool lang_psarc = !name.starts_with("lang_") && path.find("/psarc/") != path.npos &&
                                    (suffix.starts_with("_a") || suffix.starts_with("_b"));
            if (lang_xpps || lang_psarc) {
                return tag;
            }
        }
    }
    return {};
}

void NoteLanguageFile(const char* path, std::string_view op, s32 result) {
    const auto tag = LanguageTagOf(path);
    if (tag.empty()) {
        return;
    }
    {
        std::scoped_lock lock{g_seen_mutex};
        if (!g_seen_language_files.insert(std::string(op) + ':' + path).second) {
            return;
        }
    }
    SSPC_LOG(Language, Info, "game {} language file: {} (tag={}, result={})", op, path, tag, result);
}

} // namespace

SSPC_SYSV_ABI s32 ObserveOpen(const char* path, s32 flags, u16 mode) {
    const auto original = reinterpret_cast<OpenFn>(g_open_original);
    const s32 result = original(path, flags, mode);
    if (path != nullptr) {
        SSPC_LOG(Filesystem, Debug, "sceKernelOpen(\"{}\", flags={:#x}, mode={:#o}) -> {}", path, flags,
                 mode, result);
        NoteLanguageFile(path, "opened", result);
    } else {
        SSPC_LOG(Filesystem, Warning, "sceKernelOpen(nullptr) -> {}", result);
    }
    return result;
}

SSPC_SYSV_ABI s32 ObserveStat(const char* path, void* sb) {
    const auto original = reinterpret_cast<StatFn>(g_stat_original);
    const s32 result = original(path, sb);
    if (path != nullptr) {
        SSPC_LOG(Filesystem, Debug, "sceKernelStat(\"{}\") -> {:#x}", path, static_cast<u32>(result));
    }
    return result;
}

} // namespace SecondSon::Filesystem
