// SecondSonPC — the guest's virtual memory manager (Phase 2, P2.3.3b/c; EXPERIMENTAL, docs/PHASE2_P233_NATIVE_VMM.md).
// The operations follow the bridge's MemoryManager (third_party/shadPS4/src/core/memory.cpp @ 0fe263a: MapMemory,
// CreateArea, UnmapMemory/UnmapBytesFromEntry, ProtectBytes, NameVirtualRange, VirtualQuery, QueryProtection,
// SearchFree, ClampRangeSize, CopySparseMemory, TryWriteBacking, SetPrtArea, UnmapDirectPhysical) step by step,
// including the order of the GPU notifications; every difference is a fixed defect listed in the header.
#include "runtime/memory/virtual_memory.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <shared_mutex>
#include <utility>

#include <fmt/format.h>

#include "base/log.h"

namespace SecondSon::Memory {

namespace {

constexpr u32 kFw200 = 0x2000000;
constexpr u64 kMiB = 1ull << 20;
constexpr u64 kFlexibleMemoryBase = 64 * kMiB; // ORBIS_KERNEL_FLEXIBLE_MEMORY_BASE
constexpr u64 kHostGranularity = 0x10000;

bool AlignUp(u64 value, u64 alignment, u64& out) {
    const u64 rem = value % alignment;
    if (rem == 0) {
        out = value;
        return true;
    }
    if (value > ~0ull - (alignment - rem)) {
        return false;
    }
    out = value + (alignment - rem);
    return true;
}

/// The host protection the bridge gives a guest protection (ProtectBytes): any CPU or GPU read/write bit
/// means read/write (CpuReadWrite and GpuReadWrite are tested with "any bit"), execute adds execute.
HostAccess GuestToHost(u32 prot) {
    const bool rw = (prot & (Prot::kCpuReadWrite | Prot::kGpuReadWrite)) != 0;
    const bool exec = (prot & Prot::kCpuExec) != 0;
    if (rw) {
        return exec ? HostAccess::ExecuteReadWrite : HostAccess::ReadWrite;
    }
    return exec ? HostAccess::Execute : HostAccess::None;
}

/// The host refused an operation on memory this manager owns (unmapping or protecting its own mapping): the
/// address space no longer matches the areas. The bridge asserts here; going on would hand out memory that is
/// still mapped, so the process stops with the reason in the log.
[[noreturn]] void HostRefused(std::string_view what, u64 addr, u64 size, u32 error) {
    SSPC_LOG(Memory, Critical,
             "native virtual memory: the host refused to {} [{:#x}, +{:#x}) (Windows error {}); the guest address "
             "space no longer matches its areas, stopping",
             what, addr, size, error);
    Log::Flush();
    std::quick_exit(6);
}

[[noreturn]] void Inconsistent(std::string_view what, u64 addr) {
    SSPC_LOG(Memory, Critical, "native virtual memory: internal inconsistency ({} at {:#x}), stopping", what, addr);
    Log::Flush();
    std::quick_exit(6);
}

} // namespace

// ---------------------------------------------------------------------------- FlexibleMemory

void FlexibleMemory::Configure(u64 base, u64 size) {
    free_.clear();
    base_ = base;
    size_ = size;
    used_ = 0;
    if (size != 0) {
        free_.emplace(base, size);
    }
}

bool FlexibleMemory::Allocate(u64 size, std::vector<PhysRange>* out) {
    out->clear();
    if (size > size_ - used_) {
        return false;
    }
    u64 remaining = size;
    for (auto it = free_.begin(); it != free_.end() && remaining != 0;) {
        const u64 take = std::min(remaining, it->second);
        out->push_back({it->first, take});
        remaining -= take;
        if (take == it->second) {
            it = free_.erase(it);
        } else {
            const u64 rest_base = it->first + take;
            const u64 rest_size = it->second - take;
            it = free_.erase(it);
            free_.emplace_hint(it, rest_base, rest_size);
        }
    }
    used_ += size;
    return true;
}

void FlexibleMemory::Free(u64 base, u64 size) {
    if (size == 0) {
        return;
    }
    auto it = free_.emplace(base, size).first;
    const auto next = std::next(it);
    if (next != free_.end() && it->first + it->second == next->first) {
        it->second += next->second;
        free_.erase(next);
    }
    if (it != free_.begin()) {
        const auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) {
            prev->second += it->second;
            free_.erase(it);
        }
    }
    used_ -= size;
}

std::string FlexibleMemory::CheckInvariants() const {
    u64 free_total = 0;
    u64 last_end = base_;
    bool first = true;
    for (const auto& [b, s] : free_) {
        if (b < last_end || (!first && b == last_end) || s == 0 || s % kPageSize != 0 || b + s > base_ + size_) {
            return fmt::format("flexible free range [{:#x}, +{:#x}) out of order, adjacent or outside", b, s);
        }
        free_total += s;
        last_end = b + s;
        first = false;
    }
    return free_total + used_ == size_
               ? ""
               : fmt::format("flexible: free {:#x} + used {:#x} != {:#x}", free_total, used_, size_);
}

// ---------------------------------------------------------------------------- ReaderFirstMutex

void ReaderFirstMutex::lock() {
    std::unique_lock lock{mutex_};
    cv_.wait(lock, [this] { return !writer_ && readers_ == 0; });
    writer_ = true;
}

