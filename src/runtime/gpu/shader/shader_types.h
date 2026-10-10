// SecondSonPC — the shader frontend's own types (Phase 4, P4.3; docs/PHASE4_P43_SHADER_TRANSLATION.md §1, §5, §9).
//
// What a translation takes and gives, in SecondSonPC's terms (no type of the bridge): the stage's runtime parameters
// (register fields the program's translation depends on, as the hardware defines them), the device profile (what the
// Vulkan device supports), and the reflection of a translated program (the resources it reads, where their
// descriptors are found, its interface). The translator itself sits behind ShaderTranslator (shader_translator.h);
// for now the adapter layer implements it over the bridge's GCN -> SPIR-V recompiler.
//
// RuntimeParams holds hardware register fields, not the translator's derived values: the frontend keys caches on it
// without knowing how the translator interprets a format. Fields that do not apply to the stage are zero, so two
// RuntimeParams are equal exactly when their bytes are (keys hash and compare the bytes).
#pragma once

#include <array>
#include <string>
#include <type_traits>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/gcn_program.h"

namespace SecondSon::Gpu::Shader {

/// One colour target as a fragment program's translation sees it.
struct ColorTargetParams {
    u32 bound = 0;      ///< CB_COLORn_BASE != 0, INFO.FORMAT != 0, its CB_TARGET_MASK nibble != 0, colour writes on
    u32 info = 0;       ///< CB_COLORn_INFO: FORMAT, NUMBER_TYPE, COMP_SWAP, BLEND_BYPASS (kColorInfoMask)
    u32 min_max_self = 0; ///< CB_BLENDn_CONTROL: enabled, MIN/MAX colour function with SRC_COLOR/DST_COLOR factors
};
inline constexpr u32 kColorInfoMask = 0x7Cu | 0x700u | 0x1800u | (1u << 16);

/// The register fields a program's translation depends on (the stage's part; the rest zero).
struct RuntimeParams {
    HwStage hw = HwStage::Vs;
    SwStage sw = SwStage::Vertex;
    u8 pad0[2] = {};
    u32 rsrc1 = 0; ///< SPI_SHADER_PGM_RSRC1 / COMPUTE_PGM_RSRC1, Rsrc1TranslationMask
    u32 rsrc2 = 0; ///< ... RSRC2, Rsrc2TranslationMask

    // vertex (logical stage)
    u32 step_rate0 = 0, step_rate1 = 0; ///< VGT_INSTANCE_STEP_RATE_0/1
    u32 rect_list = 0, quad_list = 0;   ///< VGT_PRIMITIVE_TYPE is RECTLIST / QUADLIST
    u32 vertex_sgpr_offset = 0;         ///< indirect draws: the SH offsets the GPU writes base vertex / start instance to
    u32 instance_sgpr_offset = 0;
    // vertex (hardware stage)
    u32 vs_out_cntl = 0; ///< PA_CL_VS_OUT_CNTL
    u32 clip_cntl = 0;   ///< PA_CL_CLIP_CNTL: UCP_ENA, CLIP_DISABLE, DX_CLIP_SPACE_DEF (kClipCntlMask)

    // fragment
    u32 ps_input_ena = 0, ps_input_addr = 0; ///< SPI_PS_INPUT_ENA / ADDR
    u32 num_interp = 0;                      ///< SPI_PS_IN_CONTROL.NUM_INTERP
    u32 front_face_all_bits = 0;             ///< SPI_BARYC_CNTL.FRONT_FACE_ALL_BITS
    u32 db_shader_control = 0;               ///< DB_SHADER_CONTROL: exports, DEPTH_BEFORE_SHADER (kDbShaderMask)
    u32 z_format = 0;                        ///< SPI_SHADER_Z_FORMAT
    u32 col_format = 0;                      ///< SPI_SHADER_COL_FORMAT
    u32 aa_samples_log2 = 0;                 ///< PA_SC_AA_CONFIG.MSAA_NUM_SAMPLES, when sample coverage is an input
    u32 dual_source = 0;                     ///< CB_BLEND0_CONTROL enabled with a SRC1 factor
    u32 stages_en = 0;                       ///< VGT_SHADER_STAGES_EN (clip distances emulated: GS/LS enabled)
    u32 ps_vs_out_cntl = 0;                  ///< PA_CL_VS_OUT_CNTL.CLIP_DIST_ENA (the fragment side of emulation)
    u32 ps_ucp_enable = 0;                   ///< PA_CL_CLIP_CNTL.UCP_ENA
    std::array<u32, 32> ps_input_cntl{};     ///< SPI_PS_INPUT_CNTL_0..31 (kPsInputCntlMask); beyond NUM_INTERP zero
    std::array<ColorTargetParams, 8> targets{};

