// SecondSonPC R6-Core — snapshot reuse in the CUE commit 0x14ad0 (see cue_dedup.h).
#include "native_engine/r6/cue_dedup.h"

#include <atomic>
#include <cstring>

#include <windows.h>

namespace SecondSon::NativeEngine::R6::CueDedup {

namespace {

constexpr u64 kAllocRva = 0x123d0;       ///< CUE::AllocSnapshot (cue, dwords)
constexpr u64 kMemcpyRva = 0x9843a0;     ///< memcpy (dst, src, n)
constexpr u64 kSetPointerRva = 0x626400; ///< setPointerInUserData (dcb, stage, sgpr, pointer)
constexpr u64 kCueCur = 0x20;            ///< the allocator's next free byte
constexpr u64 kCueStart = 0x28;          ///< the allocator's chunk start (set with cur on every new chunk / reset)
constexpr u64 kCueShadow = 0x90;
constexpr u64 kCueShadowStride = 0x1800;
constexpr u32 kStages = 8;

using GuestFn = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64);

u64 g_base = 0;
void (*g_redirect)(u64, u64) = nullptr;
GuestFn g_alloc = nullptr;
GuestFn g_set_pointer = nullptr;

struct Counters {
    std::atomic<u64> commits{0}, snapshots{0}, reused{0}, bytes_saved{0}, miss_cold{0}, miss_chunk{0}, miss_bytes{0},
        moved_pointers{0}, other_copies{0};
};
Counters g_stats;

/// The last snapshot of (CUE, stage) this thread made: a CUE is recorded by one thread at a time, and an entry left
/// by another thread or an older chunk is judged by the same checks as any other (a stale one only misses).
struct Entry {
    u64 cue = 0;
    u64 snapshot = 0;
    u64 start = 0;
    u32 bytes = 0;
    u32 stage = ~0u;
};
constexpr u32 kEntries = 2048;
thread_local Entry t_entries[kEntries];

/// The commit in progress: its last allocation, and the snapshots reused in it (the new one -> the earlier one).
struct Pending {
    u64 cue = 0, snapshot = 0, bytes = 0;
};
/// The allocator at the commit's entry: an earlier snapshot is taken only from [start, cur) as it was then - the
/// commit's own allocations may be filled after it takes them (a descriptor its builders write), and a stale entry
/// pointing there could still hold equal old bytes (found by r6_core's offline test, case 48).
thread_local u64 t_entry_start = 0, t_entry_cur = 0;
struct Move {
    u64 from, to, bytes;
};
thread_local Pending t_pending;
thread_local Move t_moves[kStages];
thread_local u32 t_move_count = 0;

Entry& Slot(u64 cue, u32 stage) {
    const u64 h = ((cue >> 4) * 0x9E3779B97F4A7C15ull) >> 40;
    return t_entries[(h ^ (u64{stage} * 0x165667B1u)) & (kEntries - 1)];
}

SSPC_SYSV_ABI u64 Alloc(u64 cue, u64 dwords, u64, u64, u64, u64) {
    const u64 p = g_alloc(cue, dwords, 0, 0, 0, 0);
    t_pending = Pending{cue, p, u64{static_cast<u32>(dwords)} * 4};
    return p;
}

SSPC_SYSV_ABI u64 Copy(u64 dst, u64 src, u64 n, u64, u64, u64) {
    const Pending pending = t_pending;
    t_pending = {};
    const u64 shadow = pending.cue + kCueShadow;
    // only the copy of a stage's shadow into the snapshot just taken; every other memcpy as it is
    if (dst == 0 || dst != pending.snapshot || n != pending.bytes || n == 0 || src < shadow ||
        (src - shadow) % kCueShadowStride != 0 || (src - shadow) / kCueShadowStride >= kStages ||
        t_move_count >= kStages) {
        g_stats.other_copies.fetch_add(1, std::memory_order_relaxed);
        std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), n);
        return dst;
    }
    g_stats.snapshots.fetch_add(1, std::memory_order_relaxed);
    const u32 stage = static_cast<u32>((src - shadow) / kCueShadowStride);
    const u64 start = *reinterpret_cast<const u64*>(pending.cue + kCueStart);
    Entry& e = Slot(pending.cue, stage);
    if (e.cue != pending.cue || e.stage != stage || e.bytes != n) {
        g_stats.miss_cold.fetch_add(1, std::memory_order_relaxed);
    } else if (e.start != start || start != t_entry_start || e.snapshot < start || e.snapshot + n > t_entry_cur) {
        // not in [start, cur) of this chunk as it was when the commit began
        g_stats.miss_chunk.fetch_add(1, std::memory_order_relaxed);
    } else if (std::memcmp(reinterpret_cast<const void*>(e.snapshot), reinterpret_cast<const void*>(src), n) != 0) {
        g_stats.miss_bytes.fetch_add(1, std::memory_order_relaxed);
    } else {
        // the earlier snapshot holds these bytes in this chunk instance: the new one stays allocated (the arena's use
        // is the game's) but unwritten, and its user-data pointers go to the earlier one
        t_moves[t_move_count++] = Move{dst, e.snapshot, n};
        g_stats.reused.fetch_add(1, std::memory_order_relaxed);
        g_stats.bytes_saved.fetch_add(n, std::memory_order_relaxed);
        return dst;
    }
    std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), n);
    e = Entry{pending.cue, dst, start, static_cast<u32>(n), stage};
    return dst;
}

