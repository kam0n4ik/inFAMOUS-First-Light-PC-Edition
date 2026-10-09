// SecondSonPC — the bridge's memory interfaces over SecondSonPC's own memory manager (Phase 2, P2.3.3c;
// EXPERIMENTAL: only the self-test creates these, the game runs on the bridge's MemoryManager until P2.3.4;
// docs/PHASE2_P233_NATIVE_VMM.md).
//
// Core::GuestVirtualMemory, Core::GuestPhysicalMemory and Core::GuestWriteNotifier (patch 0009) and
// Core::GpuMemory with its Core::GpuMemoryListener (patch 0008) over a Memory::VirtualMemory. No state but the
// listener and the budget's console part: each call is the manager's call with the same arguments, the way
// the bridge's adapters (memory.cpp) call its MemoryManager.
#pragma once

#include <atomic>

#include "core/gpu_memory.h"
#include "core/guest_memory.h"
#include "runtime/memory/virtual_memory.h"

namespace SecondSon::Bridge {

class NativeGuestMemory final : public Core::GuestVirtualMemory,
                                public Core::GuestPhysicalMemory,
                                public Core::GuestWriteNotifier {
public:
    /// `console`: the budget's part that comes from the configuration (Neo, dev kit, extra memory);
    /// SetupRegions adds the executable's part.
    NativeGuestMemory(Memory::VirtualMemory& vm, const Memory::Budget& console) : vm_{vm}, console_{console} {}

    // Core::GuestVirtualMemory
    void Reserve() override {} // the manager took its ranges when it was created
    s32 Map(void** out_addr, const Core::GuestMapping& mapping) override;
    s32 Unmap(VAddr addr, u64 size) override;
    s32 Protect(VAddr addr, u64 size, Core::MemoryProt prot) override;
    void NameRange(VAddr addr, u64 size, std::string_view name) override;
    bool IsValid(VAddr addr, u64 size) override;
    VAddr SystemReservedBase() override;
    std::vector<Core::GuestArea> Areas() override;

    // Core::GuestPhysicalMemory
    void SetupRegions(u64 flexible_size, bool use_extended_mem1, bool use_extended_mem2) override;
    u64 DirectSize() override;
    PAddr AllocateDirect(PAddr search_start, PAddr search_end, u64 size, u64 alignment, s32 memory_type) override;
    s32 ReleaseDirect(PAddr start, u64 size, bool checked) override;

    // Core::GuestWriteNotifier
    void NotifyHostWrite(VAddr addr, u64 size) override;

private:
    Memory::VirtualMemory& vm_;
    Memory::Budget console_;
};

class NativeGpuMemory final : public Core::GpuMemory {
public:
    explicit NativeGpuMemory(Memory::VirtualMemory& vm) : vm_{vm}, forward_{this} {}
    ~NativeGpuMemory() override;
    NativeGpuMemory(const NativeGpuMemory&) = delete;
    NativeGpuMemory& operator=(const NativeGpuMemory&) = delete;

    Core::GpuMemoryListener* SetListener(Core::GpuMemoryListener* listener) override;
    void CopySparseMemory(VAddr addr, u8* dest, u64 size) override;
    bool TryWriteBacking(void* address, const void* data, u64 size) override;
    void ProtectTracking(VAddr addr, u64 size, Core::MemoryPermission perms) override;
    VAddr SystemReservedEnd() override;

protected:
    u64 ClampLargeRange(VAddr addr, u64 size) override;

private:
    /// The manager's listener: hands each notification on unchanged, in the manager's thread and locks.
    class Forward final : public Memory::GpuListener {
    public:
        explicit Forward(NativeGpuMemory* owner) : owner_{owner} {}
        void OnGpuUnmap(u64 addr, u64 size) override;
        void OnGpuRegister(u64 addr, u64 size) override;
        void OnGpuMap(u64 addr, u64 size) override;
        void OnGuestWrite(u64 addr, u64 size) override;

    private:
        NativeGpuMemory* owner_;
    };

    Memory::VirtualMemory& vm_;
    Forward forward_;
    std::atomic<Core::GpuMemoryListener*> listener_{nullptr};
};

} // namespace SecondSon::Bridge
