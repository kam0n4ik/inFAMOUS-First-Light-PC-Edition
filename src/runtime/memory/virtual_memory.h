// SecondSonPC — the guest's virtual memory manager (Phase 2, P2.3.3b/c; EXPERIMENTAL: only the self-test
// creates one, the game runs on the bridge's manager; docs/PHASE2_P233_NATIVE_VMM.md).
//
// Owns a guest address space (HostAddressSpace), its areas (VmaMap), the physical memory behind them (a
// DirectMemory for direct memory, FlexibleMemory for flexible memory, both offsets in one section) and the
// notifications to the GPU. Semantics are the bridge's MemoryManager (shadPS4 0fe263a, the oracle), at the
// memory manager's level (libkernel's argument checks are the caller's), minus these defects of the oracle,
// each with a test: D5 (an invalid range asserts), D6 (flexible budget checked before the lock), D10 (GPU
// range check wraps), D11 (a backing write across two physical areas repeats the data), D12 (backing reads
// past 2^40), D13 (IsValidMapping without a lock), D16 (gaps between the host's regions count as valid), D17
// (unmapping anonymous memory split into several areas asserts), D19 (what stays of a partly unmapped view loses
// its protection: the guest's and the GPU's page tracking), H4 (allocation in TryWriteBacking).
// Not supported (no user, an explicit error): memory pools (Pooled/PoolReserved), file mappings.
//
// Locks, outermost first (the bridge's order): the unmap lock -> the writer/reader lock (readers first) ->
// the host's leaf lock -> DirectMemory's lock. GPU notifications: OnGpuUnmap before the address space
// changes (unmap lock held, writer lock not), OnGpuRegister after host pages were mapped or unmapped (both
// held), OnGpuMap afterwards (unmap lock only); a PRT area without locks. Lock-free: CopySparse (the
// backing table, H2). In the access violation handler: ProtectTracking (the host's leaf lock only).
#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "runtime/memory/direct_memory.h"
#include "runtime/memory/host_address_space.h"
#include "runtime/memory/vma_map.h"

namespace SecondSon::Memory {

constexpr s32 kErrEacces = static_cast<s32>(0x8002000D);
constexpr u64 kNoPhys = ~0ull;

/// The libkernel map flags (Core::MemoryMapFlags) the manager looks at; the others are the API's.
namespace MapFlag {
constexpr u32 kFixed = 0x10;
constexpr u32 kNoOverwrite = 0x80;
constexpr u32 kNoCoalesce = 0x400000;
} // namespace MapFlag

/// The GPU side (Core::GpuMemoryListener, docs/PHASE2_P232_GPU_MEMORY.md §3.2).
class GpuListener {
public:
    virtual ~GpuListener() = default;
    /// [addr, addr + size) is about to be unmapped or replaced.
    virtual void OnGpuUnmap(u64 addr, u64 size) = 0;
    /// Host pages of [addr, addr + size) were mapped or unmapped.
    virtual void OnGpuRegister(u64 addr, u64 size) = 0;
    /// [addr, addr + size) is mapped and visible to the GPU.
    virtual void OnGpuMap(u64 addr, u64 size) = 0;
    /// The host writes [addr, addr + size) past the page tracking.
    virtual void OnGuestWrite(u64 addr, u64 size) = 0;
};

struct Layout {
    std::vector<AddressRange> ranges; ///< the guest address space, taken from the host
    u64 lowest = 0;               ///< searches and queries start no lower (the bridge: its first region)
    u64 search_base = 0;          ///< a search without an address starts here (0x200000000 in the game)
    u64 search_limit = 0;         ///< a search returns addresses below this (the end of the user area)
    u64 system_reserved_base = 0; ///< 0x7FFFFC000 in the game
    u64 system_reserved_end = 0;  ///< 0x1000000000 in the game
    AddressRange gpu;             ///< GPU-visible addresses: [0, 2^40) in the game
    u64 direct_size = 0;          ///< direct memory before SetupRegions: section [0, direct_size)
    u64 flexible_base = 0;        ///< section offset of the flexible memory (>= direct_size)
    u64 flexible_size = 0;        ///< flexible memory (capacity and the budget before SetupRegions)
    CommitMode commit = CommitMode::Upfront;
    u32 sdk_version = 0; ///< direct mappings merge with their neighbours from SDK 2.00 on
};

/// sceKernelVirtualQuery's answer.
struct QueryInfo {
    u64 start = 0;
    u64 end = 0;
    u64 offset = 0;
    s32 protection = 0;
    s32 memory_type = 0;
    bool is_flexible = false;
    bool is_direct = false;
    bool is_stack = false;
    bool is_pooled = false;
    bool is_committed = false;
    std::string name;
};

struct AreaInfo {
    u64 base = 0;
    u64 size = 0;
    VmaType type = VmaType::Free;
    u32 prot = 0;
    u64 phys = kNoPhys;  ///< the first physical address
    u32 phys_slices = 0; ///< physical pieces behind it
    std::string name;
};

/// The physical flexible memory: [base, base + size) of the section, handed out lowest free first, in
/// pieces (the bridge's fmem_map).
class FlexibleMemory {
public:
    void Configure(u64 base, u64 size);
    /// `size` bytes in the lowest free pieces; all or nothing.
    bool Allocate(u64 size, std::vector<PhysRange>* out);
    void Free(u64 base, u64 size);
    u64 Used() const {
        return used_;
    }
    std::string CheckInvariants() const;

private:
    std::map<u64, u64> free_; ///< base -> size, adjacent ones merged
    u64 base_ = 0;
    u64 size_ = 0;
    u64 used_ = 0;
};

/// A reader/writer lock where readers go first (the bridge's Common::SharedFirstMutex): a waiting writer
/// never holds up a reader, so a reader may take it again while another one holds it.
class ReaderFirstMutex {
public:
    void lock();
    void unlock();
    void lock_shared();
    bool try_lock_shared();
    void unlock_shared();

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    u32 readers_ = 0;
    bool writer_ = false;
};

/// Per page of the GPU-visible addresses: a pointer into the section, or nullptr. Lock-free reads; written
/// under the writer lock. Leaves (16 MiB of addresses each) are created on first use and live as long as
/// the table, so a reader never sees a freed one.
class BackingTable {
public:
    void Init(const AddressRange& covered);
    /// nullptr clears.
    void Set(u64 addr, u64 size, const u8* backing);
    const u8* Find(u64 addr) const;

private:
    static constexpr u64 kLeafPages = 1024;
    using Leaf = std::array<std::atomic<const u8*>, kLeafPages>;
    AddressRange covered_{};
    std::unique_ptr<std::atomic<Leaf*>[]> top_;
    u64 top_size_ = 0;
    std::vector<std::unique_ptr<Leaf>> leaves_;
};

class VirtualMemory final : public Unmapper {
public:
    struct MapRequest {
        u64 addr = 0; ///< with kFixed the address, else where the search starts (0: search_base)
        u64 size = 0;
        u32 prot = Prot::kNone;
        u32 flags = 0;
        VmaType type = VmaType::Reserved;
        std::string_view name = "anon";
        u64 phys = kNoPhys;     ///< Direct only
        bool exclusive = false; ///< Direct: EBUSY when a page is already mapped
        u64 alignment = 0;      ///< 0: one page
    };

