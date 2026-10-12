// SecondSonPC — the pipeline state a draw needs and SecondSonPC's cache of it (Phase 4, P4.3;
// docs/PHASE4_P43_SHADER_TRANSLATION.md §6).
//
// A graphics pipeline is its stage modules (program + specialization, shader_cache.h) and the fixed-function state a
// Vulkan pipeline bakes in: primitive topology, polygon mode, provoking vertex, clip space and depth clip/clamp, the
// depth/stencil formats and samples, the colour targets' formats, blending, write masks and samples, the logic op,
// the shader's colour exports. Everything a Vulkan backend can set dynamically (viewport, scissor, depth bias, blend
// constants, stencil reference, depth bounds, depth/stencil tests, cull mode, front face — as the bridge's backend
// does with the extended dynamic state) is not part of it. Fields are hardware register fields; an unbound target's
// fields are zero, so two states are the same pipeline exactly when their bytes are equal.
//
// The cache is indexed by a 64-bit hash of the state's bytes; a hit is confirmed by comparing every byte (two states
// with the same hash and different bytes are two pipelines, counted as a collision). The registers it reads belong to
// a fixed set of P4.2 register groups (PipelineStateGroups): a draw that changed none of them and kept its modules
// keeps the previous draw's pipeline without a lookup.
#pragma once

#include <array>
#include <deque>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/cp_registers.h"

namespace SecondSon::Gpu::Shader {

struct ColorTargetState {
    u32 info = 0;         ///< CB_COLORn_INFO: FORMAT, NUMBER_TYPE, COMP_SWAP, BLEND_BYPASS
    u32 export_format = 0;///< SPI_SHADER_COL_FORMAT's nibble
    u32 blend = 0;        ///< CB_BLENDn_CONTROL when blending is on for the target (else 0)
    u32 write_mask = 0;   ///< CB_TARGET_MASK's nibble in the image's component order (COMP_SWAP applied)
    u32 samples_log2 = 0; ///< CB_COLORn_ATTRIB.NUM_FRAGMENTS
};

struct GraphicsPipelineState {
    u32 vs_module = 0; ///< module ids (0: no stage)
    u32 ps_module = 0;
    u32 prim_type = 0;      ///< VGT_PRIMITIVE_TYPE
    u32 poly_mode = 0;      ///< PA_SU_SC_MODE_CNTL: POLYMODE_FRONT_PTYPE when POLY_MODE is on, else fill (2)
    u32 provoking_last = 0; ///< PA_SU_SC_MODE_CNTL.PROVOKING_VTX_LAST
    u32 clip_space = 0;     ///< PA_CL_CLIP_CNTL.DX_CLIP_SPACE_DEF
    u32 depth_clip = 0;     ///< PA_CL_CLIP_CNTL: ZCLIP_NEAR/FAR not disabled (both alike)
    u32 depth_clamp = 0;    ///< !DB_RENDER_OVERRIDE.DISABLE_VIEWPORT_CLAMP
    u32 z_format = 0;       ///< DB_Z_INFO.FORMAT when the depth buffer is valid
    u32 stencil_format = 0; ///< DB_STENCIL_INFO.FORMAT when the stencil buffer is valid
    u32 depth_samples_log2 = 0;
    u32 logic_op = 0;       ///< CB_COLOR_CONTROL.ROP3
    u32 color_mode = 0;     ///< CB_COLOR_CONTROL.MODE
    u32 cb_shader_mask = 0; ///< CB_SHADER_MASK
    u32 mrt_mask = 0;       ///< the fragment program's colour exports (its reflection)
    std::array<ColorTargetState, 8> targets{};
};
static_assert(std::has_unique_object_representations_v<GraphicsPipelineState>, "compared as bytes");

/// The state's fields from the register file (the registers SecondSonPC reads for it).
void BuildGraphicsPipelineState(const Cp::RegisterFile& regs, u32 vs_module, u32 ps_module, u32 mrt_mask,
                                GraphicsPipelineState& out);

/// CB_TARGET_MASK's nibble (export components) as the colour write mask of the image (its components), through
/// the target's COMP_SWAP as the translated program writes them (P4.5).
u32 ImageWriteMask(u32 cb_info, u32 export_mask);

/// The P4.2 register groups BuildGraphicsPipelineState reads (computed from its registers).
Cp::GroupMask PipelineStateGroups();

struct PipelineCacheStats {
    u64 lookups = 0, hits = 0, misses = 0, collisions = 0;
    u64 graphics = 0, compute = 0;
};

/// SecondSonPC's pipelines (graphics: their state; compute: the module). Ids from 1; one owner thread.
class PipelineStateCache {
public:
    /// The id of the graphics pipeline for `state` (created when new: `created`).
    u32 GetGraphics(const GraphicsPipelineState& state, bool& created);
    u32 GetCompute(u32 cs_module, bool& created);
    const GraphicsPipelineState* Graphics(u32 id) const;
    const PipelineCacheStats& Stats() const { return stats_; }
    size_t Size() const { return entries_.size(); }
    u64 MemoryBytes() const;

    /// Self-test: a hash function replacing the cache's (to force collisions); nullptr restores it.
    void SetTestHash(u64 (*hash)(const void* data, size_t bytes)) { test_hash_ = hash; }

private:
    struct Entry {
        bool compute = false;
        GraphicsPipelineState state;
        u32 cs_module = 0;
    };
    u64 Hash(const void* data, size_t bytes) const;
    std::unordered_multimap<u64, u32> index_;
    /// (a deque: an entry never moves - P4.10 hands Graphics()'s pointer to the recording thread while this thread adds
    /// pipelines; entries are never removed)
    std::deque<Entry> entries_;
    PipelineCacheStats stats_;
    u64 (*test_hash_)(const void*, size_t) = nullptr;
};

} // namespace SecondSon::Gpu::Shader
