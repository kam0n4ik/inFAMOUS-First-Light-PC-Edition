// SecondSonPC — the guest's direct memory (Phase 2, P2.3.1, EXPERIMENTAL; docs/PHASE2_P23_MEMORY.md).
// Placement, error codes and budget follow the Phase 0 oracle's dmem_map
// (third_party/shadPS4/src/core/memory.cpp @ 0fe263a). Deliberate differences, each a defect there
// (docs/PHASE2_P23_MEMORY.md §4): every sum is overflow-checked (D1, D9); a release frees exactly the
// range clipped to the size (D4); mappings are counted per page, so unmapping one of two views keeps
// the pages mapped (D3); a release never frees a page that is still mapped.
#include "runtime/memory/direct_memory.h"

#include <algorithm>
#include <atomic>
#include <limits>

#include <fmt/format.h>

#include "base/log.h"

namespace SecondSon::Memory {

namespace {

constexpr u64 kMiB = 1ull << 20;
constexpr u64 kMax = std::numeric_limits<u64>::max();

/// AlignUp for any page multiple (the oracle accepts alignments that are not powers of two);
/// false when the result does not fit in 64 bits.
bool AlignUp(u64 value, u64 alignment, u64& out) {
    const u64 rem = value % alignment;
    if (rem == 0) {
        out = value;
        return true;
    }
    const u64 add = alignment - rem;
    if (value > kMax - add) {
        return false;
    }
    out = value + add;
    return true;
}

std::atomic<Unmapper*> g_unmapper{nullptr};

} // namespace

u64 DirectMemorySize(const Budget& b) {
    u64 total = b.neo ? 5888 * kMiB : 5248 * kMiB;
    if (b.dev_kit) {
        total = b.neo ? 7936 * kMiB : 6656 * kMiB;
    }
    const u64 extra_fmem = u64{b.extra_fmem_mib} * kMiB;
    total += u64{b.extra_dmem_mib} * kMiB + extra_fmem;
    if (!b.extended1 && b.neo) {
        total -= 256 * kMiB;
    }
    if (!b.extended2 && !b.neo) {
        total -= 128 * kMiB;
    }
    if (b.flexible_size > kMax - extra_fmem) {
        return 0;
    }
    const u64 flexible = b.flexible_size + extra_fmem;
    return flexible < total ? total - flexible : 0;
}

DirectMemory::DirectMemory(u64 size) {
    Configure(size);
}

bool DirectMemory::Configure(u64 size) {
    if (size % kPageSize != 0) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    if (!PristineLocked()) {
        return false;
    }
    size_ = size;
    areas_.clear();
    if (size != 0) {
        areas_.emplace(0, Info{.size = size});
    }
    return true;
}

bool DirectMemory::Resize(u64 size) {
    if (size == 0 || size % kPageSize != 0) {
        return false;
    }
    std::scoped_lock lock{mutex_};
    if (size_ == 0) {
        return false;
    }
    if (size < size_) {
        for (auto it = std::prev(areas_.upper_bound(size)); it != areas_.end(); ++it) {
            if (it->second.state != AreaState::Free) {
                return false;
            }
        }
        SplitAt(size);
        areas_.erase(areas_.lower_bound(size), areas_.end());
    } else if (size > size_) {
        areas_.emplace(size_, Info{.size = size - size_});
    }
    size_ = size;
    Coalesce(0, size_);
    return true;
}

u64 DirectMemory::Size() const {
    std::scoped_lock lock{mutex_};
    return size_;
}

DirectMemory::Map::iterator DirectMemory::SplitAt(u64 addr) {
    if (addr >= size_) {
        return areas_.end();
    }
    auto it = std::prev(areas_.upper_bound(addr));
    if (it->first == addr) {
        return it;
    }
    Info tail = it->second;
    tail.size = it->first + it->second.size - addr;
    it->second.size = addr - it->first;
    return areas_.emplace_hint(std::next(it), addr, tail);
}

void DirectMemory::Coalesce(u64 from, u64 to) {
    if (areas_.empty()) {
        return;
    }
    auto it = std::prev(areas_.upper_bound(std::min(from, size_ - 1)));
    if (it != areas_.begin()) {
        --it;
    }
    while (it != areas_.end() && it->first <= to) {
        const auto next = std::next(it);
        if (next == areas_.end()) {
            break;
        }
        const Info& a = it->second;
        const Info& b = next->second;
        if (a.state == b.state && a.memory_type == b.memory_type && a.mappings == b.mappings) {
            it->second.size += b.size;
            areas_.erase(next);
            continue;
        }
        it = next;
    }
}

template <typename F>
void DirectMemory::ForEachIn(u64 from, u64 to, F&& f) {
    if (areas_.empty() || from >= to) {
        return;
    }
    for (auto it = std::prev(areas_.upper_bound(from)); it != areas_.end() && it->first < to; ++it) {
        f(it);
    }
}

bool DirectMemory::PristineLocked() const {
    return std::ranges::all_of(areas_, [](const auto& a) { return a.second.state == AreaState::Free; });
}

bool DirectMemory::Pristine() const {
    std::scoped_lock lock{mutex_};
    return PristineLocked();
}

s32 DirectMemory::Allocate(u64 search_start, u64 search_end, u64 len, u64 alignment, s32 memory_type,
                           u64* out) {
    if (out == nullptr || len == 0 || len % kPageSize != 0 || alignment % kPageSize != 0) {
        return kErrEinval;
    }
    const u64 align = alignment == 0 ? kPageSize : alignment;
    std::scoped_lock lock{mutex_};
    const u64 limit = std::min(search_end, size_);
    if (search_start >= limit || len > limit) {
        return kErrEagain;
    }
    for (auto it = std::prev(areas_.upper_bound(search_start)); it != areas_.end() && it->first < limit;
         ++it) {
        if (it->second.state != AreaState::Free) {
            continue;
        }
        const u64 area_end = it->first + it->second.size;
        u64 base = 0;
        if (!AlignUp(std::max(search_start, it->first), align, base)) {
            break; // every later area starts higher
        }
        if (base >= area_end || area_end - base < len) {
            continue;
        }
        if (base > limit - len) {
            break; // the first fit ends beyond search_end; later ones end even higher
        }
        const auto first = SplitAt(base);
        SplitAt(base + len);
        first->second.state = AreaState::Allocated;
        first->second.memory_type = memory_type;
        first->second.mappings = 0;
        Coalesce(base, base + len);
        *out = base;
        return kOk;
    }
    return kErrEagain;
}

s32 DirectMemory::Release(u64 start, u64 len, bool checked, Unmapper* unmapper) {
    if (start % kPageSize != 0 || len % kPageSize != 0) {
        return kErrEinval;
    }
    if (len == 0) {
        return kOk;
    }
    if (len > kMax - start) {
        return kErrEinval; // D1: the oracle frees up to the end and leaves the mappings
    }
    std::scoped_lock release_lock{release_mutex_};
    u64 end = start + len;
    std::vector<PhysRange> parts;
    bool mapped = false;
    {
        std::scoped_lock lock{mutex_};
        if (checked) {
            if (end > size_) {
                return kErrEnoent;
            }
        } else {
            if (start >= size_) {
                return kOk;
            }
            end = std::min(end, size_);
        }
        for (auto it = std::prev(areas_.upper_bound(start)); it != areas_.end() && it->first < end; ++it) {
            const u64 lo = std::max(start, it->first);
            const u64 hi = std::min(end, it->first + it->second.size);
            if (it->second.state != AreaState::Allocated) {
                if (checked) {
                    return kErrEnoent;
                }
                continue;
            }
            if (!parts.empty() && parts.back().base + parts.back().size == lo) {
                parts.back().size += hi - lo;
            } else {
                parts.push_back({lo, hi - lo});
            }
            mapped = mapped || it->second.mappings > 0;
        }
        if (parts.empty()) {
            return kOk;
        }
        for (const auto& p : parts) {
            SplitAt(p.base);
            SplitAt(p.base + p.size);
            ForEachIn(p.base, p.base + p.size, [](auto it) { it->second.state = AreaState::Releasing; });
        }
        Coalesce(parts.front().base, parts.back().base + parts.back().size);
    }

    // Applies `change` to every area of the parts still in state Releasing (the lock is held).
    const auto finish = [&](auto&& change) {
        for (const auto& p : parts) {
            SplitAt(p.base);
            SplitAt(p.base + p.size);
            ForEachIn(p.base, p.base + p.size, [&](auto it) {
                if (it->second.state == AreaState::Releasing) {
                    change(it->second);
                }
            });
        }
        Coalesce(parts.front().base, parts.back().base + parts.back().size);
    };

    if (mapped) {
        const s32 result = unmapper != nullptr ? unmapper->UnmapPhysical(parts) : kErrEbusy;
        if (result != kOk) {
            SSPC_LOG(Memory, Error,
                     "direct memory release [{:#x}, {:#x}): {} ({:#x}), the range stays allocated", start, end,
                     unmapper != nullptr ? "unmapping failed" : "no unmapper for its mappings",
                     static_cast<u32>(result));
            std::scoped_lock lock{mutex_};
            finish([](Info& i) { i.state = AreaState::Allocated; });
            return result;
        }
    }

    std::scoped_lock lock{mutex_};
    bool still_mapped = false;
    finish([&](Info& i) {
        if (i.mappings > 0) {
            i.state = AreaState::Allocated; // a view survived the unmapper: never free mapped memory
            still_mapped = true;
        } else {
            i.state = AreaState::Free;
            i.memory_type = 0;
        }
    });
    if (still_mapped) {
        SSPC_LOG(Memory, Critical,
                 "direct memory release [{:#x}, {:#x}): pages still mapped after the unmapper ran; they "
                 "stay allocated",
                 start, end);
        return kErrEbusy;
    }
    return kOk;
}

s32 DirectMemory::BeginMap(u64 phys, u64 len, bool exclusive, std::vector<PhysSlice>* out) {
    if (out != nullptr) {
        out->clear();
    }
    if (len == 0 || phys % kPageSize != 0 || len % kPageSize != 0) {
        return kErrEinval;
    }
    if (len > kMax - phys) {
        return kErrEnomem; // D9
    }
    const u64 end = phys + len;
    std::scoped_lock lock{mutex_};
    if (end > size_) {
        return kErrEnomem;
    }
    for (auto it = std::prev(areas_.upper_bound(phys)); it != areas_.end() && it->first < end; ++it) {
        if (it->second.state != AreaState::Allocated) {
            return kErrEnomem;
        }
        if (exclusive && it->second.mappings > 0) {
            return kErrEbusy;
        }
        if (it->second.mappings == std::numeric_limits<u32>::max()) {
            return kErrEnomem;
        }
    }
    SplitAt(phys);
    SplitAt(end);
    ForEachIn(phys, end, [&](auto it) {
        ++it->second.mappings;
        if (out == nullptr) {
            return;
        }
        if (!out->empty() && out->back().memory_type == it->second.memory_type) {
            out->back().size += it->second.size;
        } else {
            out->push_back({it->first, it->second.size, it->second.memory_type});
        }
    });
    Coalesce(phys, end);
    return kOk;
}

void DirectMemory::EndMap(u64 phys, u64 len) {
    if (len == 0) {
        return;
    }
    std::scoped_lock lock{mutex_};
    if (phys % kPageSize != 0 || len % kPageSize != 0 || len > kMax - phys || phys + len > size_) {
        SSPC_LOG(Memory, Critical, "direct memory: unmap of [{:#x}, +{:#x}) outside the map", phys, len);
        return;
    }
    const u64 end = phys + len;
    SplitAt(phys);
    SplitAt(end);
    bool unmapped = false;
    ForEachIn(phys, end, [&](auto it) {
        if (it->second.state == AreaState::Free || it->second.mappings == 0) {
            unmapped = true;
        } else {
            --it->second.mappings;
        }
    });
    Coalesce(phys, end);
    if (unmapped) {
        SSPC_LOG(Memory, Critical,
                 "direct memory: unmap of [{:#x}, {:#x}) that was not mapped (accounting error)", phys, end);
    }
}

std::vector<Area> DirectMemory::Snapshot() const {
    std::scoped_lock lock{mutex_};
    std::vector<Area> out;
    out.reserve(areas_.size());
    for (const auto& [base, i] : areas_) {
        out.push_back({base, i.size, i.state, i.memory_type, i.mappings});
    }
    return out;
}

std::string DirectMemory::CheckInvariants() const {
    std::scoped_lock lock{mutex_};
    if (size_ == 0) {
        return areas_.empty() ? "" : "unconfigured map has areas";
    }
    u64 expected = 0;
    const Info* prev = nullptr;
    for (const auto& [base, i] : areas_) {
        if (base != expected) {
            return fmt::format("area at {:#x}, expected {:#x}", base, expected);
        }
        if (i.size == 0 || i.size % kPageSize != 0 || base % kPageSize != 0) {
            return fmt::format("area at {:#x}: size {:#x}", base, i.size);
        }
        if (i.state == AreaState::Free && (i.memory_type != 0 || i.mappings != 0)) {
            return fmt::format("free area at {:#x}: type {} mappings {}", base, i.memory_type, i.mappings);
        }
        if (prev != nullptr && prev->state == i.state && prev->memory_type == i.memory_type &&
            prev->mappings == i.mappings) {
            return fmt::format("area at {:#x} not merged with its equal neighbour", base);
        }
        prev = &i;
        expected = base + i.size;
    }
    return expected == size_ ? "" : fmt::format("areas end at {:#x}, size {:#x}", expected, size_);
}

DirectMemory& GameDirectMemory() {
    static DirectMemory memory;
    return memory;
}

void SetUnmapper(Unmapper* unmapper) {
    g_unmapper.store(unmapper, std::memory_order_release);
}

Unmapper* GetUnmapper() {
    return g_unmapper.load(std::memory_order_acquire);
}

} // namespace SecondSon::Memory
