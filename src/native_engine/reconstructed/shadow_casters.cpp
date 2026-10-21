// SecondSonPC — R7-S: hand-written reconstruction of eboot 0x43d590 / 0x43d480 (see shadow_casters.h).
//
// Floating point: the instance records are the original's instructions in the original's operand order (0x43e1a4..
// 0x43e280: per row vpermilps broadcasts, vmulps, vaddps), emitted as {vex3} inline asm - the same four row schedules
// as R5's ObjectConstants (0x11cb03..0x11cbd8), verified equal there. No contraction, no reassociation.
#include "native_engine/reconstructed/shadow_casters.h"

#include <immintrin.h>

#include <cstring>

#include <windows.h>

#include "native_engine/r7/shadows/shadow_params.h"
#include "native_engine/r7/shadows/shadow_stats.h"

#pragma clang fp contract(off)

namespace SecondSon::NativeEngine::Reconstructed::ShadowCasters {

namespace {

namespace Stats = R7Shadows::Stats;

enum Callee : u32 {
    kCDcbAlloc,
    kCBindConstants,
    kCObjectConstants,
    kCCameraConstants,
    kCCommitDrawConstants,
    kCSetBuffers,
    kCSetVertexBuffers,
    kCMeshDraw,
    kCFrameScratch,
    kCInstancedDraw,
    kCMaterialBind,
    kCParams,
    kCParamsInstanced,
    kCInstanceUv,
    kCPrng,
    kCInitBuffer,
    kCStage,
    kCNumInstances,
    kCPrimType,
    kCIndexSize,
    kCCueCommit,
    kCDrawIndex,
    kCalleeCount
};

constexpr u64 kCalleeRva[kCalleeCount] = {
    kDcbAlloc,     kBindConstants, kObjectConstants, kCameraConstants, kCommitDrawConstants, kSetBuffers,
    kSetVertexBuffers, kMeshDraw,  kFrameScratch,    kInstancedDraw,   kMaterialBind,        kParams,
    kParamsInstanced, kInstanceUv, kPrng,            kInitBuffer,      kStage,               kNumInstances,
    kPrimType,     kIndexSize,     kCueCommit,       kDrawIndex,
};

struct Table {
    u64 fn[kCalleeCount];
};

u64 g_module = 0;
Table g_live{};
thread_local Table t_redirected{};
thread_local const Table* t_table = nullptr;

inline const Table& Callees() {
    return t_table != nullptr ? *t_table : g_live;
}

// System V signatures of the callees (every integer argument passed as a full register, zero-extended where the
// original moves a 32-bit value: the registers the original's callees see are the same).
using F1 = SSPC_SYSV_ABI u64 (*)(u64);
using F2 = SSPC_SYSV_ABI u64 (*)(u64, u64);
using F3 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64);
using F4 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64);
using F5 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64);
using F6 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64);
using F7 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64, u64);
using F8 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64, u64, u64);
/// 0x11cde0: xmm0 = 16 bytes of the mesh at +0x38, xmm1 = its float at +0x44 broadcast; rdi rows, rsi camera,
/// edx flags, rcx the block
using FCamera = SSPC_SYSV_ABI void (*)(__m128, __m128, u64, u64, u64, u64);

template <typename F>
inline F Fn(const Table& t, Callee c) {
    return reinterpret_cast<F>(t.fn[c]);
}

template <typename T>
inline T At(u64 address) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(T));
    return v;
}
template <typename T>
inline void Put(u64 address, T v) {
    std::memcpy(reinterpret_cast<void*>(address), &v, sizeof(T));
}

inline __m128 Load(u64 p) {
    return _mm_loadu_ps(reinterpret_cast<const float*>(p));
}
inline void Store(u64 p, __m128 v) {
    _mm_storeu_ps(reinterpret_cast<float*>(p), v);
}
/// vsubps with the original's operand order (see object_constants.cpp: {vex3} keeps the written sources).
inline __m128 Sub(__m128 a, __m128 b) {
    __m128 r;
    __asm__("%{vex3%} vsubps {%2, %1, %0|%0, %1, %2}" : "=x"(r) : "x"(a), "x"(b));
    return r;
}
/// vpblendw zero, m, 0x3f: x, y, z kept, w = +0 (bit copy).
inline __m128 KeepXyz(__m128 v) {
    return _mm_castsi128_ps(_mm_blend_epi16(_mm_setzero_si128(), _mm_castps_si128(v), 0x3f));
}