    struct Stats {
        u64 maps = 0;
        u64 unmaps = 0;
        u64 protects = 0;
        u64 failures = 0;
        u64 gpu_unmap = 0;
        u64 gpu_register = 0;
        u64 gpu_map = 0;
        u64 gpu_write = 0;
        u64 vma_count = 0;
        VmaMap::Counters vma;
        HostAddressSpace::Counters host;
    };

    /// nullptr and *error when the layout is inconsistent or the host refuses its ranges.
    static std::unique_ptr<VirtualMemory> Create(const Layout& layout, std::string* error);
    ~VirtualMemory() override;
    VirtualMemory(const VirtualMemory&) = delete;
    VirtualMemory& operator=(const VirtualMemory&) = delete;

    // --- virtual memory (Core::GuestVirtualMemory)
    /// 0 or a libkernel error code; *out = where it was mapped.
    s32 Map(const MapRequest& request, u64* out);
    s32 Unmap(u64 addr, u64 size);
    s32 Protect(u64 addr, u64 size, u32 prot);
    s32 NameRange(u64 addr, u64 size, std::string_view name);
    /// addr (and with a size [addr, addr + size)) inside one managed range.
    bool IsValid(u64 addr, u64 size) const;
    s32 Query(u64 addr, bool find_next, QueryInfo* info) const;
    s32 QueryProtection(u64 addr, u64* start, u64* end, u32* prot) const;
    std::vector<AreaInfo> Areas(bool with_free) const;
    u64 SystemReservedBase() const {
        return layout_.system_reserved_base;
    }
    u64 SystemReservedEnd() const {
        return layout_.system_reserved_end;
    }

    // --- physical memory (Core::GuestPhysicalMemory)
    /// The budgets from the executable's SceKernelMemParam: direct = DirectMemorySize(budget) (it has to fit
    /// in the section's direct part and its cut-off part must be free), flexible = the flexible size + the
    /// extra flexible memory - 64 MiB (it has to cover what is in use).
    bool SetupRegions(const Budget& budget);
    u64 DirectSize() const;
    u64 FlexibleBudget() const;
    u64 FlexibleUsed() const;
    /// DirectMemory::Allocate; with OnDemand the pages are committed (ENOMEM when the system refuses).
    s32 AllocateDirect(u64 search_start, u64 search_end, u64 size, u64 alignment, s32 memory_type, u64* out);
    /// DirectMemory::Release: the views are unmapped first (GPU first), see UnmapPhysical.
    s32 ReleaseDirect(u64 start, u64 size, bool checked);

