// SecondSonPC — Native Render State (P5.3D; docs/native-reconstruction/NATIVE_RENDER_INTERFACE.md §6).
//
// What the render context's state methods do, computed from the engine's objects instead of written as PM4:
//   0x11f910 SetRenderTargets(R, rtSet, rect)  colour targets (0x52e0 -> 0x622ff0), depth target (0x6230e0), DB reg
//                                              0x2DE, CB_TARGET_MASK, viewport (0x11b60), window scissor, stale
//                                              colour targets switched off; R+0xDE68/+0xDE70/+0xDE80 updated
//   0x120020 TransitionTo(R, block)            only the block's registers that differ from R's (mask field 6, R's
//                                              valid bits +0xDE60), the stencil reference/mask, the target set when
//                                              it changed; R+0xDE48.. updated as it goes
//   0x11ffe0 SetStateBlock(R, block)           writes no PM4 (a copy into R+0xDE48..): nothing to replace
//   0x623740 setPrimitiveType / 0x624f80 setIndexSize: one register each
// The result is the register writes the game's packets would make, in their order, plus the target extents the
// driver's NOP hints carry (kStateCbExtent0.., kStateDbExtent), and `pm4_dwords` - the packet dwords the game's
// function writes for the same call (verify mode compares it with what the game wrote: a missed packet shows).
// The functions touch no command buffer. Pure helpers of the game's Gnm library are called for the targets' base
// addresses (tile-alignment, as 0x622ff0/0x6230e0 decide whether a target is bound).
#pragma once

#include "base/abi.h"
#include "engine/render/native_render_ops.h"

