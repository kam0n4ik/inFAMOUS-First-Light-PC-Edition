// SecondSonPC — PSF (param.sfo) reader.
#include "runtime/psf/param_sfo.h"

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

} // namespace SecondSon::Psf
