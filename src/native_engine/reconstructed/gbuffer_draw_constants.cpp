// SecondSonPC — reconstruction of eboot 0x11e5d0 (see gbuffer_draw_constants.h).
#include "native_engine/reconstructed/gbuffer_draw_constants.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

#include <fmt/format.h>

#include "native_engine/guest_abi_bridge.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

namespace SecondSon::NativeEngine::Reconstructed {

namespace DCS = DrawConstantsSet;

namespace {

constexpr Engine::GuestFunction kFunction = {0x11e5d0, 0x36, 0x8bdff4be403c81edull};

/// The original's `vucomiss old, new; jz`: ZF = equal or unordered, on the thread's MXCSR.
inline bool SameOrUnordered(u32 old_bits, u32 new_bits) {
    float o, n;
    std::memcpy(&o, &old_bits, 4);
    std::memcpy(&n, &new_bits, 4);
    u8 z;
    __asm__ volatile("vucomiss %2, %1\n\tsetz %0" : "=r"(z) : "x"(o), "x"(n) : "cc");
    return z != 0;
}

DCS::Fn g_impl = &DCS::Native;
const char* g_impl_source = "hand-written reconstruction";

/// The span one call writes: the slots and the dirty byte (it may lie among the slots).
struct Span {
    u64 lo, hi;
};
bool MakeSpan(u64 block, u32 offset, u64 src, u32 count, Span& s) {
    const s32 n = static_cast<s32>(count);
    if (n <= 0) {
        s = {block + DCS::kDirtyOffset, block + DCS::kDirtyOffset + 1};
        return true;
    }
    const u64 d0 = block + static_cast<u64>(static_cast<u16>(offset)) * 4;
    const u64 d1 = d0 + static_cast<u64>(n) * 4;
    s.lo = std::min(d0, block + DCS::kDirtyOffset);
    s.hi = std::max(d1, block + DCS::kDirtyOffset + 1);
    if (s.hi - s.lo > DCS::kMaxSpan) {
        return false;
    }
    const u64 s0 = src, s1 = src + static_cast<u64>(n) * 4;
    return s1 <= s.lo || s0 >= s.hi; // src overlapping the written span: the shadow copies would change the aliasing
}

struct Shadows {
    alignas(64) u8 a[DCS::kMaxSpan];
    alignas(64) u8 b[DCS::kMaxSpan];
};
thread_local Shadows* t_shadows = nullptr;
Shadows& GetShadows() {
    if (t_shadows == nullptr) { // per verifying thread, never freed
        t_shadows = new Shadows{};
    }
    return *t_shadows;
}

bool Compare(DCS::Fn original, const Span& s, u64 block, u32 offset, u64 src, u32 count) {
    Shadows& sh = GetShadows();
    const u64 n = s.hi - s.lo;
    std::memcpy(sh.a, reinterpret_cast<const void*>(s.lo), n);
    std::memcpy(sh.b, sh.a, n);
    const u64 rel = block - s.lo;
    original(reinterpret_cast<u64>(sh.a) + rel, offset, src, count);
    g_impl(reinterpret_cast<u64>(sh.b) + rel, offset, src, count);
    return std::memcmp(sh.a, sh.b, n) == 0;
}

/// A mismatch: the span before the call, its place relative to the block, the source values.
struct Case {
    u32 offset;
    u32 count;
    u32 span_bytes;
    u32 block_rel; ///< block - span.lo
    u8 span[1024];
    u8 src[960];
};
static_assert(sizeof(Case) <= kCaseBytes);

void RecordMismatch(u32 index, const Span& s, u64 block, u32 offset, u64 src, u32 count) {
    const u64 n = s.hi - s.lo;
    const s32 c = static_cast<s32>(count);
    if (n > sizeof(Case::span) || (c > 0 && static_cast<u64>(c) * 4 > sizeof(Case::src))) {
        return; // too large for a case file (counted as a mismatch all the same)
    }
    Case k{};
    k.offset = offset;
    k.count = count;
    k.span_bytes = static_cast<u32>(n);
    k.block_rel = static_cast<u32>(block - s.lo);
    std::memcpy(k.span, reinterpret_cast<const void*>(s.lo), n);
    if (c > 0) {
        std::memcpy(k.src, reinterpret_cast<const void*>(src), static_cast<u64>(c) * 4);
    }
    RecordCase(index, {reinterpret_cast<const u8*>(&k), sizeof(k)});
}

struct Impl {
    static Slot slot;

