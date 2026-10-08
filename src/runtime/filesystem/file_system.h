// SecondSonPC — FileSystem (NATIVE, Phase 1): libkernel's file functions over the Vfs.
// Behaviour (flags, errno values, stat fields) follows the Phase 0 oracle,
// third_party/shadPS4/src/core/libraries/kernel/file_system.cpp @ 0fe263a.
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Filesystem {

struct OrbisKernelTimespec {
    s64 tv_sec;
    s64 tv_nsec;
};

/// FreeBSD struct stat as the console's libkernel returns it.
struct OrbisKernelStat {
    u32 st_dev;
    u32 st_ino;
    u16 st_mode;
    u16 st_nlink;
    u32 st_uid;
    u32 st_gid;
    u32 st_rdev;
    OrbisKernelTimespec st_atim;
    OrbisKernelTimespec st_mtim;
    OrbisKernelTimespec st_ctim;
    s64 st_size;
    s64 st_blocks;
    u32 st_blksize;
    u32 st_flags;
    u32 st_gen;
    s32 st_lspare;
    OrbisKernelTimespec st_birthtim;
};
static_assert(sizeof(OrbisKernelStat) == 120);

/// The FileSystem exports (group "fs"): sceKernel* file functions, _read/_write, DiscMap and the
/// SaveData mount observers.
std::span<const Runtime::Export> FileSystemExports();

} // namespace SecondSon::Filesystem
