// SecondSonPC — the bridge's memory interfaces over SecondSonPC's own memory manager (P2.3.3c).
#include "bridge/shadps4/native_memory.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include <windows.h>

#include "base/log.h"
#include "core/libraries/kernel/memory.h"
#include "runtime/kernel/kernel_memory.h"

namespace SecondSon::Bridge {

namespace {

/// What the manager does not have (no user in this game: docs/PHASE2_P234_NATIVE_MEMORY.md): an error in the
/// log and the code, never a silent answer.
s32 NotSupported(std::string_view what) {
    SSPC_LOG(Memory, Critical, "native memory: {} is not supported by SecondSonPC's memory manager", what);
    return Memory::kErrEinval;
}

u64 AlignUpTo(u64 value, u64 alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

/// The bridge's AddressSpace::Protect takes these combinations; write implies read on the host.
Memory::HostAccess TrackingAccess(Core::MemoryPermission perms) {
    const u32 bits = static_cast<u32>(perms);
    const bool read = (bits & static_cast<u32>(Core::MemoryPermission::Read)) != 0;
    const bool write = (bits & static_cast<u32>(Core::MemoryPermission::Write)) != 0;
    const bool exec = (bits & static_cast<u32>(Core::MemoryPermission::Execute)) != 0;
    if (exec) {
        return write ? Memory::HostAccess::ExecuteReadWrite
                     : read ? Memory::HostAccess::ExecuteRead : Memory::HostAccess::Execute;
    }
    return write ? Memory::HostAccess::ReadWrite : read ? Memory::HostAccess::Read : Memory::HostAccess::None;
}

} // namespace

// ---------------------------------------------------------------------------- NativeGuestMemory

s32 NativeGuestMemory::Map(void** out_addr, const Core::GuestMapping& m) {
    const Memory::VirtualMemory::MapRequest request{
        .addr = m.addr,
        .size = m.size,
        .prot = static_cast<u32>(m.prot),
        .flags = static_cast<u32>(m.flags),
        .type = static_cast<Memory::VmaType>(m.type),
        .name = m.name,
        .phys = static_cast<u64>(m.phys),
        .exclusive = m.exclusive,
        .alignment = m.alignment,
    };
    u64 out = 0;
    const s32 result = vm_.Map(request, &out);
    if (result == Memory::kOk) {
        *out_addr = reinterpret_cast<void*>(out);
    }
    return result;
}

s32 NativeGuestMemory::Unmap(VAddr addr, u64 size) {
    return vm_.Unmap(addr, size);
}

s32 NativeGuestMemory::Protect(VAddr addr, u64 size, Core::MemoryProt prot) {
    return vm_.Protect(addr, size, static_cast<u32>(prot));
}

void NativeGuestMemory::NameRange(VAddr addr, u64 size, std::string_view name) {
    if (const s32 result = vm_.NameRange(addr, size, name); result != Memory::kOk) {
        // The bridge asserts on a range it does not manage.
        SSPC_LOG(Memory, Error, "native memory: cannot name [{:#x}, +{:#x}) '{}': {:#x}", addr, size, name,
                 static_cast<u32>(result));
    }
}

bool NativeGuestMemory::IsValid(VAddr addr, u64 size) {
    return vm_.IsValid(addr, size);
}

VAddr NativeGuestMemory::SystemReservedBase() {
    return vm_.SystemReservedBase();
}

std::vector<Core::GuestArea> NativeGuestMemory::Areas() {
    std::vector<Core::GuestArea> out;
    for (auto& a : vm_.Areas(false)) {
        out.push_back(Core::GuestArea{
            .base = a.base,
            .size = a.size,
            .type = static_cast<Core::VMAType>(a.type),
            .prot = static_cast<Core::MemoryProt>(a.prot),
            .phys = static_cast<PAddr>(a.phys),
            .phys_slices = a.phys_slices,
            .name = std::move(a.name),
        });
    }
    return out;
}

s32 NativeGuestMemory::Query(VAddr addr, s32 flags, Libraries::Kernel::OrbisVirtualQueryInfo* info) {
    // The bridge's MemoryManager::VirtualQuery: flags 1 looks for the next area in use. Every field it sets is set
    // here (the struct's other bits stay as the caller had them, as there).
    Memory::QueryInfo q;
    const s32 r = vm_.Query(addr, flags == 1, &q);
    if (r != Memory::kOk) {
        return r;
    }
    info->start = q.start;
    info->end = q.end;
    info->offset = q.offset;
    info->protection = q.protection;
    info->memory_type = q.memory_type;
    info->is_flexible = q.is_flexible ? 1 : 0;
    info->is_direct = q.is_direct ? 1 : 0;
    info->is_stack = q.is_stack ? 1 : 0;
    info->is_pooled = q.is_pooled ? 1 : 0;
    info->is_committed = q.is_committed ? 1 : 0;
    std::strncpy(info->name, q.name.c_str(), Libraries::Kernel::ORBIS_KERNEL_MAXIMUM_NAME_LENGTH);
    return Memory::kOk;
}

s32 NativeGuestMemory::QueryProtection(VAddr addr, void** start, void** end, u32* prot) {
    u64 s = 0;
    u64 e = 0;
    u32 p = 0;
    const s32 r = vm_.QueryProtection(addr, &s, &e, &p);
    if (r != Memory::kOk) {
        return r;
    }
    if (start != nullptr) {
        *start = reinterpret_cast<void*>(s);
    }
    if (end != nullptr) {
        *end = reinterpret_cast<void*>(e);
    }
    if (prot != nullptr) {
        *prot = p;
    }
    return Memory::kOk;
}

s32 NativeGuestMemory::IsStack(VAddr addr, void** start, void** end) {
    // The bridge's MemoryManager::IsStack: EACCES on free memory (it asserts outside its map), else the stack's
    // range or [0, 0).
    Memory::QueryInfo q;
    const s32 r = vm_.Query(addr, false, &q);
    if (r != Memory::kOk) {
        return r;
    }
    if (start != nullptr) {
        *start = reinterpret_cast<void*>(q.is_stack ? q.start : 0);
    }
    if (end != nullptr) {
        *end = reinterpret_cast<void*>(q.is_stack ? q.end : 0);
    }
    return Memory::kOk;
}

s32 NativeGuestMemory::SetDirectMemoryType(VAddr addr, u64 size, s32 memory_type) {
    (void)addr;
    (void)size;
    (void)memory_type;
    return NotSupported("changing the memory type of mapped direct memory (sceKernelMapDirectMemory2, "
                        "sceKernelMtypeprotect)");
}

s32 NativeGuestMemory::MapFile(void** out_addr, VAddr addr, u64 size, Core::MemoryProt prot,
                               Core::MemoryMapFlags flags, s32 fd, s64 offset) {
    (void)out_addr;
    SSPC_LOG(Memory, Error, "native memory: mmap of fd {} [{:#x}, +{:#x}) at {:#x} prot {:#x} flags {:#x}", fd, offset,
             size, addr, static_cast<u32>(prot), static_cast<u32>(flags));
    return NotSupported("mapping a file (mmap with a file descriptor)");
}

s32 NativeGuestMemory::PoolCommit(VAddr addr, u64 size, Core::MemoryProt prot, s32 memory_type) {
    (void)addr;
    (void)size;
    (void)prot;
    (void)memory_type;
    return NotSupported("memory pools (sceKernelMemoryPoolCommit)");
}

s32 NativeGuestMemory::PoolDecommit(VAddr addr, u64 size) {
    (void)addr;
    (void)size;
    return NotSupported("memory pools (sceKernelMemoryPoolDecommit)");
}

void NativeGuestMemory::SetPrtArea(u32 id, VAddr addr, u64 size) {
    vm_.SetPrtArea(id, addr, size);
}

void NativeGuestMemory::SetupRegions(u64 flexible_size, bool use_extended_mem1, bool use_extended_mem2) {
    Memory::Budget budget = console_;
    budget.flexible_size = flexible_size;
    budget.extended1 = use_extended_mem1;
    budget.extended2 = use_extended_mem2;
    if (!vm_.SetupRegions(budget)) {
        SSPC_LOG(Memory, Critical,
                 "native memory: the budgets for flexible size {:#x} (extended {} {}) do not fit the manager's "
                 "physical memory; the previous budgets stay",
                 flexible_size, use_extended_mem1, use_extended_mem2);
    }
}

u64 NativeGuestMemory::DirectSize() {
    return vm_.DirectSize();
}

PAddr NativeGuestMemory::AllocateDirect(PAddr search_start, PAddr search_end, u64 size, u64 alignment,
                                        s32 memory_type) {
    u64 out = 0;
    if (vm_.AllocateDirect(search_start, search_end, size, alignment, memory_type, &out) != Memory::kOk) {
        return static_cast<PAddr>(-1);
    }
    return out;
}

s32 NativeGuestMemory::ReleaseDirect(PAddr start, u64 size, bool checked) {
    return vm_.ReleaseDirect(start, size, checked);
}

u64 NativeGuestMemory::FlexibleSize() {
    return vm_.FlexibleBudget();
}

u64 NativeGuestMemory::FlexibleAvailable() {
    const u64 budget = vm_.FlexibleBudget();
    return budget - std::min(budget, vm_.FlexibleUsed());
}

s32 NativeGuestMemory::DirectQuery(PAddr addr, bool find_next, Libraries::Kernel::OrbisQueryInfo* info) {
    // The bridge's MemoryManager::DirectMemoryQuery over the manager's map: the allocated area at addr (or the next
    // one), extended over the allocated areas of the same memory type right after it. The bridge's map splits an
    // area where it becomes mapped, the manager's where the number of views changes; neither split shows here.
    const auto areas = vm_.Direct().Snapshot();
    if (addr >= vm_.DirectSize()) {
        return Memory::kErrEacces;
    }
    auto it = std::find_if(areas.begin(), areas.end(),
                           [addr](const Memory::Area& a) { return addr >= a.base && addr - a.base < a.size; });
    while (it != areas.end() && it->state == Memory::AreaState::Free && find_next) {
        ++it;
    }
    if (it == areas.end() || it->state != Memory::AreaState::Allocated) {
        return Memory::kErrEacces;
    }
    info->start = it->base;
    info->memoryType = it->memory_type;
    for (; it != areas.end() && it->memory_type == info->memoryType && it->state == Memory::AreaState::Allocated;
         ++it) {
        info->end = it->base + it->size;
    }
    return Memory::kOk;
}

s32 NativeGuestMemory::DirectAvailable(PAddr search_start, PAddr search_end, u64 alignment, PAddr* out, u64* size) {
    // The bridge's MemoryManager::DirectQueryAvailable, step by step (including how it trims an area that starts
    // before search_start), over the manager's free areas.
    const auto areas = vm_.Direct().Snapshot();
    auto it = std::find_if(areas.rbegin(), areas.rend(), [search_start](const Memory::Area& a) {
                  return a.base <= search_start;
              }).base();
    if (it != areas.begin()) {
        --it;
    }
    PAddr paddr = 0;
    u64 max_size = 0;
    for (; it != areas.end(); ++it) {
        if (it->state != Memory::AreaState::Free) {
            continue;
        }
        u64 aligned_base = alignment > 0 ? AlignUpTo(it->base, alignment) : it->base;
        const u64 alignment_size = aligned_base - it->base;
        u64 remaining = it->size >= alignment_size ? it->size - alignment_size : 0;
        if (it->base < search_start) {
            remaining = remaining > search_start - it->base ? remaining - (search_start - it->base) : 0;
            aligned_base = alignment > 0 ? AlignUpTo(search_start, alignment) : search_start;
        }
        const u64 end = it->base + it->size;
        if (end > search_end) {
            remaining = remaining > end - search_end ? remaining - (end - search_end) : 0;
        }
        if (remaining > max_size) {
            paddr = aligned_base;
            max_size = remaining;
        }
    }
    *out = paddr;
    *size = max_size;
    return Memory::kOk;
}

s32 NativeGuestMemory::DirectMemoryType(PAddr addr, s32* type, void** start, void** end) {
    const auto areas = vm_.Direct().Snapshot();
    const auto it = std::find_if(areas.begin(), areas.end(),
                                 [addr](const Memory::Area& a) { return addr >= a.base && addr - a.base < a.size; });
    if (addr >= vm_.DirectSize() || it == areas.end() || it->state != Memory::AreaState::Allocated) {
        return Memory::kErrEnoent;
    }
    *start = reinterpret_cast<void*>(it->base);
    *end = reinterpret_cast<void*>(it->base + it->size);
    *type = it->memory_type;
    return Memory::kOk;
}

PAddr NativeGuestMemory::PoolExpand(PAddr search_start, PAddr search_end, u64 size, u64 alignment) {
    (void)search_start;
    (void)search_end;
    (void)size;
    (void)alignment;
    NotSupported("memory pools (sceKernelMemoryPoolExpand)");
    return static_cast<PAddr>(-1);
}

s32 NativeGuestMemory::PoolStats(Libraries::Kernel::OrbisKernelMemoryPoolBlockStats* stats) {
    // No pool memory exists without pools: everything is 0 (the bridge counts its committed pool areas).
    stats->allocated_flushed_blocks = 0;
    stats->available_flushed_blocks = 0;
    stats->allocated_cached_blocks = 0;
    stats->available_cached_blocks = 0;
    return Memory::kOk;
}

void NativeGuestMemory::NotifyHostWrite(VAddr addr, u64 size) {
    vm_.NotifyHostWrite(addr, size);
}

// ---------------------------------------------------------------------------- NativeGpuMemory

NativeGpuMemory::~NativeGpuMemory() {
    if (listener_.load() != nullptr) {
        vm_.SetListener(nullptr);
    }
}

Core::GpuMemoryListener* NativeGpuMemory::SetListener(Core::GpuMemoryListener* listener) {
    Core::GpuMemoryListener* const previous = listener_.exchange(listener, std::memory_order_acq_rel);
    vm_.SetListener(listener != nullptr ? &forward_ : nullptr);
    return previous;
}

void NativeGpuMemory::CopySparseMemory(VAddr addr, u8* dest, u64 size) {
    vm_.CopySparse(addr, dest, size);
}

bool NativeGpuMemory::TryWriteBacking(void* address, const void* data, u64 size) {
    return vm_.TryWriteBacking(reinterpret_cast<u64>(address), data, size);
}

void NativeGpuMemory::ProtectTracking(VAddr addr, u64 size, Core::MemoryPermission perms) {
    // As the bridge's AddressSpace::Protect: no result (this runs in the access violation handler).
    (void)vm_.ProtectTracking(addr, size, TrackingAccess(perms));
}

VAddr NativeGpuMemory::SystemReservedEnd() {
    return vm_.SystemReservedEnd();
}

u64 NativeGpuMemory::ClampLargeRange(VAddr addr, u64 size) {
    return vm_.ClampRange(addr, size);
}

void NativeGpuMemory::Forward::OnGpuUnmap(u64 addr, u64 size) {
    if (auto* listener = owner_->listener_.load(std::memory_order_acquire)) {
        listener->OnGpuUnmap(addr, size);
    }
}

void NativeGpuMemory::Forward::OnGpuRegister(u64 addr, u64 size) {
    if (auto* listener = owner_->listener_.load(std::memory_order_acquire)) {
        listener->OnGpuRegister(addr, size);
    }
}

void NativeGpuMemory::Forward::OnGpuMap(u64 addr, u64 size) {
    if (auto* listener = owner_->listener_.load(std::memory_order_acquire)) {
        listener->OnGpuMap(addr, size);
    }
}

void NativeGpuMemory::Forward::OnGuestWrite(u64 addr, u64 size) {
    if (auto* listener = owner_->listener_.load(std::memory_order_acquire)) {
        listener->OnGuestWrite(addr, size);
    }
}

// ---------------------------------------------------------------------------- the game's guest memory (P2.3.4)

namespace {

NativeGame g_game; // set once at start-up; never destroyed (guest and GPU threads use it until the process ends)

u32 HostWindowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtl = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return rtl != nullptr && rtl(&info) == 0 ? info.dwBuildNumber : 0;
}

/// The host's regions from the system managed minimum on, queried the way the bridge's AddressSpace walks them
/// (address_space.cpp: VirtualQuery at each region's end aligned up to the allocation granularity), so that
/// GameLayout finds the same free regions the bridge would take at this moment.
std::vector<Memory::HostRegionInfo> HostRegions(u64 granularity) {
    std::vector<Memory::HostRegionInfo> out;
    for (u64 next = 0x400000; next < 0x7FFFFFFF0000ull;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(next), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const u64 base = reinterpret_cast<u64>(mbi.BaseAddress);
        out.push_back({base, mbi.RegionSize, mbi.State == MEM_FREE});
        next = AlignUpTo(base + mbi.RegionSize, granularity);
    }
    return out;
}

} // namespace

bool InstallNativeMemory(const App::Config& cfg, s32 sdk_version, u32 psf_attributes) {
    SYSTEM_INFO system{};
    GetSystemInfo(&system);
    const u64 granularity = system.dwAllocationGranularity;
    const u32 build = HostWindowsBuild();
    Memory::Layout layout = Memory::GameLayout(HostRegions(granularity), static_cast<u32>(sdk_version), build,
                                               granularity, cfg.extra_dmem_mib, cfg.extra_fmem_mib);
    layout.commit = cfg.memory_commit_on_demand ? Memory::CommitMode::OnDemand : Memory::CommitMode::Upfront;
    std::string error;
    auto vm = Memory::VirtualMemory::Create(layout, &error);
    if (vm == nullptr) {
        SSPC_LOG(Memory, Critical, "--native memory: SecondSonPC's memory manager cannot take the guest address space: {}",
                 error);
        return false;
    }
    const bool title_neo = ((psf_attributes >> 23) & 1) != 0; // param.sfo ATTRIBUTE: supports PS4 Pro
    const Memory::Budget console{
        .neo = cfg.neo_mode && title_neo,
        .dev_kit = cfg.dev_kit,
        .extra_dmem_mib = cfg.extra_dmem_mib,
        .extra_fmem_mib = cfg.extra_fmem_mib,
    };
    Memory::VirtualMemory* manager = vm.release();
    auto* guest = new NativeGuestMemory(*manager, console);
    auto* gpu = new NativeGpuMemory(*manager);
    if (!Core::SelectGuestMemory({guest, guest, guest, gpu})) {
        SSPC_LOG(Memory, Critical, "--native memory: too late, the guest memory is already in use (the bridge's manager "
                                   "stays)");
        return false;
    }
    g_game = {manager, guest, gpu};
    if (std::ranges::find(cfg.native_groups, "kernel-memory") != cfg.native_groups.end()) {
        // P3.5.2: the game's memory imports are SecondSonPC's functions on this manager (the bridge's functions,
        // which reach it through `guest`, stay for the bridge's own callers).
        Kernel::MemoryApi::Install(*manager, static_cast<u32>(sdk_version));
    }
    u64 total = 0;
    for (const auto& r : layout.ranges) {
        total += r.size;
    }
    SSPC_LOG(Memory, Warning,
             "EXPERIMENTAL: the guest memory is SecondSonPC's manager (--native memory): {} ranges, {:#x} bytes in "
             "[{:#x}, {:#x}); section {} MiB (direct {} MiB + flexible {} MiB), commit {}; SDK {:#x}, Windows build {}",
             layout.ranges.size(), total, layout.ranges.front().base, layout.ranges.back().End(),
             manager->Host().BackingSize() >> 20, layout.direct_size >> 20, layout.flexible_size >> 20,
             layout.commit == Memory::CommitMode::OnDemand ? "on demand" : "upfront (as the bridge)",
             static_cast<u32>(sdk_version), build);
    for (const auto& r : layout.ranges) {
        SSPC_LOG(Memory, Info, "  guest range {:#x} - {:#x}", r.base, r.End());
    }
    return true;
}

NativeGame GetNativeGame() {
    return g_game;
}

} // namespace SecondSon::Bridge
