// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — directory descriptors: getdents/read/lseek over a directory. Two entry formats,
// as on the console: PFS (/app0, the package file system) and "normal" (writable mounts).
// Ported from third_party/shadPS4/src/core/file_sys/directories/{base,normal,pfs}_directory.cpp
// @ 0fe263a, reading the host folder directly (entries: ".", "..", then the folder in order).
#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Filesystem {

struct OrbisKernelStat;

class Directory {
public:
    static std::unique_ptr<Directory> Create(const std::filesystem::path& host, bool pfs);

    s64 Read(void* buf, u64 nbytes);
    s64 Getdents(void* buf, u64 nbytes, s64* basep);
    s64 Lseek(s64 offset, s32 whence); // ORBIS error code (< 0) on failure
    void Fstat(OrbisKernelStat& st) const;
    s64 Tell() const {
        return file_offset_;
    }
    void SetOffset(s64 offset) {
        file_offset_ = offset;
    }

private:
    Directory(std::filesystem::path host, bool pfs);
    void Build();
    void BuildNormal();
    s64 ReadPfs(void* buf, u64 nbytes);
    s64 GetdentsPfs(void* buf, u64 nbytes, s64* basep);
    s64 GetdentsNormal(void* buf, u64 nbytes, s64* basep);

    std::filesystem::path host_;
    bool pfs_;
    std::vector<u8> cache_;
    s64 file_offset_ = 0;
    s64 previous_file_offset_ = -1;
    u64 directory_size_ = 0;
};

} // namespace SecondSon::Filesystem
