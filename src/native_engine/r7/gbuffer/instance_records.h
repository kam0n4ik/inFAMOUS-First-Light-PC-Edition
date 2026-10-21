// SecondSonPC R7-G — the mesh pass's instance records (docs/NATIVE_ENGINE_R7_GBUFFER.md §2).
//
// 0x1a2030 (GBuffer::MeshPass) writes one 0xD4-byte record per drawn instance of an instanced group into a frame
// allocation (0x1f0870) and hands the group to 0x1a1770, whose VS reads the records as a buffer of stride 0xD4. The
// record [A, reconstruction of the loop body; bit-exact against the eboot's code in the function-level verification]:
//
//   +0x00 / +0x0c / +0x18  rows 0..2 of the object's world matrix normalised (xyz; rsqrt + one Newton step)
//   +0x24 / +0x34 / +0x44  world rows 0..2 (w = 0) x the view rows                       [H: world x view]
//   +0x54                  the view applied to world row 3 (x, y, z multiplied, the view's row 3 added)
//   +0x64 / +0x74 / +0x84  world rows 0..2 with w = 0
//   +0x94                  world row 3 with w = 1
//   +0xa4 (2 x f32)        fade: (1, -k) when the object's flag bit 0 is set; else (0, 1)-row constant when k == 1,
//                          else (-1, k); k = the object's byte +0x12 / 64                  [H: dissolve parameters]
//   +0xac (u64)            0
//   +0xb4 / +0xb8 / +0xbc  the instance's u32 +0x2c / +0x28 (light index) / +0x30
//   +0xc0                  k
//   +0xc4 (16 bytes)       0
//
// The arithmetic keeps the original's operation order (no FMA: the file is built with -ffp-contract=off, SSE/AVX
// intrinsics one per instruction) so the records are equal bit for bit.
#pragma once

#include <immintrin.h>

#include "base/abi.h"

namespace SecondSon::NativeEngine::R7G {

inline constexpr u32 kRecordBytes = 0xd4;

/// The eboot constants the loop reads (module-relative addresses; loaded once per module base).
struct PassConstants {
    __m128 newton_add;  ///< 0x999c50 (the Newton step's additive constant)
    __m128 newton_mul;  ///< 0x999c60
    __m128 sign;        ///< 0x999c70 (xor mask)
    __m128 ident1;      ///< 0x999c80 (identity rows 1..3; row 0 = {one, 0, 0, 0})
    __m128 ident2;      ///< 0x999c90
    __m128 ident3;      ///< 0x999ca0
    float one;          ///< 0x999d30
    float k64;          ///< 0x999d34
    float minus_one;    ///< 0x999d38
};
void LoadPassConstants(u64 module, PassConstants& c);

inline constexpr u64 kConstNewtonAdd = 0x999c50, kConstNewtonMul = 0x999c60, kConstSign = 0x999c70,
                     kConstIdent1 = 0x999c80, kConstIdent2 = 0x999c90, kConstIdent3 = 0x999ca0,
                     kConstOne = 0x999d30, kConstK64 = 0x999d34, kConstMinusOne = 0x999d38;

/// The view rows the records are built with (0x1a2030's copy of the view's matrix).
struct ViewRows {
    __m128 r[4];
};

/// The xyz of `v` normalised with the original's rsqrt + Newton step, multiplied back per lane.
inline __m128 NormalizeRow(__m128 v, const PassConstants& c) {
    const __m128 zero = _mm_setzero_ps();
    __m128 s = _mm_mul_ps(v, v);
    s = _mm_blend_ps(s, zero, 0x8);
    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);
    const __m128 y = _mm_rsqrt_ps(s);
    __m128 n = _mm_mul_ps(y, y);
    n = _mm_mul_ps(n, s);
    const __m128 t = _mm_mul_ps(y, c.newton_mul);
    n = _mm_add_ps(n, c.newton_add);
    n = _mm_mul_ps(t, n);
    const __m128 r = _mm_sub_ps(y, n);
    return _mm_mul_ps(r, v);
}

/// Row of world x view: ((V1 * y + V0 * x) + V2 * z) + V3 * w, as the loop orders it.
inline __m128 TransformRow(__m128 a, const ViewRows& v) {
    const __m128 x = _mm_permute_ps(a, 0x00), y = _mm_permute_ps(a, 0x55), z = _mm_permute_ps(a, 0xaa),
                 w = _mm_permute_ps(a, 0xff);
    __m128 r = _mm_add_ps(_mm_mul_ps(v.r[1], y), _mm_mul_ps(v.r[0], x));
    r = _mm_add_ps(r, _mm_mul_ps(v.r[2], z));
    return _mm_add_ps(r, _mm_mul_ps(v.r[3], w));
}

