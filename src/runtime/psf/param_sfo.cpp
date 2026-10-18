// SecondSonPC — PSF (param.sfo) reader.
#include "runtime/psf/param_sfo.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include <fmt/format.h>

namespace SecondSon::Psf {

namespace {

constexpr u32 kMagic = 0x46535000; // "\0PSF"
constexpr size_t kHeaderSize = 20;
constexpr size_t kIndexEntrySize = 16;
constexpr u32 kMaxEntries = 4096; // real files have a few dozen

u16 Read16(std::span<const u8> b, size_t at) {
    u16 v;
    std::memcpy(&v, b.data() + at, sizeof(v));
    return v;
}

u32 Read32(std::span<const u8> b, size_t at) {
    u32 v;
    std::memcpy(&v, b.data() + at, sizeof(v));
    return v;
}

} // namespace

bool ParamSfo::Parse(std::span<const u8> bytes, std::string& error) {
    entries_.clear();
    if (bytes.size() < kHeaderSize) {
        error = fmt::format("file too small ({} bytes)", bytes.size());
        return false;
    }
    if (Read32(bytes, 0) != kMagic) {
        error = fmt::format("bad magic {:#010x}", Read32(bytes, 0));
        return false;
    }
    const u32 version = Read32(bytes, 4);
    const u32 key_table = Read32(bytes, 8);
    const u32 data_table = Read32(bytes, 12);
    const u32 count = Read32(bytes, 16);
    if (version != 0x101 && version != 0x100) {
        error = fmt::format("unsupported version {:#x}", version);
        return false;
    }
    if (count > kMaxEntries || kHeaderSize + size_t{count} * kIndexEntrySize > bytes.size() ||
        key_table > bytes.size() || data_table > bytes.size()) {
        error = fmt::format("inconsistent header: {} entries, key table {:#x}, data table {:#x}, "
                            "size {:#x}",
                            count, key_table, data_table, bytes.size());
        return false;
    }

    std::vector<Entry> entries;
    entries.reserve(count);
    for (u32 i = 0; i < count; ++i) {
        const size_t at = kHeaderSize + size_t{i} * kIndexEntrySize;
        const size_t key_at = size_t{key_table} + Read16(bytes, at);
        const auto format = static_cast<Format>(Read16(bytes, at + 2));
        const u32 length = Read32(bytes, at + 4);
        const u32 max_length = Read32(bytes, at + 8);
        const size_t data_at = size_t{data_table} + Read32(bytes, at + 12);

        if (key_at >= bytes.size()) {
            error = fmt::format("entry {}: key outside the file", i);
            return false;
        }
        const auto* key_begin = reinterpret_cast<const char*>(bytes.data() + key_at);
        const size_t key_length = strnlen(key_begin, bytes.size() - key_at);
        if (key_at + key_length >= bytes.size()) {
            error = fmt::format("entry {}: key not terminated", i);
            return false;
        }
        const std::string_view key(key_begin, key_length);
        if (format != Format::Binary && format != Format::Text && format != Format::Integer) {
            error = fmt::format("entry {} ({}): unknown format {:#06x}", i, key,
                                static_cast<u16>(format));
            return false;
        }
        if (length > max_length || data_at + max_length > bytes.size() ||
            (format == Format::Integer && length != sizeof(u32))) {
            error = fmt::format("entry {} ({}): data length {} / {} at {:#x} does not fit", i, key,
                                length, max_length, data_at);
            return false;
        }
        entries.push_back(Entry{
            .key = std::string(key),
            .format = format,
            .max_length = max_length,
            .data = std::vector<u8>(bytes.begin() + data_at, bytes.begin() + data_at + length),
        });
    }
    entries_ = std::move(entries);
    return true;
}

bool ParamSfo::Load(const std::filesystem::path& file, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = fmt::format("cannot open {}", file.string());
        return false;
    }
    const std::vector<u8> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (!Parse(bytes, error)) {
        error = fmt::format("{}: {}", file.string(), error);
        return false;
    }
    return true;
}

const Entry* ParamSfo::Find(std::string_view key) const {
    for (const auto& e : entries_) {
        if (e.key == key) {
            return &e;
        }
    }
    return nullptr;
}

std::optional<std::string> ParamSfo::GetString(std::string_view key) const {
    const Entry* e = Find(key);
    if (e == nullptr || e->format != Format::Text) {
        return std::nullopt;
    }
    if (e->data.empty()) {
        return std::string{};
    }
    const auto* text = reinterpret_cast<const char*>(e->data.data());
    return std::string(text, strnlen(text, e->data.size()));
}

std::optional<u32> ParamSfo::GetInteger(std::string_view key) const {
    const Entry* e = Find(key);
    if (e == nullptr || e->format != Format::Integer) {
        return std::nullopt;
    }
    u32 value;
    std::memcpy(&value, e->data.data(), sizeof(value));
    return value;
}

