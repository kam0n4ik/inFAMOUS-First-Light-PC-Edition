// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY Phase 1-3 bridge: Filesystem::FsHost on top of the shadPS4 core
// (third_party/shadPS4 @ 0fe263a). What is used, and why it is still BRIDGE:
//   __Error()                    guest errno (Kernel, Phase 2)
//   HandleTable::Create/Delete   descriptor numbers shared with event queues/sockets (Kernel, Phase 2)
//   GuestWriteNotifier           GPU-tracked pages (Memory/GPU, Phase 2/4; patch 0009)
//   MntPoints::GetHostPath       only for the mount points SaveData creates (SaveData, Phase 3)
//   posix_* / open / close ...   descriptors SecondSonPC did not open (std handles, /dev, equeues)
#include <memory>
#include <string>

#include "bridge/shadps4/loader_host.h"
#include "runtime/filesystem/fs_host.h"

#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/guest_memory.h"
#include "core/libraries/kernel/file_system.h"
#include "core/libraries/kernel/kernel.h"

namespace Libraries::Kernel { // defined in core/libraries/kernel/{file_system,kernel}.cpp
s32* PS4_SYSV_ABI __Error();
s32 PS4_SYSV_ABI open(const char* raw_path, s32 flags, u16 mode);
s32 PS4_SYSV_ABI close(s32 fd);
s64 PS4_SYSV_ABI read(s32 fd, void* buf, u64 nbytes);
s64 PS4_SYSV_ABI write(s32 fd, const void* buf, u64 nbytes);
s64 PS4_SYSV_ABI posix_pread(s32 fd, void* buf, u64 nbytes, s64 offset);
s64 PS4_SYSV_ABI posix_pwrite(s32 fd, void* buf, u64 nbytes, s64 offset);
s64 PS4_SYSV_ABI posix_lseek(s32 fd, s64 offset, s32 whence);
s32 PS4_SYSV_ABI fstat(s32 fd, OrbisKernelStat* sb);
s32 PS4_SYSV_ABI posix_fsync(s32 fd);
} // namespace Libraries::Kernel

namespace SecondSon::Bridge {

namespace {

namespace K = Libraries::Kernel;

class FsHostImpl final : public Filesystem::FsHost {
public:
    s32* Errno() override {
        return K::__Error();
    }

    s32 ReserveDescriptor() override {
        auto* handles = Common::Singleton<Core::FileSys::HandleTable>::Instance();
        const int fd = handles->CreateHandle();
        handles->GetFile(fd)->m_guest_name = "<SecondSonPC FileSystem>";
        return fd;
    }

    void ReleaseDescriptor(s32 fd) override {
        Common::Singleton<Core::FileSys::HandleTable>::Instance()->DeleteHandle(fd);
    }

    void InvalidateGuestMemory(u64 address, u64 size) override {
        if (size != 0) {
            Core::GetGuestWriteNotifier().NotifyHostWrite(address, size);
        }
    }

    std::optional<Filesystem::BridgeMount> BridgeMountOf(std::string_view guest_mount) override {
        auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        bool read_only = false;
        auto host = mnt->GetHostPath(guest_mount, &read_only);
        if (host.empty()) {
            return std::nullopt;
        }
        return Filesystem::BridgeMount{std::move(host), read_only};
    }

    s32 ForeignOpen(const char* path, s32 flags, u16 mode) override {
        return K::open(path, flags, mode);
    }
    s32 ForeignClose(s32 fd) override {
        return K::close(fd);
    }
    s64 ForeignRead(s32 fd, void* buf, u64 nbytes) override {
        return K::read(fd, buf, nbytes);
    }
    s64 ForeignWrite(s32 fd, const void* buf, u64 nbytes) override {
        return K::write(fd, buf, nbytes);
    }
    s64 ForeignPread(s32 fd, void* buf, u64 nbytes, s64 offset) override {
        return K::posix_pread(fd, buf, nbytes, offset);
    }
    s64 ForeignPwrite(s32 fd, const void* buf, u64 nbytes, s64 offset) override {
        return K::posix_pwrite(fd, const_cast<void*>(buf), nbytes, offset);
    }
    s64 ForeignLseek(s32 fd, s64 offset, s32 whence) override {
        return K::posix_lseek(fd, offset, whence);
    }
    s32 ForeignFstat(s32 fd, void* sb) override {
        return K::fstat(fd, static_cast<K::OrbisKernelStat*>(sb));
    }
    s32 ForeignFsync(s32 fd) override {
        return K::posix_fsync(fd);
    }
};

static_assert(sizeof(K::OrbisKernelStat) == 120);

} // namespace

std::unique_ptr<Filesystem::FsHost> CreateFsHost() {
    return std::make_unique<FsHostImpl>();
}

} // namespace SecondSon::Bridge