void ReaderFirstMutex::unlock() {
    {
        std::scoped_lock lock{mutex_};
        writer_ = false;
    }
    cv_.notify_all();
}

void ReaderFirstMutex::lock_shared() {
    std::unique_lock lock{mutex_};
    cv_.wait(lock, [this] { return !writer_; });
    ++readers_;
}

bool ReaderFirstMutex::try_lock_shared() {
    std::scoped_lock lock{mutex_};
    if (writer_) {
        return false;
    }
    ++readers_;
    return true;
}

void ReaderFirstMutex::unlock_shared() {
    bool last = false;
    {
        std::scoped_lock lock{mutex_};
        last = --readers_ == 0;
    }
    if (last) {
        cv_.notify_all();
    }
}

// ---------------------------------------------------------------------------- BackingTable

void BackingTable::Init(const AddressRange& covered) {
    covered_ = covered;
    const u64 pages = covered.size / kPageSize;
    top_size_ = (pages + kLeafPages - 1) / kLeafPages;
    top_ = std::make_unique<std::atomic<Leaf*>[]>(top_size_);
    leaves_.clear();
}

void BackingTable::Set(u64 addr, u64 size, const u8* backing) {
    // Only the covered part; a missing leaf is skipped whole when clearing (unmapping 1 TiB of reservations
    // must not walk 64 Mi pages under the writer lock).
    const u64 lo = std::max(addr, covered_.base);
    const u64 end = size > ~0ull - addr ? ~0ull : addr + size;
    const u64 hi = std::min(end, covered_.End());
    for (u64 page_addr = lo; page_addr < hi;) {
        const u64 page = (page_addr - covered_.base) / kPageSize;
        auto& slot = top_[page / kLeafPages];
        Leaf* leaf = slot.load(std::memory_order_acquire);
        if (leaf == nullptr) {
            if (backing == nullptr) {
                page_addr = covered_.base + (page / kLeafPages + 1) * kLeafPages * kPageSize;
                continue;
            }
            leaves_.push_back(std::make_unique<Leaf>());
            leaf = leaves_.back().get();
            slot.store(leaf, std::memory_order_release);
        }
        (*leaf)[page % kLeafPages].store(backing != nullptr ? backing + (page_addr - addr) : nullptr,
                                         std::memory_order_release);
        page_addr += kPageSize;
    }
}

const u8* BackingTable::Find(u64 addr) const {
    if (!covered_.Contains(addr, 1)) {
        return nullptr;
    }
    const u64 page = (addr - covered_.base) / kPageSize;
    const Leaf* leaf = top_[page / kLeafPages].load(std::memory_order_acquire);
    return leaf != nullptr ? (*leaf)[page % kLeafPages].load(std::memory_order_acquire) : nullptr;
}

// ---------------------------------------------------------------------------- VirtualMemory

std::unique_ptr<VirtualMemory> VirtualMemory::Create(const Layout& layout, std::string* error) {
    const auto fail = [&](std::string message) -> std::unique_ptr<VirtualMemory> {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return nullptr;
    };
    if (layout.ranges.empty() || layout.direct_size == 0 || layout.direct_size % kPageSize != 0 ||
        layout.flexible_base < layout.direct_size || layout.flexible_base % kPageSize != 0 ||
        layout.flexible_size % kPageSize != 0 || layout.flexible_size > ~0ull - layout.flexible_base) {
        return fail(fmt::format("inconsistent layout: direct {:#x}, flexible {:#x} at {:#x}", layout.direct_size,
                                layout.flexible_size, layout.flexible_base));
    }
    u64 backing_size = 0;
    if (!AlignUp(layout.flexible_base + layout.flexible_size, kHostGranularity, backing_size)) {
        return fail("section size overflows");
    }
    std::unique_ptr<VirtualMemory> vm(new VirtualMemory);
    vm->layout_ = layout;
    std::ranges::sort(vm->layout_.ranges, {}, &AddressRange::base);
    vm->host_ = HostAddressSpace::Create({vm->layout_.ranges, backing_size, layout.commit}, error);
    if (vm->host_ == nullptr) {
        return nullptr;
    }
    vm->areas_.Reset(vm->host_->Ranges());
    vm->direct_.Configure(layout.direct_size);
    vm->flexible_.Configure(layout.flexible_base, layout.flexible_size);
    vm->flexible_budget_ = layout.flexible_size;
    vm->backing_.Init(layout.gpu);
    return vm;
}

VirtualMemory::~VirtualMemory() = default;

bool VirtualMemory::IsGpuVisible(u64 addr, u64 size) const {
    const AddressRange& gpu = layout_.gpu;
    return gpu.size != 0 && addr >= gpu.base && addr - gpu.base < gpu.size && size <= gpu.size - (addr - gpu.base);
}

void VirtualMemory::NotifyUnmap(u64 addr, u64 size) {
    ++gpu_unmap_;
    if (GpuListener* l = listener_.load(std::memory_order_acquire)) {
        l->OnGpuUnmap(addr, size);
    }
}

void VirtualMemory::NotifyRegister(u64 addr, u64 size) {
    ++gpu_register_;
    if (GpuListener* l = listener_.load(std::memory_order_acquire)) {
        l->OnGpuRegister(addr, size);
    }
}

void VirtualMemory::NotifyMap(u64 addr, u64 size) {
    ++gpu_map_;
    if (GpuListener* l = listener_.load(std::memory_order_acquire)) {
        l->OnGpuMap(addr, size);
    }
}