SSPC_SYSV_ABI u64 SetPointer(u64 dcb, u64 stage, u64 sgpr, u64 pointer, u64 a4, u64 a5) {
    for (u32 i = 0; i < t_move_count; ++i) {
        const Move& m = t_moves[i];
        if (pointer >= m.from && pointer < m.from + m.bytes) {
            pointer = m.to + (pointer - m.from);
            g_stats.moved_pointers.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    return g_set_pointer(dcb, stage, sgpr, pointer, a4, a5);
}

bool ReadEnabled() {
    char v[4] = {};
    return GetEnvironmentVariableA("SSPC_R6_CUE_DEDUP", v, sizeof(v)) == 1 && v[0] == '1';
}

} // namespace

bool Enabled() {
    static const bool on = ReadEnabled();
    return on;
}

void Prepare(u64 module_base, void (*redirect)(u64 from, u64 to)) {
    g_base = module_base;
    g_redirect = redirect;
    g_alloc = reinterpret_cast<GuestFn>(module_base + kAllocRva);
    g_set_pointer = reinterpret_cast<GuestFn>(module_base + kSetPointerRva);
}

void Commit(SSPC_SYSV_ABI void (*impl)(u64), u64 cue) {
    g_stats.commits.fetch_add(1, std::memory_order_relaxed);
    t_pending = {};
    t_move_count = 0;
    t_entry_cur = *reinterpret_cast<const u64*>(cue + kCueCur);
    t_entry_start = *reinterpret_cast<const u64*>(cue + kCueStart);
    g_redirect(g_base + kAllocRva, reinterpret_cast<u64>(&Alloc));
    g_redirect(g_base + kMemcpyRva, reinterpret_cast<u64>(&Copy));
    g_redirect(g_base + kSetPointerRva, reinterpret_cast<u64>(&SetPointer));
    impl(cue);
    g_redirect(0, 0);
    t_move_count = 0;
}

Stats Totals() {
    const auto r = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    return Stats{r(g_stats.commits),    r(g_stats.snapshots),  r(g_stats.reused),         r(g_stats.bytes_saved),
                 r(g_stats.miss_cold),  r(g_stats.miss_chunk), r(g_stats.miss_bytes),     r(g_stats.moved_pointers),
                 r(g_stats.other_copies)};
}

} // namespace SecondSon::NativeEngine::R6::CueDedup
