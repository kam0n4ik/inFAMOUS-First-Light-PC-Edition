// SecondSonPC — own logger (E:\SecondSonPC\logs\SecondSonPC.log).
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <utility>
#include <fmt/format.h>

namespace SecondSon::Log {

enum class Category : unsigned char {
    App,
    Loader,
    Kernel,
    SystemService,
    User,
    Filesystem,
    Input,
    Audio,
    GPU,
    Shader,
    SaveData,
    Language,
    Bridge, // temporary shadPS4 bootstrap (Phase 0-4)
    Trace,  // import call counter
    Count,
};

enum class Level : unsigned char { Trace, Debug, Info, Warning, Error, Critical, Off };

bool Init(const std::filesystem::path& file, Level level);
void SetLevel(Level level);
bool Enabled(Level level);
void Write(Category category, Level level, std::string_view text);
void Flush();

std::optional<Level> ParseLevel(std::string_view name);
std::string_view LevelName(Level level);

template <typename... Args>
void Logf(Category category, Level level, fmt::format_string<Args...> format, Args&&... args) {
    if (Enabled(level)) {
        Write(category, level, fmt::format(format, std::forward<Args>(args)...));
    }
}

} // namespace SecondSon::Log

#define SSPC_LOG(category, level, ...)                                                             \
    ::SecondSon::Log::Logf(::SecondSon::Log::Category::category, ::SecondSon::Log::Level::level,   \
                           __VA_ARGS__)
