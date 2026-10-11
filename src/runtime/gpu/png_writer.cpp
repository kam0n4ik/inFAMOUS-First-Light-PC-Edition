// SecondSonPC — the minimal PNG writer (P4.4). See png_writer.h.
#include "runtime/gpu/png_writer.h"

#include <array>
#include <fstream>
#include <vector>

namespace SecondSon::Gpu {

namespace {

const std::array<u32, 256>& CrcTable() {
    static const std::array<u32, 256> table = [] {
        std::array<u32, 256> t{};
        for (u32 n = 0; n < 256; ++n) {
            u32 c = n;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[n] = c;
        }
        return t;
    }();
    return table;
}

void Be32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v >> 24));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

void Chunk(std::vector<u8>& out, const char type[4], std::span<const u8> data) {
    Be32(out, static_cast<u32>(data.size()));
    const size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    Be32(out, Crc32(std::span<const u8>(out.data() + start, out.size() - start)));
}

} // namespace

u32 Crc32(std::span<const u8> data, u32 crc) {
    const auto& t = CrcTable();
    crc = ~crc;
    for (const u8 b : data) {
        crc = t[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

bool WritePng(const std::filesystem::path& path, u32 width, u32 height, std::span<const u8> rgba) {
    if (width == 0 || height == 0 || rgba.size() < u64{width} * height * 4) {
        return false;
    }
    // the raw scanlines: filter byte 0 + the row
    const u64 row = u64{width} * 4;
    std::vector<u8> raw;
    raw.reserve((row + 1) * height);
    for (u32 y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + static_cast<ptrdiff_t>(y * row),
                   rgba.begin() + static_cast<ptrdiff_t>((y + 1) * row));
    }
    // zlib: header, stored blocks of at most 65535 bytes, Adler-32
    std::vector<u8> z;
    z.reserve(raw.size() + raw.size() / 65535 * 5 + 16);
    z.push_back(0x78);
    z.push_back(0x01);
    u32 a = 1, b = 0;
    for (const u8 c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n == raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<u8>(n));
        z.push_back(static_cast<u8>(n >> 8));
        z.push_back(static_cast<u8>(~n));
        z.push_back(static_cast<u8>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<ptrdiff_t>(pos), raw.begin() + static_cast<ptrdiff_t>(pos + n));
        pos += n;
    } while (pos < raw.size());
    Be32(z, (b << 16) | a);

    std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<u8> ihdr;
    Be32(ihdr, width);
    Be32(ihdr, height);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8-bit, RGBA, deflate, filter 0, no interlace
    Chunk(out, "IHDR", ihdr);
    Chunk(out, "IDAT", z);
    Chunk(out, "IEND", {});
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) {
        return false;
    }
    f.write(reinterpret_cast<const char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(f);
}

} // namespace SecondSon::Gpu