GpuListener* VirtualMemory::SetListener(GpuListener* listener) {
    return listener_.exchange(listener, std::memory_order_acq_rel);
}

u64 VirtualMemory::SearchFree(u64 addr, u64 size, u64 alignment) const {
    // Under the unmap lock (every change of the areas holds it).
    if (alignment == 0) {
        alignment = kPageSize;
    }
    // MemoryManager::SearchFree: from the address (no lower than the lowest), first in its own area, then
    // the first free area whose aligned start lies below the search limit and leaves room for the size.
    if (addr < layout_.lowest) {
        addr = layout_.lowest;
    }
    if (areas_.Find(addr) == areas_.end()) {
        return kNoAddr; // the bridge: "is outside the memory map" (D16: a gap is outside too)
    }
    u64 candidate = 0;
    if (!AlignUp(addr, alignment, candidate)) {
        return kNoAddr;
    }
    auto it = areas_.FirstFrom(candidate);
    if (it != areas_.end() && it->second.type == VmaType::Free && candidate >= it->first &&
        size <= it->second.End() - candidate) {
        return candidate;
    }
    if (it != areas_.end() && candidate >= it->first) {
        ++it;
    }
    for (; it != areas_.end(); ++it) {
        const Vma& v = it->second;
        if (v.type != VmaType::Free) {
            continue;
        }
        if (!AlignUp(v.base, alignment, candidate)) {
            break;
        }
        if (candidate > v.End()) {
            continue;
        }
        if (candidate >= layout_.search_limit) {
            break;
        }
        if (v.End() - candidate >= size) {
            return candidate;
        }
    }
    return kNoAddr;
}

s32 VirtualMemory::Map(const MapRequest& req, u64* out) {
    const auto fail = [this](s32 code) {
        ++failures_;
        return code;
    };
    const bool fixed = (req.flags & MapFlag::kFixed) != 0;
    const bool direct = req.type == VmaType::Direct;
    if (out == nullptr || req.size == 0 || req.size % kPageSize != 0 || req.alignment % kPageSize != 0 ||
        (fixed && req.addr % kPageSize != 0) || req.type == VmaType::Free || direct != (req.phys != kNoPhys)) {
        return fail(kErrEinval);
    }
    if (req.type == VmaType::Pooled || req.type == VmaType::PoolReserved) {
        SSPC_LOG(Memory, Error, "native virtual memory: memory pools are not supported");
        return fail(kErrEinval);
    }
    std::scoped_lock unmap_lock{unmap_mutex_};
    // The budget under the unmap lock (D6: the bridge checks before taking it). Every change of the
    // flexible memory holds this lock.
    if (req.type == VmaType::Flexible && req.size > flexible_budget_ - std::min(flexible_budget_, flexible_.Used())) {
        return fail(kErrEinval);
    }
    std::vector<PhysSlice> slices;
    if (direct) {
        const s32 r = direct_.BeginMap(req.phys, req.size, req.exclusive, &slices);
        if (r != kOk) {
            return fail(r);
        }
    }
    const auto undo_direct = [&] {
        if (direct) {
            direct_.EndMap(req.phys, req.size);
        }
    };
    u64 addr = req.addr;
    if (fixed) {
        if (!areas_.Contains(addr, req.size)) {
            undo_direct();
            return fail(kErrEnomem);
        }
        if ((req.flags & MapFlag::kNoOverwrite) != 0) {
            const auto it = areas_.Find(addr);
            if (it->second.type != VmaType::Free || req.size > it->second.End() - addr) {
                undo_direct();
                return fail(kErrEnomem);
            }
        } else if (host_->SplitsPrivate(addr, req.size)) {
            SSPC_LOG(Memory, Error,
                     "native virtual memory: mapping over part of anonymous memory [{:#x}, +{:#x}) is not "
                     "possible (the host cannot split it)",
                     addr, req.size);
            undo_direct();
            return fail(kErrEnomem);
        }
    } else {
        addr = SearchFree(req.addr == 0 ? layout_.search_base : req.addr, req.size,
                          req.alignment == 0 ? kPageSize : req.alignment);
        if (addr == kNoAddr) {
            undo_direct();
            return fail(kErrEnomem);
        }
    }

    // Early GPU unmap, before the writer lock (the bridge: "to avoid deadlocks").
    if (IsGpuVisible(addr, req.size)) {
        NotifyUnmap(addr, req.size);
    }
    std::unique_lock lock{mutex_};
    if (fixed) {
        UnmapRange(addr, req.size);
    }
    auto it = areas_.Carve(addr, req.size);
    if (it == areas_.end() || it->second.type != VmaType::Free) {
        lock.unlock();
        undo_direct();
        SSPC_LOG(Memory, Critical, "native virtual memory: [{:#x}, +{:#x}) is not free after the search", addr,
                 req.size);
        return fail(kErrEnomem);
    }
    const bool exec = (req.prot & Prot::kCpuExec) != 0;
    u32 prot = req.prot;
    if ((prot & Prot::kCpuWrite) != 0) {
        prot |= Prot::kCpuRead; // on the PS4 read comes with write
    }
    {
        Vma& v = it->second;
        v.type = req.type;
        v.prot = prot;
        v.name = std::string(req.name);
        v.no_merge = (req.flags & MapFlag::kNoCoalesce) != 0;
        v.phys.clear();
    }

    // Back to a free area and nothing allocated: a host refusal (commit limit, an internal error).
    std::vector<PhysRange> pieces;
    u64 host_mapped = 0;
    const auto revert = [&] {
        if (host_mapped != 0) {
            if (!host_->Unmap(addr, host_mapped)) {
                HostRefused("unmap a failed mapping", addr, host_mapped, host_->LastError());
            }
            backing_.Set(addr, host_mapped, nullptr);
            NotifyRegister(addr, host_mapped);
        }
        for (const auto& p : pieces) {
            flexible_.Free(p.base, p.size);
        }
        Vma& v = it->second;
        v.type = VmaType::Free;
        v.prot = Prot::kNone;
        v.name.clear();
        v.no_merge = false;
        v.phys.clear();
        areas_.MergeAround(it);
        undo_direct();
    };

    if (req.type == VmaType::Flexible) {
        if (!flexible_.Allocate(req.size, &pieces)) {
            revert();
            return fail(kErrEnomem);
        }
        for (const auto& p : pieces) {
            if (!host_->CommitBacking(p.base, p.size)) {
                SSPC_LOG(Memory, Error, "native virtual memory: the system refused to commit flexible memory");
                revert();
                return fail(kErrEnomem);
            }
        }
        u64 offset = 0;
        for (const auto& p : pieces) {
            if (!host_->MapView(addr + offset, p.size, p.base, exec)) {
                SSPC_LOG(Memory, Critical, "native virtual memory: cannot map flexible memory at {:#x}", addr + offset);
                revert();
                return fail(kErrEnomem);
            }
            host_mapped = offset + p.size;
            Vma& v = it->second;
            if (!v.phys.empty() && v.phys.back().base + v.phys.back().size == p.base) {
                v.phys.back().size += p.size;
            } else {
                v.phys.push_back({offset, p.base, p.size, 0});
            }
            backing_.Set(addr + offset, p.size, BackingBase() + p.base);
            NotifyRegister(addr + offset, p.size);
            offset += p.size;
        }
    } else if (direct) {
        u64 offset = 0;
        for (const auto& s : slices) {
            it->second.phys.push_back({offset, s.base, s.size, s.memory_type});
            offset += s.size;
        }
    }

    if (req.type != VmaType::Reserved && req.type != VmaType::Flexible) {
        const bool mapped = direct ? host_->MapView(addr, req.size, req.phys, exec)
                                   : host_->MapPrivate(addr, req.size, exec);
        if (!mapped) {
            SSPC_LOG(Memory, Error, "native virtual memory: the host refused to map [{:#x}, +{:#x})", addr, req.size);
            revert();
            return fail(kErrEnomem);
        }
        if (direct) {
            backing_.Set(addr, req.size, BackingBase() + req.phys);
        }
        NotifyRegister(addr, req.size);
    }
    if (!direct || layout_.sdk_version >= kFw200) {
        areas_.MergeAround(it); // direct mappings merge only on SDK 2.00 or later
    }
    *out = addr;
    ++maps_;
    if (req.type != VmaType::Reserved) {
        lock.unlock();
        if (IsGpuVisible(addr, req.size)) {
            NotifyMap(addr, req.size);
        }
    }
    return kOk;
}

