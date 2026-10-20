// SecondSonPC — the user settings file the in-game menu writes (docs/PC_SETTINGS_IMPLEMENTATION.md §4):
// %LOCALAPPDATA%\SecondSonPC\settings.toml, the same tables and keys as SecondSonPC.toml, only the settings the user
// changed. SecondSonPC.toml (hand-edited, strict) stays the defaults; this file overrides it. Reading is lenient (a
// bad key is skipped with a warning, a file that does not parse is moved aside); writing is atomic and off the
// game's threads. Own code.
#pragma once

#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "settings/setting_types.h"

namespace SecondSon::Settings {

/// Settings changed by the user: id -> value (settings with a SecondSonPC.toml key only).
using Overrides = std::map<std::string, Value, std::less<>>;

/// Keys of the user file this version does not take (a key it does not know, a setting it does not store, a value of
/// the wrong type or range): kept as they were written and written back by every save, so a file from a newer version
/// or edited by hand loses nothing. A setting the user changes in the menu replaces its kept line.
struct KeptKey {
    std::string table, key;
    std::string text; // the value as TOML text
    bool operator==(const KeptKey&) const = default;
};
using KeptKeys = std::vector<KeptKey>;

/// %LOCALAPPDATA%\SecondSonPC\settings.toml; empty if LOCALAPPDATA is unknown.
std::filesystem::path DefaultUserFile();

struct LoadReport {
    enum class Status : u8 { Missing, Loaded, Corrupt, Unreadable };
    Status status = Status::Missing;
    std::filesystem::path moved_to;    // Corrupt: where the file that did not parse went
    std::string error;                 // Corrupt/Unreadable: why
    std::vector<std::string> warnings; // keys skipped (unknown, wrong type, out of range, not stored)
};

/// Reads the user file into `out` (cleared first). See the file comment.
LoadReport LoadUserFile(const std::filesystem::path& file, Overrides& out, KeptKeys* kept = nullptr);

/// The parsing half of LoadUserFile (tests): false + `parse_error` if the text is not TOML.
bool ParseUserText(std::string_view text, Overrides& out, std::vector<std::string>& warnings,
                   std::string& parse_error, KeptKeys* kept = nullptr);

/// The file's text for these overrides, in registry order, with a header comment; the kept keys in their tables
/// (tables this version does not know at the end).
std::string SerializeUserFile(const Overrides& overrides, const KeptKeys& kept = {});

/// Writes `text` to `file` atomically: <file>.tmp written and flushed, then it replaces the file (the previous
/// version kept as <file>.bak). The folder is created. false + `error`: the old file is untouched.
bool WriteFileAtomic(const std::filesystem::path& file, std::string_view text, std::string& error);

/// A background writer of one file: Save() hands over the newest text and returns at once; the writer coalesces
/// (the newest wins) and writes atomically. Flush() waits until everything handed over is on disk.
class AsyncWriter {
public:
    explicit AsyncWriter(std::filesystem::path file);
    ~AsyncWriter();
    AsyncWriter(const AsyncWriter&) = delete;
    AsyncWriter& operator=(const AsyncWriter&) = delete;

    void Save(std::string text);
    /// true: everything handed over is on disk; false: still pending after the timeout, or the last write failed
    /// (LastError).
    bool Flush(u32 timeout_ms);
    u64 Writes() const;
    std::string LastError() const;
    const std::filesystem::path& File() const {
        return file_;
    }

private:
    void Run();

    const std::filesystem::path file_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string pending_;
    u64 requested_ = 0; // generations
    u64 done_ = 0;
    u64 writes_ = 0;
    std::string last_error_;
    bool stop_ = false;
    std::thread thread_;
};

} // namespace SecondSon::Settings