namespace SecondSon::Engine::Render::NativeState {

/// R+0xDE48..+0xDE84: the context's state block as SetStateBlock/TransitionTo/SetRenderTargets keep it.
struct StateBlock {
    u32 reg[6] = {};    ///< +0xDE48: PA_SU_SC_MODE_CNTL, PA_CL_CLIP_CNTL, DB_RENDER_CONTROL, PA_CL_VTE_CNTL,
                        ///< DB_DEPTH_CONTROL, DB_STENCIL_CONTROL
    u32 valid = 0;      ///< +0xDE60: bit per reg[] holding a written value
    u32 stencil = 0;    ///< +0xDE64: DB_STENCILREFMASK (and _BF)
    u64 targets = 0;    ///< +0xDE68: the render target set last bound (guest pointer)
    u32 rect[4] = {};   ///< +0xDE70: x0, y0, x1, y1
    u32 color_mask = 0; ///< +0xDE80: colour targets bound (bit per slot)
};
inline constexpr u64 kStateBlockOffset = 0xDE48;
inline constexpr u64 kStateBlockBytes = 0x3C;
static_assert(sizeof(StateBlock) >= kStateBlockBytes);
void LoadBlock(u64 r, StateBlock& out);
void StoreBlock(u64 r, const StateBlock& in);

/// The game's pure Gnm helpers (SysV), and data the methods read.
using TargetFn = SSPC_SYSV_ABI u32 (*)(const void* target);    ///< 0x637bf0, 0x637f30, 0x621450, 0x6219e0
using SetFmaskFn = SSPC_SYSV_ABI void (*)(void* target, u32 base); ///< 0x637e30
struct Env {
    TargetFn rt_base = nullptr;       ///< 0x637bf0 RenderTarget base (256-byte blocks, tile aligned); 0: not bound
    TargetFn rt_fmask_base = nullptr; ///< 0x637f30
    SetFmaskFn rt_set_fmask = nullptr; ///< 0x637e30
    TargetFn dt_z_base = nullptr;     ///< 0x621450 DepthRenderTarget Z base
    TargetFn dt_htile = nullptr;      ///< 0x6219e0 ... HTILE / stencil base
    u32 db_reg_2de = 0;               ///< eboot 0xaefb40[3]: the value 0x6272e0(3) writes to context reg 0x2DE
};

struct Writer {
    RegWrite* out = nullptr;
    u32 capacity = 0;
    u32 n = 0;
    u32 pm4_dwords = 0;
    bool overflow = false;
    void Reg(u32 reg, u32 value);
    /// One SET_*_REG packet of `count` consecutive registers from `first` (absolute index).
    void Packet(u32 first, const u32* values, u32 count);
    void One(u32 first, u32 value) { Packet(first, &value, 1); }
};

/// 0x11f910. `targets`/`rect` as the game passes them (either may be 0). `state` is R's block, updated.
void SetRenderTargets(StateBlock& state, u64 targets, const u32* rect, const Env& env, Writer& w);
/// 0x120020 with `block` (guest pointer to the 0x38-byte state block plus the rectangle at +0x28).
void TransitionTo(StateBlock& state, u64 block, const Env& env, Writer& w);
/// 0x623740 / 0x624f80.
void SetPrimitiveType(u32 prim, Writer& w);
void SetIndexSize(u32 index_size, u32 cache_policy, Writer& w);

/// The viewport 0x11b60 writes (zscale 1, zoffset 0 as SetRenderTargets calls it), separately for tests.
void Viewport(u32 x0, u32 y0, u32 x1, u32 y1, Writer& w);

// --- the Gnm command buffer's one-packet state setters, as the game's material and mesh code calls them directly ---
// (the blend, depth/stencil, rasterizer, depth bias and instance state of a draw; RENDER_ENGINE_ARCHITECTURE.md §4.3)

enum class Leaf : u8 {
    Masked,       ///< (dcb, v): context reg = v & mask
    Pair,         ///< (dcb, v): context reg, reg + 1 = v (DB_STENCILREFMASK and _BF)
    ModeCntl1,    ///< (dcb, v): PA_SC_MODE_CNTL_1 = (v & 1) << 16 | 0x6020000
    Blend,        ///< (dcb, slot, v): CB_BLEND<slot>_CONTROL = v & 0xffff1fff
    ColorControl, ///< (dcb, mode, rop): CB_COLOR_CONTROL = (mode & 7) << 4 | (rop & 0xff) << 16
    FloatPair,    ///< (x, y, dcb): context reg, reg + 1 = the floats' bits (PA_SU_POLY_OFFSET_*)
    NumInstances, ///< (dcb, n): the NUM_INSTANCES packet -> VGT_NUM_INSTANCES
    Generic,      ///< (dcb, reg, v): context reg `reg` (absolute) = v
};
struct LeafSpec {
    u64 rva;
    u32 size;
    u64 hash;
    u32 prologue; ///< bytes the entry probe moves (whole instructions, no RIP-relative operand, no jump into them)
    Leaf kind;
    u32 reg;      ///< context register offset from 0xA000 (Masked, Pair, FloatPair)
    u32 mask;
};
// eboot.bin of CUSA00004 (tools/native-reconstruction/function_hashes.py, queries/prologue.py).
inline constexpr LeafSpec kLeaves[] = {
    {0x6270e0, 0x5d, 0x9b42043ce2d534f5ull, 17, Leaf::Masked, 0x205, 0x393FFF},   // PA_SU_SC_MODE_CNTL
    {0x626e10, 0x5d, 0x56f28569ee5a1994ull, 17, Leaf::Masked, 0x204, 0xF7FE03F},  // PA_CL_CLIP_CNTL
    {0x626c60, 0x5d, 0x330f3b5d7dfcad25ull, 17, Leaf::Masked, 0x000, 0x1FFF},     // DB_RENDER_CONTROL
    {0x626a80, 0x5d, 0x89142acdfcab3371ull, 17, Leaf::Masked, 0x206, 0xF3F},      // PA_CL_VTE_CNTL
    {0x622bb0, 0x5d, 0xb09863b8c3f33d43ull, 17, Leaf::Masked, 0x200, 0xC07007FF}, // DB_DEPTH_CONTROL
    {0x622c10, 0x5d, 0x63d9d7df8cf0a3a6ull, 17, Leaf::Masked, 0x10B, 0xFFFFFF},   // DB_STENCIL_CONTROL
    {0x6227e0, 0x56, 0x3c090973a7f597bcull, 17, Leaf::Masked, 0x08E, 0xFFFFFFFF}, // CB_TARGET_MASK
    {0x6229a0, 0x57, 0x6f3e2fe841518655ull, 17, Leaf::Pair, 0x10C, 0},            // DB_STENCILREFMASK(_BF)
    {0x626cc0, 0x61, 0xb6ff9c7e23acb4a8ull, 16, Leaf::ModeCntl1, 0x293, 0},       // PA_SC_MODE_CNTL_1
    {0x6228a0, 0x6d, 0xae27446f9a7a2a2full, 16, Leaf::Blend, 0x1E0, 0},           // CB_BLEND0..7_CONTROL
    {0x622b30, 0x75, 0xa039615b1645a8c1ull, 16, Leaf::ColorControl, 0x202, 0},    // CB_COLOR_CONTROL
    {0x627350, 0x6b, 0x8334cfbfbe49e893ull, 17, Leaf::FloatPair, 0x2E0, 0},       // PA_SU_POLY_OFFSET_FRONT_*
    {0x6273c0, 0x6b, 0x7353b1aa5ef35eedull, 17, Leaf::FloatPair, 0x2E2, 0},       // PA_SU_POLY_OFFSET_BACK_*
    {0x6250b0, 0x4c, 0x725bf87e9748a749ull, 17, Leaf::NumInstances, 0, 0},        // NUM_INSTANCES
    {0x6221a0, 0x1f, 0x4c281509b571c50dull, 16, Leaf::Generic, 0, 0},             // setContextReg (DB_RENDER_OVERRIDE)
};
inline constexpr u32 kLeafCount = sizeof(kLeaves) / sizeof(kLeaves[0]);

/// What leaf `spec` writes for its arguments after the command buffer (`a`, `b`; FloatPair: the floats' bits).
void LeafWrite(const LeafSpec& spec, u32 a, u32 b, Writer& w);

} // namespace SecondSon::Engine::Render::NativeState
