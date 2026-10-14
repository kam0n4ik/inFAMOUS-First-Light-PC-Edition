// SecondSonPC — own logger. Synchronous: every line is flushed, so the log survives crashes.
#include "base/log.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <io.h>
#include <windows.h>

namespace SecondSon::Log {

namespace {

constexpr std::array<std::string_view, static_cast<size_t>(Category::Count)> kCategoryNames{
    "App",   "Loader",   "Kernel",   "SystemService", "User", "Filesystem", "Input", "Audio",
    "GPU",   "Shader",   "SaveData", "Language",      "Bridge",     "Trace", "Memory", "Cpu",
    "Engine",
};
constexpr std::array<std::string_view, 7> kLevelNames{"Trace", "Debug",    "Info", "Warning",
                                                       "Error", "Critical", "Off"};

std::mutex g_mutex;
std::FILE* g_file = nullptr;
std::atomic<Level> g_level{Level::Info};

} // namespace

bool Init(const std::filesystem::path& file, Level level) {
    std::scoped_lock lock{g_mutex};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (g_file != nullptr) {
        std::fclose(g_file);
    }
    g_file = _wfopen(file.c_str(), L"wb"); // one file per session
    g_level = level;
    if (g_file == nullptr) {
        return false;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    const auto header = fmt::format("SecondSonPC log, session started {:04}-{:02}-{:02} {:02}:{:02}:{:02}\n",
                                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::fwrite(header.data(), 1, header.size(), g_file);
    std::fflush(g_file);
    return true;
}

void SetLevel(Level level) {
    g_level = level;
}

bool Enabled(Level level) {
    return level != Level::Off && level >= g_level.load(std::memory_order_relaxed);
}

void Write(Category category, Level level, std::string_view text) {
    if (!Enabled(level)) {
        return;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    const auto line = fmt::format("[{:02}:{:02}:{:02}.{:03}] [{}] [{}] (tid {}) {}\n", st.wHour,
                                  st.wMinute, st.wSecond, st.wMilliseconds,
                                  kLevelNames[static_cast<size_t>(level)],
                                  kCategoryNames[static_cast<size_t>(category)],
                                  GetCurrentThreadId(), text);
    std::scoped_lock lock{g_mutex};
    if (g_file != nullptr) {
        std::fwrite(line.data(), 1, line.size(), g_file);
        std::fflush(g_file);
    }
    if (level >= Level::Error || g_file == nullptr) {
        std::fwrite(line.data(), 1, line.size(), stderr);
    }
}

void Flush() {
    std::scoped_lock lock{g_mutex};
    if (g_file != nullptr) {
        std::fflush(g_file);
    }
}

void EmergencyWrite(const char* text, size_t size) {
    std::FILE* const file = g_file;
    const HANDLE handle =
        file != nullptr ? reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(file))) : GetStdHandle(STD_ERROR_HANDLE);
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr) {
        return;
    }
    DWORD written = 0;
    WriteFile(handle, text, static_cast<DWORD>(size), &written, nullptr);
    FlushFileBuffers(handle);
}

std::optional<Level> ParseLevel(std::string_view name) {
    for (size_t i = 0; i < kLevelNames.size(); ++i) {
        const auto& candidate = kLevelNames[i];
        if (candidate.size() == name.size() &&
            _strnicmp(candidate.data(), name.data(), name.size()) == 0) {
            return static_cast<Level>(i);
        }
    }
    return std::nullopt;
}

std::string_view LevelName(Level level) {
    return kLevelNames[static_cast<size_t>(level)];
}

} // namespace SecondSon::Log
