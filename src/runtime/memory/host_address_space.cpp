// SecondSonPC — the host side of the guest address space (Phase 2, P2.3.3b; docs/PHASE2_P233_NATIVE_VMM.md).
// The Windows mechanics are the bridge's (third_party/shadPS4/src/core/address_space.cpp @ 0fe263a): placeholders
// (VirtualAlloc2 MEM_RESERVE_PLACEHOLDER) split with VirtualFree(MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER) and
// merged with MEM_COALESCE_PLACEHOLDERS, views of one pagefile-backed section with MapViewOfFile3
// (MEM_REPLACE_PLACEHOLDER), private memory with VirtualAlloc2 (MEM_REPLACE_PLACEHOLDER). Different from the
// bridge: the address ranges and the section size are parameters (the bridge takes every free region below
// 1 TiB and 7936 + 512 MiB), the section may be SEC_RESERVE (commit on demand), a partial unmap of private
// memory is refused instead of asserting (D17), the host's refusals are returned instead of asserting.
#include "runtime/memory/host_address_space.h"

#include <algorithm>
#include <utility>

#include <windows.h>

#include <fmt/format.h>

namespace SecondSon::Memory {

namespace {

DWORD ToWindows(HostAccess access) {
    switch (access) {
    case HostAccess::None:
        return PAGE_NOACCESS;
    case HostAccess::Read:
        return PAGE_READONLY;
    case HostAccess::ReadWrite:
        return PAGE_READWRITE;
    case HostAccess::Execute:
        return PAGE_EXECUTE;
    case HostAccess::ExecuteRead:
        return PAGE_EXECUTE_READ;
    case HostAccess::ExecuteReadWrite:
        return PAGE_EXECUTE_READWRITE;
    }
    return PAGE_NOACCESS;
}

void* Ptr(u64 addr) {
    return reinterpret_cast<void*>(addr);
}

u64 Addr(const void* ptr) {
    return reinterpret_cast<u64>(ptr);
}

DWORD MapProtection(bool exec) {
    return exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
}

/// A view of the section at addr: mapped read/write/execute, then lowered to read/write unless executable.
/// The protection a view is mapped with is the most VirtualProtect may give it later; mapped read/write, a
/// guest mprotect adding execute would be refused by the host (the bridge maps read/write and asserts then,
/// D18).
bool MapSectionView(void* process, void* section, u64 addr, u64 phys, u64 size, bool exec) {
    if (MapViewOfFile3(section, process, Ptr(addr), phys, size, MEM_REPLACE_PLACEHOLDER, PAGE_EXECUTE_READWRITE,
                       nullptr, 0) != Ptr(addr)) {
        return false;
    }
    DWORD old = 0;
    return exec || VirtualProtect(Ptr(addr), size, PAGE_READWRITE, &old);
}

} // namespace

std::unique_ptr<HostAddressSpace> HostAddressSpace::Create(const Config& config, std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<HostAddressSpace> {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return nullptr;
    };
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const u64 granularity = info.dwAllocationGranularity;
    if (config.ranges.empty()) {
        return fail("no address range");
    }
    if (config.backing_size == 0 || config.backing_size % granularity != 0) {
        return fail(fmt::format("section size {:#x} is not a multiple of {:#x}", config.backing_size, granularity));
    }
    std::vector<AddressRange> ranges = config.ranges;
    std::ranges::sort(ranges, {}, &AddressRange::base);
    for (size_t i = 0; i < ranges.size(); ++i) {
        const AddressRange& r = ranges[i];
        if (r.size == 0 || r.base % granularity != 0 || r.size % granularity != 0 || r.base > ~0ull - r.size) {
            return fail(fmt::format("range [{:#x}, +{:#x}) is empty, unaligned or wraps", r.base, r.size));
        }
        if (i > 0 && ranges[i - 1].End() > r.base) {
            return fail(fmt::format("ranges overlap at {:#x}", r.base));
        }
        for (u64 addr = r.base; addr < r.End();) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(Ptr(addr), &mbi, sizeof(mbi)) == 0) {
                return fail(fmt::format("VirtualQuery({:#x}) failed: {}", addr, GetLastError()));
            }
            if (mbi.State != MEM_FREE) {
                return fail(fmt::format("[{:#x}, {:#x}) is not free on the host: {:#x} bytes at {:#x} are in use",
                                        r.base, r.End(), mbi.RegionSize, Addr(mbi.BaseAddress)));
            }
            addr = Addr(mbi.BaseAddress) + mbi.RegionSize;
        }
    }

    std::unique_ptr<HostAddressSpace> space(new HostAddressSpace);
    space->process_ = GetCurrentProcess();
    space->commit_ = config.commit;
    for (u32 i = 0; i < ranges.size(); ++i) {
        const AddressRange& r = ranges[i];
        void* p = VirtualAlloc2(space->process_, Ptr(r.base), r.size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                                PAGE_NOACCESS, nullptr, 0);
        if (p != Ptr(r.base)) {
            const DWORD why = GetLastError();
            if (p != nullptr) {
                VirtualFree(p, 0, MEM_RELEASE);
            }
            return fail(fmt::format("cannot reserve [{:#x}, {:#x}): error {}", r.base, r.End(), why));
        }
        space->ranges_.push_back(r);
        space->regions_.emplace(r.base, Region{.base = r.base, .size = r.size, .range = i});
    }
    space->section_ = CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_ALL_ACCESS, PAGE_EXECUTE_READWRITE,
                                         config.commit == CommitMode::Upfront ? SEC_COMMIT : SEC_RESERVE,
                                         config.backing_size, nullptr, nullptr, 0);
    if (space->section_ == nullptr) {
        return fail(fmt::format("cannot create the {:#x} byte section ({}): error {}", config.backing_size,
                                config.commit == CommitMode::Upfront ? "SEC_COMMIT" : "SEC_RESERVE", GetLastError()));
    }
    space->backing_base_ = static_cast<u8*>(MapViewOfFile3(space->section_, space->process_, nullptr, 0,
                                                           config.backing_size, 0, PAGE_READWRITE, nullptr, 0));
    if (space->backing_base_ == nullptr) {
        return fail(fmt::format("cannot map the section: error {}", GetLastError()));
    }
    space->backing_size_ = config.backing_size;
    return space;
}

