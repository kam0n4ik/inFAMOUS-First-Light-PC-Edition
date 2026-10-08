// SecondSonPC — read-only access to a PS4 module file (C++ port of tools/ps4elf.py).
#include "loader/image_file.h"

#include <cstring>
#include <fstream>

#include <fmt/format.h>

namespace SecondSon::Loader {

namespace {

template <typename T>
bool ReadAt(const std::vector<u8>& data, u64 offset, T& out) {
    if (offset > data.size() || data.size() - offset < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

} // namespace

bool ImageFile::Open(const std::filesystem::path& path, std::string& error) {
    path_ = path;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = fmt::format("cannot open {}", path.string());
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    data_.resize(size);
    if (!f.read(reinterpret_cast<char*>(data_.data()), static_cast<std::streamsize>(size))) {
        error = fmt::format("cannot read {}", path.string());
        return false;
    }

    u32 magic = 0;
    if (!ReadAt(data_, 0, magic)) {
        error = fmt::format("{}: file too small", path.string());
        return false;
    }
    if (magic == Elf::kSelfMagic) {
        is_self_ = true;
        u16 count = 0;
        ReadAt(data_, Elf::kSelfSegmentCountOffset, count);
        self_segments_.resize(count);
        for (u16 i = 0; i < count; ++i) {
            if (!ReadAt(data_, Elf::kSelfSegmentTableOffset + i * sizeof(Elf::SelfSegment),
                        self_segments_[i])) {
                error = fmt::format("{}: truncated SELF segment table", path.string());
                return false;
            }
        }
        elf_offset_ = Elf::kSelfSegmentTableOffset + count * sizeof(Elf::SelfSegment);
    }
    if (!ReadAt(data_, elf_offset_, header_) || std::memcmp(header_.ident, "\x7f" "ELF", 4) != 0) {
        error = fmt::format("{}: ELF header not found at {:#x}", path.string(), elf_offset_);
        return false;
    }
    if (header_.phentsize != sizeof(Elf::ProgramHeader)) {
        error = fmt::format("{}: unexpected e_phentsize {}", path.string(), header_.phentsize);
        return false;
    }
    phdrs_.resize(header_.phnum);
    for (u16 i = 0; i < header_.phnum; ++i) {
        if (!ReadAt(data_, elf_offset_ + header_.phoff + i * sizeof(Elf::ProgramHeader), phdrs_[i])) {
            error = fmt::format("{}: truncated program headers", path.string());
            return false;
        }
    }
    return true;
}

bool ImageFile::Read(u64 file_offset, u64 size, void* dest) const {
    if (size == 0) {
        return true;
    }
    u64 source = 0;
    if (!is_self_) {
        source = file_offset;
    } else {
        bool found = false;
        for (const auto& seg : self_segments_) {
            if (!seg.IsBlocked() || seg.PhdrIndex() >= phdrs_.size()) {
                continue;
            }
            const auto& phdr = phdrs_[seg.PhdrIndex()];
            if (file_offset >= phdr.offset && file_offset < phdr.offset + phdr.filesz) {
                source = seg.file_offset + (file_offset - phdr.offset);
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    if (source > data_.size() || data_.size() - source < size) {
        return false;
    }
    std::memcpy(dest, data_.data() + source, size);
    return true;
}

std::vector<u8> ImageFile::ReadSegment(const Elf::ProgramHeader& phdr) const {
    std::vector<u8> out(phdr.filesz);
    if (!Read(phdr.offset, phdr.filesz, out.data())) {
        out.clear();
    }
    return out;
}

u64 ReadSdkVersion(const std::filesystem::path& path) {
    ImageFile f;
    std::string error;
    if (!f.Open(path, error)) {
        return 0;
    }
    for (const auto& p : f.ProgramHeaders()) {
        if (p.type == Elf::PT_SCE_PROCPARAM && p.filesz >= 0x18) {
            u64 sdk = 0; // SceProcParam: u64 size, u32 magic, u32 entry_count, u64 sdk_version
            return f.Read(p.offset + 0x10, sizeof(sdk), &sdk) ? sdk : 0;
        }
    }
    return 0;
}

} // namespace SecondSon::Loader
