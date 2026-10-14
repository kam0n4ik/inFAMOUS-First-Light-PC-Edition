// SecondSonPC — Native Render State (see native_state.h).
#include "engine/render/native_state.h"

#include <cmath>
#include <cstddef>
#include <cstring>

#include <xmmintrin.h>

// The viewport and scissor arithmetic repeats the game's SSE instructions one by one (0x11b60, 0x11f910): no
// contraction into fused multiply-adds, which would round differently.
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace SecondSon::Engine::Render::NativeState {

namespace {

constexpr u32 kCtx = 0xA000; ///< context register space (SET_CONTEXT_REG offset 0)

static_assert(offsetof(StateBlock, valid) == 0x18 && offsetof(StateBlock, stencil) == 0x1C &&
              offsetof(StateBlock, targets) == 0x20 && offsetof(StateBlock, rect) == 0x28 &&
              offsetof(StateBlock, color_mask) == 0x38);

template <typename T>
T At(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

u32 Bits(float f) {
    u32 u;
    std::memcpy(&u, &f, 4);
    return u;
}

float Float(u32 u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

/// MINSS: the first operand when it is smaller, otherwise (also when unordered) the second.
float MinSs(float a, float b) {
    return a < b ? a : b;
}

s32 Cvtt32(float f) {
    return _mm_cvtt_ss2si(_mm_set_ss(f));
}

s64 Cvtt64(float f) {
    return _mm_cvttss_si64(_mm_set_ss(f));
}

/// VCVTSI2SS of a 64-bit register holding a zero-extended u32.
float FromU32(u32 v) {
    return static_cast<float>(static_cast<s64>(v));
}

/// 0x635860 (0x43F) then 0x635870(.., 0): PA_CL_VTE_CNTL with the viewport transform on.
u32 VteControl() {
    const u32 v = 0x43F;
    const u32 e = 0 ^ 1;
    return (e << 10 | (v & 0xFFFFFAC0u) | e << 5 | e * 8 | e << 8 | ((e * 5 | e << 4) + e * 2)) ^ 0x100;
}

/// 0x622ff0 (through 0x52e0 for a bound target): colour target `slot` from the 16-dword RenderTarget at `rt`.
void ColorTarget(u32 slot, u64 rt, const Env& env, Writer& w) {
    const u32 info_reg = kCtx + 0x318 + slot * 15;
    if (rt == 0) {
        w.One(info_reg + 4, 0); // CB_COLORn_INFO = 0: the target is off
        return;
    }
    u32 t[16];
    std::memcpy(t, reinterpret_cast<const void*>(rt), sizeof(t));
    // 0x52e0: a target with FMASK compression but no FMASK of its own gets one derived from the colour surface.
    if ((t[4] & 0x6000) == 0x2000 && env.rt_fmask_base(t) == 0 && t[7] != 0) {
        t[4] &= ~0x4000u;
        t[5] = (t[5] & 0x1F) << 5 | (t[5] & 0xFFFFFC1Fu);
        t[10] = (t[10] & 0xFFC00000u) | (t[2] & 0x3FFFFFu);
        t[1] = (t[1] & 0x7FF) << 20 | (t[1] & 0x800FFFFFu);
        env.rt_set_fmask(t, env.rt_base(t));
    }
    if (env.rt_base(t) == 0) {
        w.One(info_reg + 4, 0);
        return;
    }
    w.Packet(info_reg, t, 14);            // CB_COLORn_BASE .. CB_COLORn_FMASK_SLICE
    w.Reg(kStateCbExtent0 + slot, t[15]); // the NOP hint after it
    w.pm4_dwords += 2;
}

/// 0x6230e0: the depth target at `dt` (the 13-dword DepthRenderTarget) or none.
void DepthTarget(u64 dt, const Env& env, Writer& w) {
    if (dt != 0) {
        u32 d[13];
        std::memcpy(d, reinterpret_cast<const void*>(dt), sizeof(d));
        if (env.dt_z_base(d) != 0 || env.dt_htile(d) != 0) {
            w.Packet(kCtx + 0x010, d, 8); // DB_Z_INFO .. DB_DEPTH_SLICE
            w.One(kCtx + 0x00F, d[11]);   // DB_DEPTH_INFO
            w.One(kCtx + 0x002, d[8]);    // DB_DEPTH_VIEW
            w.One(kCtx + 0x005, d[9]);    // DB_HTILE_DATA_BASE
            w.One(kCtx + 0x2AF, d[10]);   // DB_HTILE_SURFACE
            w.Reg(kStateDbExtent, d[12]);
            w.pm4_dwords += 2;
            return;
        }
    }
    w.One(kCtx + 0x010, 0); // DB_Z_INFO, DB_STENCIL_INFO: no depth target
    w.One(kCtx + 0x011, 0);
    w.Reg(kStateDbExtent, 0);
}

/// 0x11f910's window scissor from the target set's normalised rectangle (+0x14 .. +0x20) over `rc`.
void WindowScissor(u64 targets, const u32* rc, Writer& w) {
    const float half = 0.5f;
    const float width = FromU32(rc[2] - rc[0]);
    const float height = FromU32(rc[3] - rc[1]);
    const auto x = [&](u32 at) {
        float v = half * At<float>(targets + at);
        v = v + half;
        v = v * width;
        v = v + half;
        return static_cast<u32>(Cvtt64(v)) + rc[0];
    };
    const auto y = [&](u32 at) {
        float v = half * At<float>(targets + at);
        v = half - v;
        v = v * height;
        v = v + half;
        return static_cast<u32>(Cvtt64(v)) + rc[1];
    };
    const u32 left = x(0x14), top = y(0x20), right = x(0x1C), bottom = y(0x18);
    const u32 values[2] = {1u << 31 | (top & 0x7FFF) << 16 | (left & 0x7FFF), (bottom & 0x7FFF) << 16 | (right & 0x7FFF)};
    w.Packet(kCtx + 0x081, values, 2); // PA_SC_WINDOW_SCISSOR_TL/BR (WINDOW_OFFSET_DISABLE)
}

} // namespace

void Writer::Reg(u32 reg, u32 value) {
    if (n < capacity) {
        out[n++] = RegWrite{reg, value};
    } else {
        overflow = true;
    }
}

void Writer::Packet(u32 first, const u32* values, u32 count) {
    for (u32 i = 0; i < count; ++i) {
        Reg(first + i, values[i]);
    }
    pm4_dwords += 2 + count;
}

void LoadBlock(u64 r, StateBlock& out) {
    std::memcpy(&out, reinterpret_cast<const void*>(r + kStateBlockOffset), kStateBlockBytes);
}

void StoreBlock(u64 r, const StateBlock& in) {
    std::memcpy(reinterpret_cast<void*>(r + kStateBlockOffset), &in, kStateBlockBytes);
}

void Viewport(u32 x0, u32 y0, u32 x1, u32 y1, Writer& w) {
    const float half = 0.5f, neg_half = -0.5f;
    const float fw = static_cast<float>(static_cast<s32>(x1 - x0));
    const float fh = static_cast<float>(static_cast<s32>(y1 - y0));
    const float xs = fw * half;
    const float ys = fh * neg_half;
    const float xo = xs + FromU32(x0);
    const float yo = fh * half + FromU32(y0);
    const u32 z[2] = {Bits(0.0f), Bits(1.0f)};
    w.Packet(kCtx + 0x0B4, z, 2); // PA_SC_VPORT_ZMIN_0 / ZMAX_0 (0x6266f0)
    const u32 xf[6] = {Bits(xs), Bits(xo), Bits(ys), Bits(yo), Bits(1.0f), Bits(0.0f)};
    w.Packet(kCtx + 0x10F, xf, 6);             // PA_CL_VPORT_XSCALE .. ZOFFSET
    w.One(kCtx + 0x206, VteControl() & 0xF3F); // PA_CL_VTE_CNTL (0x626a80)
    const u32 screen[2] = {y0 << 16 | (x0 & 0xFFFF), y1 << 16 | (x1 & 0xFFFF)};
    w.Packet(kCtx + 0x00C, screen, 2); // PA_SC_SCREEN_SCISSOR_TL/BR (0x626890)
    const float sixteenth = 0.0625f;
    const auto offset = [&](float o) {
        const s32 v = Cvtt32(std::floor(o * sixteenth + half));
        return v >= 0x1FC ? 0x1FCu : static_cast<u32>(v) & ~3u;
    };
    const u32 hx = offset(xo), hy = offset(yo);
    w.One(kCtx + 0x08D, (hy & 0x1FC) << 16 | (hx & 0x1FC)); // PA_SU_HARDWARE_SCREEN_OFFSET (0x622e60)
    // Guard band (0x622ee0): how far the clip space reaches before the 16.8 fixed-point screen space ends.
    const float big = 32768.0f, big_less = Float(0x46FFFFFEu);
    const float axs = std::fabs(xs), ays = std::fabs(ys);
    const float ox = static_cast<float>(static_cast<s32>(hx << 4));
    const float oy = static_cast<float>(static_cast<s32>(hy << 4));
    float a = big - axs;
    a = a - ox;
    a = a + xo;
    float b = big_less - axs;
    b = b + ox;
    b = b - xo;
    const float x_adj = MinSs(a, b) / axs;
    float c = oy + big_less;
    c = c - ays;
    c = c - yo;
    float d = big - oy;
    d = d - ays;
    d = d + yo;
    const float y_adj = MinSs(d, c) / ays;
    const u32 gb[4] = {Bits(y_adj), Bits(1.0f), Bits(x_adj), Bits(1.0f)};
    w.Packet(kCtx + 0x2FA, gb, 4); // PA_CL_GB_VERT_CLIP_ADJ, VERT_DISC, HORZ_CLIP, HORZ_DISC
}

void SetRenderTargets(StateBlock& s, u64 targets, const u32* rect, const Env& env, Writer& w) {
    u32 rc[4] = {};
    if (rect != nullptr) {
        std::memcpy(rc, rect, sizeof(rc));
    } else if (targets != 0) {
        rc[2] = At<u32>(targets); // the set's width and height
        rc[3] = At<u32>(targets + 4);
    }
    if (targets == 0) {
        for (u32 i = 0, m = s.color_mask; m != 0; ++i, m >>= 1) {
            if ((m & 1) != 0) {
                ColorTarget(i, 0, env, w);
            }
        }
        s.color_mask = 0;
        DepthTarget(0, env, w);
    } else {
        const s32 count = At<s32>(targets + 0x77C);
        const s32 depth = At<s32>(targets + 0x784);
        const u32 bound = 1u << (static_cast<u32>(count) & 31);
        for (s32 i = 0; i < count; ++i) {
            ColorTarget(static_cast<u32>(i), targets + 0x28 + static_cast<u64>(i) * 0xD0, env, w);
        }
        if (depth != -1) {
            DepthTarget(targets + static_cast<u64>(static_cast<s64>(depth) * 0xD0) + 0x74, env, w);
            w.One(kCtx + 0x2DE, env.db_reg_2de); // 0x6272e0(3)
        }
        w.One(kCtx + 0x08E, At<u32>(targets + 0x24)); // CB_TARGET_MASK (0x6227e0)
        Viewport(rc[0], rc[1], rc[2], rc[3], w);
        WindowScissor(targets, rc, w);
        for (u32 i = 0, m = (0u - bound) & s.color_mask; m != 0; ++i, m >>= 1) {
            if ((m & 1) != 0) {
                ColorTarget(i, 0, env, w); // targets of the previous set this one does not have
            }
        }
        s.color_mask = bound - 1;
        if (depth < 0) {
            DepthTarget(0, env, w);
        }
    }
    s.targets = targets;
    std::memcpy(s.rect, rc, sizeof(rc));
}

void TransitionTo(StateBlock& s, u64 block, const Env& env, Writer& w) {
    static constexpr u32 kRegs[6] = {0x205, 0x204, 0x000, 0x206, 0x200, 0x10B};
    static constexpr u32 kMasks[6] = {0x393FFF, 0xF7FE03F, 0x1FFF, 0xF3F, 0xC07007FF, 0xFFFFFF};
    for (u32 i = 0; i < 6; ++i) {
        const u32 bit = 1u << i;
        const u32 v = At<u32>(block + i * 4);
        if ((At<u32>(block + 0x18) & bit) != 0 && ((s.valid & bit) == 0 || s.reg[i] != v)) {
            s.reg[i] = v;
            s.valid |= bit;
            w.One(kCtx + kRegs[i], v & kMasks[i]);
        }
    }
    const u32 stencil = At<u32>(block + 0x1C);
    if (s.stencil != stencil) {
        s.stencil = stencil;
        const u32 both[2] = {stencil, stencil};
        w.Packet(kCtx + 0x10C, both, 2); // DB_STENCILREFMASK, _BF (0x6229a0)
    }
    const u64 targets = At<u64>(block + 0x20);
    if (targets != 0 && targets != s.targets) {
        SetRenderTargets(s, targets, reinterpret_cast<const u32*>(block + 0x28), env, w);
    }
}

void SetPrimitiveType(u32 prim, Writer& w) {
    w.One(0xC242, prim & 0x3F); // VGT_PRIMITIVE_TYPE (SET_UCONFIG_REG; the Neo index field is not a value)
}

void LeafWrite(const LeafSpec& spec, u32 a, u32 b, Writer& w) {
    switch (spec.kind) {
    case Leaf::Masked:
        w.One(kCtx + spec.reg, a & spec.mask);
        break;
    case Leaf::Pair: {
        const u32 both[2] = {a, a};
        w.Packet(kCtx + spec.reg, both, 2);
        break;
    }
    case Leaf::ModeCntl1:
        w.One(kCtx + spec.reg, (a & 1) << 16 | 0x6020000);
        break;
    case Leaf::Blend:
        w.One(kCtx + spec.reg + a, b & 0xFFFF1FFFu);
        break;
    case Leaf::ColorControl:
        w.One(kCtx + spec.reg, (a & 7) << 4 | (b & 0xFF) << 16);
        break;
    case Leaf::FloatPair: {
        const u32 both[2] = {a, b};
        w.Packet(kCtx + spec.reg, both, 2);
        break;
    }
    case Leaf::NumInstances:
        w.Reg(0xC24D, a); // VGT_NUM_INSTANCES
        w.pm4_dwords += 2;
        break;
    case Leaf::Generic:
        if (a < kCtx || a >= kCtx + 0x400) {
            w.overflow = true; // not a context register: nothing native (the caller runs the game's setter)
            break;
        }
        w.One(a, b);
        break;
    }
}

void SetIndexSize(u32 index_size, u32 cache_policy, Writer& w) {
    const u32 policy = cache_policy != 2 ? (cache_policy & 3) << 6 : 0u;
    w.Reg(0xA29F, u32{cache_policy != 2} << 10 | policy | (index_size & 0xFFFFFB3Fu)); // INDEX_TYPE packet
    w.pm4_dwords += 2;
}

} // namespace SecondSon::Engine::Render::NativeState