void VirtualMemory::UnmapRange(u64 addr, u64 size) {
    struct Unmapped {
        u64 addr;
        u64 size;
    };
    std::vector<Unmapped> host_parts;
    const u64 end = addr + size;
    // Which areas have host pages. The host unmaps first, so nothing below is freed while it is still mapped
    // (a guest write cannot land in flexible memory after it was cleared).
    for (u64 cursor = addr; cursor < end;) {
        const auto it = areas_.Find(cursor);
        if (it == areas_.end()) {
            Inconsistent("unmapping outside the areas", cursor);
        }
        const u64 len = std::min(it->second.End() - cursor, end - cursor);
        if (it->second.IsMapped()) {
            host_parts.push_back({cursor, len});
        }
        cursor += len;
    }
    if (!host_parts.empty()) {
        if (!host_->Unmap(addr, size)) {
            HostRefused("unmap", addr, size, host_->LastError());
        }
        backing_.Set(addr, size, nullptr);
    }
    for (u64 cursor = addr; cursor < end;) {
        auto it = areas_.Find(cursor);
        const VmaType type = it->second.type;
        const u64 start_in = cursor - it->first;
        const u64 len = std::min(it->second.size - start_in, end - cursor);
        if (type != VmaType::Free) {
            for (const auto& p : it->second.phys) {
                const u64 lo = std::max(p.offset, start_in);
                const u64 hi = std::min(p.offset + p.size, start_in + len);
                if (lo >= hi) {
                    continue;
                }
                const u64 phys = p.base + (lo - p.offset);
                if (type == VmaType::Direct) {
                    direct_.EndMap(phys, hi - lo);
                } else if (type == VmaType::Flexible) {
                    std::memset(BackingBase() + phys, 0, hi - lo); // the bridge clears freed flexible memory
                    flexible_.Free(phys, hi - lo);
                }
            }
            auto f = areas_.Carve(cursor, len);
            if (f == areas_.end()) {
                Inconsistent("carving an area to free", cursor);
            }
            Vma& fv = f->second;
            fv.type = VmaType::Free;
            fv.prot = Prot::kNone;
            fv.name.clear();
            fv.no_merge = false;
            fv.phys.clear();
            areas_.MergeAround(f);
        }
        cursor += len;
    }
    for (const auto& part : host_parts) {
        NotifyRegister(part.addr, part.size);
    }
}

