// SecondSonPC — reconstruction of eboot 0x1f0870 (see frame_scratch.h).
#include "native_engine/reconstructed/frame_scratch.h"

#include <cstring>
#include <random>

#include <fmt/format.h>

#include "native_engine/guest_abi_bridge.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

namespace SecondSon::NativeEngine::Reconstructed {

namespace FS = FrameScratch;

namespace {

/// One arena: CAS the aligned cursor forward by `size`; returns the aligned start, `next` the new cursor.
inline u64 Bump(u64* cursor, u64 add, u64 mask, u64 size, u64& next) {
    u64 old = __atomic_load_n(cursor, __ATOMIC_RELAXED);
    u64 p;
    do {
        p = (add + old) & mask;
        next = p + size;
    } while (!__atomic_compare_exchange_n(cursor, &old, next, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST));
    return p;
}

} // namespace

SSPC_SYSV_ABI u64 FS::Native(u64 scratch, u32 size, u32 alignment) {
    if (size == 0) {
        return 0;
    }
    const u64 add = static_cast<u64>(alignment) - 1;
    const u64 mask = 0 - static_cast<u64>(alignment);
    const u64 end = __atomic_load_n(GuestPtr<u64>(scratch + 0x40), __ATOMIC_RELAXED);
    u64 next = 0;
    const u64 p = Bump(GuestPtr<u64>(scratch + 0x38), add, mask, size, next);
    if (p != 0 && next <= end) {
        return p;
    }
    const u64 end2 = __atomic_load_n(GuestPtr<u64>(scratch + 0x70), __ATOMIC_RELAXED);
    const u64 q = Bump(GuestPtr<u64>(scratch + 0x68), add, mask, size, next);
    return next <= end2 ? q : 0;
}

namespace {

constexpr u32 N = FS::kObjectBytes;
FS::Fn g_impl = &FS::Native;
const char* g_impl_source = "hand-written reconstruction";

struct Case {
    u32 size;
    u32 alignment;
    u64 result_original;
    u64 result_native;
    alignas(8) u8 before[N];
    alignas(8) u8 original[N];
    alignas(8) u8 native[N];
};
static_assert(sizeof(Case) <= kCaseBytes);

struct Shadow {
    alignas(16) u8 before[N + 8];
    alignas(16) u8 a[N + 8];
    alignas(16) u8 b[N + 8];
    Case c;
};
thread_local Shadow t_shadow;

/// A snapshot of the live allocator: 8-byte fields read atomically (other threads may be allocating).
void Snapshot(u64 scratch, u8* out) {
    for (u32 i = 0; i < N; i += 8) {
        const u64 v = __atomic_load_n(GuestPtr<u64>(scratch + i), __ATOMIC_RELAXED);
        std::memcpy(out + i, &v, 8);
    }
}

struct Impl {
    static Slot slot;

    static FS::Fn Original() {
        return reinterpret_cast<FS::Fn>(slot.original);
    }

