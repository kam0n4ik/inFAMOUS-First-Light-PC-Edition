// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — directory descriptors (ported from shadPS4 @ 0fe263a, see directory.h).
#include "runtime/filesystem/directory.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>

#include "base/log.h"
#include "runtime/filesystem/file_system.h"

namespace SecondSon::Filesystem {

namespace {

constexpr s32 ORBIS_KERNEL_ERROR_EINVAL = static_cast<s32>(0x80020016);

#pragma pack(push, 1)
struct NormalDirent {
    u32 d_fileno;
    u16 d_reclen;
    u8 d_type;
    u8 d_namlen;
    char d_name[256];
};
struct PfsDirent {
    u32 d_fileno;
    u32 d_type;
    u32 d_namlen;
    u32 d_reclen;
    char d_name[256];
};
#pragma pack(pop)

constexpr u32 kNormalMeta = sizeof(u32) + sizeof(u16) + sizeof(u8) + sizeof(u8);
constexpr u32 kPfsMeta = 4 * sizeof(u32);

std::atomic<u32> g_next_fileno{0};

u32 NextFileno() {
    return ++g_next_fileno;
}

u64 AlignUp(u64 v, u64 a) {
    return (v + a - 1) / a * a;
}

/// ".", "..", then the host folder's entries (name, is_file), like the oracle's IterateDirectory.
template <typename F>
void ForEachEntry(const std::filesystem::path& host, F&& f) {
    f(std::string("."), false);
    f(std::string(".."), false);
    std::error_code ec;
    for (std::filesystem::directory_iterator it(host, ec), end; !ec && it != end; it.increment(ec)) {
        const auto u8 = it->path().filename().u8string();
        f(std::string(u8.begin(), u8.end()), !it->is_directory(ec));
    }
}

} // namespace

std::unique_ptr<Directory> Directory::Create(const std::filesystem::path& host, bool pfs) {
    return std::unique_ptr<Directory>(new Directory(host, pfs));
}

Directory::Directory(std::filesystem::path host, bool pfs) : host_{std::move(host)}, pfs_{pfs} {
    Build();
}

void Directory::Build() {
    if (!pfs_) {
        BuildNormal();
        return;
    }
    cache_.reserve(512);
    ForEachEntry(host_, [this](const std::string& leaf, bool is_file) {
        PfsDirent d{};
        d.d_fileno = NextFileno();
        d.d_namlen = static_cast<u32>(std::min<size_t>(leaf.size(), 255));
        std::memcpy(d.d_name, leaf.data(), d.d_namlen);
        d.d_type = is_file ? 2 : 4;
        d.d_reclen = static_cast<u32>(AlignUp(kPfsMeta + d.d_namlen + 1, 8));
        const auto* p = reinterpret_cast<const u8*>(&d);
        cache_.insert(cache_.end(), p, p + d.d_reclen);
    });
    directory_size_ = AlignUp(cache_.size(), 0x10000);
}

void Directory::BuildNormal() {
    // regenerated whenever the reader moved (the oracle re-lists the folder on every read)
    if (previous_file_offset_ == file_offset_) {
        return;
    }
    previous_file_offset_ = file_offset_;
    u64 next_ceiling = 0;
    u64 dirent_offset = 0;
    u64 last_reclen_offset = 4;
    cache_.clear();
    cache_.reserve(512);
    ForEachEntry(host_, [&](const std::string& leaf, bool is_file) {
        NormalDirent d{};
        d.d_fileno = NextFileno();
        d.d_namlen = static_cast<u8>(std::min<size_t>(leaf.size(), 255));
        std::memcpy(d.d_name, leaf.data(), d.d_namlen);
        d.d_type = static_cast<u8>((is_file ? 0100000 : 0040000) >> 12);
        d.d_reclen = static_cast<u16>(AlignUp(kNormalMeta + d.d_namlen + 1, 4));
        if (d.d_reclen + dirent_offset > next_ceiling) {
            if (!cache_.empty()) {
                *reinterpret_cast<u16*>(cache_.data() + last_reclen_offset) +=
                    static_cast<u16>(next_ceiling - dirent_offset);
            }
            dirent_offset = next_ceiling;
            next_ceiling += 512;
            cache_.resize(next_ceiling, 0);
        }
        last_reclen_offset = dirent_offset + 4;
        std::memcpy(cache_.data() + dirent_offset, &d, d.d_reclen);
        dirent_offset += d.d_reclen;
    });
    if (!cache_.empty()) {
        *reinterpret_cast<u16*>(cache_.data() + last_reclen_offset) +=
            static_cast<u16>(next_ceiling - dirent_offset);
    }
    directory_size_ = next_ceiling;
}

s64 Directory::Read(void* buf, u64 nbytes) {
    if (pfs_) {
        return ReadPfs(buf, nbytes);
    }
    BuildNormal();
    s64 available = static_cast<s64>(cache_.size()) - file_offset_;
    if (available <= 0) {
        return 0;
    }
    available = std::min<s64>(available, static_cast<s64>(nbytes));
    std::memcpy(buf, cache_.data() + file_offset_, available);
    file_offset_ += available;
    return available;
}

s64 Directory::ReadPfs(void* buf, u64 nbytes) {
    s64 available = static_cast<s64>(cache_.size()) - file_offset_;
    if (available <= 0) {
        return 0;
    }
    available = std::min<s64>(available, static_cast<s64>(nbytes));
    std::memcpy(buf, cache_.data() + file_offset_, available);
    const s64 to_fill =
        std::min<s64>(static_cast<s64>(directory_size_), static_cast<s64>(nbytes)) - available - file_offset_;
    if (to_fill < 0) {
        SSPC_LOG(Filesystem, Error, "directory read may have leaked {} bytes", -to_fill);
        return -to_fill + available;
    }
    std::memset(static_cast<u8*>(buf) + available, 0, to_fill);
    file_offset_ += to_fill + available;
    return to_fill + available;
}

s64 Directory::Getdents(void* buf, u64 nbytes, s64* basep) {
    return pfs_ ? GetdentsPfs(buf, nbytes, basep) : GetdentsNormal(buf, nbytes, basep);
}

s64 Directory::GetdentsNormal(void* buf, u64 nbytes, s64* basep) {
    BuildNormal();
    if (basep) {
        *basep = file_offset_;
    }
    if (file_offset_ >= static_cast<s64>(directory_size_)) {
        return 0;
    }
    s64 written = 0;
    s64 working_offset = file_offset_;
    u64 pos = 0;
    const s64 aligned_count = static_cast<s64>(nbytes / 512 * 512);
    while (pos < cache_.size()) {
        const auto* d = reinterpret_cast<const NormalDirent*>(cache_.data() + pos);
        const u16 reclen = d->d_reclen;
        if (d->d_namlen == 0) {
            break;
        }
        if (working_offset >= reclen) {
            pos += reclen;
            working_offset -= reclen;
            continue;
        }
        if (written + reclen > aligned_count) {
            break;
        }
        std::memcpy(static_cast<u8*>(buf) + written, cache_.data() + pos + working_offset,
                    reclen - working_offset);
        written += reclen - working_offset;
        pos += reclen;
        working_offset = 0;
    }
    file_offset_ += written;
    return written;
}

s64 Directory::GetdentsPfs(void* buf, u64 nbytes, s64* basep) {
    if (basep) {
        *basep = file_offset_;
    }
    if (file_offset_ >= static_cast<s64>(directory_size_)) {
        return 0;
    }
    u64 written = 0;
    u64 starting_offset = 0;
    u64 pos = 0;
    while (pos < cache_.size()) {
        const auto* d = reinterpret_cast<const PfsDirent*>(cache_.data() + pos);
        if (d->d_namlen == 0) {
            break;
        }
        if (starting_offset < static_cast<u64>(file_offset_)) {
            starting_offset += d->d_reclen;
            pos = written + starting_offset;
            continue;
        }
        if (written + d->d_reclen > nbytes) {
            break;
        }
        NormalDirent n{};
        n.d_fileno = d->d_fileno;
        n.d_reclen = static_cast<u16>(d->d_reclen);
        n.d_type = d->d_type == 2 ? 8 : 4;
        n.d_namlen = static_cast<u8>(d->d_namlen);
        std::memcpy(n.d_name, d->d_name, d->d_namlen);
        std::memcpy(static_cast<u8*>(buf) + written, &n, n.d_reclen);
        written += n.d_reclen;
        pos = written + starting_offset;
    }
    file_offset_ = pos >= cache_.size() ? static_cast<s64>(directory_size_) : file_offset_ + static_cast<s64>(written);
    return static_cast<s64>(written);
}

s64 Directory::Lseek(s64 offset, s32 whence) {
    const s64 next = (whence == 0 ? offset : 0) + (whence == 1 ? file_offset_ + offset : 0) +
                     (whence == 2 ? static_cast<s64>(directory_size_) + offset : 0);
    if (next < 0) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    file_offset_ = next;
    return file_offset_;
}

void Directory::Fstat(OrbisKernelStat& st) const {
    st.st_mode = 0000777u | 0040000u;
    st.st_size = static_cast<s64>(directory_size_);
    st.st_blksize = pfs_ ? 0x10000 : 0x8000;
    st.st_blocks = pfs_ ? 0x80 : 8;
}

} // namespace SecondSon::Filesystem
