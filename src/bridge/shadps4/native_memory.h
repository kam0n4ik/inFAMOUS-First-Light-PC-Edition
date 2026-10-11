// SecondSonPC — the bridge's memory interfaces over SecondSonPC's own memory manager (Phase 2, P2.3.3c; the game
// runs on it with --native memory since P2.3.4, EXPERIMENTAL; the bridge's MemoryManager is the default;
// docs/PHASE2_P233_NATIVE_VMM.md, docs/PHASE2_P234_NATIVE_MEMORY.md).
//
// Core::GuestVirtualMemory, Core::GuestPhysicalMemory and Core::GuestWriteNotifier (patches 0009, 0012) and
// Core::GpuMemory with its Core::GpuMemoryListener (patch 0008) over a Memory::VirtualMemory. No state but the
// listener and the budget's console part: each call is the manager's call with the same arguments, the way
// the bridge's adapters (memory.cpp) call its MemoryManager.
#pragma once

#include <atomic>

#include "app/config.h"
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
    // patch 0012 (libkernel's memory functions)
    s32 Query(VAddr addr, s32 flags, Libraries::Kernel::OrbisVirtualQueryInfo* info) override;
    s32 QueryProtection(VAddr addr, void** start, void** end, u32* prot) override;
    s32 IsStack(VAddr addr, void** start, void** end) override;
    s32 SetDirectMemoryType(VAddr addr, u64 size, s32 memory_type) override; ///< not supported
    s32 MapFile(void** out_addr, VAddr addr, u64 size, Core::MemoryProt prot, Core::MemoryMapFlags flags, s32 fd,
                s64 offset) override; ///< not supported
    s32 PoolCommit(VAddr addr, u64 size, Core::MemoryProt prot, s32 memory_type) override; ///< not supported
    s32 PoolDecommit(VAddr addr, u64 size) override;                                        ///< not supported
    void SetPrtArea(u32 id, VAddr addr, u64 size) override;

    // Core::GuestPhysicalMemory
    void SetupRegions(u64 flexible_size, bool use_extended_mem1, bool use_extended_mem2) override;
    u64 DirectSize() override;
    PAddr AllocateDirect(PAddr search_start, PAddr search_end, u64 size, u64 alignment, s32 memory_type) override;
    s32 ReleaseDirect(PAddr start, u64 size, bool checked) override;
    // patch 0012 (libkernel's memory functions)
    u64 FlexibleSize() override;
    u64 FlexibleAvailable() override;
    s32 DirectQuery(PAddr addr, bool find_next, Libraries::Kernel::OrbisQueryInfo* info) override;
    s32 DirectAvailable(PAddr search_start, PAddr search_end, u64 alignment, PAddr* out, u64* size) override;
    s32 DirectMemoryType(PAddr addr, s32* type, void** start, void** end) override;
    PAddr PoolExpand(PAddr search_start, PAddr search_end, u64 size, u64 alignment) override; ///< not supported
    s32 PoolStats(Libraries::Kernel::OrbisKernelMemoryPoolBlockStats* stats) override;

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
    bool BackingRun(VAddr addr, u8** host, VAddr* run_begin, u64* run_size) override;
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

/// The game's guest memory on SecondSonPC's manager (--native memory, P2.3.4).
struct NativeGame {
    Memory::VirtualMemory* vm = nullptr;
    NativeGuestMemory* guest = nullptr;
    NativeGpuMemory* gpu = nullptr;
};

/// --native memory: SecondSonPC's manager takes the game's address space the way the bridge's would (GameLayout over
/// the host's free regions now, the 8448 MiB section; commit upfront, or on demand with --memory-commit on-demand)
/// and becomes the guest memory (Core::SelectGuestMemory). At start-up, before anything reaches the guest memory.
/// false, with the reason in the log, when the host refuses or the selection is closed; the bridge's manager stays.
bool InstallNativeMemory(const App::Config& cfg, s32 sdk_version, u32 psf_attributes);

/// What InstallNativeMemory installed; all null without --native memory. Lives until the process ends.
NativeGame GetNativeGame();

/// --selftest --native memory: the manager the game would run on, started as for the game, through the bridge's
/// libkernel functions and the interfaces (docs/PHASE2_P234_NATIVE_MEMORY.md). Exit code 0 when everything passed.
int RunNativeMemorySelfTest(const App::Config& cfg);

} // namespace SecondSon::Bridge
