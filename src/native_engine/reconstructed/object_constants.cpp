// SecondSonPC — reconstruction of eboot 0x11c890 (see object_constants.h).
//
// Floating point: every operation is the original's own instruction with the original's operand order (vmulps
// src1, src2 / vaddps / vsubps / vhaddps / vrsqrtps / vcmpeqps / vucomiss / vcvtsi2ss), no contraction, no
// reassociation - on the same CPU and MXCSR the results are bit-identical, including rsqrtps (an approximation
// whose exact bits are the host CPU's, for the original as well). Built with /arch:AVX (CMakeLists.txt): the
// original is AVX code.
#include "native_engine/reconstructed/object_constants.h"

#include <immintrin.h>

#include <cstring>
#include <random>

#include <fmt/format.h>

#include "native_engine/guest_abi_bridge.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

#pragma clang fp contract(off)

namespace SecondSon::NativeEngine::Reconstructed {

namespace {

namespace OC = ObjectConstants;

struct Impl;
using D = Dispatch<void(const float*, const float*, const float*, u64, const float*, s32, u32, u8*)>;

// --- the reconstruction -----------------------------------------------------------------------------------------

inline __m128 Load(const void* p) {
    return _mm_loadu_ps(static_cast<const float*>(p));
}
inline void Store(void* p, __m128 v) {
    _mm_storeu_ps(static_cast<float*>(p), v);
}
/// vmulps / vaddps / vsubps with the original's operand order. IEEE arithmetic is commutative except for which NaN
/// comes out when both operands are NaN: x86 returns the first source's. The compiler treats fmul/fadd as commutative
/// and the assembler swaps VEX sources for a shorter encoding, so the three are emitted as written with a forced
/// 3-byte VEX prefix (in-game NaN inputs are rare, but the result must be the original's bit for bit: the self-check
/// generates NaN payloads and caught exactly this).
inline __m128 Mul(__m128 a, __m128 b) {
    __m128 r;
    __asm__("%{vex3%} vmulps {%2, %1, %0|%0, %1, %2}" : "=x"(r) : "x"(a), "x"(b));
    return r;
}
inline __m128 Add(__m128 a, __m128 b) {
    __m128 r;
    __asm__("%{vex3%} vaddps {%2, %1, %0|%0, %1, %2}" : "=x"(r) : "x"(a), "x"(b));
    return r;
}
inline __m128 Sub(__m128 a, __m128 b) {
    __m128 r;
    __asm__("%{vex3%} vsubps {%2, %1, %0|%0, %1, %2}" : "=x"(r) : "x"(a), "x"(b));
    return r;
}
/// vpblendw zero, m, 0x3f: x, y, z kept, w = +0 (bit copy).
inline __m128 KeepXyz(__m128 v) {
    return _mm_castsi128_ps(_mm_blend_epi16(_mm_setzero_si128(), _mm_castps_si128(v), 0x3f));
}
/// vinsertps v, [1.0], 0x30: w = 1.0.
inline __m128 WithW1(__m128 v) {
    return _mm_insert_ps(v, _mm_set_ss(1.0f), 0x30);
}
template <int Lane>
inline __m128 Splat(__m128 v) {
    return _mm_permute_ps(v, Lane * 0x55);
}
inline u32 Same(__m128 a, const u8* block, u32 offset) {
    return static_cast<u32>(_mm_movemask_ps(_mm_cmpeq_ps(a, Load(block + offset))));
}
/// vucomiss + jz/setz: ZF is set for "equal" and for "unordered" (a NaN on either side).
inline bool UcomissEqual(float a, float b) {
    return a == b || __builtin_isnan(a) || __builtin_isnan(b);
}
inline void MarkChanged(u8* block, bool changed) {
    block[0x158] |= static_cast<u8>(changed);
}

/// Four rows: 0-2 with w = 0, 3 with w = 1 (flags bits 0, 7, 3).
inline void TransformBlock(const float* m, u8* block, u32 offset) {
    const __m128 r1 = KeepXyz(Load(m + 4));
    const __m128 r3 = WithW1(Load(m + 12));
    const __m128 r0 = KeepXyz(Load(m + 0));
    const __m128 r2 = KeepXyz(Load(m + 8));
    const u32 same = Same(r1, block, offset + 0x10) & Same(r0, block, offset + 0x00) & Same(r2, block, offset + 0x20) &
                     Same(r3, block, offset + 0x30);
    MarkChanged(block, same != 0xf);
    Store(block + offset + 0x00, r0);
    Store(block + offset + 0x10, r1);
    Store(block + offset + 0x20, r2);
    Store(block + offset + 0x30, r3);
}

// One row of world x view-projection, the original's instructions in the original's order (0x11cb03..0x11cbd8):
//   row 0: r = V0*x;  r = V1*y + r;  r = r + V2*z;  r = r + V3*w
//   row 1: t = x*V0;  r = V1*y + t;  r = r + V2*z;  r = r + V3*w
//   row 2: t = x*V0;  r = y*V1 + t;  r = r + V2*z;  r = r + V3*w
//   row 3: r = V1*y;  r = r + x*V0;  r = r + V2*z;  r = r + V3          (w of the world row unused, implicit 1)
// The products and sums are the same in any order except for WHICH NaN comes out when both operands of an
// instruction are NaN (x86 returns the first source's). LLVM's assembler swaps the sources of commutative VEX
// instructions (vmulps, vaddps, ...) when that allows the shorter 2-byte VEX prefix - for compiled intrinsics and
// for inline asm alike - so the NaN payload depended on register allocation (measured: 5 710 of 200 000 special-value
// cases differed, all NaN payloads). `{vex3}` forces the 3-byte encoding, which keeps the written operand order.
#define SSPC_PERM(imm, src, dst) "vpermilps {$" #imm ", %[" #src "], %[" #dst "]|%[" #dst "], %[" #src "], " #imm "}\n\t"
#define SSPC_OP3(op, dst, s1, s2) "%{vex3%} " #op " {%[" #s2 "], %[" #s1 "], %[" #dst "]|%[" #dst "], %[" #s1 "], %[" #s2 "]}\n\t"
template <int Row>
inline __m128 ProjectRow(__m128 a, __m128 v0, __m128 v1, __m128 v2, __m128 v3) {
    __m128 r, t, u;
    if constexpr (Row == 0) {
        __asm__(SSPC_PERM(0x00, a, t) SSPC_OP3(vmulps, r, v0, t) SSPC_PERM(0x55, a, t) SSPC_OP3(vmulps, t, v1, t)
                    SSPC_OP3(vaddps, r, t, r) SSPC_PERM(0xaa, a, t) SSPC_OP3(vmulps, t, v2, t) SSPC_OP3(vaddps, r, r, t)
                        SSPC_PERM(0xff, a, t) SSPC_OP3(vmulps, t, v3, t) SSPC_OP3(vaddps, r, r, t)
                : [r] "=&x"(r), [t] "=&x"(t), [u] "=&x"(u)
                : [a] "x"(a), [v0] "x"(v0), [v1] "x"(v1), [v2] "x"(v2), [v3] "x"(v3));
    } else if constexpr (Row == 1) {
        __asm__(SSPC_PERM(0x00, a, u) SSPC_OP3(vmulps, u, u, v0) SSPC_PERM(0x55, a, t) SSPC_OP3(vmulps, t, v1, t)
                    SSPC_OP3(vaddps, r, t, u) SSPC_PERM(0xaa, a, t) SSPC_OP3(vmulps, t, v2, t) SSPC_OP3(vaddps, r, r, t)
                        SSPC_PERM(0xff, a, t) SSPC_OP3(vmulps, t, v3, t) SSPC_OP3(vaddps, r, r, t)
                : [r] "=&x"(r), [t] "=&x"(t), [u] "=&x"(u)
                : [a] "x"(a), [v0] "x"(v0), [v1] "x"(v1), [v2] "x"(v2), [v3] "x"(v3));
    } else if constexpr (Row == 2) {
        __asm__(SSPC_PERM(0x00, a, u) SSPC_OP3(vmulps, u, u, v0) SSPC_PERM(0x55, a, t) SSPC_OP3(vmulps, t, t, v1)
                    SSPC_OP3(vaddps, r, t, u) SSPC_PERM(0xaa, a, t) SSPC_OP3(vmulps, t, v2, t) SSPC_OP3(vaddps, r, r, t)
                        SSPC_PERM(0xff, a, t) SSPC_OP3(vmulps, t, v3, t) SSPC_OP3(vaddps, r, r, t)
                : [r] "=&x"(r), [t] "=&x"(t), [u] "=&x"(u)
                : [a] "x"(a), [v0] "x"(v0), [v1] "x"(v1), [v2] "x"(v2), [v3] "x"(v3));
    } else {
        __asm__(SSPC_PERM(0x55, a, t) SSPC_OP3(vmulps, r, v1, t) SSPC_PERM(0x00, a, u) SSPC_OP3(vmulps, u, u, v0)
                    SSPC_OP3(vaddps, r, r, u) SSPC_PERM(0xaa, a, t) SSPC_OP3(vmulps, t, v2, t)
                        SSPC_OP3(vaddps, r, r, t) SSPC_OP3(vaddps, r, r, v3)
                : [r] "=&x"(r), [t] "=&x"(t), [u] "=&x"(u)
                : [a] "x"(a), [v0] "x"(v0), [v1] "x"(v1), [v2] "x"(v2), [v3] "x"(v3));
    }
    return r;
}
#undef SSPC_PERM
#undef SSPC_OP3

/// One row scaled to unit length of x, y, z: rsqrtps and one Newton-Raphson step, as the original.
inline __m128 NormalizeRow(__m128 row) {
    const __m128 zero = _mm_setzero_ps();
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 minus_one = _mm_set1_ps(-1.0f);
    __m128 len2 = Mul(row, row);
    len2 = _mm_blend_ps(len2, zero, 0x8);
    len2 = _mm_hadd_ps(len2, len2);
    len2 = _mm_hadd_ps(len2, len2);
    const __m128 y = _mm_rsqrt_ps(len2);
    __m128 t = Mul(Mul(y, y), len2);
    const __m128 hy = Mul(y, half);
    t = Add(t, minus_one);
    t = Mul(hy, t);
    t = Sub(y, t);
    return Mul(t, row);
}

} // namespace

SSPC_SYSV_ABI void OC::Native(const float* world, const float* previous, const float* view_projection, u64 object,
                              const float* extra, s32 value, u32 flags, u8* block) {
    const u8 f = static_cast<u8>(flags);
    if ((f & 0x01) != 0) {
        TransformBlock(world, block, 0x00);
    }
    if ((f & 0x80) != 0) {
        TransformBlock(previous, block, 0x40);
    }
    if ((f & 0x02) != 0) {
        const __m128 zero = _mm_setzero_ps();
        const __m128 n0 = NormalizeRow(Load(world + 0));
        const __m128 n1 = NormalizeRow(Load(world + 4));
        const __m128 n2 = NormalizeRow(Load(world + 8));
        const u32 same = Same(_mm_blend_ps(n1, zero, 0x8), block, 0x90) & Same(_mm_blend_ps(n0, zero, 0x8), block, 0x80) &
                         Same(_mm_blend_ps(n2, zero, 0x8), block, 0xa0);
        MarkChanged(block, same != 0xf);
        const __m128i z = _mm_setzero_si128();
        Store(block + 0x80, _mm_castsi128_ps(_mm_blend_epi16(_mm_castps_si128(n0), z, 0xc0)));
        Store(block + 0x90, _mm_castsi128_ps(_mm_blend_epi16(_mm_castps_si128(n1), z, 0xc0)));
        Store(block + 0xa0, _mm_castsi128_ps(_mm_blend_epi16(_mm_castps_si128(n2), z, 0xc0)));
    }
    if ((f & 0x04) != 0) {
        const __m128 a0 = KeepXyz(Load(world + 0));
        const __m128 a1 = KeepXyz(Load(world + 4));
        const __m128 a2 = KeepXyz(Load(world + 8));
        const __m128 a3 = Load(world + 12);
        const __m128 v0 = Load(view_projection + 0);
        const __m128 v1 = Load(view_projection + 4);
        const __m128 v2 = Load(view_projection + 8);
        const __m128 v3 = Load(view_projection + 12);
        const __m128 r0 = ProjectRow<0>(a0, v0, v1, v2, v3);
        const __m128 r1 = ProjectRow<1>(a1, v0, v1, v2, v3);
        const __m128 r2 = ProjectRow<2>(a2, v0, v1, v2, v3);
        const __m128 r3 = ProjectRow<3>(a3, v0, v1, v2, v3);
        const u32 same = Same(r1, block, 0xc0) & Same(r0, block, 0xb0) & Same(r3, block, 0xe0) & Same(r2, block, 0xd0);
        MarkChanged(block, same != 0xf);
        Store(block + 0xb0, r0);
        Store(block + 0xc0, r1);
        Store(block + 0xd0, r2);
        Store(block + 0xe0, r3);
    }
    if ((f & 0x08) != 0) {
        TransformBlock(extra, block, 0xf0);
    }
    if ((f & 0x10) != 0) {
        const u32 bits = GuestLoad<u32>(object + 0x10);
        const s32 level = static_cast<s32>((bits >> 16) & 0xff);
        const __m128 k = _mm_mul_ss(_mm_cvtsi32_ss(_mm_setzero_ps(), level), _mm_set_ss(0.015625f));
        __m128 v;
        if ((bits & 1) != 0) {
            const __m128 neg = _mm_xor_ps(k, _mm_set1_ps(-0.0f));
            v = _mm_insert_ps(_mm_insert_ps(_mm_insert_ps(_mm_set_ss(1.0f), neg, 0x10), neg, 0x20), neg, 0x30);
        } else if (UcomissEqual(_mm_cvtss_f32(k), 1.0f)) {
            v = _mm_setr_ps(0.0f, 1.0f, 0.0f, 0.0f);
        } else {
            v = _mm_insert_ps(_mm_insert_ps(_mm_insert_ps(_mm_set_ss(-1.0f), k, 0x10), k, 0x20), k, 0x30);
        }
        // vucomiss + setnz: "differs" only when ordered and not equal (NaN counts as the same)
        float lane[4];
        _mm_storeu_ps(lane, v);
        float old0, old1;
        std::memcpy(&old0, block + 0x130, 4);
        std::memcpy(&old1, block + 0x134, 4);
        const bool d0 = !UcomissEqual(lane[0], old0);
        const bool d1 = !UcomissEqual(lane[1], old1);
        MarkChanged(block, d0 || d1);
        std::memcpy(block + 0x130, lane, 8);
    }
    if ((f & 0x20) != 0) {
        s32 old;
        std::memcpy(&old, block + 0x138, 4);
        MarkChanged(block, old != value);
        std::memcpy(block + 0x138, &value, 4);
    }
    if ((f & 0x40) != 0) {
        s32 old;
        std::memcpy(&old, block + 0x13c, 4);
        MarkChanged(block, old != value);
        std::memcpy(block + 0x13c, &value, 4);
    }
}


// --- verification -----------------------------------------------------------------------------------------------

namespace {

constexpr u32 N = OC::kBlockBytes;

/// The case format (native_engine_verify.h): what the original read and wrote, both results.
struct Case {
    u32 flags;
    s32 value;
    u32 object_word; ///< object + 0x10 (read by bit 4)
    u32 pad;
    float world[16], previous[16], view_projection[16], extra[16]; ///< zero when the flags do not read them
    u8 before[N];
    u8 original[N];
    u8 native[N];
};
static_assert(sizeof(Case) <= kCaseBytes);

/// Shadow buffers per thread: guest stacks are small (guest_abi_bridge.h), so nothing large goes on them.
struct Shadow {
    alignas(16) u8 before[N];
    alignas(16) u8 a[N];
    alignas(16) u8 b[N];
    Case c;
};
thread_local Shadow t_shadow;

bool Overlaps(const void* p, u32 size, const u8* block) {
    const u64 x = reinterpret_cast<u64>(p), y = reinterpret_cast<u64>(block);
    return x < y + N && y < x + size;
}

/// The inputs this call reads (by its flags) overlap the block: shadow copies would not reproduce the original's
/// read-after-write order, so such a call is not shadowed (counted as skipped).
bool Aliased(const float* world, const float* previous, const float* vp, u64 object, const float* extra, u32 flags,
             const u8* block) {
    const u8 f = static_cast<u8>(flags);
    return ((f & 0x07) != 0 && Overlaps(world, 64, block)) || ((f & 0x80) != 0 && Overlaps(previous, 64, block)) ||
           ((f & 0x04) != 0 && Overlaps(vp, 64, block)) || ((f & 0x08) != 0 && Overlaps(extra, 64, block)) ||
           ((f & 0x10) != 0 && Overlaps(reinterpret_cast<const void*>(object + 0x10), 4, block));
}

struct Impl {
    static Slot slot;