s32 VirtualMemory::Unmap(u64 addr, u64 size) {
    if (size == 0) {
        return kOk;
    }
    std::scoped_lock unmap_lock{unmap_mutex_};
    // As the bridge: the address down and the size up to whole pages, each on its own.
    const u64 aligned_addr = addr - addr % kPageSize;
    u64 aligned_size = 0;
    if (!AlignUp(size, kPageSize, aligned_size) || !areas_.Contains(aligned_addr, aligned_size)) {
        ++failures_;
        return kErrEinval;
    }
    if (host_->SplitsPrivate(aligned_addr, aligned_size)) {
        SSPC_LOG(Memory, Error,
                 "native virtual memory: unmapping part of anonymous memory [{:#x}, +{:#x}) is not possible (the "
                 "host cannot split it; the bridge asserts, D17)",
                 aligned_addr, aligned_size);
        ++failures_;
        return kErrEinval;
    }
    if (IsGpuVisible(aligned_addr, aligned_size)) {
        NotifyUnmap(aligned_addr, aligned_size);
    }
    std::unique_lock lock{mutex_};
    UnmapRange(aligned_addr, aligned_size);
    ++unmaps_;
    return kOk;
}

u64 VirtualMemory::ProtectArea(u64 addr, u64 size, u32 prot) {
    const auto it = areas_.Find(addr);
    if (it == areas_.end()) {
        Inconsistent("protecting outside the areas", addr);
    }
    const Vma& v = it->second;
    const u64 len = std::min(v.End() - addr, size);
    const u32 old_prot = v.prot;
    const VmaType type = v.type;
    if (type == VmaType::Free || type == VmaType::PoolReserved) {
        return len; // on the PS4 protecting free memory does nothing
    }
    u32 new_prot = prot;
    if ((new_prot & Prot::kCpuWrite) != 0) {
        new_prot |= Prot::kCpuRead;
    }
    const HostAccess access = GuestToHost(new_prot);
    if (type == VmaType::Direct || type == VmaType::Pooled || type == VmaType::File) {
        new_prot &= ~Prot::kCpuExec; // hidden from the guest; the host keeps execute
    }
    auto carved = areas_.Carve(addr, len);
    if (carved == areas_.end()) {
        Inconsistent("carving an area to protect", addr);
    }
    carved->second.prot = new_prot;
    areas_.MergeAround(carved);
    if (type == VmaType::Reserved) {
        return len; // recorded, not applied: nothing is mapped
    }
    if (prot != old_prot && !host_->Protect(addr, len, access)) {
        HostRefused("protect", addr, len, host_->LastError());
    }
    return len;
}

s32 VirtualMemory::Protect(u64 addr, u64 size, u32 prot) {
    if (size == 0) {
        return kOk;
    }
    std::scoped_lock unmap_lock{unmap_mutex_};
    std::unique_lock lock{mutex_};
    if (addr % kPageSize != 0 || size % kPageSize != 0 || !areas_.Contains(addr, size)) {
        ++failures_;
        return kErrEinval; // D5: the bridge asserts
    }
    for (u64 done = 0; done < size;) {
        done += ProtectArea(addr + done, size - done, prot);
    }
    ++protects_;
    return kOk;
}

s32 VirtualMemory::NameRange(u64 addr, u64 size, std::string_view name) {
    std::scoped_lock unmap_lock{unmap_mutex_};
    std::unique_lock lock{mutex_};
    const u64 aligned_addr = addr - addr % kPageSize;
    u64 aligned_size = 0;
    if (!AlignUp(size, kPageSize, aligned_size) || !areas_.Contains(aligned_addr, aligned_size)) {
        ++failures_;
        return kErrEinval;
    }
    const u64 end = aligned_addr + aligned_size;
    for (u64 cursor = aligned_addr; cursor < end;) {
        auto it = areas_.Find(cursor);
        const u64 len = std::min(it->second.End() - cursor, end - cursor);
        if (it->second.type != VmaType::Free) {
            if (len < it->second.size) {
                it = areas_.Carve(cursor, len);
                if (it == areas_.end()) {
                    Inconsistent("carving an area to name", cursor);
                }
            }
            it->second.name = std::string(name);
            areas_.MergeAround(it);
        }
        cursor += len;
    }
    return kOk;
}

bool VirtualMemory::IsValid(u64 addr, u64 size) const {
    std::shared_lock lock{mutex_};
    return areas_.Contains(addr, size);
}

s32 VirtualMemory::Query(u64 addr, bool find_next, QueryInfo* info) const {
    const u64 query_addr = addr < layout_.lowest ? layout_.lowest : addr;
    if (addr < query_addr && !find_next) {
        return kErrEacces;
    }
    std::shared_lock lock{mutex_};
    auto it = areas_.FirstFrom(query_addr);
    if (!find_next && (it == areas_.end() || query_addr < it->first)) {
        return kErrEacces; // in a gap between the host's regions (D16: the bridge answers for the area before)
    }
    while (it != areas_.end() && it->second.type == VmaType::Free && find_next) {
        ++it;
    }
    if (it == areas_.end() || it->second.type == VmaType::Free) {
        return kErrEacces;
    }
    const Vma& v = it->second;
    *info = QueryInfo{};
    info->start = v.base;
    info->end = v.End();
    info->protection = static_cast<s32>(v.prot);
    info->is_flexible = v.type == VmaType::Flexible;
    info->is_direct = v.type == VmaType::Direct;
    info->is_stack = v.type == VmaType::Stack;
    info->is_pooled = v.type == VmaType::PoolReserved || v.type == VmaType::Pooled;
    info->is_committed = v.IsMapped();
    if (v.type == VmaType::Direct && !v.phys.empty()) {
        info->offset = v.phys.front().base;
        info->memory_type = v.phys.front().memory_type;
    }
    if (v.type == VmaType::Reserved || v.type == VmaType::PoolReserved) {
        info->protection = 0;
    }
    info->name = v.name.substr(0, 32);
    return kOk;
}

