// SecondSonPC R8-A2 — the snapshot table (cp_native_table.h).
#include <windows.h>

#include "runtime/gpu/r8/cp_native_table.h"

#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <string_view>

#include "base/log.h"

namespace SecondSon::Gpu::R8 {

namespace {

bool ReadOn() {
    for (const char* name : {"SSPC_R8_CP_DIAG", "SSPC_R8_CP_STAGE_REUSE"}) {
        char v[16] = {};
        const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
        if (n > 0 && n < sizeof(v) && std::string_view(v, n) != "0") {
            return true;
        }
    }
    return false;
}

/// One entry: a sequence lock and seven words (a cache line). Words: base, cue, version, dwords | stage << 32 |
/// refs << 40, the seven pointer SGPRs, the seven offsets (two words).
struct alignas(64) Slot {
    std::atomic<u64> seq{0};
    std::atomic<u64> w[7]{};
};

constexpr u32 kSlotBits = 17; // 128 Ki entries, 8 MiB: a city frame commits ~18.6 k snapshots
constexpr u32 kSlots = 1u << kSlotBits;

std::unique_ptr<Slot[]> g_slots;
std::atomic<u64> g_lo{~0ull}, g_hi{0}; ///< the published snapshots' address bounds (Find's cheap filter)
std::atomic<u64> g_published{0}, g_lookups{0}, g_found{0}, g_torn{0};
std::atomic<bool> g_tests{false};

inline u32 Index(u64 base) {
    return static_cast<u32>(((base >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - kSlotBits));
}

Slot* Slots() {
    static Slot* const s = [] {
        if (!TableOn()) {
            return static_cast<Slot*>(nullptr);
        }
        g_slots = std::make_unique<Slot[]>(kSlots);
        SSPC_LOG(GPU, Info, "R8 CP: the native commit's snapshot table is on ({} entries)", kSlots);
        return g_slots.get();
    }();
    return s;
}

void Widen(u64 base, u64 end) {
    u64 lo = g_lo.load(std::memory_order_relaxed);
    while (base < lo && !g_lo.compare_exchange_weak(lo, base, std::memory_order_relaxed)) {
    }
    u64 hi = g_hi.load(std::memory_order_relaxed);
    while (end > hi && !g_hi.compare_exchange_weak(hi, end, std::memory_order_relaxed)) {
    }
}

} // namespace

bool TableOn() {
    static const bool on = ReadOn();
    return on || g_tests.load(std::memory_order_relaxed);
}

void EnableForTests() {
    g_tests.store(true, std::memory_order_relaxed);
}

void Publish(const SnapshotInfo& s) {
    Slot* slots = Slots();
    if (slots == nullptr || s.base == 0) {
        return;
    }
    Slot& e = slots[Index(s.base)];
    // the entry's lock: even -> odd (two commits of colliding addresses on two threads write one after the other)
    u64 q = e.seq.load(std::memory_order_relaxed);
    for (;;) {
        if ((q & 1) != 0) {
            _mm_pause();
            q = e.seq.load(std::memory_order_relaxed);
            continue;
        }
        if (e.seq.compare_exchange_weak(q, q + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            break;
        }
    }
    u64 sg = 0, o0 = 0, o1 = 0;
    for (u32 i = 0; i < 7; ++i) {
        sg |= u64{s.ref_sgpr[i]} << (8 * i);
        (i < 4 ? o0 : o1) |= u64{s.ref_offset[i]} << (16 * (i & 3));
    }
    e.w[0].store(s.base, std::memory_order_relaxed);
    e.w[1].store(s.cue, std::memory_order_relaxed);
    e.w[2].store(s.version, std::memory_order_relaxed);
    e.w[3].store(u64{s.dwords} | (u64{s.stage} << 32) | (u64{s.refs} << 40), std::memory_order_relaxed);
    e.w[4].store(sg, std::memory_order_relaxed);
    e.w[5].store(o0, std::memory_order_relaxed);
    e.w[6].store(o1, std::memory_order_relaxed);
    e.seq.store(q + 2, std::memory_order_release);
    Widen(s.base, s.base + u64{s.dwords} * 4);
    g_published.fetch_add(1, std::memory_order_relaxed);
}

bool Lookup(u64 base, SnapshotInfo& out) {
    Slot* slots = Slots();
    if (slots == nullptr || base == 0) {
        return false;
    }
    const Slot& e = slots[Index(base)];
    const u64 q = e.seq.load(std::memory_order_acquire);
    if ((q & 1) != 0 || e.w[0].load(std::memory_order_relaxed) != base) {
        return false;
    }
    u64 w[7];
    for (u32 i = 0; i < 7; ++i) {
        w[i] = e.w[i].load(std::memory_order_relaxed);
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    if (e.seq.load(std::memory_order_relaxed) != q) {
        g_torn.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    out.base = w[0];
    out.cue = w[1];
    out.version = w[2];
    out.dwords = static_cast<u32>(w[3]);
    out.stage = static_cast<u8>(w[3] >> 32);
    out.refs = static_cast<u8>(std::min<u64>((w[3] >> 40) & 0xFF, 7));
    for (u32 i = 0; i < 7; ++i) {
        out.ref_sgpr[i] = static_cast<u8>(w[4] >> (8 * i));
        out.ref_offset[i] = static_cast<u16>((i < 4 ? w[5] : w[6]) >> (16 * (i & 3)));
    }
    return out.base == base;
}

bool Find(std::span<const u32, 16> user_data, u32 cue_stage, SnapshotInfo& out, u32& pointer_mask) {
    pointer_mask = 0;
    if (Slots() == nullptr) {
        return false;
    }
    const u64 lo = g_lo.load(std::memory_order_relaxed), hi = g_hi.load(std::memory_order_relaxed);
    for (u32 i = 0; i + 1 < 16; ++i) {
        const u64 a = user_data[i] | (u64{user_data[i + 1]} << 32);
        if (a < lo || a >= hi) {
            continue;
        }
        g_lookups.fetch_add(1, std::memory_order_relaxed);
        SnapshotInfo s;
        if (!Lookup(a, s) || s.stage != cue_stage || s.refs == 0) {
            continue;
        }
        u32 mask = 0;
        bool same = true;
        for (u32 j = 0; j < s.refs && same; ++j) {
            const u32 sg = s.ref_sgpr[j];
            const u64 want = s.base + u64{s.ref_offset[j]} * 4;
            same = sg + 1 < 16 && user_data[sg] == static_cast<u32>(want) &&
                   user_data[sg + 1] == static_cast<u32>(want >> 32);
            mask |= 3u << sg;
        }
        if (!same) {
            continue;
        }
        g_found.fetch_add(1, std::memory_order_relaxed);
        out = s;
        pointer_mask = mask;
        return true;
    }
    return false;
}

TableStats Stats() {
    return {g_published.load(std::memory_order_relaxed), g_lookups.load(std::memory_order_relaxed),
            g_found.load(std::memory_order_relaxed), g_torn.load(std::memory_order_relaxed)};
}

} // namespace SecondSon::Gpu::R8