    static OC::Fn Original() {
        return reinterpret_cast<OC::Fn>(slot.original);
    }

    static void Record(const float* world, const float* previous, const float* vp, u64 object, const float* extra,
                       s32 value, u32 flags, const Shadow& s) {
        Case& c = t_shadow.c;
        std::memset(&c, 0, sizeof(c));
        const u8 f = static_cast<u8>(flags);
        c.flags = flags;
        c.value = value;
        if ((f & 0x10) != 0) {
            c.object_word = GuestLoad<u32>(object + 0x10);
        }
        if ((f & 0x07) != 0) {
            std::memcpy(c.world, world, 64);
        }
        if ((f & 0x80) != 0) {
            std::memcpy(c.previous, previous, 64);
        }
        if ((f & 0x04) != 0) {
            std::memcpy(c.view_projection, vp, 64);
        }
        if ((f & 0x08) != 0) {
            std::memcpy(c.extra, extra, 64);
        }
        std::memcpy(c.before, s.before, N);
        std::memcpy(c.original, s.a, N);
        std::memcpy(c.native, s.b, N);
        RecordCase(slot.index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
    }

    static SSPC_SYSV_ABI void Native(const float* world, const float* previous, const float* vp, u64 object,
                                     const float* extra, s32 value, u32 flags, u8* block) {
        OC::Native(world, previous, vp, object, extra, value, flags, block);
    }

    static SSPC_SYSV_ABI void Verify(const float* world, const float* previous, const float* vp, u64 object,
                                     const float* extra, s32 value, u32 flags, u8* block) {
        const OC::Fn original = Original();
        if (Aliased(world, previous, vp, object, extra, flags, block)) {
            slot.skipped.fetch_add(1, std::memory_order_relaxed);
            Count(slot.index, kCallsOriginal);
            original(world, previous, vp, object, extra, value, flags, block);
            return;
        }
        Shadow& s = t_shadow;
        std::memcpy(s.before, block, N);
        const bool same = OC::Compare(original, world, previous, vp, object, extra, value, flags, s.before, s.a, s.b);
        Count(slot.index, kCallsChecked);
        slot.verified.fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            slot.mismatches.fetch_add(1, std::memory_order_relaxed);
            Record(world, previous, vp, object, extra, value, flags, s);
        }
        original(world, previous, vp, object, extra, value, flags, block); // authoritative
        if (std::memcmp(block, s.a, N) != 0) {
            slot.live_diff.fetch_add(1, std::memory_order_relaxed);
        }
    }

