// SecondSonPC — the guest's virtual memory areas (Phase 2, P2.3.3b; docs/PHASE2_P233_NATIVE_VMM.md): the
// managed address ranges cut into areas (free ones included), each with its type, protection, name and the
// physical memory behind it. A plain data structure: no host calls, no locks (the manager's).
//
// Structure: a std::map keyed by the base. The areas partition each managed range, so "which area holds
// addr" is one upper_bound (O(log n)); carving a range out of an area is at most two splits (O(log n) each
// plus the area's physical pieces); merging with both neighbours O(log n); walking a range O(log n + k).
// An interval tree adds nothing for a partition (no overlaps to search), a sorted vector costs O(n) per
// split and merge, a page table O(pages) per operation and 64 Mi entries for 1 TiB (the GPU's lock-free
// backing lookups keep one separately, VirtualMemory::BackingTable).
//
// Merge rules are the bridge's (core/memory.h VirtualMemoryArea::CanMergeWith @ 0fe263a): same type,
// protection and name, no NoCoalesce, adjacent, and for two direct areas physically contiguous with the
// same memory type. Merging appends the physical pieces as they are (the bridge does not join them).
#pragma once

#include <map>
#include <span>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/memory/host_address_space.h"

namespace SecondSon::Memory {

/// The kind of an area (the numbers of libkernel and the bridge's Core::VMAType).
enum class VmaType : u8 {
    Free = 0,
    Reserved = 1,
    Direct = 2,
    Flexible = 3,
    Pooled = 4,
    PoolReserved = 5,
    Stack = 6,
    Code = 7,
    File = 8,
    System = 9,
};

/// libkernel protection bits (Core::MemoryProt).
namespace Prot {
constexpr u32 kNone = 0;
constexpr u32 kCpuRead = 1;
constexpr u32 kCpuWrite = 2;
constexpr u32 kCpuReadWrite = 3;
constexpr u32 kCpuExec = 4;
constexpr u32 kGpuRead = 16;
constexpr u32 kGpuWrite = 32;
constexpr u32 kGpuReadWrite = 48;
} // namespace Prot

/// A physically contiguous piece of an area's backing.
struct PhysPiece {
    u64 offset = 0; ///< in the area
    u64 base = 0;   ///< in the section
    u64 size = 0;
    s32 memory_type = 0;
};

struct Vma {
    u64 base = 0;
    u64 size = 0;
    VmaType type = VmaType::Free;
    u32 prot = Prot::kNone;
    std::string name;
    bool no_merge = false;       ///< MemoryMapFlags::NoCoalesce
    std::vector<PhysPiece> phys; ///< Direct/Flexible/Pooled: [0, size) in order
    u32 range = 0;               ///< which managed range

    u64 End() const {
        return base + size;
    }
    bool HasBacking() const {
        return type == VmaType::Direct || type == VmaType::Flexible || type == VmaType::Pooled;
    }
    /// Memory behind it (on the host too): not free, not a reservation.
    bool IsMapped() const {
        return type != VmaType::Free && type != VmaType::Reserved && type != VmaType::PoolReserved;
    }
};

class VmaMap {
public:
    using Areas = std::map<u64, Vma>;
    using iterator = Areas::iterator;
    using const_iterator = Areas::const_iterator;

    struct Counters {
        u64 splits = 0;
        u64 merges = 0;
    };

    /// One free area per range (sorted, not overlapping).
    void Reset(std::span<const AddressRange> ranges);

    /// The area holding addr, or end() (outside the ranges).
    iterator Find(u64 addr);
    const_iterator Find(u64 addr) const;
    /// The area holding addr, else the first one after it (end() if none).
    const_iterator FirstFrom(u64 addr) const;
    /// [addr, addr + size) inside one managed range; size 0: addr inside one.
    bool Contains(u64 addr, u64 size) const;
    /// The area exactly [addr, addr + size), split out of the one area holding all of it; end() otherwise.
    iterator Carve(u64 addr, u64 size);
    /// Merges `it` with its neighbours where the rules allow (next first, then previous, as the bridge);
    /// returns the area that holds it now.
    iterator MergeAround(iterator it);

    iterator begin() {
        return areas_.begin();
    }
    iterator end() {
        return areas_.end();
    }
    const_iterator begin() const {
        return areas_.begin();
    }
    const_iterator end() const {
        return areas_.end();
    }
    size_t Size() const {
        return areas_.size();
    }
    const std::vector<AddressRange>& Ranges() const {
        return ranges_;
    }
    Counters Stats() const {
        return counters_;
    }

    static bool CanMerge(const Vma& a, const Vma& b);
    /// "" when the areas partition the ranges and each is well formed, else what is wrong. Two free areas
    /// side by side count as wrong: freeing always merges them (as in the bridge).
    std::string CheckInvariants() const;

private:
    iterator Split(iterator it, u64 offset);

    Areas areas_;
    std::vector<AddressRange> ranges_;
    Counters counters_;
};

} // namespace SecondSon::Memory