HostAddressSpace::~HostAddressSpace() {
    for (const auto& [base, r] : regions_) {
        if (r.kind == Kind::View) {
            UnmapViewOfFile2(process_, Ptr(base), MEM_PRESERVE_PLACEHOLDER);
        } else if (r.kind == Kind::Private) {
            VirtualFreeEx(process_, Ptr(base), r.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER);
        }
    }
    for (const auto& [base, r] : regions_) {
        VirtualFreeEx(process_, Ptr(base), 0, MEM_RELEASE);
    }
    if (backing_base_ != nullptr) {
        UnmapViewOfFile2(process_, backing_base_, 0);
    }
    if (section_ != nullptr) {
        CloseHandle(section_);
    }
}

HostAddressSpace::Map::iterator HostAddressSpace::Find(u64 addr) {
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) {
        return regions_.end();
    }
    --it;
    return addr - it->first < it->second.size ? it : regions_.end();
}

HostAddressSpace::Map::const_iterator HostAddressSpace::Find(u64 addr) const {
    auto it = regions_.upper_bound(addr);
    if (it == regions_.begin()) {
        return regions_.end();
    }
    --it;
    return addr - it->first < it->second.size ? it : regions_.end();
}

bool HostAddressSpace::Refused() {
    last_error_.store(GetLastError(), std::memory_order_relaxed);
    return false;
}

