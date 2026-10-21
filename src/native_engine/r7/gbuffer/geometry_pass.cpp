// SecondSonPC R7-G — the native geometry pass (see geometry_pass.h).
#include "native_engine/r7/gbuffer/geometry_pass.h"

#include <cstring>

namespace SecondSon::NativeEngine::R7G {

namespace {

template <typename T>
inline T Ld(u64 a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}
template <typename T>
inline void St(u64 a, T v) {
    std::memcpy(reinterpret_cast<void*>(a), &v, sizeof(T));
}
inline __m128 L4(u64 a) {
    return _mm_loadu_ps(reinterpret_cast<const float*>(a));
}
inline void S4(void* a, __m128 v) {
    _mm_storeu_ps(static_cast<float*>(a), v);
}

// R fields
constexpr u64 kGfx = 0x8, kCue = 0x128, kObjBlock = 0xd190, kObjSlotBuf = 0xd2d0, kObjChanged = 0xd2e8,
              kBlock2 = 0xd2f0, kBlock2SlotBuf = 0xd370, kBlock2Changed = 0xd388, kDrawConst = 0xda18,
              kRenderControl = 0xde48, kRenderDirty = 0xde60, kStencil = 0xde64;

/// The per-call view state 0x1a2030 builds before its loop.
struct PassView {
    __m128 offset;   ///< view + 0x420 when M + 0x21 & 2, else 0 (the camera-relative origin)
    ViewRows view;   ///< rows of view + 0x430 (M + 0x21 & 2) or + 0x200
    __m128 cam[4];   ///< view + 0x6c0..0x6e0, + 0x6f0 - offset (w = 1)
};

void BuildView(u64 m, u64 view, const PassConstants& c, PassView& v) {
    const bool rel = (Ld<u8>(m + 0x21) & 2) != 0;
    v.offset = rel ? L4(view + 0x420) : _mm_setzero_ps();
    const u64 src = rel ? view + 0x430 : view + 0x200;
    for (u32 i = 0; i < 4; ++i) {
        v.view.r[i] = L4(src + 0x10 * i);
    }
    v.cam[0] = L4(view + 0x6c0);
    v.cam[1] = L4(view + 0x6d0);
    v.cam[2] = L4(view + 0x6e0);
    v.cam[3] = _mm_insert_ps(_mm_sub_ps(L4(view + 0x6f0), v.offset), _mm_set_ss(c.one), 0x30);
}

/// Identity rows as the loop writes them ({one, 0, 0, 0}, then the three constant rows).
inline void Identity(__m128* out, const PassConstants& c) {
    out[0] = _mm_set_ss(c.one);
    out[1] = c.ident1;
    out[2] = c.ident2;
    out[3] = c.ident3;
}

/// Constant-block upload after 0x11c890 / 0x11cde0: the block's changed u16 at `changed`: low byte -> a new buffer
/// (0x11be90) with the block copied in, then bound; high byte only -> bound again; then cleared.
template <u32 kBytes>
inline int UploadBlock(const PassFns& f, u64 r, u64 block, u64 slot_buf, u64 changed, u32 slot) {
    const u16 fl = Ld<u16>(r + changed);
    int what = 0;
    if (static_cast<u8>(fl) != 0) {
        f.upload(r + slot_buf, r, kBytes);
        std::memcpy(reinterpret_cast<void*>(Ld<u64>(r + slot_buf)), reinterpret_cast<const void*>(r + block), kBytes);
        what = 1;
    } else if (fl >= 0x100) {
        what = 2;
    } else {
        return 0;
    }
    f.bind(r + slot_buf, 2, slot, r);
    f.bind(r + slot_buf, 1, slot, r);
    St<u16>(r + changed, 0);
    return what;
}

/// Stencil cache (R + 0xde64) and DB_STENCILREFMASK (0x6229a0), as 0x1a1770 does it.
inline bool Stencil(const PassFns& f, u64 m, u64 r, u64 mesh) {
    const u16 st = Ld<u16>(m + 0x1c);
    const u8 lo = static_cast<u8>(st);
    if (lo == 0) {
        return false;
    }
    const u8 hi = static_cast<u8>((st >> 8) | Ld<u8>(mesh + 0x1d));
    const u32 v = (u32{lo} << 16) + (u32{hi} << 24) + 0xff00u;
    if (v == Ld<u32>(r + kStencil)) {
        return false;
    }
    u8* p = reinterpret_cast<u8*>(r + kStencil);
    p[0] = 0;
    p[1] = 0xff;
    p[2] = lo;
    p[3] = hi;
    f.stencil(r + kGfx, v);
    return true;
}

/// M + 0x124: the draw goes twice, the first time with render control bits 0..1 = 1 (0x635350), then restored.
template <typename Draw>
inline void DoubleDraw(const PassFns& f, u64 m, u64 r, Draw&& draw) {
    if (Ld<u8>(m + 0x124) != 0) {
        const u32 old = Ld<u32>(r + kRenderControl);
        const u32 t = (old & ~3u) | 1u; // 0x635350(&t, 1)
        St<u32>(r + kRenderControl, t);
        St<u8>(r + kRenderDirty, Ld<u8>(r + kRenderDirty) | 1);
        f.render_control(r + kGfx, t);
        draw();
        St<u32>(r + kRenderControl, old);
        St<u8>(r + kRenderDirty, Ld<u8>(r + kRenderDirty) | 1);
        f.render_control(r + kGfx, old);
    }
    draw();
}

inline bool Culled(const PassFns& f, u64 module, s32 dither) {
    if (dither == 0) {
        return false;
    }
    const u32 v = f.random(module + kRandomTable);
    return static_cast<s32>(v % 0x401u) < dither;
}

inline void Prefetch(u64 inst_base, u64 keys, s64 i, s64 hi, u32 ahead) {
    const s64 k = i + ahead;
    if (k < hi) {
        const u64 inst = inst_base + u64{Ld<u16>(keys + 8 * k)} * 0x80;
        _mm_prefetch(reinterpret_cast<const char*>(inst), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(inst + 0x40), _MM_HINT_T0);
    }
    const s64 k2 = i + ahead / 2;
    if (ahead >= 2 && k2 < hi) {
        const u64 inst = inst_base + u64{Ld<u16>(keys + 8 * k2)} * 0x80;
        const u64 mi = Ld<u64>(inst + 0x18);
        _mm_prefetch(reinterpret_cast<const char*>(mi), _MM_HINT_T0);
    }
}

/// The per-object variant (M + 0x1f == 0).
void PerObject(const PassFns& f, u64 module, const PassConstants& c, u64 m, u64 r, u64 s, s32 lo, s32 hi,
               s32 dither, const PassView& pv, const PassOptions& o, PassCounters* st) {
    alignas(32) u8 bounds[0x80] = {};
    f.bounds_init(reinterpret_cast<u64>(bounds));
    if (lo >= hi) {
        return;
    }
    const __m128 off3 = _mm_blend_ps(_mm_setzero_ps(), pv.offset, 7);
    const __m128 ox = _mm_permute_ps(off3, 0x00), oy = _mm_permute_ps(off3, 0x55), oz = _mm_permute_ps(off3, 0xaa);
    const u64 cue = r + kCue;
    u64 last_material = 0;
    u64 prev_world = 0;
    constexpr u32 kSeen = 8;
    u64 seen[kSeen] = {};
    u32 nseen = 0;
    alignas(16) __m128 view_rows[4];
    for (u32 i = 0; i < 4; ++i) {
        view_rows[i] = pv.view.r[i];
    }
    alignas(16) __m128 cam[4] = {pv.cam[0], pv.cam[1], pv.cam[2], pv.cam[3]};
    for (s64 i = lo; i < hi; ++i) {
        if (st) {
            st->Add(kPsObjects);
        }
        if (Culled(f, module, dither)) {
            if (st) {
                st->Add(kPsCulled);
            }
            continue;
        }
        const u64 inst_base = Ld<u64>(s + 0x4db8);
        const u64 keys = Ld<u64>(s + 0x4dc0);
        if (o.prefetch != 0) {
            Prefetch(inst_base, keys, i, hi, o.prefetch);
        }
        const u64 inst = inst_base + u64{Ld<u16>(keys + 8 * i)} * 0x80;
        const u64 mi = Ld<u64>(inst + 0x18);
        const u64 mesh = Ld<u64>(mi);
        if (Ld<u16>(mesh + 0x18) == 0) {
            if (st) {
                st->Add(kPsEmptyMesh);
            }
            continue;
        }
        const u64 md = Ld<u64>(inst + 0x10);
        const u64 mat = Ld<u64>(mi + 8);
        if (mat != last_material) {
            if (st) {
                st->Add(kPsMaterials);
                bool again = false;
                for (u32 k = 0; k < nseen && k < kSeen; ++k) {
                    again = again || seen[k] == mat;
                }
                st->Add(kPsMaterialRepeat, again);
                seen[nseen++ % kSeen] = mat;
            }
            f.material(m, r, mat);
            last_material = mat;
        }
        alignas(16) __m128 world[4];
        for (u32 k = 0; k < 4; ++k) {
            world[k] = L4(inst + 0x40 + 0x10 * k);
        }
        if (st) {
            st->Add(kPsSameWorld, prev_world != 0 && std::memcmp(reinterpret_cast<const void*>(prev_world),
                                                                 reinterpret_cast<const void*>(inst + 0x40), 0x40) == 0);
            prev_world = inst + 0x40;
        }
        alignas(16) __m128 light[4] = {_mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps(), _mm_setzero_ps()};
        const u32 value = Ld<u32>(inst + 0x2c);
        if (Ld<u8>(m + 0x135) != 0xff) {
            const s64 li = Ld<s32>(inst + 0x28);
            u64 lp = 0;
            if (li >= 0) {
                lp = Ld<u64>(module + kLightTable + 8 * li);
                light[0] = L4(lp + 0x130);
                light[1] = L4(lp + 0x140);
                light[2] = L4(lp + 0x150);
                __m128 x = _mm_mul_ps(light[0], ox);
                const __m128 y = _mm_mul_ps(light[1], oy);
                const __m128 z = _mm_mul_ps(light[2], oz);
                x = _mm_add_ps(x, L4(lp + 0x160));
                x = _mm_add_ps(x, y);
                x = _mm_add_ps(x, z);
                light[3] = _mm_insert_ps(x, _mm_set_ss(c.one), 0x30);
                if (st) {
                    st->Add(kPsLightRows);
                }
            }
            f.light(m + 0x134, r, 0, lp);
            if (st) {
                st->Add(kPsLight);
            }
        }
        if (Ld<u8>(m + 0x125) != 0xff) {
            const u32 bones = static_cast<u32>(Ld<u16>(md + 0x54)) - 1u;
            alignas(16) u8 vs[16] = {};
            f.init_buffer(reinterpret_cast<u64>(vs), Ld<u64>(md + 8), 0x30, bones);
            alignas(16) u8 v1[16];
            std::memcpy(v1, vs, 16);
            f.set_buffers(cue, 2, Ld<u8>(m + 0x125), 1, reinterpret_cast<u64>(v1));
            if (st) {
                st->Add(kPsSkin);
            }
            if (Ld<u8>(m + 0x126) != 0xff) {
                std::memset(vs, 0, 16);
                f.init_buffer(reinterpret_cast<u64>(vs), Ld<u64>(md + 0x10), 0x30, bones);
                alignas(16) u8 v2[16];
                std::memcpy(v2, vs, 16);
                f.set_buffers(cue, 2, Ld<u8>(m + 0x126), 1, reinterpret_cast<u64>(v2));
                if (st) {
                    st->Add(kPsSkin2);
                }
            }
        }
        const u8 flags21 = Ld<u8>(m + 0x21);
        alignas(16) __m128 prev[4];
        Identity(prev, c);
        if (Ld<s32>(m + 0x110) == 0x1d) {
            if (Ld<s32>(Ld<u64>(md + 0x88) + 0x138) < 0) {
                if (st) {
                    st->Add(kPsPass1dSkip);
                }
                continue;
            }
            alignas(16) __m128 out[4];
            f.pass1d(pv.offset, reinterpret_cast<u64>(out), md, mi, mesh);
            for (u32 k = 0; k < 4; ++k) {
                prev[k] = out[k];
            }
            if (st) {
                st->Add(kPsPass1d);
            }
            if ((Ld<u8>(Ld<u64>(m + 0xf0) + 0x12) & 8) != 0) {
                const u64 streams = Ld<u64>(mesh + 0x60);
                const u64 k16 = u64{Ld<u8>(mesh + 0x6e)} << 4;
                const s64 first = Ld<s8>(Ld<u64>(Ld<u64>(m + 0x10) + 0x58));
                f.set_buffers(cue, 2, Ld<u8>(m + 0x16d), 1, streams + static_cast<u64>(first) * 16);
                f.set_buffers(cue, 2, Ld<u8>(m + 0x16e), 1, streams + k16);
                f.set_buffers(cue, 2, Ld<u8>(m + 0x16f), 1, streams + k16 + 0x10);
                if (st) {
                    st->Add(kPsPass1dStreams);
                }
            }
        }
        const bool ident = (flags21 & 4) != 0;
        if (const s32 bits = Ld<s32>(m + 0x11c); bits != 0) {
            alignas(16) __m128 w[4];
            if (ident) {
                Identity(w, c);
            } else {
                for (u32 k = 0; k < 4; ++k) {
                    w[k] = world[k];
                }
            }
            f.object_constants(reinterpret_cast<u64>(w), reinterpret_cast<u64>(prev), reinterpret_cast<u64>(view_rows),
                               mi, reinterpret_cast<u64>(light), value, static_cast<u32>(bits), r + kObjBlock);
            const int up = UploadBlock<0x140>(f, r, kObjBlock, kObjSlotBuf, kObjChanged, Ld<u32>(module + kObjectSlot));
            if (st) {
                st->Add(kPsObjConst);
                st->Add(kPsObjUpload, up == 1);
                st->Add(kPsObjRebind, up == 2);
            }
        }
        if (const s32 v2 = Ld<s32>(m + 0x120); v2 != 0) {
            alignas(16) __m128 w[4];
            if (ident) {
                Identity(w, c);
            } else {
                for (u32 k = 0; k < 4; ++k) {
                    w[k] = world[k];
                }
            }
            f.constants2(L4(mesh + 0x38), _mm_set1_ps(Ld<float>(mesh + 0x44)), reinterpret_cast<u64>(w),
                         reinterpret_cast<u64>(cam), static_cast<u32>(v2), r + kBlock2);
            const int up =
                UploadBlock<0x80>(f, r, kBlock2, kBlock2SlotBuf, kBlock2Changed, Ld<u32>(module + kObjectSlot2));
            if (st) {
                st->Add(kPsConst2);
                st->Add(kPsConst2Upload, up == 1);
                st->Add(kPsConst2Rebind, up == 2);
            }
        }
        f.setup_446490(m + 0x140, m, r, md, mi);
        f.setup_445e00(pv.offset, m + 0x12c, m, r, reinterpret_cast<u64>(world), s + 0x44f0, md, mi,
                       reinterpret_cast<u64>(bounds));
        if (const u16 slot = Ld<u16>(m + 0x128); slot != 0xffff) {
            alignas(16) float k[4] = {static_cast<float>(static_cast<s32>(Ld<u8>(mi + 0x12))) * c.k64, 0, 0, 0};
            f.draw_constants(r + kDrawConst, slot, reinterpret_cast<u64>(k), 1);
            if (st) {
                st->Add(kPsDrawConst);
            }
        }
        if (Ld<s32>(m + 0x90) > 0) {
            f.constants_450460(m + 0x90, r + kDrawConst, m + 0x28, s + 0x44f0, Ld<u64>(mat + 0x28), md, mi);
            if (st) {
                st->Add(kPs450460);
            }
        }
        if (Ld<u8>(m + 0x144) != 0) {
            f.constants_1742f0(m + 0x144, r, md, r + kDrawConst);
            if (st) {
                st->Add(kPs1742f0);
            }
        }
        f.commit_constants(r);
        const bool sw = o.fault != 1 && Stencil(f, m, r, mesh);
        if (st) {
            st->Add(kPsStencil, sw);
            st->Add(kPsDouble, Ld<u8>(m + 0x124) != 0);
            st->Add(kPsDrawn);
        }
        DoubleDraw(f, m, r, [&] { f.mesh_draw(m, r, inst); });
    }
}

/// The instanced variant (M + 0x1f != 0).
void Instanced(const PassFns& f, u64 module, const PassConstants& c, u64 m, u64 r, u64 s, s32 lo, s32 hi, s32 dither,
               const PassView& pv, const PassOptions& o, PassCounters* st) {
    if (lo >= hi) {
        return;
    }
    const u64 keys0 = Ld<u64>(s + 0x4dc0);
    u64 last_material = 0;
    s32 cur = lo;
    while (cur < hi) {
        // the run of keys equal above bit 16
        const s32 start = cur;
        const u64 key0 = Ld<u64>(keys0 + 8 * static_cast<s64>(start));
        s64 end = static_cast<s64>(start) + 1;
        while (end < hi && (Ld<u64>(keys0 + 8 * end) ^ key0) < 0x10000) {
            ++end;
        }
        cur = static_cast<s32>(end);
        const u32 n = static_cast<u32>(cur - start);
        const u64 recs = f.frame_alloc(module + kFrameObject, n * kRecordBytes, 0x10);
        if (recs == 0) {
            if (st) {
                st->Add(kPsAllocFail);
            }
            continue;
        }
        const u64 keys = Ld<u64>(s + 0x4dc0);
        const u64 inst_base = Ld<u64>(s + 0x4db8);
        const u64 mi0 = Ld<u64>(inst_base + u64{Ld<u16>(keys + 8 * static_cast<s64>(start))} * 0x80 + 0x18);
        const u64 mesh = Ld<u64>(mi0);
        const u64 mat = Ld<u64>(mi0 + 8);
        u32 k = 0;
        for (s64 j = start; j < cur; ++j) {
            if (st) {
                st->Add(kPsObjects);
            }
            if (Culled(f, module, dither)) {
                if (st) {
                    st->Add(kPsCulled);
                }
                continue;
            }
            if (Ld<u16>(mesh + 0x18) == 0) {
                if (st) {
                    st->Add(kPsEmptyMesh);
                }
                continue;
            }
            if (o.prefetch != 0) {
                Prefetch(inst_base, keys, j, cur, o.prefetch);
            }
            const u64 inst = inst_base + u64{Ld<u16>(keys + 8 * j)} * 0x80;
            u8* rec = reinterpret_cast<u8*>(recs + u64{k} * kRecordBytes);
            BuildRecord(rec, inst, Ld<u64>(inst + 0x18), pv.view, c);
            if (o.fault == 2) {
                rec[0xac] ^= 1;
            }
            if (Ld<s32>(m + 0x94) > 0) {
                f.record_451440(m + 0x90, reinterpret_cast<u64>(rec), m + 0x28, s + 0x44f0, Ld<u64>(mat + 0x28));
                if (st) {
                    st->Add(kPs451440);
                }
            }
            ++k;
        }
        if (k == 0) {
            if (st) {
                st->Add(kPsGroupsEmpty);
            }
            continue;
        }
        if (mat != last_material) {
            if (st) {
                st->Add(kPsMaterials);
            }
            f.material(m, r, mat);
            last_material = mat;
        }
        if (Ld<s32>(m + 0x90) > 0) {
            f.group_451290(m + 0x90, r + kDrawConst, m + 0x28, s + 0x44f0, Ld<u64>(mat + 0x28), mesh);
            if (st) {
                st->Add(kPs451290);
            }
        }
        if (st) {
            st->Add(kPsGroups);
            st->Add(kPsDrawn, k);
            st->Add(kPsDouble, Ld<u8>(m + 0x124) != 0);
        }
        DoubleDraw(f, m, r, [&] { f.instanced_draw(m, r, recs, k, mesh); });
    }
}

} // namespace

void LoadPassConstants(u64 module, PassConstants& c) {
    c.newton_add = L4(module + kConstNewtonAdd);
    c.newton_mul = L4(module + kConstNewtonMul);
    c.sign = L4(module + kConstSign);
    c.ident1 = L4(module + kConstIdent1);
    c.ident2 = L4(module + kConstIdent2);
    c.ident3 = L4(module + kConstIdent3);
    c.one = Ld<float>(module + kConstOne);
    c.k64 = Ld<float>(module + kConstK64);
    c.minus_one = Ld<float>(module + kConstMinusOne);
}

void RunPass(const PassFns& f, u64 module, const PassConstants& c, u64 m, u64 chunk, const PassOptions& o,
             PassCounters* st) {
    const u64 r = Ld<u64>(chunk);
    const u64 s = Ld<u64>(chunk + 8);
    const u64 range = Ld<u64>(chunk + 0x18);
    const s32 lo = static_cast<s32>(range), hi = static_cast<s32>(range >> 32);
    PassView pv;
    BuildView(m, Ld<u64>(s), c, pv);
    const s32 idx = Ld<s32>(module + kFrameObject + kFrameDitherIndex);
    const s32 dither = Ld<s32>(module + kDitherTable + 0x10 + 0x14 * static_cast<s64>(idx));
    if (st) {
        st->Add(kPsCalls);
    }
    if (Ld<u8>(m + 0x1f) == 0) {
        PerObject(f, module, c, m, r, s, lo, hi, dither, pv, o, st);
    } else {
        if (st) {
            st->Add(kPsCallsInstanced);
        }
        Instanced(f, module, c, m, r, s, lo, hi, dither, pv, o, st);
    }
}

} // namespace SecondSon::NativeEngine::R7G
