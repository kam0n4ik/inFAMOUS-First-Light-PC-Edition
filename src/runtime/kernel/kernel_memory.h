// SecondSonPC — libkernel's memory functions on SecondSonPC's own memory manager (Phase 3, P3.5.2, EXPERIMENTAL:
// --native kernel-memory, which needs --native memory; docs/PHASE3_P35_KERNEL_HLE.md §3).
//
// The game's memory imports (eboot.bin: sceKernelGetDirectMemorySize, AllocateDirectMemory, MapDirectMemory,
// ReleaseDirectMemory, Mprotect, QueryMemoryProtection; libc.prx: mmap, sceKernelMapNamedFlexibleMemory,
// sceKernelMunmap) as SecondSonPC's functions over Memory::VirtualMemory — the manager that already owns the game's
// address space, its physical memory and the GPU notifications with --native memory (P2.3.4). Until now the bridge's
// functions (core/libraries/kernel/memory.cpp, patch 0012) reached that manager through the bridge's interfaces
// (bridge/shadps4/native_memory.cpp); these do the same calls with the same arguments, so ownership does not move:
// the manager keeps every area, physical page and protection, and notifies the GPU bridge itself (unmap before the
// address space changes, register/map after; a host write after mprotect), exactly as on the bridge's path.
//
// Argument checks, their order and the error codes are the Phase 0 oracle's (third_party/shadPS4/src/core/libraries/
// kernel/memory.cpp @ 0fe263a + patch 0012, not verified on a PS4) except where noted in kernel_memory.cpp (a null
// pointer argument the oracle dereferences, a size that wraps). With the bridge's memory manager (no --native memory)
// the bridge's functions stay: they are the API of its manager.
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Memory {
class VirtualMemory;
}
namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Kernel::MemoryApi {

/// libkernel's memory functions over one manager (the game's, or a self-test's). Results are libkernel's
/// (0 or 0x8002xxxx); Mmap gives the POSIX result and the errno value separately.
class Api {
public:
    Api(Memory::VirtualMemory& vm, u32 sdk_version) : vm_{vm}, sdk_{sdk_version} {}

    u64 GetDirectMemorySize() const;
    s32 AllocateDirectMemory(s64 search_start, s64 search_end, u64 len, u64 alignment, s32 memory_type,
                             s64* phys_out);
    s32 ReleaseDirectMemory(u64 start, u64 len);
    s32 MapDirectMemory(void** addr, u64 len, s32 prot, s32 flags, s64 phys, u64 alignment);
    s32 MapNamedFlexibleMemory(void** addr, u64 len, s32 prot, s32 flags, const char* name);
    s32 Munmap(void* addr, u64 len);
    s32 Mprotect(const void* addr, u64 size, s32 prot);
    s32 QueryMemoryProtection(void* addr, void** start, void** end, u32* prot);
    /// POSIX mmap: the address, or MAP_FAILED ((void*)-1) with *error = the errno value (FreeBSD's).
    void* Mmap(void* addr, u64 len, s32 prot, s32 flags, s32 fd, s64 offset, s32* error);

private:
    s32 MapNamedDirectMemory(void** addr, u64 len, s32 prot, s32 flags, s64 phys, u64 alignment, const char* name);

    Memory::VirtualMemory& vm_;
    u32 sdk_;
};

/// --native kernel-memory: the game's exports work on `vm` (the game's manager, --native memory) from now on. Once,
/// at start-up, right after the manager became the guest memory and before guest code.
void Install(Memory::VirtualMemory& vm, u32 sdk_version);
bool Installed();

/// Group "kernel-memory" (experimental).
std::span<const Runtime::Export> Exports();

} // namespace SecondSon::Kernel::MemoryApi
