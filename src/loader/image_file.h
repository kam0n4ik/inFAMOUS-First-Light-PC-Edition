// SecondSonPC — read-only access to a PS4 module file (fake-signed SELF or plain ELF).
// C++ port of tools/ps4elf.py (Ps4Elf). The file is read once into memory and never modified.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

#include "loader/elf.h"

namespace SecondSon::Loader {

class ImageFile {
public:
    /// Reads and validates the file. On failure returns false and sets `error`.
    bool Open(const std::filesystem::path& path, std::string& error);

    bool IsSelf() const {
        return is_self_;
    }
    const Elf::Header& Header() const {
        return header_;
    }
    std::span<const Elf::ProgramHeader> ProgramHeaders() const {
        return phdrs_;
    }

    /// Copies `size` bytes of ELF file offset `file_offset` to `dest` (SELF: through the segment
    /// table, like the console loader). Returns false if the range is not stored in the file.
    bool Read(u64 file_offset, u64 size, void* dest) const;

    /// Contents of a program header (p_filesz bytes); empty on error.
    std::vector<u8> ReadSegment(const Elf::ProgramHeader& phdr) const;

    const std::filesystem::path& Path() const {
        return path_;
    }

private:
    std::filesystem::path path_;
    std::vector<u8> data_;
    bool is_self_ = false;
    u64 elf_offset_ = 0;
    std::vector<Elf::SelfSegment> self_segments_;
    Elf::Header header_{};
    std::vector<Elf::ProgramHeader> phdrs_;
};

/// SceProcParam::sdk_version of an executable file (0 if it has none / cannot be read).
u64 ReadSdkVersion(const std::filesystem::path& path);

} // namespace SecondSon::Loader