    // compute
    std::array<u32, 3> threads{}; ///< COMPUTE_NUM_THREAD_X/Y/Z (full groups)
};
static_assert(std::has_unique_object_representations_v<RuntimeParams>, "RuntimeParams is compared as bytes");

inline constexpr u32 kClipCntlMask = 0x3Fu | (1u << 16) | (1u << 19);
/// Z/stencil exports (0-2), COVERAGE_TO_MASK (7), MASK_EXPORT (8), DEPTH_BEFORE_SHADER (12)
inline constexpr u32 kDbShaderMask = 0x7u | (1u << 7) | (1u << 8) | (1u << 12);
inline constexpr u32 kPsInputCntlMask = 0x1Fu | (1u << 5) | (3u << 8) | (1u << 10);

/// What the Vulkan device supports that a translation depends on (the bridge's Shader::Profile, and the two device
/// properties its runtime information reads).
struct DeviceProfile {
    u32 max_viewport_width = 0, max_viewport_height = 0, max_shared_memory_size = 0;
    u32 supported_spirv = 0x00010600;
    u32 subgroup_size = 0;
    u32 sparse_page_shift = 0;
    bool support_int8 = false, support_int16 = false, support_int64 = false, support_float16 = false,
         support_float64 = false;
    bool supports_denorm_behavior_independence = false, supports_rounding_mode_independence = false;
    bool support_fp16_denorm_preserve = false, support_fp16_denorm_flush = false, support_fp16_round_to_zero = false;
    bool support_fp32_denorm_preserve = false, support_fp32_denorm_flush = false, support_fp32_round_to_zero = false;
    bool support_fp64_denorm_preserve = false, support_fp64_denorm_flush = false, support_fp64_round_to_zero = false;
    bool support_fp16_signed_zero_inf_nan_preserve = false, support_fp32_signed_zero_inf_nan_preserve = false,
         support_fp64_signed_zero_inf_nan_preserve = false;
    bool supports_image_load_store_lod = false, supports_native_cube_calc = false, supports_trinary_minmax = false;
    bool supports_buffer_fp32_atomic_min_max = false, supports_image_fp32_atomic_min_max = false;
    bool supports_buffer_int64_atomics = false, supports_shared_int64_atomics = false;
    bool supports_workgroup_explicit_memory_layout = false, supports_amd_shader_explicit_vertex_parameter = false;
    bool supports_fragment_shader_barycentric = false, supports_shader_subgroup_clock = false;
    bool has_broken_spirv_clamp = false, lower_left_origin_mode = false;
    bool needs_manual_interpolation = false, needs_lds_barriers = false, needs_buffer_offsets = false,
         needs_unorm_fixup = false, needs_clip_distance_emulation = false;
    bool supports_shader_stencil_export = false;
    // read by the runtime information, not the profile
    bool depth_clip_control = false; ///< VK_EXT_depth_clip_control (else [-W, W] clip space is emulated)
    std::string source;              ///< where it came from (a device name, "bridge", "test")
};

/// The first descriptor bindings of a stage: the stages of a pipeline are numbered one after another (fragment first).
struct BindingStart {
    u32 unified = 0;
    u32 buffer = 0;
    bool operator==(const BindingStart&) const = default;
};

/// Where a resource's descriptor is in the flattened user data (the user data registers, then what the SRT walk
/// copied after them): one load of consecutive dwords, or dwords each from an offset or an immediate.
struct SharpLocation {
    enum class Summary : u8 { SingleLoad, MultiLoad, Invalid };
    Summary summary = Summary::Invalid;
    u8 dwords = 0;    ///< 4 (V#, S#), 8 (T#)
    u8 load_mask = 0; ///< MultiLoad: bit i = dword i from offsets[i] (else immediates[i])
    std::array<u16, 8> offsets{};
    std::array<u32, 8> immediates{};
};

enum class BufferKind : u8 { Guest, Flatbuf, BdaPagetable, FaultBuffer, GdsBuffer, SharedMemory, ClipPlanes };
enum class SharpPostOp : u8 {
    None,
    BitwiseOrDw1WithImm,
    OffsetByProgramBase, ///< an inline constant buffer: its V# address is relative to the program
    ConvertCubeTo2DArray,
    DisableAnisoIfSingleLod,
    ForceRepeatXyzClamp,
    ForceLastTexelXyClamp,
    ClearAnisoRatioAndThreshold,
};
enum class MipFallback : u8 { None, DynamicIndex, ConstantIndex };

struct BufferResource {
    SharpLocation sharp;
    BufferKind kind = BufferKind::Guest;
    bool is_written = false;
    bool is_formatted = false;
    SharpPostOp post_op = SharpPostOp::None;
    u32 post_op_dw1_mask = 0;
    u32 used_types = 0; ///< the translator's IR type mask (informational)
};
struct ImageResource {
    SharpLocation sharp;
    bool is_depth = false, is_atomic = false, is_array = false, is_written = false, is_r128 = false;
    u8 constant_mip_index = 0;
    MipFallback mip_fallback = MipFallback::None;
    SharpPostOp post_op = SharpPostOp::None;
};
struct SamplerResource {
    SharpLocation sharp;
    SharpPostOp post_op = SharpPostOp::None;
    u16 post_op_tsharp_dw3 = 0; ///< DisableAnisoIfSingleLod: the T#'s dword 3 in the flattened user data
    bool is_depth = false;
};
struct FmaskResource {
    u16 sharp_idx = 0;
};

/// A vertex attribute the fetch shader loads (the fetch shader is the VS's prologue the driver generated).
struct FetchAttribute {
    u32 dest_vgpr = 0;
    u32 num_elements = 0;
    u32 sgpr_base = 0;    ///< user data pair holding the vertex buffer table's address
    u32 dword_offset = 0; ///< the V#'s dword offset in that table
    u32 inst_offset = 0;
    u32 instance_data = 0; ///< 0 per vertex, 1/2 per instance over step rate 0/1, 3 per instance
    u32 data_format = 0;  ///< MTBUF's format (0 for MUBUF: from the V#)
    u32 num_format = 0;
    bool operator==(const FetchAttribute&) const = default;
};

struct FetchShader {
    bool present = false;
    u32 sgpr_base = 0; ///< user data pair holding the fetch shader's address
    std::vector<FetchAttribute> attributes;
    s32 vertex_offset_sgpr = -1;
    s32 instance_offset_sgpr = -1;
    u32 size = 0; ///< bytes up to and with its s_setpc_b64
};

/// What a translated program reads and how it fits in a pipeline.
struct ShaderReflection {
    HwStage hw = HwStage::Vs;
    SwStage sw = SwStage::Vertex;
    u64 footer_hash = 0; ///< the program's footer hash as the translator saw it
    std::vector<BufferResource> buffers;
    std::vector<ImageResource> images;
    std::vector<SamplerResource> samplers;
    std::vector<FmaskResource> fmasks;
    u32 flatbuf_dwords = 16;    ///< flattened user data: 16 registers, then the SRT walk's copies
    std::vector<u8> srt_walker; ///< the SRT walk as the translator generated it (x86; SecondSonPC interprets it)
    FetchShader fetch;
    u16 user_data_used = 0;     ///< user data registers the program reads after optimisation (bit i: s_i)
    u8 mrt_mask = 0;            ///< colour exports (fragment)
    bool has_discard = false, uses_dma = false, has_storage_images = false, uses_lane_id = false,
         uses_shared = false, uses_fp16 = false, uses_fp64 = false, has_bitwise_xor = false;
    u32 shared_memory_size = 0;
    s32 tess_consts_dword_offset = -1;
    BindingStart start;    ///< the bindings the stage's resources start at
    BindingStart end;      ///< ... and the next stage starts at
};

} // namespace SecondSon::Gpu::Shader