// One row of object x the cascade's view-projection (0x43e1a4..0x43e280), the original's instructions and operand
// order; the schedules equal ObjectConstants' rows 0..3 (object_constants.cpp ProjectRow, R5-verified):
//   row 0: r = V0*x;  r = V1*y + r;  r = r + V2*z;  r = r + V3*w      (w = 0 after vpblendw)
//   row 1: t = x*V0;  r = V1*y + t;  r = r + V2*z;  r = r + V3*w
//   row 2: t = x*V0;  r = y*V1 + t;  r = r + V2*z;  r = r + V3*w
//   row 3: r = V1*y;  r = r + x*V0;  r = r + V2*z;  r = r + V3        (the object's translation row, w unused)
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

// --- SSPC_R7_SHADOWS_DEDUP: the instanced draw's state packets that repeat what the GPU already has ----------------
//
// 0x43d480 writes VGT_PRIMITIVE_TYPE (TriList), INDEX_TYPE (16-bit) and NUM_INSTANCES before every draw. Within one live
// call of the loop, every draw is on the same context's DCB; when everything written to that DCB since the previous
// shadow draw's last word (same chunk, the write pointer only advanced) holds no packet that sets those registers (or
// could reset them), the GPU still has the values that draw set: the packets are skipped. The scan decodes each PM4
// packet header in between (a few: the draw constants' commit, the CUE's user data, the draw and its NOP); anything it
// does not know means "write them". No other state, no draw, no order changes.
bool DedupOn() {
    static const bool on = [] {
        char v[8] = {};
        const DWORD n = GetEnvironmentVariableA("SSPC_R7_SHADOWS_DEDUP", v, sizeof(v));
        return n > 0 && n < sizeof(v) && v[0] != '0';
    }();
    return on;
}

struct DedupState {
    u64 loop_r = 0;     ///< the context of the live loop call running on this thread (0: none)
    bool valid = false; ///< a shadow draw of this call wrote its state
    u64 chunk = 0;      ///< the DCB chunk's start then
    u64 after = 0;      ///< the DCB write pointer after it
    u32 instances = 0;  ///< the NUM_INSTANCES it left
};
thread_local DedupState t_dedup;

constexpr u64 kDcbBegin = 0x0, kDcbCmdPtr = 0x10; // Gnm command buffer (R + 8)

} // namespace

u32 UntouchedState(u64 from, u64 to) {
    constexpr u64 kMaxScan = 4096 * 4;
    if (to < from || to - from > kMaxScan || ((to - from) & 3) != 0) {
        return 0;
    }
    u32 keep = 3;
    for (u64 p = from; p < to;) {
        const u32 header = At<u32>(p);
        if ((header >> 30) != 3) {
            return 0; // type 0/1/2: not expected here
        }
        const u32 count = (header >> 16) & 0x3fff;
        const u32 op = (header >> 8) & 0xff;
        const u64 bytes = (u64{count} + 2) * 4;
        if (p + bytes > to) {
            return 0;
        }
        switch (op) {
        case 0x2a: // INDEX_TYPE
            keep &= ~1u;
            break;
        case 0x2f: // NUM_INSTANCES
            keep &= ~2u;
            break;
        case 0x79: { // SET_UCONFIG_REG: VGT_PRIMITIVE_TYPE 0x242, VGT_INDEX_TYPE 0x243, VGT_NUM_INSTANCES 0x24d
            const u32 reg = At<u32>(p + 4) & 0xffff;
            const u32 n = count; // values after the offset dword
            const auto hits = [&](u32 r) { return r >= reg && r < reg + n; };
            if (hits(0x242) || hits(0x243)) {
                keep &= ~1u;
            }
            if (hits(0x24d)) {
                keep &= ~2u;
            }
            break;
        }
        case 0x10: // NOP
        case 0x76: // SET_SH_REG
        case 0x69: // SET_CONTEXT_REG
        case 0x27: // DRAW_INDEX_2
        case 0x2d: // DRAW_INDEX_AUTO
        case 0x37: // WRITE_DATA
        case 0x26: // INDEX_BASE
        case 0x13: // INDEX_BUFFER_SIZE
            break;
        default:
            return 0;
        }
        p += bytes;
    }
    return keep;
}

