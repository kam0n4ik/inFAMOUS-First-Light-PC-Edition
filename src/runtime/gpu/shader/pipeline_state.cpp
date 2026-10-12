// SecondSonPC — pipeline state and its cache (P4.3). See pipeline_state.h.
#include "runtime/gpu/shader/pipeline_state.h"

#include <cstring>

#include <xxhash.h>

#include "runtime/gpu/image_layout.h"
#include "runtime/gpu/shader/runtime_params.h"
#include "runtime/gpu/shader/shader_types.h"

namespace SecondSon::Gpu::Shader {

namespace {

constexpr u32 kDbRenderOverride = 0xA003;
constexpr u32 kDbZInfo = 0xA010;
constexpr u32 kDbStencilInfo = 0xA011;
constexpr u32 kDbZReadBase = 0xA012;
constexpr u32 kDbStencilReadBase = 0xA013;
constexpr u32 kCbShaderMask = 0xA08F;
constexpr u32 kPaSuScModeCntl = 0xA205;
constexpr u32 kCbColorAttribOffset = 5;

} // namespace

u32 ImageWriteMask(u32 cb_info, u32 export_mask) {
    // the translated program writes image component i from export component swizzle.Map(i) (COMP_SWAP; the
    // recompiler's MRT export), and CB_TARGET_MASK enables export components: image component i is written when its
    // export component is. For the 4-component swaps but ALT_REV (involutions) this is the bridge's
    // swizzle.ApplyMask(mask). Raw, an ALT target's mask 0x3 wrote R and G instead of G and B (P4.5: the G-buffer
    // normal's R zeroed by a later full-screen pass)
    CompMapping swizzle;
    ColorTargetSwizzle(cb_info, swizzle);
    u32 out = 0;
    for (u32 i = 0; i < 4; ++i) {
        out |= ((export_mask >> swizzle.Map(i)) & 1) << i;
    }
    return out;
}

void BuildGraphicsPipelineState(const Cp::RegisterFile& regs, u32 vs_module, u32 ps_module, u32 mrt_mask,
                                GraphicsPipelineState& out) {
    out = {};
    out.vs_module = vs_module;
    out.ps_module = ps_module;
    out.prim_type = regs.Get(Reg::VgtPrimitiveType);
    const u32 mode = regs.Get(kPaSuScModeCntl);
    out.poly_mode = ((mode >> 3) & 3) != 0 ? (mode >> 5) & 7 : 2;
    out.provoking_last = (mode >> 19) & 1;
    const u32 clip = regs.Get(Reg::PaClClipCntl);
    out.clip_space = (clip >> 19) & 1;
    const u32 near_off = (clip >> 26) & 1, far_off = (clip >> 27) & 1;
    out.depth_clip = (near_off == far_off && near_off == 0) ? 1 : 0;
    out.depth_clamp = ((regs.Get(kDbRenderOverride) >> 16) & 1) ? 0 : 1;
    const u32 z_info = regs.Get(kDbZInfo);
    const bool depth_valid = regs.Get(kDbZReadBase) != 0 && (z_info & 3) != 0;
    const bool stencil_valid = regs.Get(kDbStencilReadBase) != 0 && (regs.Get(kDbStencilInfo) & 1) != 0;
    out.z_format = depth_valid ? z_info & 3 : 0;
    out.stencil_format = stencil_valid ? 1 : 0;
    out.depth_samples_log2 = (depth_valid || stencil_valid) ? (z_info >> 2) & 3 : 0;
    const u32 color_control = regs.Get(Reg::CbColorControl);
    out.logic_op = (color_control >> 16) & 0xFF;
    out.color_mode = (color_control >> 4) & 7;
    out.cb_shader_mask = regs.Get(kCbShaderMask);
    out.mrt_mask = mrt_mask & 0xFF;
    if (out.color_mode == 0) {
        return; // colour writes disabled: no target is bound
    }
    const u32 target_mask = regs.Get(Reg::CbTargetMask);
    const u32 col_format = regs.Get(Reg::SpiShaderColFormat);
    for (u32 cb = 0; cb < 8; ++cb) {
        const u32 base = regs.Get(Reg::CbColor0Base + cb * Reg::CbColorStride);
        const u32 info = regs.Get(Reg::CbColor0Base + cb * Reg::CbColorStride + Reg::CbColorInfoOffset);
        const u32 write = (target_mask >> (cb * 4)) & 0xF;
        if (base == 0 || ((info >> 2) & 0x1F) == 0 || write == 0 || ((out.mrt_mask >> cb) & 1) == 0) {
            continue; // unbound, writes off, or not exported by the fragment program
        }
        ColorTargetState& t = out.targets[cb];
        t.info = info & kColorInfoMask;
        t.export_format = (col_format >> (cb * 4)) & 0xF;
        const u32 blend = regs.Get(Reg::CbBlend0Control + cb);
        t.blend = ((blend >> 30) & 1) != 0 && ((info >> 16) & 1) == 0 ? blend : 0;
        t.write_mask = ImageWriteMask(info, write);
        t.samples_log2 =
            (regs.Get(Reg::CbColor0Base + cb * Reg::CbColorStride + kCbColorAttribOffset) >> 15) & 3;
    }
}

Cp::GroupMask PipelineStateGroups() {
    static const Cp::GroupMask mask = [] {
        Cp::GroupMask m = 0;
        auto add = [&](u32 reg) { m |= Cp::Bit(Cp::GroupOf(reg)); };
        for (const u32 r : {Reg::VgtPrimitiveType, kPaSuScModeCntl, Reg::PaClClipCntl, kDbRenderOverride, kDbZInfo,
                            kDbStencilInfo, kDbZReadBase, kDbStencilReadBase, Reg::CbColorControl, kCbShaderMask,
                            Reg::CbTargetMask, Reg::SpiShaderColFormat}) {
            add(r);
        }
        for (u32 cb = 0; cb < 8; ++cb) {
            add(Reg::CbBlend0Control + cb);
            add(Reg::CbColor0Base + cb * Reg::CbColorStride);
            add(Reg::CbColor0Base + cb * Reg::CbColorStride + Reg::CbColorInfoOffset);
            add(Reg::CbColor0Base + cb * Reg::CbColorStride + kCbColorAttribOffset);
        }
        return m;
    }();
    return mask;
}

u64 PipelineStateCache::Hash(const void* data, size_t bytes) const {
    return test_hash_ != nullptr ? test_hash_(data, bytes) : XXH3_64bits(data, bytes);
}

u32 PipelineStateCache::GetGraphics(const GraphicsPipelineState& state, bool& created) {
    ++stats_.lookups;
    const u64 h = Hash(&state, sizeof(state));
    auto [first, last] = index_.equal_range(h);
    bool collided = false;
    for (auto it = first; it != last; ++it) {
        const Entry& e = entries_[it->second - 1];
        if (!e.compute && std::memcmp(&e.state, &state, sizeof(state)) == 0) {
            ++stats_.hits;
            created = false;
            return it->second;
        }
        collided = true;
    }
    stats_.collisions += collided ? 1 : 0;
    ++stats_.misses;
    ++stats_.graphics;
    entries_.push_back({false, state, 0});
    const u32 id = static_cast<u32>(entries_.size());
    index_.emplace(h, id);
    created = true;
    return id;
}

u32 PipelineStateCache::GetCompute(u32 cs_module, bool& created) {
    ++stats_.lookups;
    const u64 h = Hash(&cs_module, sizeof(cs_module)) ^ 0xC0C0C0C0C0C0C0C0ull;
    auto [first, last] = index_.equal_range(h);
    bool collided = false;
    for (auto it = first; it != last; ++it) {
        const Entry& e = entries_[it->second - 1];
        if (e.compute && e.cs_module == cs_module) {
            ++stats_.hits;
            created = false;
            return it->second;
        }
        collided = true;
    }
    stats_.collisions += collided ? 1 : 0;
    ++stats_.misses;
    ++stats_.compute;
    entries_.push_back({true, {}, cs_module});
    const u32 id = static_cast<u32>(entries_.size());
    index_.emplace(h, id);
    created = true;
    return id;
}

const GraphicsPipelineState* PipelineStateCache::Graphics(u32 id) const {
    if (id == 0 || id > entries_.size() || entries_[id - 1].compute) {
        return nullptr;
    }
    return &entries_[id - 1].state;
}

u64 PipelineStateCache::MemoryBytes() const {
    return entries_.size() * sizeof(Entry) + index_.size() * (sizeof(u64) + sizeof(u32) + 2 * sizeof(void*));
}

} // namespace SecondSon::Gpu::Shader