    static SSPC_SYSV_ABI void Guarded(const float* world, const float* previous, const float* vp, u64 object,
                                      const float* extra, s32 value, u32 flags, u8* block) {
        if (SampleThisCall(slot.index, slot.sample_period) &&
            !Aliased(world, previous, vp, object, extra, flags, block)) {
            Shadow& s = t_shadow;
            std::memcpy(s.before, block, N);
            const OC::Fn original = Original();
            const bool same =
                OC::Compare(original, world, previous, vp, object, extra, value, flags, s.before, s.a, s.b);
            Count(slot.index, kCallsChecked);
            slot.verified.fetch_add(1, std::memory_order_relaxed);
            if (!same) {
                slot.mismatches.fetch_add(1, std::memory_order_relaxed);
                Record(world, previous, vp, object, extra, value, flags, s);
                Fallback(slot); // the original from now on, on every thread
                Count(slot.index, kCallsOriginal);
                original(world, previous, vp, object, extra, value, flags, block);
                return;
            }
        }
        Count(slot.index, kCallsNative);
        OC::Native(world, previous, vp, object, extra, value, flags, block);
    }

    static SSPC_SYSV_ABI void Bench(const float* world, const float* previous, const float* vp, u64 object,
                                    const float* extra, s32 value, u32 flags, u8* block) {
        const OC::Fn original = Original();
        if (SampleThisCall(slot.index, slot.sample_period) &&
            !Aliased(world, previous, vp, object, extra, flags, block)) {
            Shadow& s = t_shadow;
            std::memcpy(s.a, block, N);
            std::memcpy(s.b, block, N);
            const u64 e0 = TimerTicks();
            const u64 e1 = TimerTicks();
            u64 to = 0, tn = 0;
            const bool original_first = (slot.bench_samples.load(std::memory_order_relaxed) & 1) == 0;
            for (int k = 0; k < 2; ++k) {
                if ((k == 0) == original_first) {
                    const u64 t0 = TimerTicks();
                    original(world, previous, vp, object, extra, value, flags, s.a);
                    to = TimerTicks() - t0;
                } else {
                    const u64 t0 = TimerTicks();
                    OC::Native(world, previous, vp, object, extra, value, flags, s.b);
                    tn = TimerTicks() - t0;
                }
            }
            slot.bench_samples.fetch_add(1, std::memory_order_relaxed);
            slot.bench_original_ticks.fetch_add(to, std::memory_order_relaxed);
            slot.bench_native_ticks.fetch_add(tn, std::memory_order_relaxed);
            slot.bench_empty_ticks.fetch_add(e1 - e0, std::memory_order_relaxed);
        }
        Count(slot.index, kCallsOriginal);
        original(world, previous, vp, object, extra, value, flags, block);
    }
};
Slot Impl::slot;

// --- the install-time self-check: generated inputs, bit-exact against the loaded original -----------------------

float GeneratedValue(std::mt19937_64& rng) {
    const u32 pick = static_cast<u32>(rng() % 100);
    if (pick < 70) {
        return static_cast<float>(static_cast<double>(static_cast<s64>(rng() % 2000001) - 1000000) / 977.0);
    }
    u32 bits;
    switch (pick % 10) {
    case 0: bits = 0x00000000u; break;                                        // +0
    case 1: bits = 0x80000000u; break;                                        // -0
    case 2: bits = 0x3f800000u; break;                                        // 1
    case 3: bits = 0x00000001u + static_cast<u32>(rng() & 0xff); break;       // denormal
    case 4: bits = 0x7f7fffffu; break;                                        // FLT_MAX
    case 5: bits = 0x7f800000u; break;                                        // +inf
    case 6: bits = 0xff800000u; break;                                        // -inf
    case 7: bits = 0x7fc00000u | static_cast<u32>(rng() & 0x3fffff); break;  // quiet NaN with a payload
    case 8: bits = 0xffc00000u | static_cast<u32>(rng() & 0x3fffff); break;
    default: bits = static_cast<u32>(rng()); break;                           // any bit pattern
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

bool SelfCheck(void* original, std::string& error) {
    const auto fn = reinterpret_cast<OC::Fn>(original);
    std::mt19937_64 rng{0x53504e4500000001ull};
    alignas(16) float world[16], previous[16], vp[16], extra[16];
    alignas(16) u8 object[0x20] = {};
    alignas(16) u8 block[N], a[N], b[N];
    for (u32 i = 0; i < N; ++i) {
        block[i] = static_cast<u8>(rng());
    }
    for (u32 n = 0; n < 4096; ++n) {
        for (float* m : {world, previous, vp, extra}) {
            for (int i = 0; i < 16; ++i) {
                m[i] = GeneratedValue(rng);
            }
        }
        const u32 word = static_cast<u32>(rng());
        std::memcpy(object + 0x10, &word, 4);
        const u32 flags = (n & 0xff) | static_cast<u32>(rng() & 0xffffff00u);
        const s32 value = (n % 3 == 0) ? 0 : static_cast<s32>(rng());
        if (n % 5 == 0) { // a fresh block now and then; otherwise the previous result (the "unchanged" paths)
            for (u32 i = 0; i < N; ++i) {
                block[i] = static_cast<u8>(rng());
            }
        }
        if (!OC::Compare(fn, world, previous, vp, reinterpret_cast<u64>(object), extra, value, flags, block, a, b)) {
            u32 at = 0;
            while (at < N && a[at] == b[at]) {
                ++at;
            }
            error = fmt::format("case {} (flags {:#x}): first different byte +{:#x} (original {:#04x}, native {:#04x})",
                                n, flags, at, a[at], b[at]);
            return false;
        }
        std::memcpy(block, a, N);
    }
    return true;
}

constexpr Engine::GuestFunction kFunction{0x11c890, 0x507, 0xa3cfda22c0d0ac59ull};
constexpr GuestData kData[] = {
    {0x9934e0, 0x40, 0x14bbcaba6eb31648ull}, // -1.0 x4, 0.5 x4, -0.0 x4, (0, 1, 0, 0)
    {0x9936d8, 0xc, 0x68defeaeada2c471ull},  // 1.0, 1/64, -1.0
};

} // namespace

bool OC::Compare(Fn original, const float* world, const float* previous, const float* view_projection, u64 object,
                 const float* extra, s32 value, u32 flags, const u8* block, u8* a, u8* b) {
    std::memcpy(a, block, N);
    std::memcpy(b, block, N);
    original(world, previous, view_projection, object, extra, value, flags, a);
    Native(world, previous, view_projection, object, extra, value, flags, b);
    return std::memcmp(a, b, N) == 0;
}

bool OC::Replay(Fn original, const std::string& bytes, std::string& report) {
    if (bytes.size() != sizeof(Case)) {
        report = fmt::format("case of {} bytes, expected {}", bytes.size(), sizeof(Case));
        return false;
    }
    Case c;
    std::memcpy(&c, bytes.data(), sizeof(c));
    alignas(16) u8 object[0x20] = {};
    std::memcpy(object + 0x10, &c.object_word, 4);
    alignas(16) u8 a[N], b[N];
    const bool same = Compare(original, c.world, c.previous, c.view_projection, reinterpret_cast<u64>(object), c.extra,
                              c.value, c.flags, c.before, a, b);
    report = fmt::format("flags {:#x}: native {} original; recorded original reproduced: {}, recorded native "
                         "reproduced: {}",
                         c.flags, same ? "==" : "!=", std::memcmp(a, c.original, N) == 0,
                         std::memcmp(b, c.native, N) == 0);
    return same;
}

const FunctionSpec kObjectConstantsSpec{
    "object_constants",
    "ObjectConstants::Update",
    kFunction,
    nullptr,
    0,
    kData,
    static_cast<u32>(std::size(kData)),
    reinterpret_cast<const void*>(&D::Entry<Impl>),
    &Impl::slot,
    &SelfCheck,
};

} // namespace SecondSon::NativeEngine::Reconstructed