namespace {

/// The generator's draw passes the quality drop: (u32)r % 1025 (unsigned, as the original's multiply-shift) >= the
/// threshold (signed compare).
inline bool Kept(const Table& t, s32 threshold) {
    const u32 r = static_cast<u32>(Fn<F1>(t, kCPrng)(g_module + kPrngState));
    return static_cast<s32>(r % 0x401u) >= threshold;
}

// Scene (param_2[1]) offsets
constexpr u64 kSceneItems = 0x4db8;  ///< [0x9b7]: items, 0x80 bytes each
constexpr u64 kSceneKeys = 0x4dc0;   ///< [0x9b8]: sorted keys (u64; low 16 bits: the item)
constexpr u64 kSceneView = 0x44f0;   ///< [0x89e]: what the parameter writers read
// Context R offsets
constexpr u64 kRObjectBlock = 0xd190; ///< 0x140 bytes
constexpr u64 kRObjectDcb = 0xd2d0;   ///< {DCB copy, V#}
constexpr u64 kRObjectFlags = 0xd2e8; ///< u16: low byte changed, high byte bound before
constexpr u64 kRCameraBlock = 0xd2f0; ///< 0x80 bytes
constexpr u64 kRCameraDcb = 0xd370;
constexpr u64 kRCameraFlags = 0xd388;
constexpr u64 kRParams = 0xda18;

/// The block's DCB copy and its binding into both stages (0x43da91..0x43dc4c / 0x43dd30..0x43ddf7).
inline void PublishBlock(const Table& t, u64 r, u64 block, u64 dcb, u64 flags, u32 bytes, u32 slot) {
    const u16 f = At<u16>(r + flags);
    if (static_cast<u8>(f) != 0) {
        Fn<F3>(t, kCDcbAlloc)(r + dcb, r, bytes);
        const u64 dst = At<u64>(r + dcb);
        for (u32 i = 0; i < bytes; i += 16) {
            Store(dst + i, Load(r + block + i));
        }
    } else if (f < 0x100) {
        return;
    }
    Fn<F4>(t, kCBindConstants)(r + dcb, 2, slot, r);
    Fn<F4>(t, kCBindConstants)(r + dcb, 1, slot, r);
    Put<u16>(r + flags, 0);
}

} // namespace

bool Prepare(u64 module_base, std::string& error) {
    if (module_base == 0) {
        error = "no module";
        return false;
    }
    g_module = module_base;
    for (u32 i = 0; i < kCalleeCount; ++i) {
        g_live.fn[i] = module_base + kCalleeRva[i];
    }
    return true;
}

u64 ModuleBase() {
    return g_module;
}

int Redirects(const u64* from, const u64* to, u32 n) {
    if (n == 0) {
        t_table = nullptr;
        return 1;
    }
    t_redirected = g_live;
    for (u32 c = 0; c < kCalleeCount; ++c) {
        for (u32 i = 0; i < n; ++i) {
            if (from[i] == g_live.fn[c]) {
                t_redirected.fn[c] = to[i];
            }
        }
    }
    t_table = &t_redirected;
    return 1;
}

SSPC_SYSV_ABI void Draw(u64 pass, u64 r, u64 records, u64 n, u64 mesh) {
    const Table& t = Callees();
    Fn<F1>(t, kCCommitDrawConstants)(r);
    const u32 slot = At<u8>(pass + 0x121);
    alignas(16) u8 vsharp[16] = {};
    const u32 count = static_cast<u32>(n); // ECX: the instance count is 32-bit from here on
    Fn<F4>(t, kCInitBuffer)(reinterpret_cast<u64>(vsharp), records, 0x48, count);
    const u32 stage = static_cast<u32>(Fn<F1>(t, kCStage)(0));
    Fn<F5>(t, kCSetBuffers)(r + 0x128, stage, slot, 1, reinterpret_cast<u64>(vsharp));
    Fn<F5>(t, kCSetVertexBuffers)(r + 0x128, 2, 0, At<u8>(mesh + 0x6c), At<u64>(mesh + 0x60));
    // (live calls of a live loop call only: a verification's runs always make every call)
    DedupState& d = t_dedup;
    const bool dedup = t_table == nullptr && DedupOn() && d.loop_r == r;
    u32 keep = 0;
    if (dedup && d.valid && At<u64>(r + 8 + kDcbBegin) == d.chunk) {
        keep = UntouchedState(d.after, At<u64>(r + 8 + kDcbCmdPtr));
    }
    const bool skip_instances = (keep & 2) != 0 && d.instances == count;
    const bool skip_state = (keep & 1) != 0;
    if (!skip_instances) {
        Fn<F2>(t, kCNumInstances)(r + 8, count);
    }
    if (!skip_state) {
        Fn<F2>(t, kCPrimType)(r + 8, 4);
        Fn<F3>(t, kCIndexSize)(r + 8, 0, 2);
    }
    const u32 index_count = At<u32>(mesh + 0x68);
    const u64 index_base = At<u64>(mesh + 0x58);
    Fn<F1>(t, kCCueCommit)(r + 0x128);
    Fn<F4>(t, kCDrawIndex)(r + 8, index_count, index_base, 0);
    if (dedup) {
        d.valid = true;
        d.chunk = At<u64>(r + 8 + kDcbBegin);
        d.after = At<u64>(r + 8 + kDcbCmdPtr);
        d.instances = count;
        Stats::DrawState((skip_instances ? 0 : 1) + (skip_state ? 0 : 2), (skip_instances ? 1 : 0) + (skip_state ? 2 : 0));
    }
}