s32 VirtualMemory::QueryProtection(u64 addr, u64* start, u64* end, u32* prot) const {
    std::shared_lock lock{mutex_};
    if (addr < layout_.lowest) {
        return kErrEacces;
    }
    const auto it = areas_.Find(addr);
    if (it == areas_.end() || it->second.type == VmaType::Free) {
        return kErrEacces;
    }
    if (start != nullptr) {
        *start = it->second.base;
    }
    if (end != nullptr) {
        *end = it->second.End();
    }
    if (prot != nullptr) {
        *prot = it->second.prot;
    }
    return kOk;
}

std::vector<AreaInfo> VirtualMemory::Areas(bool with_free) const {
    std::shared_lock lock{mutex_};
    std::vector<AreaInfo> out;
    for (const auto& [base, v] : areas_) {
        if (v.type == VmaType::Free && !with_free) {
            continue;
        }
        out.push_back({base, v.size, v.type, v.prot, v.phys.empty() ? kNoPhys : v.phys.front().base,
                       static_cast<u32>(v.phys.size()), v.name});
    }
    return out;
}

bool VirtualMemory::SetupRegions(const Budget& budget) {
    std::scoped_lock unmap_lock{unmap_mutex_};
    std::unique_lock lock{mutex_};
    const u64 direct = DirectMemorySize(budget);
    const u64 extra_flexible = u64{budget.extra_fmem_mib} * kMiB;
    if (budget.flexible_size > ~0ull - extra_flexible ||
        budget.flexible_size + extra_flexible < kFlexibleMemoryBase) {
        return false;
    }
    const u64 flexible = budget.flexible_size + extra_flexible - kFlexibleMemoryBase;
    if (direct == 0 || direct > layout_.direct_size || flexible < flexible_.Used() || flexible > layout_.flexible_size) {
        SSPC_LOG(Memory, Error, "native virtual memory: budgets direct {:#x} flexible {:#x} do not fit the layout",
                 direct, flexible);
        return false;
    }
    if (!direct_.Resize(direct)) {
        return false;
    }
    flexible_budget_ = flexible;
    return true;
}

u64 VirtualMemory::DirectSize() const {
    return direct_.Size();
}

u64 VirtualMemory::FlexibleBudget() const {
    std::shared_lock lock{mutex_};
    return flexible_budget_;
}

u64 VirtualMemory::FlexibleUsed() const {
    std::shared_lock lock{mutex_};
    return flexible_.Used();
}

s32 VirtualMemory::AllocateDirect(u64 search_start, u64 search_end, u64 size, u64 alignment, s32 memory_type,
                                  u64* out) {
    const s32 r = direct_.Allocate(search_start, search_end, size, alignment, memory_type, out);
    if (r == kOk && !host_->CommitBacking(*out, size)) {
        SSPC_LOG(Memory, Error, "native virtual memory: the system refused to commit direct memory [{:#x}, +{:#x})",
                 *out, size);
        if (direct_.Release(*out, size, false, nullptr) != kOk) {
            SSPC_LOG(Memory, Critical, "native virtual memory: cannot give back [{:#x}, +{:#x})", *out, size);
        }
        return kErrEnomem;
    }
    return r;
}

s32 VirtualMemory::ReleaseDirect(u64 start, u64 size, bool checked) {
    return direct_.Release(start, size, checked, this);
}

s32 VirtualMemory::UnmapPhysical(std::span<const PhysRange> ranges) {
    std::scoped_lock unmap_lock{unmap_mutex_};
    // Exactly the overlap of every direct view with the ranges (the bridge's UnmapDirectPhysical). Every
    // change of the areas holds the unmap lock, so reading them needs no more.
    std::vector<std::pair<u64, u64>> remove;
    for (const auto& [base, v] : areas_) {
        if (v.type != VmaType::Direct) {
            continue;
        }
        for (const auto& p : v.phys) {
            for (const auto& r : ranges) {
                const u64 lo = std::max(r.base, p.base);
                const u64 hi = std::min(r.base + r.size, p.base + p.size);
                if (lo < hi) {
                    remove.emplace_back(v.base + p.offset + (lo - p.base), hi - lo);
                }
            }
        }
    }
    for (const auto& [addr, size] : remove) {
        if (IsGpuVisible(addr, size)) {
            NotifyUnmap(addr, size);
        }
    }
    std::unique_lock lock{mutex_};
    for (const auto& [addr, size] : remove) {
        UnmapRange(addr, size);
    }
    return kOk;
}

void VirtualMemory::NotifyHostWrite(u64 addr, u64 size) {
    ++gpu_write_;
    if (GpuListener* l = listener_.load(std::memory_order_acquire)) {
        l->OnGuestWrite(addr, size);
    }
}

