// SecondSonPC — the guest memory as a GPU backend uses it (Phase 4, P4.1.3; docs/PHASE4_P41_GPU_FOUNDATION.md §5).
//
// The bridge's contract (Core::GpuMemory / GpuMemoryListener, patch 0008, docs/PHASE2_P232_GPU_MEMORY.md §3) lives in
// the bridge's headers, which SecondSonPC's own code may not include (P3.6 boundary). This is the same contract in
// SecondSonPC's headers, so a SecondSonPC backend can be written against it; the adapter layer
// (src/bridge/shadps4/gpu_memory_host.cpp) implements it over the selected guest memory manager (the bridge's, or
// SecondSonPC's with --native memory — the one manager that owns the guest address space either way). There is no
// second owner of mappings or physical memory: a backend only reads, writes past protection and sets tracking
// protection through this interface.
//
// Semantics (unchanged from the bridge's): notifications for one map/unmap/release, in this order — OnUnmap (before
// the address space changes, writer lock not held) → OnRegister (host pages mapped or unmapped, writer lock held) →
// OnMap (writer lock not held); the operation's unmap lock is held throughout, except for a PRT area. A listener must
// not map or unmap guest memory. CopySparse never faults (unmapped pages read as zeros, a racing unmap reads stale
// data); TryWriteBacking writes past page protection and runs in the access violation handler too; ProtectTracking
// runs there as well: no allocation, no logging.
//
// One listener: SetListener takes it for the owner of Resource::MemoryListener (backend.h) only, and refuses while any
// other listener is registered with the manager — including one the bridge's rasterizer registered directly. The
// notifications are not synchronized with SetListener: one already running may still reach the previous listener after
// SetListener returns, so a listener stays alive while guest memory can change (register at start-up, keep it). The
// bridge's manager needs a listener at every map/unmap; the adapter never leaves its slot empty.
#pragma once

#include <string>

#include "base/abi.h"
#include "runtime/gpu/backend.h"

namespace SecondSon::Gpu {

/// Host protection of mapped guest pages for GPU write tracking; the guest's own protection stays.
enum class TrackingProtection : u8 { None, Read, ReadWrite };

class MemoryListener {
public:
    virtual ~MemoryListener() = default;
    /// [addr, addr + size) is about to be unmapped or replaced: write back and drop GPU caches.
    virtual void OnUnmap(u64 addr, u64 size) = 0;
    /// The host pages of [addr, addr + size) were mapped or unmapped (page tracking tables).
    virtual void OnRegister(u64 addr, u64 size) = 0;
    /// [addr, addr + size) is mapped and visible to the GPU.
    virtual void OnMap(u64 addr, u64 size) = 0;
    /// Host code wrote [addr, addr + size) past the page tracking (file reads, mprotect).
    virtual void OnGuestWrite(u64 addr, u64 size) = 0;
};

class GuestMemory {
public:
    /// Smaller ranges are not clamped and cost no lookup.
    static constexpr u64 kMinSizeToClamp = 1ull << 30;

    /// The GPU's 40-bit address space: [addr, addr + size) inside [0, 2^40), without wrapping.
    static constexpr bool IsValidGpuMapping(u64 addr, u64 size) {
        constexpr u64 kMax = 1ull << 40;
        return addr < kMax && size <= kMax - addr;
    }

    virtual ~GuestMemory() = default;

    /// Registers `listener` for `owner`, which must hold Resource::MemoryListener; nullptr unregisters (the owner only).
    /// false with `error`: not the owner, or another listener is registered with the manager.
    virtual bool SetListener(MemoryListener* listener, BackendKind owner, std::string* error) = 0;

    u64 ClampRangeSize(u64 addr, u64 size) {
        return size < kMinSizeToClamp ? size : ClampLargeRange(addr, size);
    }
    virtual void CopySparse(u64 addr, u8* dest, u64 size) = 0;
    virtual bool TryWriteBacking(void* address, const void* data, u64 size) = 0;
    virtual void ProtectTracking(u64 addr, u64 size, TrackingProtection protection) = 0;
    /// End of the system reserved part of the guest address space (constant).
    virtual u64 SystemReservedEnd() = 0;

protected:
    virtual u64 ClampLargeRange(u64 addr, u64 size) = 0;
};

/// The process's guest memory for a GPU backend: installed by the adapter layer at start-up once the guest memory
/// manager is selected; nullptr before. Valid for the process lifetime.
GuestMemory* GetGuestMemory();
void InstallGuestMemory(GuestMemory* memory);

} // namespace SecondSon::Gpu