    static void Record(u32 size, u32 alignment, const Shadow& s, u64 ra, u64 rb) {
        Case& c = t_shadow.c;
        c = Case{};
        c.size = size;
        c.alignment = alignment;
        c.result_original = ra;
        c.result_native = rb;
        std::memcpy(c.before, s.before, N);
        std::memcpy(c.original, s.a, N);
        std::memcpy(c.native, s.b, N);
        RecordCase(slot.index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
    }

    /// Shadow comparison from a snapshot of the live object; false on a mismatch (recorded).
    static bool Check(u64 scratch, u32 size, u32 alignment, u64& shadow_result) {
        Shadow& s = t_shadow;
        Snapshot(scratch, s.before);
        u64 ra = 0, rb = 0;
        const bool same = FS::Compare(Original(), s.before, size, alignment, s.a, s.b, ra, rb);
        Count(slot.index, kCallsChecked);
        slot.verified.fetch_add(1, std::memory_order_relaxed);
        shadow_result = ra;
        if (!same) {
            slot.mismatches.fetch_add(1, std::memory_order_relaxed);
            Record(size, alignment, s, ra, rb);
        }
        return same;
    }

    static SSPC_SYSV_ABI u64 Native(u64 scratch, u32 size, u32 alignment) {
        return g_impl(scratch, size, alignment);
    }

    static SSPC_SYSV_ABI u64 Verify(u64 scratch, u32 size, u32 alignment) {
        u64 shadow = 0;
        Check(scratch, size, alignment, shadow);
        const u64 r = Original()(scratch, size, alignment); // authoritative
        if (r != shadow) {
            slot.live_diff.fetch_add(1, std::memory_order_relaxed); // another thread allocated in between
        }
        return r;
    }

    static SSPC_SYSV_ABI u64 Guarded(u64 scratch, u32 size, u32 alignment) {
        if (SampleThisCall(slot.index, slot.sample_period)) {
            u64 shadow = 0;
            if (!Check(scratch, size, alignment, shadow)) {
                Fallback(slot);
                Count(slot.index, kCallsOriginal);
                return Original()(scratch, size, alignment);
            }
        }
        Count(slot.index, kCallsNative);
        return g_impl(scratch, size, alignment);
    }

    static SSPC_SYSV_ABI u64 Bench(u64 scratch, u32 size, u32 alignment) {
        if (SampleThisCall(slot.index, slot.sample_period)) {
            Shadow& s = t_shadow;
            Snapshot(scratch, s.a);
            std::memcpy(s.b, s.a, N);
            const u64 base_a = reinterpret_cast<u64>(s.a), base_b = reinterpret_cast<u64>(s.b);
            const u64 e0 = TimerTicks();
            const u64 e1 = TimerTicks();
            u64 to = 0, tn = 0;
            const bool original_first = (slot.bench_samples.load(std::memory_order_relaxed) & 1) == 0;
            for (int k = 0; k < 2; ++k) {
                if ((k == 0) == original_first) {
                    const u64 t0 = TimerTicks();
                    Original()(base_a, size, alignment);
                    to = TimerTicks() - t0;
                } else {
                    const u64 t0 = TimerTicks();
                    g_impl(base_b, size, alignment);
                    tn = TimerTicks() - t0;
                }
            }
            slot.bench_samples.fetch_add(1, std::memory_order_relaxed);
            slot.bench_original_ticks.fetch_add(to, std::memory_order_relaxed);
            slot.bench_native_ticks.fetch_add(tn, std::memory_order_relaxed);
            slot.bench_empty_ticks.fetch_add(e1 - e0, std::memory_order_relaxed);
        }
        Count(slot.index, kCallsOriginal);
        return Original()(scratch, size, alignment);
    }
};
Slot Impl::slot;

using D = Dispatch<u64(u64, u32, u32)>;

bool SelfCheck(void* original, std::string& error) {
    const auto fn = reinterpret_cast<FS::Fn>(original);
    std::mt19937_64 rng{0x53504e4500000002ull};
    alignas(16) u8 object[N], a[N], b[N];
    static constexpr u32 kAlign[] = {0, 1, 2, 3, 4, 8, 16, 64, 128, 256, 4096, 0x80000000u, 0xffffffffu};
    for (u32 n = 0; n < 20000; ++n) {
        const auto field = [&](u32 offset, u64 v) { std::memcpy(object + offset, &v, 8); };
        for (u32 i = 0; i < N; ++i) {
            object[i] = static_cast<u8>(rng());
        }
        const u64 base = (n % 7 == 0) ? 0 : (rng() & 0x0000'7fff'ffff'f000ull);
        const u64 used = rng() % 0x10000;
        const u64 cap = rng() % 0x20000;
        field(0x38, n % 11 == 0 ? 0 : base + used);
        field(0x40, n % 13 == 0 ? ~0ull : base + cap);
        field(0x68, n % 17 == 0 ? ~0ull - (rng() & 0xff) : base + 0x100000 + (rng() % 0x1000));
        field(0x70, base + 0x100000 + (rng() % 0x2000));
        const u32 size = (n % 9 == 0) ? 0 : (n % 23 == 0) ? static_cast<u32>(rng()) : static_cast<u32>(rng() % 0x3000);
        const u32 alignment = kAlign[rng() % std::size(kAlign)];
        u64 ra = 0, rb = 0;
        if (!FS::Compare(fn, object, size, alignment, a, b, ra, rb)) {
            error = fmt::format("case {}: size {:#x} alignment {:#x}: original {:#x}, native {:#x}", n, size,
                                alignment, ra, rb);
            return false;
        }
    }
    return true;
}

constexpr Engine::GuestFunction kFunction{0x1f0870, 0x73, 0xaf5edf11446f3a57ull};

} // namespace

bool FS::Compare(Fn original, const u8* object, u32 size, u32 alignment, u8* a, u8* b, u64& result_a, u64& result_b) {
    std::memcpy(a, object, N);
    std::memcpy(b, object, N);
    result_a = original(reinterpret_cast<u64>(a), size, alignment);
    result_b = g_impl(reinterpret_cast<u64>(b), size, alignment);
    return result_a == result_b && std::memcmp(a, b, N) == 0;
}

bool FS::Replay(Fn original, const std::string& bytes, std::string& report) {
    if (bytes.size() != sizeof(Case)) {
        report = fmt::format("case of {} bytes, expected {}", bytes.size(), sizeof(Case));
        return false;
    }
    Case c;
    std::memcpy(&c, bytes.data(), sizeof(c));
    alignas(16) u8 a[N], b[N];
    u64 ra = 0, rb = 0;
    const bool same = Compare(original, c.before, c.size, c.alignment, a, b, ra, rb);
    report = fmt::format("size {:#x} alignment {:#x}: original {:#x} native {:#x} (recorded {:#x} / {:#x})", c.size,
                         c.alignment, ra, rb, c.result_original, c.result_native);
    return same;
}

void FS::UseImplementation(Fn fn, const char* source) {
    g_impl = fn;
    g_impl_source = source;
}

FS::Fn FS::Implementation() {
    return g_impl;
}

const char* FS::ImplementationSource() {
    return g_impl_source;
}

const FunctionSpec kFrameScratchAllocSpec{
    "frame_alloc",
    "FrameScratch::Alloc",
    kFunction,
    nullptr,
    0,
    nullptr,
    0,
    reinterpret_cast<const void*>(&D::Entry<Impl>),
    &Impl::slot,
    &SelfCheck,
};

} // namespace SecondSon::NativeEngine::Reconstructed