bool HostAddressSpace::SplitPlaceholder(Map::iterator it, u64 offset) {
    // The map's node first: an allocation failure must not leave the host split and the map not.
    Region tail = it->second;
    tail.base = it->second.base + offset;
    tail.size = it->second.size - offset;
    const auto inserted = regions_.emplace_hint(std::next(it), tail.base, tail);
    if (!VirtualFreeEx(process_, Ptr(it->second.base), offset, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        regions_.erase(inserted);
        return Refused();
    }
    it->second.size = offset;
    ++counters_.splits;
    return true;
}

HostAddressSpace::Map::iterator HostAddressSpace::Carve(u64 addr, u64 size) {
    auto it = Find(addr);
    if (it == regions_.end() || it->second.kind != Kind::Placeholder || size == 0 ||
        size > it->second.base + it->second.size - addr) {
        return regions_.end();
    }
    if (addr > it->first) {
        if (!SplitPlaceholder(it, addr - it->first)) {
            return regions_.end();
        }
        it = std::next(it);
    }
    if (it->second.size > size && !SplitPlaceholder(it, size)) {
        Coalesce(addr, size);
        return regions_.end();
    }
    return it;
}

void HostAddressSpace::Coalesce(u64 addr, u64 size) {
    // From the left neighbour of the region holding addr to the region holding the end: every run of
    // adjacent placeholders of one reservation becomes one placeholder.
    auto it = regions_.upper_bound(addr);
    for (int back = 0; back < 2 && it != regions_.begin(); ++back) {
        --it;
    }
    const u64 stop = size > ~0ull - addr ? ~0ull : addr + size;
    while (it != regions_.end() && it->first <= stop) {
        if (it->second.kind != Kind::Placeholder) {
            ++it;
            continue;
        }
        auto run_end = std::next(it);
        u64 total = it->second.size;
        while (run_end != regions_.end() && run_end->second.kind == Kind::Placeholder &&
               run_end->second.range == it->second.range && it->first + total == run_end->first) {
            total += run_end->second.size;
            ++run_end;
        }
        if (run_end != std::next(it)) {
            if (VirtualFreeEx(process_, Ptr(it->first), total, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
                it->second.size = total;
                regions_.erase(std::next(it), run_end);
                ++counters_.coalesces;
            } else {
                Refused(); // the placeholders stay apart (CheckInvariants reports it)
                ++counters_.coalesce_failures;
            }
        }
        it = run_end;
    }
}

bool HostAddressSpace::MapView(u64 addr, u64 size, u64 phys, bool exec) {
    std::scoped_lock lock{mutex_};
    if (phys > backing_size_ || size > backing_size_ - phys) {
        return false;
    }
    const auto it = Carve(addr, size);
    if (it == regions_.end()) {
        return false;
    }
    if (!MapSectionView(process_, section_, addr, phys, size, exec)) {
        Refused();
        UnmapViewOfFile2(process_, Ptr(addr), MEM_PRESERVE_PLACEHOLDER); // when only lowering failed
        Coalesce(addr, size);
        return false;
    }
    it->second.kind = Kind::View;
    it->second.phys = phys;
    it->second.exec = exec;
    ++counters_.views;
    return true;
}

bool HostAddressSpace::MapPrivate(u64 addr, u64 size, bool exec) {
    std::scoped_lock lock{mutex_};
    const auto it = Carve(addr, size);
    if (it == regions_.end()) {
        return false;
    }
    void* p = VirtualAlloc2(process_, Ptr(addr), size, MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                            MapProtection(exec), nullptr, 0);
    if (p != Ptr(addr)) {
        Refused();
        Coalesce(addr, size);
        return false;
    }
    it->second.kind = Kind::Private;
    it->second.phys = 0;
    it->second.exec = exec;
    ++counters_.privates;
    return true;
}

bool HostAddressSpace::SplitsPrivate(u64 addr, u64 size) const {
    std::scoped_lock lock{mutex_};
    const u64 end = size > ~0ull - addr ? ~0ull : addr + size;
    auto it = regions_.upper_bound(addr);
    if (it != regions_.begin()) {
        --it;
    }
    for (; it != regions_.end() && it->first < end; ++it) {
        const Region& r = it->second;
        if (r.kind == Kind::Private && r.base + r.size > addr && (r.base < addr || r.base + r.size > end)) {
            return true;
        }
    }
    return false;
}

bool HostAddressSpace::Unmap(u64 addr, u64 size) {
    if (size == 0) {
        return true;
    }
    if (SplitsPrivate(addr, size)) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    if (size > ~0ull - addr) {
        return false;
    }
    // Whatever happens below, free neighbours are merged again on the way out.
    struct CoalesceOnExit {
        HostAddressSpace* space;
        u64 addr;
        u64 size;
        ~CoalesceOnExit() {
            space->Coalesce(addr, size);
        }
    } coalesce_on_exit{this, addr, size};
    const u64 end = addr + size;
    for (u64 cursor = addr; cursor < end;) {
        auto it = Find(cursor);
        if (it == regions_.end()) {
            return false; // outside the taken ranges
        }
        const Region r = it->second;
        const u64 r_end = r.base + r.size;
        const u64 lo = std::max(addr, r.base);
        const u64 hi = std::min(end, r_end);
        if (r.kind == Kind::Private) {
            if (!VirtualFreeEx(process_, Ptr(r.base), r.size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                return Refused();
            }
            it->second = Region{.base = r.base, .size = r.size, .range = r.range};
            ++counters_.unmaps;
        } else if (r.kind == Kind::View) {
            if (!UnmapViewOfFile2(process_, Ptr(r.base), MEM_PRESERVE_PLACEHOLDER)) {
                return Refused();
            }
            it->second = Region{.base = r.base, .size = r.size, .range = r.range};
            ++counters_.unmaps;
            // Windows cannot shorten a view: what stays mapped is mapped again (read/write(+execute)).
            if (lo > r.base) {
                if (!SplitPlaceholder(it, lo - r.base) ||
                    !MapSectionView(process_, section_, r.base, r.phys, lo - r.base, r.exec)) {
                    return Refused();
                }
                it->second.kind = Kind::View;
                it->second.phys = r.phys;
                it->second.exec = r.exec;
                it = std::next(it);
                ++counters_.remaps;
            }
            if (hi < r_end) {
                if (!SplitPlaceholder(it, hi - lo)) {
                    return Refused();
                }
                const auto tail = std::next(it);
                if (!MapSectionView(process_, section_, hi, r.phys + (hi - r.base), r_end - hi, r.exec)) {
                    return Refused();
                }
                tail->second.kind = Kind::View;
                tail->second.phys = r.phys + (hi - r.base);
                tail->second.exec = r.exec;
                ++counters_.remaps;
            }
        }
        cursor = hi;
    }
    return true;
}

bool HostAddressSpace::Protect(u64 addr, u64 size, HostAccess access) {
    std::scoped_lock lock{mutex_};
    const DWORD flags = ToWindows(access);
    const u64 end = size > ~0ull - addr ? ~0ull : addr + size;
    bool ok = true;
    auto it = regions_.upper_bound(addr);
    if (it != regions_.begin()) {
        --it;
    }
    for (; it != regions_.end() && it->first < end; ++it) {
        const Region& r = it->second;
        if (r.kind == Kind::Placeholder || r.base + r.size <= addr) {
            continue;
        }
        const u64 lo = std::max(addr, r.base);
        const u64 hi = std::min(end, r.base + r.size);
        DWORD old = 0;
        if (!VirtualProtect(Ptr(lo), hi - lo, flags, &old)) {
            ok = Refused(); // no logging: this runs in the access violation handler
            ++counters_.protect_failures;
        }
    }
    ++counters_.protects;
    return ok;
}

bool HostAddressSpace::CommitBacking(u64 phys, u64 size) {
    if (commit_ == CommitMode::Upfront || size == 0) {
        return true;
    }
    if (phys > backing_size_ || size > backing_size_ - phys) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    if (VirtualAlloc(backing_base_ + phys, size, MEM_COMMIT, PAGE_READWRITE) == nullptr) {
        return Refused();
    }
    ++counters_.commits;
    return true;
}

std::vector<HostAddressSpace::Region> HostAddressSpace::Snapshot() const {
    std::scoped_lock lock{mutex_};
    std::vector<Region> out;
    out.reserve(regions_.size());
    for (const auto& [base, r] : regions_) {
        out.push_back(r);
    }
    return out;
}

HostAddressSpace::Counters HostAddressSpace::Stats() const {
    std::scoped_lock lock{mutex_};
    return counters_;
}

std::string HostAddressSpace::CheckInvariants() const {
    std::scoped_lock lock{mutex_};
    auto it = regions_.begin();
    for (u32 i = 0; i < ranges_.size(); ++i) {
        u64 expected = ranges_[i].base;
        const Region* prev = nullptr;
        for (; it != regions_.end() && it->first < ranges_[i].End(); ++it) {
            const Region& r = it->second;
            if (r.base != it->first || r.base != expected) {
                return fmt::format("region at {:#x}, expected {:#x}", r.base, expected);
            }
            if (r.size == 0 || r.size % 0x1000 != 0 || r.range != i) {
                return fmt::format("region at {:#x}: size {:#x} range {}", r.base, r.size, r.range);
            }
            if (prev != nullptr && prev->kind == Kind::Placeholder && r.kind == Kind::Placeholder) {
                return fmt::format("placeholders at {:#x} and {:#x} not coalesced", prev->base, r.base);
            }
            if (r.kind == Kind::View && (r.phys > backing_size_ || r.size > backing_size_ - r.phys)) {
                return fmt::format("view at {:#x} beyond the section", r.base);
            }
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(Ptr(r.base), &mbi, sizeof(mbi)) == 0) {
                return fmt::format("VirtualQuery({:#x}) failed", r.base);
            }
            const bool own_allocation = Addr(mbi.AllocationBase) == r.base;
            switch (r.kind) {
            case Kind::Placeholder:
                if (mbi.State != MEM_RESERVE || !own_allocation || mbi.RegionSize != r.size) {
                    return fmt::format("placeholder [{:#x}, +{:#x}): host state {:#x} base {:#x} size {:#x}", r.base,
                                       r.size, mbi.State, Addr(mbi.AllocationBase), mbi.RegionSize);
                }
                break;
            case Kind::View:
                if (mbi.Type != MEM_MAPPED || !own_allocation ||
                    (commit_ == CommitMode::Upfront && mbi.State != MEM_COMMIT)) {
                    return fmt::format("view [{:#x}, +{:#x}): host type {:#x} state {:#x} base {:#x}", r.base, r.size,
                                       mbi.Type, mbi.State, Addr(mbi.AllocationBase));
                }
                break;
            case Kind::Private:
                if (mbi.Type != MEM_PRIVATE || mbi.State != MEM_COMMIT || !own_allocation) {
                    return fmt::format("private [{:#x}, +{:#x}): host type {:#x} state {:#x} base {:#x}", r.base,
                                       r.size, mbi.Type, mbi.State, Addr(mbi.AllocationBase));
                }
                break;
            }
            expected = r.base + r.size;
            prev = &r;
        }
        if (expected != ranges_[i].End()) {
            return fmt::format("range {} covered up to {:#x} of {:#x}", i, expected, ranges_[i].End());
        }
    }
    return it == regions_.end() ? "" : fmt::format("region at {:#x} outside the ranges", it->first);
}

} // namespace SecondSon::Memory