std::optional<std::span<const u8>> ParamSfo::GetBinary(std::string_view key) const {
    const Entry* e = Find(key);
    if (e == nullptr || e->format != Format::Binary) {
        return std::nullopt;
    }
    return std::span<const u8>(e->data);
}

namespace {

/// The console's reserved lengths (the oracle's psf_known_max_sizes).
u32 KnownMaxLength(std::string_view key, u32 fallback) {
    struct Known {
        std::string_view key;
        u32 length;
    };
    static constexpr Known kKnown[] = {
        {"ACCOUNT_ID", 8}, {"CATEGORY", 4},         {"DETAIL", 1024},  {"FORMAT", 4},          {"MAINTITLE", 128},
        {"PARAMS", 1024},  {"SAVEDATA_BLOCKS", 8}, {"SAVEDATA_DIRECTORY", 32}, {"SUBTITLE", 128}, {"TITLE_ID", 12},
    };
    for (const Known& k : kKnown) {
        if (k.key == key) {
            return k.length;
        }
    }
    return fallback;
}

void Put32(std::vector<u8>& b, size_t at, u32 v) {
    std::memcpy(b.data() + at, &v, sizeof(v));
}

void Put16(std::vector<u8>& b, size_t at, u16 v) {
    std::memcpy(b.data() + at, &v, sizeof(v));
}

} // namespace

bool ParamSfo::SetString(const std::string& key, std::string_view value) {
    std::vector<u8> data(value.begin(), value.end());
    data.push_back(0);
    for (Entry& e : entries_) {
        if (e.key == key) {
            if (e.format != Format::Text) {
                return false;
            }
            e.max_length = KnownMaxLength(key, static_cast<u32>(data.size()));
            e.data = std::move(data);
            return true;
        }
    }
    const u32 max_length = KnownMaxLength(key, static_cast<u32>(data.size()));
    entries_.push_back(Entry{key, Format::Text, max_length, std::move(data)});
    return true;
}

bool ParamSfo::SetInteger(const std::string& key, u32 value) {
    std::vector<u8> data(sizeof(value));
    std::memcpy(data.data(), &value, sizeof(value));
    for (Entry& e : entries_) {
        if (e.key == key) {
            if (e.format != Format::Integer) {
                return false;
            }
            e.max_length = sizeof(u32);
            e.data = std::move(data);
            return true;
        }
    }
    entries_.push_back(Entry{key, Format::Integer, sizeof(u32), std::move(data)});
    return true;
}

bool ParamSfo::SetBinary(const std::string& key, std::span<const u8> value) {
    std::vector<u8> data(value.begin(), value.end());
    for (Entry& e : entries_) {
        if (e.key == key) {
            if (e.format != Format::Binary) {
                return false;
            }
            e.max_length = KnownMaxLength(key, static_cast<u32>(data.size()));
            e.data = std::move(data);
            return true;
        }
    }
    const u32 max_length = KnownMaxLength(key, static_cast<u32>(data.size()));
    entries_.push_back(Entry{key, Format::Binary, max_length, std::move(data)});
    return true;
}

std::vector<u8> ParamSfo::Encode() const {
    std::vector<u8> b(kHeaderSize + entries_.size() * kIndexEntrySize, 0);
    Put32(b, 0, kMagic);
    Put32(b, 4, 0x101);
    Put32(b, 16, static_cast<u32>(entries_.size()));
    const size_t key_table = b.size();
    Put32(b, 8, static_cast<u32>(key_table));
    for (size_t i = 0; i < entries_.size(); ++i) {
        const size_t at = kHeaderSize + i * kIndexEntrySize;
        Put16(b, at, static_cast<u16>(b.size() - key_table));
        Put16(b, at + 2, static_cast<u16>(entries_[i].format));
        Put32(b, at + 8, entries_[i].max_length);
        b.insert(b.end(), entries_[i].key.begin(), entries_[i].key.end());
        b.push_back(0);
    }
    const size_t data_table = b.size();
    Put32(b, 12, static_cast<u32>(data_table));
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (b.size() % 4 != 0) {
            b.resize(b.size() + (4 - b.size() % 4), 0);
        }
        const Entry& e = entries_[i];
        const size_t at = kHeaderSize + i * kIndexEntrySize;
        Put32(b, at + 12, static_cast<u32>(b.size() - data_table));
        std::vector<u8> value = e.data;
        if (e.format == Format::Text) {
            // the text up to its first NUL, then one NUL (as the oracle stores a string)
            const auto nul = std::find(value.begin(), value.end(), u8{0});
            value.erase(nul, value.end());
            value.push_back(0);
        }
        const u32 length = static_cast<u32>(value.size());
        Put32(b, at + 4, length);
        b.insert(b.end(), value.begin(), value.end());
        if (e.max_length > length) {
            b.resize(b.size() + (e.max_length - length), 0);
        }
    }
    return b;
}

bool ParamSfo::Save(const std::filesystem::path& file) const {
    const std::vector<u8> bytes = Encode();
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out.flush());
}

} // namespace SecondSon::Psf
