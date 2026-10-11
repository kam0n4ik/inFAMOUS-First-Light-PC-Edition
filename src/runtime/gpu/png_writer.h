// SecondSonPC — a minimal PNG writer for the backend's frame captures (P4.4): 8-bit RGBA, zlib stored blocks (no
// compression), CRC-32 and Adler-32 per the PNG/zlib specifications. No dependency.
#pragma once

#include <filesystem>
#include <span>

#include "base/abi.h"

namespace SecondSon::Gpu {

/// `rgba`: width * height * 4 bytes, rows top to bottom. false if the file could not be written.
bool WritePng(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba);

/// CRC-32 (ISO-HDLC, as PNG) of `data` continuing `crc` (0 to start).
u32 Crc32(std::span<const u8> data, u32 crc = 0);

} // namespace SecondSon::Gpu