inline void Store3(u8* at, __m128 v) {
    _mm_storel_pi(reinterpret_cast<__m64*>(at), v);
    const int z = _mm_extract_ps(v, 2);
    __builtin_memcpy(at + 8, &z, 4);
}

/// One record at `rec` from the 0x80-byte instance `inst` and its mesh instance `mi` (instance + 0x18).
inline void BuildRecord(u8* rec, u64 inst, u64 mi, const ViewRows& v, const PassConstants& c) {
    const __m128i zi = _mm_setzero_si128();
    const __m128 w0 = _mm_loadu_ps(reinterpret_cast<const float*>(inst + 0x40));
    const __m128 w1 = _mm_loadu_ps(reinterpret_cast<const float*>(inst + 0x50));
    const __m128 w2 = _mm_loadu_ps(reinterpret_cast<const float*>(inst + 0x60));
    const __m128 w3 = _mm_loadu_ps(reinterpret_cast<const float*>(inst + 0x70));
    const __m128 a0 = _mm_castsi128_ps(_mm_blend_epi16(_mm_castps_si128(w0), zi, 0xc0));
    const __m128 a1 = _mm_castsi128_ps(_mm_blend_epi16(_mm_castps_si128(w1), zi, 0xc0));
    const __m128 a2 = _mm_castsi128_ps(_mm_blend_epi16(_mm_castps_si128(w2), zi, 0xc0));
    const __m128 t0 = TransformRow(a0, v), t1 = TransformRow(a1, v), t2 = TransformRow(a2, v);
    // row 3: (V0 * x + V1 * y) + V2 * z, then + V3 (its w not multiplied)
    __m128 t3 = _mm_add_ps(_mm_mul_ps(v.r[1], _mm_permute_ps(w3, 0x55)), _mm_mul_ps(_mm_permute_ps(w3, 0x00), v.r[0]));
    t3 = _mm_add_ps(t3, _mm_mul_ps(v.r[2], _mm_permute_ps(w3, 0xaa)));
    t3 = _mm_add_ps(t3, v.r[3]);
    Store3(rec + 0x00, NormalizeRow(w0, c));
    Store3(rec + 0x0c, NormalizeRow(w1, c));
    Store3(rec + 0x18, NormalizeRow(w2, c));
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0x64), a0);
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0x74), a1);
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0x84), a2);
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0x94), _mm_insert_ps(w3, _mm_set_ss(c.one), 0x30));
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0x24), t0);
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0x34), t1);
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0x44), t2);
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0x54), t3);
    const u8 kb = *reinterpret_cast<const u8*>(mi + 0x12);
    const float k = static_cast<float>(static_cast<s32>(kb)) * c.k64;
    __m128 fade;
    if ((*reinterpret_cast<const u8*>(mi + 0x10) & 1) != 0) {
        const __m128 neg = _mm_xor_ps(_mm_set_ss(k), c.sign);
        fade = _mm_insert_ps(_mm_set_ss(c.one), neg, 0x10);
    } else if (!(k != c.one)) { // the original's vucomiss + jz (equal or unordered)
        fade = c.ident1;
    } else {
        fade = _mm_insert_ps(_mm_set_ss(c.minus_one), _mm_set_ss(k), 0x10);
    }
    _mm_storel_pi(reinterpret_cast<__m64*>(rec + 0xa4), fade);
    const u64 zero64 = 0;
    __builtin_memcpy(rec + 0xac, &zero64, 8);
    __builtin_memcpy(rec + 0xb8, reinterpret_cast<const void*>(inst + 0x28), 4);
    __builtin_memcpy(rec + 0xb4, reinterpret_cast<const void*>(inst + 0x2c), 4);
    __builtin_memcpy(rec + 0xbc, reinterpret_cast<const void*>(inst + 0x30), 4);
    __builtin_memcpy(rec + 0xc0, &k, 4);
    _mm_storeu_ps(reinterpret_cast<float*>(rec + 0xc4), _mm_setzero_ps());
}

} // namespace SecondSon::NativeEngine::R7G