void VirtualMemory::CopySparse(u64 addr, u8* dest, u64 size) const {
    while (size != 0) {
        const u64 in_page = addr % kPageSize;
        const u64 len = std::min(kPageSize - in_page, size);
        const u8* page = backing_.Find(addr - in_page);
        if (page != nullptr) {
            std::memcpy(dest, page + in_page, len);
        } else {
            std::memset(dest, 0, len);
        }
        size -= len;
        addr += len;
        dest += len;
    }
}

bool VirtualMemory::TryWriteBacking(u64 addr, const void* data, u64 size) {
    std::shared_lock lock{mutex_};
    if (!areas_.Contains(addr, size)) {
        return false; // the bridge asserts
    }
    auto it = areas_.Find(addr);
    if (it == areas_.end() || !it->second.HasBacking()) {
        return false;
    }
    const u8* src = static_cast<const u8*>(data);
    u64 cursor = addr;
    u64 remaining = size;
    while (remaining != 0 && it != areas_.end() && it->second.HasBacking() && it->first <= cursor) {
        const Vma& v = it->second;
        u64 offset = cursor - v.base;
        for (const auto& p : v.phys) {
            if (remaining == 0) {
                break;
            }
            if (p.offset + p.size <= offset) {
                continue;
            }
            const u64 in_piece = offset - p.offset;
            const u64 len = std::min(p.size - in_piece, remaining);
            std::memcpy(BackingBase() + p.base + in_piece, src, len);
            src += len;
            remaining -= len;
            offset += len;
            cursor += len;
        }
        ++it;
    }
    return true;
}

bool VirtualMemory::ProtectTracking(u64 addr, u64 size, HostAccess access) {
    return host_->Protect(addr, size, access);
}

u64 VirtualMemory::ClampRange(u64 addr, u64 size) {
    if (size < kMinSizeToClamp) {
        return size; // the bridge: not clamped, no lookup (every buffer bind asks)
    }
    std::shared_lock lock{mutex_};
    auto it = areas_.Find(addr);
    if (it == areas_.end()) {
        return size; // the bridge asserts
    }
    u64 clamped = it->second.End() - addr;
    u64 next_base = it->second.End();
    for (++it; it != areas_.end() && it->first == next_base && it->second.IsMapped() && clamped < size; ++it) {
        clamped += it->second.size;
        next_base = it->second.End();
    }
    return std::min(clamped, size);
}

void VirtualMemory::SetPrtArea(u32 id, u64 addr, u64 size) {
    if (id >= prt_.size() || size > ~0ull - addr) {
        return;
    }
    PrtArea old{};
    {
        // The notifications go without any lock, as in the bridge; only the table is guarded.
        std::scoped_lock lock{prt_mutex_};
        old = prt_[id];
        prt_[id] = {addr, addr + size, true};
    }
    if (old.mapped) {
        NotifyUnmap(old.start, old.end - old.start);
    }
    // The whole area counts as mapped for the GPU; holes read as zeros (CopySparse).
    NotifyMap(addr, size);
}

u64 VirtualMemory::PhysicalAddress(u64 addr) const {
    std::shared_lock lock{mutex_};
    const auto it = areas_.Find(addr);
    if (it == areas_.end()) {
        return kNoPhys;
    }
    const u64 offset = addr - it->first;
    for (const auto& p : it->second.phys) {
        if (offset >= p.offset && offset - p.offset < p.size) {
            return p.base + (offset - p.offset);
        }
    }
    return kNoPhys;
}

bool VirtualMemory::WriterLocked() const {
    if (!mutex_.try_lock_shared()) {
        return true;
    }
    mutex_.unlock_shared();
    return false;
}

VirtualMemory::Stats VirtualMemory::GetStats() const {
    Stats s;
    s.maps = maps_;
    s.unmaps = unmaps_;
    s.protects = protects_;
    s.failures = failures_;
    s.gpu_unmap = gpu_unmap_;
    s.gpu_register = gpu_register_;
    s.gpu_map = gpu_map_;
    s.gpu_write = gpu_write_;
    {
        std::shared_lock lock{mutex_};
        s.vma_count = areas_.Size();
        s.vma = areas_.Stats();
    }
    s.host = host_->Stats();
    return s;
}