SSPC_SYSV_ABI void Loop(u64 pass, u64 args) {
    const Table& t = Callees();
    const u64 range = At<u64>(args + 0x18);
    const u64 scene = At<u64>(args + 8);
    const u64 r = At<u64>(args);
    const u64 view = At<u64>(scene);
    const bool other_view = (At<u8>(pass + 0x21) & 2) != 0;
    const __m128 offset = other_view ? Load(view + 0x420) : _mm_setzero_ps();
    const u64 vp_at = other_view ? view + 0x430 : view + 0x200;
    // the cascade's view-projection (rows V0..V3) and the camera block's 4 rows (the last: position - offset, w = 1)
    alignas(16) float vp[16];
    alignas(16) float camera[16];
    for (u32 i = 0; i < 4; ++i) {
        _mm_store_ps(vp + 4 * i, Load(vp_at + 16 * i));
    }
    for (u32 i = 0; i < 3; ++i) {
        _mm_store_ps(camera + 4 * i, Load(view + 0x6c0 + 16 * i));
    }
    const __m128 position = Sub(Load(view + 0x6f0), offset);
    _mm_store_ps(camera + 12, _mm_insert_ps(position, _mm_load_ss(reinterpret_cast<const float*>(g_module + kOne)), 0x30));
    const s32 quality = At<s32>(g_module + kQualityIndex);
    const s32 threshold = At<s32>(g_module + kQualityTable + static_cast<u64>(static_cast<s64>(quality)) * 0x14);
    const s32 lo = static_cast<s32>(range);
    const s32 hi = static_cast<s32>(range >> 32);
    // live calls only (a verification's recording runs have their callees redirected)
    const bool live = t_table == nullptr;
    Stats::Thread* const st = live ? Stats::Begin(g_module, r, At<u8>(pass + 0x1f) != 0, vp) : nullptr;
    struct DedupScope { // the draw-state dedup of this call's draws (DedupState)
        explicit DedupScope(u64 r_, bool on) : on(on) {
            if (on) {
                t_dedup = DedupState{r_, false, 0, 0, 0};
            }
        }
        ~DedupScope() {
            if (on) {
                t_dedup = DedupState{};
            }
        }
        bool on;
    } const dedup_scope(r, live && DedupOn());

    if (At<u8>(pass + 0x1f) == 0) {
        // --- one draw per object (0x43d75c..0x43de7b) ----------------------------------------------------------------
        if (lo >= hi) {
            return;
        }
        u64 previous = 0;
        u64 key = static_cast<u64>(static_cast<s64>(lo)) * 8;
        u32 left = static_cast<u32>(hi) - static_cast<u32>(lo);
        do {
            if (threshold != 0 && !Kept(t, threshold)) {
                key += 8;
                continue;
            }
            const u64 items = At<u64>(scene + kSceneItems);
            const u64 item = items + static_cast<u64>(At<u16>(At<u64>(scene + kSceneKeys) + key)) * 0x80;
            const u64 object = At<u64>(item + 0x18);
            const u64 mesh = At<u64>(object);
            if (At<u16>(mesh + 0x18) == 0) {
                key += 8;
                continue;
            }
            const u64 material = At<u64>(object + 8);
            const u64 buffers = At<u64>(item + 0x10);
            if (material != previous) {
                Fn<F3>(t, kCMaterialBind)(pass, r, material);
                previous = material;
                if (st != nullptr) {
                    Stats::MaterialBind(*st, r, material);
                }
            }
            const __m128 row1 = Load(item + 0x50), row0 = Load(item + 0x40), row2 = Load(item + 0x60);
            const __m128 row3 = Load(item + 0x70);
            if (At<u8>(pass + 0x120) != 0xff) {
                alignas(16) u8 vsharp[16] = {};
                Fn<F4>(t, kCInitBuffer)(reinterpret_cast<u64>(vsharp), At<u64>(buffers + 8), 0x30,
                                        static_cast<u32>(At<u16>(buffers + 0x54)) - 1u);
                alignas(16) u8 bound[16];
                std::memcpy(bound, vsharp, 16);
                Fn<F5>(t, kCSetBuffers)(r + 0x128, 2, At<u8>(pass + 0x120), 1, reinterpret_cast<u64>(bound));
            }
            const bool identity = (At<u8>(pass + 0x21) & 4) != 0;
            alignas(16) float rows[16];
            const auto object_rows = [&] {
                _mm_store_ps(rows + 0, identity ? _mm_load_ss(reinterpret_cast<const float*>(g_module + kOne)) : row0);
                _mm_store_ps(rows + 4, identity ? Load(g_module + kIdentityRows) : row1);
                _mm_store_ps(rows + 8, identity ? Load(g_module + kIdentityRows + 0x10) : row2);
                _mm_store_ps(rows + 12, identity ? Load(g_module + kIdentityRows + 0x20) : row3);
            };
            if (const s32 flags = At<s32>(pass + 0x118); flags != 0) {
                object_rows();
                alignas(32) float previous_rows[16] = {};
                Fn<F8>(t, kCObjectConstants)(reinterpret_cast<u64>(rows), reinterpret_cast<u64>(previous_rows),
                                             reinterpret_cast<u64>(vp), object, 0, 0xffff, static_cast<u32>(flags),
                                             r + kRObjectBlock);
                PublishBlock(t, r, kRObjectBlock, kRObjectDcb, kRObjectFlags, 0x140, At<u32>(g_module + kSlotObjectBlock));
            }
            if (const s32 flags = At<s32>(pass + 0x11c); flags != 0) {
                object_rows();
                reinterpret_cast<FCamera>(t.fn[kCCameraConstants])(Load(mesh + 0x38), _mm_set1_ps(At<float>(mesh + 0x44)),
                                                                    reinterpret_cast<u64>(rows),
                                                                    reinterpret_cast<u64>(camera),
                                                                    static_cast<u32>(flags), r + kRCameraBlock);
                PublishBlock(t, r, kRCameraBlock, kRCameraDcb, kRCameraFlags, 0x80, At<u32>(g_module + kSlotCameraBlock));
            }
            if (live && R7Shadows::Params::On()) { // S2: the census / check of 0x450460 (shadow_params.h)
                R7Shadows::Params::Call(t.fn[kCParams], pass + 0x90, r + kRParams, pass + 0x28, scene + kSceneView,
                                        At<u64>(material + 0x28), buffers, object);
            } else {
                Fn<F7>(t, kCParams)(pass + 0x90, r + kRParams, pass + 0x28, scene + kSceneView,
                                    At<u64>(material + 0x28), buffers, object);
            }
            Fn<F1>(t, kCCommitDrawConstants)(r);
            Fn<F3>(t, kCMeshDraw)(pass, r, item);
            if (st != nullptr) {
                Stats::SingleDraw(*st);
            }
            key += 8;
        } while (--left != 0);
        return;
    }

    // --- instancing groups (0x43d6d7..0x43e31b) ----------------------------------------------------------------------
    if (lo >= hi) {
        return;
    }
    const u64 keys0 = At<u64>(scene + kSceneKeys); // the group search reads the key array as of the call
    const s64 end64 = static_cast<s64>(range) >> 32;
    const __m128 v0 = _mm_load_ps(vp + 0), v1 = _mm_load_ps(vp + 4), v2 = _mm_load_ps(vp + 8), v3 = _mm_load_ps(vp + 12);
    u64 previous = 0;
    s32 start = lo;
    s32 end;
    do {
        // the group: the following keys equal to the first above bit 16 (the same mesh and material)
        const s64 s = start;
        const u64 first = At<u64>(keys0 + static_cast<u64>(s) * 8);
        s64 c = s;
        for (;;) {
            const s64 next = c + 1;
            if (next >= end64) {
                end = static_cast<s32>(static_cast<u32>(c) + 1u);
                break;
            }
            const u64 k = At<u64>(keys0 + static_cast<u64>(c) * 8 + 8);
            c = next;
            if ((k ^ first) >= 0x10000) {
                end = static_cast<s32>(next);
                break;
            }
        }
        const u64 records = Fn<F3>(t, kCFrameScratch)(g_module + kFrame,
                                                     static_cast<u32>(static_cast<u32>(end - start) * 0x48u), 0x10);
        if (records == 0) {
            start = end;
            continue;
        }
        const u16 first_item = At<u16>(At<u64>(scene + kSceneKeys) + static_cast<u64>(s) * 8);
        if (end <= start) {
            start = end;
            continue;
        }
        const u64 object0 = At<u64>(At<u64>(scene + kSceneItems) + static_cast<u64>(first_item) * 0x80 + 0x18);
        const u64 mesh = At<u64>(object0);
        const u64 material = At<u64>(object0 + 8);
        u64 count = 0;
        s64 i = s;
        s32 after; // the index after `i`
        for (;;) {
            // the next object kept (generator, then a mesh with indices: the group's mesh, the same for all)
            bool take = false;
            if (threshold != 0) {
                for (;;) {
                    after = static_cast<s32>(i + 1);
                    const bool kept = Kept(t, threshold);
                    if (kept && At<u16>(mesh + 0x18) != 0) {
                        take = true;
                        break;
                    }
                    i = i + 1;
                    if (after >= end) {
                        break;
                    }
                }
            } else {
                for (;;) {
                    after = static_cast<s32>(i) + 1;
                    if (At<u16>(mesh + 0x18) != 0) {
                        take = true;
                        break;
                    }
                    i = i + 1;
                    if (static_cast<s32>(i) >= end) {
                        break;
                    }
                }
            }
            if (!take) {
                break;
            }
            const u64 items = At<u64>(scene + kSceneItems);
            const u64 idx = At<u16>(At<u64>(scene + kSceneKeys) + static_cast<u64>(i) * 8);
            const u64 item = items + idx * 0x80;
            const __m128 a0 = KeepXyz(Load(item + 0x40));
            const __m128 a1 = KeepXyz(Load(item + 0x50));
            const __m128 a2 = KeepXyz(Load(item + 0x60));
            const __m128 a3 = Load(item + 0x70);
            const u64 object = At<u64>(item + 0x18);
            const u64 rec = records + count * 0x48;
            Store(rec + 0x00, ProjectRow<0>(a0, v0, v1, v2, v3));
            Store(rec + 0x10, ProjectRow<1>(a1, v0, v1, v2, v3));
            Store(rec + 0x20, ProjectRow<2>(a2, v0, v1, v2, v3));
            Store(rec + 0x30, ProjectRow<3>(a3, v0, v1, v2, v3));
            Put<u64>(rec + 0x40, 0);
            if (st != nullptr) {
                Stats::Record(*st, item);
            }
            if (At<s32>(pass + 0x94) > 0) {
                Fn<F6>(t, kCInstanceUv)(pass + 0x90, rec, pass + 0x28, scene + kSceneView, At<u64>(material + 0x28),
                                        object);
            }
            ++count;
            if (after >= end) {
                break;
            }
            i = after;
        }
        if (static_cast<u32>(count) != 0) {
            if (material != previous) {
                Fn<F3>(t, kCMaterialBind)(pass, r, material);
                previous = material;
                if (st != nullptr) {
                    Stats::MaterialBind(*st, r, material);
                }
            }
            if (At<s32>(pass + 0x90) > 0) {
                Fn<F6>(t, kCParamsInstanced)(pass + 0x90, r + kRParams, pass + 0x28, scene + kSceneView,
                                             At<u64>(material + 0x28), mesh);
            }
            Fn<F5>(t, kCInstancedDraw)(pass, r, records, count, mesh);
            if (st != nullptr) {
                Stats::Group(*st, count, end - start);
            }
        }
        start = end;
    } while (end < hi);
}

} // namespace SecondSon::NativeEngine::Reconstructed::ShadowCasters
