// SecondSonPC — the bridge's memory interfaces over SecondSonPC's own memory manager (P2.3.3c).
#include "bridge/shadps4/native_memory.h"

#include "base/log.h"

namespace SecondSon::Bridge {

namespace {

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

} // namespace SecondSon::Bridge
