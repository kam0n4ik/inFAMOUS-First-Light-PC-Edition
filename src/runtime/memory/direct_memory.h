// SecondSonPC — the guest's direct memory (Phase 2, P2.3.1, EXPERIMENTAL; docs/PHASE2_P23_MEMORY.md):
// which bytes of the console's physical direct memory [0, size) are allocated, their memory type and
// how many virtual mappings use them. With `--native kernel-dmem` this is the only map of it: the
// bridge's memory manager keeps the virtual side (address space, mappings, protection, GPU
// notifications) and asks this one for the physical side (patch 0007).
//
// Lock order: release lock -> the bridge's locks -> the map lock. The map lock is never held while
// calling out: a release (one at a time) marks its ranges Releasing, lets the Unmapper remove their
// mappings (GPU first, through the bridge) and frees them afterwards. Releasing ranges can be
// neither mapped nor allocated.
#pragma once

#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Memory {

constexpr u64 kPageSize = 16 * 1024; // PS4 page

// libkernel error codes (ORBIS_KERNEL_ERROR_*) used by direct memory.
constexpr s32 kOk = 0;
constexpr s32 kErrEnoent = static_cast<s32>(0x80020002);
constexpr s32 kErrEnomem = static_cast<s32>(0x8002000C);
constexpr s32 kErrEbusy = static_cast<s32>(0x80020010);
constexpr s32 kErrEinval = static_cast<s32>(0x80020016);
constexpr s32 kErrEagain = static_cast<s32>(0x80020023);

struct PhysRange {
    u64 base = 0;
    u64 size = 0;
};

/// A contiguous physical range of one memory type (what a virtual mapping records).
struct PhysSlice {
    u64 base = 0;
    u64 size = 0;
    s32 memory_type = 0;
};

enum class AreaState : u8 { Free, Allocated, Releasing };

struct Area {
    u64 base = 0;
    u64 size = 0;
    AreaState state = AreaState::Free;
    s32 memory_type = 0;
    u32 mappings = 0; // virtual mappings using every page of the area
};

/// The virtual memory side of a release (the bridge's memory manager until P2.3.2): removes every
/// mapping of the ranges, the GPU notified before. Called without any DirectMemory lock held.
class Unmapper {
public:
    virtual ~Unmapper() = default;
    /// 0 or a libkernel error code.
    virtual s32 UnmapPhysical(std::span<const PhysRange> ranges) = 0;
};

/// What the direct memory budget depends on: [Console] of SecondSonPC.toml and the executable's
/// SceKernelMemParam (flexible size, extended memory flags).
struct Budget {
    bool neo = false; // the PS4 Pro mode the game sees (sceKernelIsNeoMode)
    bool dev_kit = false;
    u32 extra_dmem_mib = 0;
    u32 extra_fmem_mib = 0;
    u64 flexible_size = 0;
    bool extended1 = true;
    bool extended2 = true;
};

/// Direct memory budget in bytes (the oracle's formula, docs/PHASE2_P23_MEMORY.md §6); 0 when the
/// flexible memory does not fit.
u64 DirectMemorySize(const Budget& budget);

class DirectMemory {
public:
    DirectMemory() = default;
    explicit DirectMemory(u64 size);
    DirectMemory(const DirectMemory&) = delete;
    DirectMemory& operator=(const DirectMemory&) = delete;

    /// Sets the size (a page multiple; 0 = unconfigured). Refused while anything is allocated.
    bool Configure(u64 size);
    u64 Size() const;

    /// First fit: the lowest AlignUp(max(search_start, free area start), alignment) whose range fits
    /// in one free area and ends at or below search_end. `alignment` 0 = one page. The libkernel
    /// parameter checks are the caller's; here: EINVAL for an empty/unaligned length or alignment,
    /// EAGAIN when nothing fits (also when a sum would overflow).
    s32 Allocate(u64 search_start, u64 search_end, u64 len, u64 alignment, s32 memory_type, u64* out);

    /// Frees the allocated parts of [start, start+len) (clipped to the size) after `unmapper` removed
    /// their mappings. Unchecked: free parts are skipped, a start beyond the size does nothing.
    /// Checked: ENOENT when any page is not allocated. EINVAL: unaligned or start+len overflows.
    /// EBUSY: a part still had mappings after the unmapper ran (it stays allocated).
    s32 Release(u64 start, u64 len, bool checked, Unmapper* unmapper);

    /// A virtual mapping of [phys, phys+len) is being created. Every page must be allocated (ENOMEM,
    /// also beyond the size), with `exclusive` none may already be mapped (EBUSY); the first page in
    /// address order that fails decides the error. All or nothing. `out`: the slices, in order.
    s32 BeginMap(u64 phys, u64 len, bool exclusive, std::vector<PhysSlice>* out);
    /// A mapping of [phys, phys+len) is gone (unmapped, or a BeginMap rolled back).
    void EndMap(u64 phys, u64 len);

    std::vector<Area> Snapshot() const;
    /// Nothing allocated (or unconfigured).
    bool Pristine() const;
    /// "" when the map is consistent, else what is wrong (self-test).
    std::string CheckInvariants() const;

private:
    struct Info {
        u64 size = 0;
        AreaState state = AreaState::Free;
        s32 memory_type = 0;
        u32 mappings = 0;
    };
    using Map = std::map<u64, Info>;

    /// An area starts at `addr` (0 < addr < size_ splits the one containing it); end() at size_.
    Map::iterator SplitAt(u64 addr);
    /// Merges equal neighbours from the area before `from` up to the area containing `to`.
    void Coalesce(u64 from, u64 to);
    /// Areas overlapping [from, to), in order.
    template <typename F>
    void ForEachIn(u64 from, u64 to, F&& f);
    bool PristineLocked() const;

    mutable std::mutex mutex_;
    // Releases one at a time (they are rare): when Release returns, its range is free, even if
    // another thread released the same pages at the same moment (the bridge serialises them too).
    // Never taken under the bridge's locks; mutex_ nests inside it.
    std::mutex release_mutex_;
    u64 size_ = 0;
    Map areas_;
};

/// The game's direct memory (one per process). Unconfigured until the loader sets up the memory
/// regions (--native kernel-dmem); the self-test configures it for its scripts and resets it after.
DirectMemory& GameDirectMemory();
void SetUnmapper(Unmapper* unmapper);
Unmapper* GetUnmapper();

} // namespace SecondSon::Memory
