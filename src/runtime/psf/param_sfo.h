// SecondSonPC — PSF (param.sfo) reader: the game's /app0/sce_sys/param.sfo (title, serial, versions,
// attributes) and, from Phase 3, the param.sfo of each save directory.
//
// Format (little-endian): 20-byte header {magic "\0PSF", version, key table offset, data table
// offset, entry count}, then one 16-byte index entry per parameter {key offset u16, format u16,
// used length u32, reserved length u32, data offset u32}. Formats: 0x0004 raw bytes (e.g. PARAMS of
// a save), 0x0204 NUL-terminated UTF-8 text, 0x0404 32-bit integer. Versions 1.0 and 1.1.
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Psf {

enum class Format : u16 {
    Binary = 0x0004,
    Text = 0x0204,
    Integer = 0x0404,
};

struct Entry {
    std::string key;
    Format format;
    u32 max_length; // reserved length in the data table
    std::vector<u8> data;
};

class ParamSfo {
public:
    /// Parses a whole PSF image; on failure returns false and describes the problem in `error`
    /// (entries parsed so far are discarded).
    bool Parse(std::span<const u8> bytes, std::string& error);
    /// Reads and parses a host file.
    bool Load(const std::filesystem::path& file, std::string& error);

    /// Text value (format 0x0204), up to the first NUL.
    std::optional<std::string> GetString(std::string_view key) const;
    /// Integer value (format 0x0404).
    std::optional<u32> GetInteger(std::string_view key) const;
    /// Raw bytes (format 0x0004), the used length.
    std::optional<std::span<const u8>> GetBinary(std::string_view key) const;

    std::span<const Entry> Entries() const {
        return entries_;
    }

private:
    const Entry* Find(std::string_view key) const;

    std::vector<Entry> entries_;
};

} // namespace SecondSon::Psf
