// SecondSonPC — what the FileSystem needs from subsystems that are still BRIDGE in Phase 1.
//  * guest errno: libkernel's per-thread errno (__error) is Kernel/Threading, BRIDGE until Phase 2;
//  * descriptor numbers: files share one descriptor namespace with event queues and sockets
//    (both BRIDGE), so SecondSonPC takes its numbers from the same table;
//  * GPU-tracked guest pages must be invalidated before a read lands in them (Memory/GPU, BRIDGE);
//  * /savedata0 is mounted by SaveData (BRIDGE until Phase 3): its host folder comes from there;
//  * descriptors SecondSonPC did not open (std handles, /dev devices, event queues, sockets) are
//    served by the BRIDGE implementation.
// Implemented in src/bridge/shadps4/fs_host.cpp. No shadPS4 header is visible here.
#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Filesystem {

struct BridgeMount {
    std::filesystem::path host;
    bool read_only = false;
};

class FsHost {
public:
    virtual ~FsHost() = default;

    virtual s32* Errno() = 0;
    virtual s32 ReserveDescriptor() = 0;
    virtual void ReleaseDescriptor(s32 fd) = 0;
    virtual void InvalidateGuestMemory(u64 address, u64 size) = 0;
    virtual std::optional<BridgeMount> BridgeMountOf(std::string_view guest_mount) = 0;

    // POSIX semantics (-1 and errno on failure), BRIDGE implementations.
    virtual s32 ForeignOpen(const char* path, s32 flags, u16 mode) = 0;
    virtual s32 ForeignClose(s32 fd) = 0;
    virtual s64 ForeignRead(s32 fd, void* buf, u64 nbytes) = 0;
    virtual s64 ForeignWrite(s32 fd, const void* buf, u64 nbytes) = 0;
    virtual s64 ForeignPread(s32 fd, void* buf, u64 nbytes, s64 offset) = 0;
    virtual s64 ForeignPwrite(s32 fd, const void* buf, u64 nbytes, s64 offset) = 0;
    virtual s64 ForeignLseek(s32 fd, s64 offset, s32 whence) = 0;
    virtual s32 ForeignFstat(s32 fd, void* sb) = 0;
    virtual s32 ForeignFsync(s32 fd) = 0;
};

void SetHost(FsHost* host);

/// A descriptor number from the table files share with event queues and sockets (-1: none left), and its release.
s32 ReserveDescriptor();
void ReleaseDescriptor(s32 fd);

/// Host code wrote guest memory [address, address + size): the GPU's copies there (textures, buffers) are refreshed.
void NotifyHostWrite(u64 address, u64 size);

} // namespace SecondSon::Filesystem