    // --- notifications (Core::GuestWriteNotifier)
    void NotifyHostWrite(u64 addr, u64 size);

    // --- the GPU (Core::GpuMemory)
    GpuListener* SetListener(GpuListener* listener);
    /// [addr, addr + size) inside the layout's GPU-visible range (no wrap, D10).
    bool IsGpuVisible(u64 addr, u64 size) const;
    /// Through the backing table, no lock: pages without physical memory, or outside the GPU-visible range,
    /// read as zeros (H3, D12).
    void CopySparse(u64 addr, u8* dest, u64 size) const;
    /// Writes through the section past any protection (H2): the areas with physical memory from addr on,
    /// each piece its own part of the data (D11); false when addr has no physical memory or is invalid. No
    /// allocation (H4).
    bool TryWriteBacking(u64 addr, const void* data, u64 size);
    /// Host protection of the mapped pages (GPU page tracking); the guest's protection stays. VEH-safe.
    bool ProtectTracking(u64 addr, u64 size, HostAccess access);
    /// size, or less: the rest of addr's area plus the adjacent mapped areas that follow (a snapshot).
    u64 ClampRange(u64 addr, u64 size);
    void SetPrtArea(u32 id, u64 addr, u64 size);

    // --- DirectMemory's release
    s32 UnmapPhysical(std::span<const PhysRange> ranges) override;

    // --- self-test and diagnostics
    /// The physical address (section offset) behind addr, kNoPhys without one.
    u64 PhysicalAddress(u64 addr) const;
    Stats GetStats() const;
    /// "" when the areas, the host's regions, the backing table, the direct and flexible maps agree.
    std::string CheckInvariants() const;
    /// The writer lock is held right now (by anyone).
    bool WriterLocked() const;
    const Layout& GetLayout() const {
        return layout_;
    }
    const DirectMemory& Direct() const {
        return direct_;
    }
    const HostAddressSpace& Host() const {
        return *host_;
    }
    u8* BackingBase() const {
        return host_->BackingBase();
    }

    /// Smaller ranges are not clamped (Core::GpuMemory::MinSizeToClamp, the bridge's ClampRangeSize).
    static constexpr u64 kMinSizeToClamp = 1ull << 30;

private:
    static constexpr u64 kNoAddr = ~0ull;

    VirtualMemory() = default;
    u64 SearchFree(u64 addr, u64 size, u64 alignment) const;
    /// Under both locks: everything in [addr, addr + size) unmapped, area by area (the bridge's
    /// UnmapBytesFromEntry loop); host pages in one go afterwards (private memory is never split: checked
    /// by the caller), then one OnGpuRegister per area that had host pages.
    void UnmapRange(u64 addr, u64 size);
    /// Under both locks: one area's protection (the bridge's ProtectBytes); returns the bytes handled.
    u64 ProtectArea(u64 addr, u64 size, u32 prot);
    void NotifyUnmap(u64 addr, u64 size);
    void NotifyRegister(u64 addr, u64 size);
    void NotifyMap(u64 addr, u64 size);

    Layout layout_;
    std::unique_ptr<HostAddressSpace> host_;
    VmaMap areas_;
    DirectMemory direct_;
    FlexibleMemory flexible_;
    BackingTable backing_;
    u64 flexible_budget_ = 0;
    mutable std::mutex unmap_mutex_;
    mutable ReaderFirstMutex mutex_;
    std::atomic<GpuListener*> listener_{nullptr};
    struct PrtArea {
        u64 start = 0;
        u64 end = 0;
        bool mapped = false;
    };
    std::mutex prt_mutex_; ///< the PRT table only; the notifications go without it
    std::array<PrtArea, 3> prt_{};
    mutable std::atomic<u64> maps_{0}, unmaps_{0}, protects_{0}, failures_{0};
    mutable std::atomic<u64> gpu_unmap_{0}, gpu_register_{0}, gpu_map_{0}, gpu_write_{0};
};

/// The game's address layout the way the bridge computes it (core/address_space.cpp, memory.cpp @ 0fe263a):
/// every free host region larger than 16 MiB in [0x400000, user_max], aligned to the allocation
/// granularity; user_max = 1 TiB on Windows builds <= 22621 or SDK >= 3.00, else 0x5FFFFFFFFFFF; direct
/// memory 7936 MiB (+extra) at section offset 0, flexible 512 MiB (+extra) right after it.
struct HostRegionInfo {
    u64 base = 0;
    u64 size = 0;
    bool free = false;
};
Layout GameLayout(std::span<const HostRegionInfo> host_map, u32 sdk_version, u32 windows_build, u64 granularity,
                  u32 extra_dmem_mib, u32 extra_fmem_mib);

} // namespace SecondSon::Memory
