// SecondSonPC — the host side of the guest address space (Phase 2, P2.3.3b; docs/PHASE2_P233_NATIVE_VMM.md).
// Windows placeholders hold the guest's address ranges; one pagefile-backed section is the guest's physical
// memory (direct and flexible) and stays mapped read/write in full for the whole lifetime (H2: writes past
// the guest's page protection, lock-free reads); a guest mapping replaces placeholder space with a view of
// the section or with private memory. Guest address = host address (H1).
//
// Granularity: placeholders take any page-aligned range (16 KiB guest pages here); the taken ranges and the
// section size are multiples of the host's allocation granularity (64 KiB). Windows cannot split a view or a
// private allocation in place: a view only partly unmapped is unmapped whole and the rest mapped again (the
// bridge does the same); private memory only partly unmapped is refused.
//
// Locking: one leaf mutex, taken inside the memory manager's locks. Protect runs in the access violation
// handler (the GPU's page tracking): no allocation and no logging on that path.
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Memory {

struct AddressRange {
    u64 base = 0;
    u64 size = 0;
    u64 End() const {
        return base + size;
    }
    /// [addr, addr + len) inside, without overflow.
    bool Contains(u64 addr, u64 len) const {
        return addr >= base && addr - base <= size && len <= size - (addr - base);
    }
};

/// Host protection of guest pages.
enum class HostAccess : u8 { None, Read, ReadWrite, Execute, ExecuteRead, ExecuteReadWrite };

/// How the section (the guest's physical memory) is charged to the system commit.
enum class CommitMode : u8 {
    /// SEC_COMMIT: all of it when the section is created (the bridge's way).
    Upfront,
    /// SEC_RESERVE: pages are committed when physical memory is handed out (experiment E1). A committed page
    /// stays committed while the section lives (a section's pages cannot be decommitted).
    OnDemand,
};

class HostAddressSpace {
public:
    struct Config {
        std::vector<AddressRange> ranges; ///< taken as placeholders: free on the host, allocation-granularity aligned
        u64 backing_size = 0;             ///< the section (a multiple of the allocation granularity)
        CommitMode commit = CommitMode::Upfront;
    };

    enum class Kind : u8 { Placeholder, View, Private };
    struct Region {
        u64 base = 0;
        u64 size = 0;
        Kind kind = Kind::Placeholder;
        u64 phys = 0; ///< View: offset in the section
        bool exec = false;
        u32 range = 0; ///< which taken range (its own reservation: placeholders never merge across)
    };

    struct Counters {
        u64 views = 0;
        u64 privates = 0;
        u64 unmaps = 0;
        u64 remaps = 0;           ///< views mapped again around a partial unmap
        u64 protect_restores = 0; ///< protection runs given back to them (D19)
        u64 splits = 0;
        u64 coalesces = 0;
        u64 protects = 0;
        u64 protect_failures = 0;
        u64 coalesce_failures = 0;
        u64 commits = 0;
    };

    /// GetLastError() of the host's last refusal (0: none yet).
    u32 LastError() const {
        return last_error_.load(std::memory_order_relaxed);
    }

    /// nullptr and *error when a range is not free on the host or not aligned, or the host refuses. Nothing is
    /// left behind on failure.
    static std::unique_ptr<HostAddressSpace> Create(const Config& config, std::string* error);
    ~HostAddressSpace();
    HostAddressSpace(const HostAddressSpace&) = delete;
    HostAddressSpace& operator=(const HostAddressSpace&) = delete;

    /// The section's permanent read/write view (H2).
    u8* BackingBase() const {
        return backing_base_;
    }
    u64 BackingSize() const {
        return backing_size_;
    }
    CommitMode Commit() const {
        return commit_;
    }
    const std::vector<AddressRange>& Ranges() const {
        return ranges_;
    }

    /// [addr, addr + size) must be free (placeholder space of one range): a view of the section at `phys`,
    /// read/write (and execute). false: nothing mapped.
    bool MapView(u64 addr, u64 size, u64 phys, bool exec);
    /// The same with committed private memory (zero-filled).
    bool MapPrivate(u64 addr, u64 size, bool exec);
    /// Whether unmapping [addr, addr + size) would have to split private memory (refused by Unmap).
    bool SplitsPrivate(u64 addr, u64 size) const;
    /// Everything in [addr, addr + size) back to placeholder space; views partly inside are mapped again
    /// around it and their remaining parts keep the host protection they had (D19: the bridge gives them
    /// read/write(+execute) again, losing the guest's mprotect and the GPU's page tracking there).
    /// false when it would split private memory (nothing changed) or the host refuses.
    bool Unmap(u64 addr, u64 size);
    /// Host protection of the mapped parts of [addr, addr + size); placeholder space is skipped. No allocation.
    bool Protect(u64 addr, u64 size, HostAccess access);
    /// OnDemand: commits [phys, phys + size) of the section (committed pages are left as they are).
    /// Upfront: nothing to do. false when the system refuses the commit.
    bool CommitBacking(u64 phys, u64 size);

    std::vector<Region> Snapshot() const;
    /// "" when the regions cover every taken range exactly and agree with the host (VirtualQuery of each
    /// region), else what is wrong. Self-test.
    std::string CheckInvariants() const;
    Counters Stats() const;

private:
    using Map = std::map<u64, Region>;

    HostAddressSpace() = default;
    Map::iterator Find(u64 addr);
    Map::const_iterator Find(u64 addr) const;
    /// The placeholder exactly [addr, addr + size) (splitting as needed); end() if the range is not
    /// inside one placeholder.
    Map::iterator Carve(u64 addr, u64 size);
    bool SplitPlaceholder(Map::iterator it, u64 offset);
    /// Merges the placeholder runs touching [addr, addr + size), each within its reservation.
    void Coalesce(u64 addr, u64 size);

    /// Records GetLastError() of a refusal; returns false.
    bool Refused();

    mutable std::mutex mutex_;
    std::atomic<u32> last_error_{0};
    void* process_ = nullptr;
    void* section_ = nullptr;
    u8* backing_base_ = nullptr;
    u64 backing_size_ = 0;
    CommitMode commit_ = CommitMode::Upfront;
    std::vector<AddressRange> ranges_;
    Map regions_;
    Counters counters_;
};

} // namespace SecondSon::Memory
