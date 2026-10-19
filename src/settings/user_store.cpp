// SecondSonPC — the user settings file (see user_store.h).
#include "settings/user_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>

#include <fmt/format.h>
#include <windows.h>

#include "settings/registry.h"
#include "settings/toml_value.h"

namespace SecondSon::Settings {

namespace {

std::string Win32Error(const char* what) {
    return fmt::format("{}: Win32 error {}", what, GetLastError());
}

/// A TOML basic string.
std::string Quote(std::string_view s) {
    std::string out = "\"";
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (ch == '"' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20 || c == 0x7F) {
            out += fmt::format("\\u{:04X}", c);
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

std::string TomlText(const Value& value) {
    if (const auto* b = std::get_if<bool>(&value)) {
        return *b ? "true" : "false";
    }
    if (const auto* i = std::get_if<s64>(&value)) {
        return std::to_string(*i);
    }
    if (const auto* d = std::get_if<double>(&value)) {
        std::string s = fmt::format("{}", *d);
        if (s.find_first_of(".eEn") == std::string::npos) {
            s += ".0"; // 1.0, not 1 (an integer in TOML)
        }
        return s;
    }
    if (const auto* s = std::get_if<std::string>(&value)) {
        return Quote(*s);
    }
    std::string out = "[";
    for (const auto& in : std::get<Bindings>(value)) {
        out += fmt::format("{}{}", out.size() > 1 ? ", " : "", Quote(Input::InputName(in)));
    }
    return out + "]";
}

std::string Timestamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

} // namespace

bool FromToml(const Desc& desc, const toml::value& v, Value& out, std::string& error) {
    switch (desc.type) {
    case Type::Bool:
        if (!v.is_boolean()) {
            error = "true/false expected";
            return false;
        }
        out = v.as_boolean();
        break;
    case Type::Int:
        if (!v.is_integer()) {
            error = "an integer expected";
            return false;
        }
        out = static_cast<s64>(v.as_integer());
        break;
    case Type::Float:
        if (v.is_floating()) {
            out = static_cast<double>(v.as_floating());
        } else if (v.is_integer()) {
            out = static_cast<double>(v.as_integer());
        } else {
            error = "a number expected";
            return false;
        }
        break;
    case Type::Choice:
    case Type::Text:
        if (!v.is_string()) {
            error = "a string expected";
            return false;
        }
        out = std::string(v.as_string());
        break;
    case Type::Binding: {
        std::vector<std::string> names;
        if (v.is_string()) {
            names.push_back(v.as_string());
        } else if (v.is_array()) {
            for (const auto& item : v.as_array()) {
                if (!item.is_string()) {
                    error = "a string or an array of strings expected";
                    return false;
                }
                names.push_back(item.as_string());
            }
        } else {
            error = "a string or an array of strings expected";
            return false;
        }
        Bindings codes;
        for (const auto& n : names) {
            Input::InputCode code;
            if (!Input::ParseInput(n, code)) {
                error = fmt::format("unknown key \"{}\"", n);
                return false;
            }
            codes.push_back(code);
        }
        out = std::move(codes);
        break;
    }
    }
    return Validate(desc, out, error);
}

std::filesystem::path DefaultUserFile() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buf) / "SecondSonPC" / "settings.toml";
}

bool ParseUserText(std::string_view text, Overrides& out, std::vector<std::string>& warnings,
                   std::string& parse_error, KeptKeys* kept) {
    out.clear();
    if (kept != nullptr) {
        kept->clear();
    }
    // a key not taken: kept for the next save (a nested table cannot be: it is reported)
    const auto keep = [kept](const std::string& table, const std::string& key, const toml::value& v) {
        if (kept == nullptr || v.is_table()) {
            return false;
        }
        try {
            kept->push_back({table, key, toml::format(v)});
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    if (text.starts_with("\xEF\xBB\xBF")) {
        text.remove_prefix(3); // a BOM from a text editor
    }
    toml::value root;
    try {
        root = toml::parse_str(std::string(text));
    } catch (const std::exception& e) {
        parse_error = e.what();
        return false;
    }
    if (!root.is_table()) {
        parse_error = "not a TOML table";
        return false;
    }
    for (const auto& [table, tv] : root.as_table()) {
        if (!tv.is_table()) {
            warnings.push_back(fmt::format("[{}] is not a table: skipped", table));
            continue;
        }
        for (const auto& [key, kv] : tv.as_table()) {
            const Desc* d = FindByKey(table, key);
            if (d == nullptr) {
                warnings.push_back(fmt::format("[{}] {}: not a setting of this version: {}", table, key,
                                               keep(table, key, kv) ? "not used, kept in the file" : "skipped"));
                continue;
            }
            if (d->apply == Apply::Unavailable) {
                warnings.push_back(fmt::format("[{}] {}: not available: {}", table, key,
                                               keep(table, key, kv) ? "not used, kept in the file" : "skipped"));
                continue;
            }
            Value value;
            std::string error;
            if (!FromToml(*d, kv, value, error)) {
                warnings.push_back(fmt::format("[{}] {}: {}: {}", table, key, error,
                                               keep(table, key, kv) ? "not used, kept in the file" : "skipped"));
                continue;
            }
            out.insert_or_assign(std::string(d->id), std::move(value));
        }
    }
    return true;
}

LoadReport LoadUserFile(const std::filesystem::path& file, Overrides& out, KeptKeys* kept) {
    LoadReport report;
    out.clear();
    if (kept != nullptr) {
        kept->clear();
    }
    std::error_code ec;
    const bool exists = !file.empty() && std::filesystem::exists(file, ec);
    if (ec) {
        report.status = LoadReport::Status::Unreadable; // not "missing": a save must not overwrite it
        report.error = ec.message();
        return report;
    }
    if (!exists) {
        return report; // Missing: SecondSonPC.toml alone
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        report.status = LoadReport::Status::Unreadable;
        report.error = "cannot open the file";
        return report;
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    in.close();
    if (!ParseUserText(text, out, report.warnings, report.error, kept)) {
        // Kept for the user to look at, out of the way of the next save.
        auto aside = file;
        aside += ".corrupt-" + Timestamp();
        if (MoveFileExW(file.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            report.moved_to = aside;
        } else {
            report.error += "; " + Win32Error("moving it aside failed");
        }
        report.status = LoadReport::Status::Corrupt;
        out.clear();
        if (kept != nullptr) {
            kept->clear(); // (the whole file is kept aside)
        }
        return report;
    }
    report.status = LoadReport::Status::Loaded;
    return report;
}

std::string SerializeUserFile(const Overrides& overrides, const KeptKeys& kept) {
    std::string out =
        "# SecondSonPC user settings, written by the PC settings (the game's Settings screen, Esc, then \"Additional\n"
        "# Settings\"). They override SecondSonPC.toml (the defaults); delete a line to use its value again.\n";
    // tables in registry order, then the kept keys' own tables; each table's lines: the overrides in registry order,
    // then its kept keys (unless a setting now written has the same key)
    std::vector<std::pair<std::string, std::vector<std::string>>> tables;
    const auto table_of = [&tables](std::string_view name) -> std::vector<std::string>& {
        for (auto& [t, lines] : tables) {
            if (t == name) {
                return lines;
            }
        }
        return tables.emplace_back(std::string(name), std::vector<std::string>{}).second;
    };
    for (const auto& d : All()) {
        const auto it = overrides.find(d.id);
        if (it == overrides.end() || d.table.empty()) {
            continue;
        }
        table_of(d.table).push_back(fmt::format("{} = {}", d.key, TomlText(it->second)));
    }
    for (const auto& k : kept) {
        const Desc* d = FindByKey(k.table, k.key);
        if (d != nullptr && overrides.contains(d->id)) {
            continue; // the user's new value replaces the line this version did not take
        }
        // a key that is not a bare TOML key is written quoted
        const bool bare = !k.key.empty() && std::ranges::all_of(k.key, [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
        table_of(k.table).push_back(fmt::format("{} = {}", bare ? k.key : Quote(k.key), k.text));
    }
    for (const auto& [t, lines] : tables) {
        if (lines.empty()) {
            continue;
        }
        out += fmt::format("\n[{}]\n", t);
        for (const auto& l : lines) {
            out += l;
            out += '\n';
        }
    }
    return out;
}

bool WriteFileAtomic(const std::filesystem::path& file, std::string_view text, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += ".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = Win32Error("creating the temporary file");
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
                    written == text.size() && FlushFileBuffers(h);
    if (!ok) {
        error = Win32Error("writing the temporary file");
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    auto bak = file;
    bak += ".bak";
    if (std::filesystem::exists(file, ec)) {
        if (ReplaceFileW(file.c_str(), tmp.c_str(), bak.c_str(), REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr,
                         nullptr)) {
            return true;
        }
        // ReplaceFile can refuse (another program holds the file briefly): keep a copy, then move over it.
        CopyFileW(file.c_str(), bak.c_str(), FALSE);
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = Win32Error("replacing the file");
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

AsyncWriter::AsyncWriter(std::filesystem::path file) : file_{std::move(file)} {
    thread_ = std::thread([this] { Run(); });
}

AsyncWriter::~AsyncWriter() {
    {
        std::scoped_lock lock{mutex_};
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void AsyncWriter::Save(std::string text) {
    {
        std::scoped_lock lock{mutex_};
        pending_ = std::move(text);
        ++requested_;
    }
    cv_.notify_all();
}

bool AsyncWriter::Flush(u32 timeout_ms) {
    std::unique_lock lock{mutex_};
    const bool done =
        cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return done_ == requested_; });
    return done && last_error_.empty();
}

u64 AsyncWriter::Writes() const {
    std::scoped_lock lock{mutex_};
    return writes_;
}

std::string AsyncWriter::LastError() const {
    std::scoped_lock lock{mutex_};
    return last_error_;
}

void AsyncWriter::Run() {
    std::unique_lock lock{mutex_};
    for (;;) {
        cv_.wait(lock, [this] { return stop_ || done_ != requested_; });
        if (done_ == requested_) {
            return; // stop with nothing pending
        }
        // A short pause lets a slider being dragged settle: one write for the burst (unless stopping).
        if (!stop_) {
            cv_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stop_; });
        }
        const u64 generation = requested_;
        const std::string text = pending_;
        lock.unlock();
        std::string error;
        const bool ok = WriteFileAtomic(file_, text, error);
        lock.lock();
        if (ok) {
            ++writes_;
            last_error_.clear();
        } else {
            last_error_ = error;
        }
        done_ = generation;
        cv_.notify_all();
    }
}

} // namespace SecondSon::Settings