std::string VirtualMemory::CheckInvariants() const {
    std::scoped_lock unmap_lock{unmap_mutex_};
    std::shared_lock lock{mutex_};
    for (std::string e : {areas_.CheckInvariants(), host_->CheckInvariants(), direct_.CheckInvariants(),
                          flexible_.CheckInvariants()}) {
        if (!e.empty()) {
            return e;
        }
    }
    const auto regions = host_->Snapshot();
    const u8* base = host_->BackingBase();
    u64 flexible_in_areas = 0;
    std::map<u64, u32> views_of_page; // direct physical page -> views
    for (const auto& [b, v] : areas_) {
        if (v.type == VmaType::Flexible) {
            flexible_in_areas += v.size;
        }
        // The physical address of a page of the area, kNoPhys without one.
        const auto phys_at = [&v](u64 offset) {
            for (const auto& p : v.phys) {
                if (offset >= p.offset && offset - p.offset < p.size) {
                    return p.base + (offset - p.offset);
                }
            }
            return kNoPhys;
        };
        for (u64 offset = 0; offset < v.size; offset += kPageSize) {
            const u64 page = v.base + offset;
            const auto r = std::find_if(regions.begin(), regions.end(),
                                        [page](const auto& r) { return page >= r.base && page - r.base < r.size; });
            if (r == regions.end()) {
                return fmt::format("page {:#x} has no host region", page);
            }
            const u64 phys = phys_at(offset);
            const bool placeholder = !v.IsMapped();
            const bool view = v.HasBacking();
            if (placeholder != (r->kind == HostAddressSpace::Kind::Placeholder) ||
                (!placeholder && view != (r->kind == HostAddressSpace::Kind::View)) ||
                (view && r->phys + (page - r->base) != phys)) {
                return fmt::format("page {:#x} of a type {} area: host region kind {} phys {:#x}, area phys {:#x}", page,
                                   static_cast<int>(v.type), static_cast<int>(r->kind), r->phys + (page - r->base),
                                   phys);
            }
            if (IsGpuVisible(page, kPageSize)) {
                const u8* expected = view ? base + phys : nullptr;
                if (backing_.Find(page) != expected) {
                    return fmt::format("backing table at {:#x}: {} instead of {}", page,
                                       static_cast<const void*>(backing_.Find(page)),
                                       static_cast<const void*>(expected));
                }
            }
            if (v.type == VmaType::Direct) {
                ++views_of_page[phys];
            }
        }
    }
    if (flexible_in_areas != flexible_.Used()) {
        return fmt::format("flexible areas {:#x}, flexible memory in use {:#x}", flexible_in_areas, flexible_.Used());
    }
    for (const auto& a : direct_.Snapshot()) {
        if (a.state == AreaState::Free) {
            continue;
        }
        for (u64 p = a.base; p < a.base + a.size; p += kPageSize) {
            const auto found = views_of_page.find(p);
            const u32 views = found == views_of_page.end() ? 0 : found->second;
            if (views != a.mappings) {
                return fmt::format("direct page {:#x}: {} views, the map counts {}", p, views, a.mappings);
            }
            if (found != views_of_page.end()) {
                views_of_page.erase(found);
            }
        }
    }
    if (!views_of_page.empty()) {
        return fmt::format("direct page {:#x} is mapped but not allocated", views_of_page.begin()->first);
    }
    return "";
}

// ---------------------------------------------------------------------------- GameLayout

Layout GameLayout(std::span<const HostRegionInfo> host_map, u32 sdk_version, u32 windows_build, u64 granularity,
                  u32 extra_dmem_mib, u32 extra_fmem_mib) {
    constexpr u64 kSystemManagedMin = 0x400000;
    constexpr u64 kSystemReservedMin = 0x7FFFFC000;
    constexpr u64 kUserMin = 0x1000000000;
    constexpr u64 kAffectedBuild = 22621; // Windows 11 22H2: VirtualAlloc2 is slow above 1 TiB
    constexpr u32 kFw300 = 0x3000000;
    const u64 user_max = windows_build <= kAffectedBuild || sdk_version >= kFw300 ? 0x10000000000ull
                                                                                 : 0x5FFFFFFFFFFFull;
    Layout layout;
    // address_space.cpp AddressSpace::Impl: walk the host's regions from the system managed minimum.
    u64 next = kSystemManagedMin;
    while (next <= user_max) {
        const auto r = std::find_if(host_map.begin(), host_map.end(),
                                    [next](const HostRegionInfo& h) { return next >= h.base && next - h.base < h.size; });
        if (r == host_map.end()) {
            break;
        }
        // The region's end aligned up; a region reaching the top of the 64-bit space would wrap (VirtualQuery
        // never reports one, a hand-made map may): it ends the walk.
        const u64 region_end = r->size > ~0ull - r->base ? ~0ull : r->base + r->size;
        u64 aligned_end = 0;
        const bool last = !AlignUp(region_end, granularity, aligned_end);
        next = last ? region_end : aligned_end;
        u64 size = r->size;
        if (next > user_max) {
            size -= std::min(size, next - user_max);
        }
        size = size / granularity * granularity;
        if (r->free && r->size > 0x1000000 && size != 0) {
            const u64 addr = (r->base + granularity - 1) / granularity * granularity;
            layout.ranges.push_back({addr, size});
        }
        if (last) {
            break;
        }
    }
    layout.lowest = layout.ranges.empty() ? kSystemManagedMin : layout.ranges.front().base;
    layout.search_base = 0x200000000;      // DEFAULT_MAPPING_BASE
    layout.search_limit = user_max - 1;    // USER_MIN + (user_max - USER_MIN - 1)
    layout.system_reserved_base = kSystemReservedMin;
    layout.system_reserved_end = kUserMin; // SYSTEM_RESERVED_MAX + 1
    layout.gpu = {0, 1ull << 40};
    const u64 extra_direct = u64{extra_dmem_mib} * kMiB;
    const u64 extra_flexible = u64{extra_fmem_mib} * kMiB;
    layout.direct_size = 7936 * kMiB + extra_direct + extra_flexible; // ORBIS_KERNEL_TOTAL_MEM_DEV_PRO + extras
    layout.flexible_base = layout.direct_size;
    layout.flexible_size = 512 * kMiB + extra_flexible; // the bridge adds the MiB count as bytes (D14)
    layout.commit = CommitMode::Upfront;
    layout.sdk_version = sdk_version;
    return layout;
}

} // namespace SecondSon::Memory