    static DCS::Fn Original() {
        return reinterpret_cast<DCS::Fn>(slot.original);
    }

    static bool Check(const Span& s, u64 block, u32 offset, u64 src, u32 count) {
        const bool same = Compare(Original(), s, block, offset, src, count);
        Count(slot.index, kCallsChecked);
        slot.verified.fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            slot.mismatches.fetch_add(1, std::memory_order_relaxed);
            RecordMismatch(slot.index, s, block, offset, src, count);
        }
        return same;
    }

    static SSPC_SYSV_ABI void Native(u64 block, u32 offset, u64 src, u32 count) {
        g_impl(block, offset, src, count);
    }

    static SSPC_SYSV_ABI void Verify(u64 block, u32 offset, u64 src, u32 count) {
        Span s;
        if (!MakeSpan(block, offset, src, count, s)) {
            slot.skipped.fetch_add(1, std::memory_order_relaxed);
            Count(slot.index, kCallsOriginal);
            Original()(block, offset, src, count);
            return;
        }
        Check(s, block, offset, src, count);
        Original()(block, offset, src, count); // authoritative
        if (std::memcmp(GetShadows().a, reinterpret_cast<const void*>(s.lo), s.hi - s.lo) != 0) {
            slot.live_diff.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static SSPC_SYSV_ABI void Guarded(u64 block, u32 offset, u64 src, u32 count) {
        Span s;
        if (SampleThisCall(slot.index, slot.sample_period) && MakeSpan(block, offset, src, count, s) &&
            !Check(s, block, offset, src, count)) {
            Fallback(slot);
            Count(slot.index, kCallsOriginal);
            Original()(block, offset, src, count);
            return;
        }
        Count(slot.index, kCallsNative);
        g_impl(block, offset, src, count);
    }

    static SSPC_SYSV_ABI void Bench(u64 block, u32 offset, u64 src, u32 count) {
        Span s;
        if (SampleThisCall(slot.index, slot.sample_period) && MakeSpan(block, offset, src, count, s)) {
            Shadows& sh = GetShadows();
            const u64 n = s.hi - s.lo, rel = block - s.lo;
            std::memcpy(sh.a, reinterpret_cast<const void*>(s.lo), n);
            std::memcpy(sh.b, sh.a, n);
            const u64 e0 = TimerTicks();
            const u64 e1 = TimerTicks();
            const u64 t0 = TimerTicks();
            Original()(reinterpret_cast<u64>(sh.a) + rel, offset, src, count);
            const u64 t1 = TimerTicks();
            g_impl(reinterpret_cast<u64>(sh.b) + rel, offset, src, count);
            const u64 t2 = TimerTicks();
            slot.bench_samples.fetch_add(1, std::memory_order_relaxed);
            slot.bench_original_ticks.fetch_add(t1 - t0, std::memory_order_relaxed);
            slot.bench_native_ticks.fetch_add(t2 - t1, std::memory_order_relaxed);
            slot.bench_empty_ticks.fetch_add(e1 - e0, std::memory_order_relaxed);
        }
        Count(slot.index, kCallsOriginal);
        Original()(block, offset, src, count);
    }
};
Slot Impl::slot;

using D = Dispatch<void(u64, u32, u64, u32)>;

u32 SpecialBits(std::mt19937_64& rng) {
    static constexpr u32 kBits[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0x00000001u, 0x807fffffu, 0x7f7fffffu,
                                    0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc12345u, 0x7fa00001u, 0x3f800001u};
    return rng() % 3 == 0 ? static_cast<u32>(rng()) : kBits[rng() % std::size(kBits)];
}

/// 4000 generated calls (offsets, counts incl. <= 0, NaN/denormal/signed-zero values, the dirty byte among the
/// slots) against the clone of the loaded original, on whole 8 KiB buffers.
bool SelfCheck(void* original, std::string& error) {
    const auto o = reinterpret_cast<DCS::Fn>(original);
    std::mt19937_64 rng{0x53504e4511e5d0ull};
    std::vector<u8> a(DCS::kMaxSpan), b(DCS::kMaxSpan);
    std::vector<u32> src(400);
    for (u32 n = 0; n < 4000; ++n) {
        for (size_t i = 0; i < a.size(); i += 4) {
            const u32 v = SpecialBits(rng);
            std::memcpy(a.data() + i, &v, 4);
        }
        const u32 offset = static_cast<u32>(rng() % 0x300) | (n % 7 == 0 ? 0xabcd0000u : 0u); // upper bits ignored
        const s32 count = n % 11 == 0 ? -static_cast<s32>(rng() % 3) : static_cast<s32>(rng() % 200);
        for (u32& v : src) {
            v = SpecialBits(rng);
        }
        for (s32 i = 0; i < count; ++i) { // some slots unchanged
            if (rng() % 3 == 0) {
                std::memcpy(&src[i], a.data() + static_cast<u16>(offset) * 4 + i * 4, 4);
            }
        }
        b = a;
        o(reinterpret_cast<u64>(a.data()), offset, reinterpret_cast<u64>(src.data()), static_cast<u32>(count));
        g_impl(reinterpret_cast<u64>(b.data()), offset, reinterpret_cast<u64>(src.data()), static_cast<u32>(count));
        if (a != b) {
            error = fmt::format("case {}: offset {:#x} count {}: different", n, offset, count);
            return false;
        }
    }
    return true;
}

} // namespace

SSPC_SYSV_ABI void DCS::Native(u64 block, u32 offset, u64 src, u32 count) {
    s32 n = static_cast<s32>(count);
    if (n <= 0) {
        return;
    }
    u64 dst = block + static_cast<u64>(static_cast<u16>(offset)) * 4;
    do {
        const u32 nv = GuestLoad<u32>(src);
        const u32 ov = GuestLoad<u32>(dst);
        if (!SameOrUnordered(ov, nv)) {
            *GuestPtr<u8>(block + kDirtyOffset) = 1;
            std::memcpy(GuestPtr<void>(dst), &nv, 4);
        }
        dst += 4;
        src += 4;
    } while (--n != 0);
}

bool DCS::Replay(const std::string& bytes, std::string& report) {
    if (bytes.size() != sizeof(Case)) {
        report = fmt::format("case of {} bytes, expected {}", bytes.size(), sizeof(Case));
        return false;
    }
    if (Impl::slot.original == nullptr) {
        report = "no original installed";
        return false;
    }
    Case k;
    std::memcpy(&k, bytes.data(), sizeof(k));
    std::vector<u8> a(k.span, k.span + k.span_bytes), b = a;
    Impl::Original()(reinterpret_cast<u64>(a.data()) + k.block_rel, k.offset, reinterpret_cast<u64>(k.src), k.count);
    g_impl(reinterpret_cast<u64>(b.data()) + k.block_rel, k.offset, reinterpret_cast<u64>(k.src), k.count);
    report = fmt::format("offset {:#x} count {}: {}", k.offset, static_cast<s32>(k.count), a == b ? "same" : "DIFFERENT");
    return a == b;
}

void DCS::UseImplementation(Fn fn, const char* source) {
    g_impl = fn;
    g_impl_source = source;
}

DCS::Fn DCS::Implementation() {
    return g_impl;
}

const char* DCS::ImplementationSource() {
    return g_impl_source;
}

const FunctionSpec kDrawConstantsSetSpec{
    "draw_constants_set",
    "R::SetDrawConstants",
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
