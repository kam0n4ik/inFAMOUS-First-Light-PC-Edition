// SecondSonPC — the guest's virtual memory areas (Phase 2, P2.3.3b; docs/PHASE2_P233_NATIVE_VMM.md).
// Split and merge follow the bridge (third_party/shadPS4/src/core/memory.cpp @ 0fe263a: Split, CarveVMA,
// MergeAdjacent); a range outside the areas is refused (end()) instead of asserting.
#include "runtime/memory/vma_map.h"

#include <iterator>
#include <utility>

#include <fmt/format.h>

#include "runtime/memory/direct_memory.h"

namespace SecondSon::Memory {

void VmaMap::Reset(std::span<const AddressRange> ranges) {
    areas_.clear();
    ranges_.assign(ranges.begin(), ranges.end());
    for (u32 i = 0; i < ranges_.size(); ++i) {
        Vma free_area;
        free_area.base = ranges_[i].base;
        free_area.size = ranges_[i].size;
        free_area.range = i;
        areas_.emplace(free_area.base, std::move(free_area));
    }
    counters_ = {};
}

VmaMap::iterator VmaMap::Find(u64 addr) {
    auto it = areas_.upper_bound(addr);
    if (it == areas_.begin()) {
        return areas_.end();
    }
    --it;
    return addr - it->first < it->second.size ? it : areas_.end();
}

VmaMap::const_iterator VmaMap::Find(u64 addr) const {
    auto it = areas_.upper_bound(addr);
    if (it == areas_.begin()) {
        return areas_.end();
    }
    --it;
    return addr - it->first < it->second.size ? it : areas_.end();
}

VmaMap::const_iterator VmaMap::FirstFrom(u64 addr) const {
    auto it = areas_.upper_bound(addr);
    if (it != areas_.begin() && addr - std::prev(it)->first < std::prev(it)->second.size) {
        return std::prev(it);
    }
    return it;
}

bool VmaMap::Contains(u64 addr, u64 size) const {
    for (const auto& r : ranges_) {
        if (addr >= r.base && addr - r.base < r.size) {
            return size <= r.size - (addr - r.base);
        }
    }
    return false;
}

VmaMap::iterator VmaMap::Split(iterator it, u64 offset) {
    Vma& old_vma = it->second;
    Vma tail = old_vma;
    tail.base = old_vma.base + offset;
    tail.size = old_vma.size - offset;
    old_vma.size = offset;
    if (old_vma.HasBacking()) {
        std::vector<PhysPiece> head_pieces;
        tail.phys.clear();
        for (const auto& p : old_vma.phys) {
            if (p.offset + p.size <= offset) {
                head_pieces.push_back(p);
            } else if (p.offset >= offset) {
                PhysPiece moved = p;
                moved.offset -= offset;
                tail.phys.push_back(moved);
            } else {
                const u64 in_head = offset - p.offset;
                head_pieces.push_back({p.offset, p.base, in_head, p.memory_type});
                tail.phys.push_back({0, p.base + in_head, p.size - in_head, p.memory_type});
            }
        }
        old_vma.phys = std::move(head_pieces);
    }
    ++counters_.splits;
    return areas_.emplace_hint(std::next(it), tail.base, std::move(tail));
}

VmaMap::iterator VmaMap::Carve(u64 addr, u64 size) {
    auto it = Find(addr);
    if (it == areas_.end() || size == 0 || size > it->second.End() - addr) {
        return areas_.end();
    }
    const u64 start = addr - it->first;
    const u64 end = start + size;
    if (end != it->second.size) {
        Split(it, end);
    }
    if (start != 0) {
        it = Split(it, start);
    }
    return it;
}

bool VmaMap::CanMerge(const Vma& a, const Vma& b) {
    if (a.no_merge || b.no_merge || a.End() != b.base || a.range != b.range) {
        return false;
    }
    if (a.type == VmaType::Direct && b.type == VmaType::Direct) {
        if (a.phys.empty() || b.phys.empty()) {
            return false;
        }
        const PhysPiece& last = a.phys.back();
        const PhysPiece& first = b.phys.front();
        if (last.base + last.size != first.base || last.memory_type != first.memory_type) {
            return false;
        }
    }
    return a.prot == b.prot && a.type == b.type && a.name == b.name;
}

VmaMap::iterator VmaMap::MergeAround(iterator it) {
    const auto absorb = [this](iterator into, iterator from) {
        const u64 shift = into->second.size;
        into->second.size += from->second.size;
        for (PhysPiece p : from->second.phys) {
            p.offset += shift;
            into->second.phys.push_back(p);
        }
        areas_.erase(from);
        ++counters_.merges;
    };
    const auto next = std::next(it);
    if (next != areas_.end() && CanMerge(it->second, next->second)) {
        absorb(it, next);
    }
    if (it != areas_.begin()) {
        const auto prev = std::prev(it);
        if (CanMerge(prev->second, it->second)) {
            absorb(prev, it);
            return prev;
        }
    }
    return it;
}

std::string VmaMap::CheckInvariants() const {
    auto it = areas_.begin();
    for (u32 i = 0; i < ranges_.size(); ++i) {
        u64 expected = ranges_[i].base;
        const Vma* prev = nullptr;
        for (; it != areas_.end() && it->first < ranges_[i].End(); ++it) {
            const Vma& v = it->second;
            if (v.base != it->first || v.base != expected) {
                return fmt::format("area at {:#x}, expected {:#x}", v.base, expected);
            }
            if (v.size == 0 || v.size % kPageSize != 0 || v.base % kPageSize != 0 || v.range != i) {
                return fmt::format("area at {:#x}: size {:#x} range {}", v.base, v.size, v.range);
            }
            if (v.type == VmaType::Free &&
                (v.prot != Prot::kNone || !v.name.empty() || !v.phys.empty() || v.no_merge)) {
                return fmt::format("free area at {:#x} carries prot {:#x} name \"{}\" {} pieces", v.base, v.prot,
                                   v.name, v.phys.size());
            }
            if (prev != nullptr && prev->type == VmaType::Free && v.type == VmaType::Free) {
                return fmt::format("free areas at {:#x} and {:#x} not merged", prev->base, v.base);
            }
            if (v.HasBacking()) {
                u64 offset = 0;
                for (const auto& p : v.phys) {
                    if (p.offset != offset || p.size == 0 || p.size % kPageSize != 0 || p.base % kPageSize != 0) {
                        return fmt::format("area at {:#x}: piece at offset {:#x} (expected {:#x}) size {:#x}", v.base,
                                           p.offset, offset, p.size);
                    }
                    offset += p.size;
                }
                if (offset != v.size) {
                    return fmt::format("area at {:#x}: pieces cover {:#x} of {:#x}", v.base, offset, v.size);
                }
            } else if (!v.phys.empty()) {
                return fmt::format("area at {:#x} without backing has {} pieces", v.base, v.phys.size());
            }
            expected = v.End();
            prev = &v;
        }
        if (expected != ranges_[i].End()) {
            return fmt::format("range {} covered up to {:#x} of {:#x}", i, expected, ranges_[i].End());
        }
    }
    return it == areas_.end() ? "" : fmt::format("area at {:#x} outside the ranges", it->first);
}

} // namespace SecondSon::Memory
