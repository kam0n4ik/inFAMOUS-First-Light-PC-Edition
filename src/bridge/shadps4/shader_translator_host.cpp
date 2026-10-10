// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — the adapter between SecondSonPC's shader frontend and the bridge's GCN -> SPIR-V recompiler (Phase 4,
// P4.3; docs/PHASE4_P43_SHADER_TRANSLATION.md §2). The integration boundary: SecondSonPC's types in and out
// (runtime/gpu/shader/shader_translator.h), the bridge's recompiler (decoder, IR, passes, SPIR-V emitter — shadPS4
// code, GPL-2.0-or-later) inside. What stays the bridge's: the translation itself. What is SecondSonPC's: which
// program, its runtime parameters (read from SecondSonPC's register file), the device profile, every read of guest
// memory the translation makes (patch 0030: through SecondSonPC's reader; the SRT walk is SecondSonPC's interpreter of
// the walker the recompiler generated), the reflection handed back, and what is done with the result.
//
// Also the bridge's side of patch 0031 (shader_bridge_hooks.h): the modules the bridge translates (captured for
// --gpu-shader-verify) and the pipeline it picks at each draw (--gpu-shaders compares it with SecondSonPC's).
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <mutex>

#include <fmt/format.h>

#include "bridge/shadps4/shader_bridge_hooks.h"
#include "runtime/gpu/shader/runtime_params.h"
#include "runtime/gpu/shader/shader_cache.h"
#include "runtime/gpu/shader/shader_capture.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs.h"

namespace SecondSon::Bridge {

namespace GS = Gpu::Shader;

namespace {

// SecondSonPC's register indices (runtime_params.h, pipeline_state.cpp) against the bridge's layout of the registers.
#define SSPC_REG_INDEX(field) (offsetof(AmdGpu::Regs, field) / sizeof(u32))
static_assert(SSPC_REG_INDEX(color_target_mask) == GS::Reg::CbTargetMask);
static_assert(SSPC_REG_INDEX(ps_inputs) == GS::Reg::SpiPsInputCntl0);
static_assert(SSPC_REG_INDEX(ps_input_ena) == GS::Reg::SpiPsInputEna);
static_assert(SSPC_REG_INDEX(ps_input_addr) == GS::Reg::SpiPsInputAddr);
static_assert(SSPC_REG_INDEX(barycentric_control) == GS::Reg::SpiBarycCntl);
static_assert(SSPC_REG_INDEX(z_export_format) == GS::Reg::SpiShaderZFormat);
static_assert(SSPC_REG_INDEX(color_export_format) == GS::Reg::SpiShaderColFormat);
static_assert(SSPC_REG_INDEX(blend_control) == GS::Reg::CbBlend0Control);
static_assert(SSPC_REG_INDEX(color_control) == GS::Reg::CbColorControl);
static_assert(SSPC_REG_INDEX(depth_shader_control) == GS::Reg::DbShaderControl);
static_assert(SSPC_REG_INDEX(clipper_control) == GS::Reg::PaClClipCntl);
static_assert(SSPC_REG_INDEX(polygon_control) == 0xA205);
static_assert(SSPC_REG_INDEX(vs_output_control) == GS::Reg::PaClVsOutCntl);
static_assert(SSPC_REG_INDEX(vgt_instance_step_rate_0) == GS::Reg::VgtInstanceStepRate0);
static_assert(SSPC_REG_INDEX(vgt_instance_step_rate_1) == GS::Reg::VgtInstanceStepRate1);
static_assert(SSPC_REG_INDEX(stage_enable) == GS::Reg::VgtShaderStagesEn);
static_assert(SSPC_REG_INDEX(aa_config) == GS::Reg::PaScAaConfig);
static_assert(SSPC_REG_INDEX(color_buffers) == GS::Reg::CbColor0Base);
static_assert(sizeof(AmdGpu::ColorBuffer) == GS::Reg::CbColorStride * sizeof(u32));
static_assert(SSPC_REG_INDEX(primitive_type) == GS::Reg::VgtPrimitiveType);
static_assert(SSPC_REG_INDEX(depth_render_override) == 0xA003);
static_assert(SSPC_REG_INDEX(depth_buffer) == 0xA010);
static_assert(SSPC_REG_INDEX(color_shader_mask) == 0xA08F);
static_assert(SSPC_REG_INDEX(ps_program) == GS::Reg::kPgmLo[0]);
static_assert(SSPC_REG_INDEX(vs_program) == GS::Reg::kPgmLo[1]);
static_assert(SSPC_REG_INDEX(cs_program) == 0x2E00);
static_assert(sizeof(AmdGpu::ComputeProgram) == 0x50 * sizeof(u32));
static_assert(offsetof(AmdGpu::ComputeProgram, num_thread_x) / 4 == GS::Reg::ComputeNumThreadX);
static_assert(offsetof(AmdGpu::ComputeProgram, user_data) / 4 == GS::Reg::ComputeUserData0);
#undef SSPC_REG_INDEX

Shader::HwStage ToHw(GS::HwStage s) {
    switch (s) {
    case GS::HwStage::Ps: return Shader::HwStage::Fragment;
    case GS::HwStage::Vs: return Shader::HwStage::Vertex;
    case GS::HwStage::Gs: return Shader::HwStage::Geometry;
    case GS::HwStage::Es: return Shader::HwStage::Export;
    case GS::HwStage::Hs: return Shader::HwStage::Hull;
    case GS::HwStage::Ls: return Shader::HwStage::Local;
    default: return Shader::HwStage::Compute;
    }
}
GS::HwStage FromHw(Shader::HwStage s) {
    switch (s) {
    case Shader::HwStage::Fragment: return GS::HwStage::Ps;
    case Shader::HwStage::Vertex: return GS::HwStage::Vs;
    case Shader::HwStage::Geometry: return GS::HwStage::Gs;
    case Shader::HwStage::Export: return GS::HwStage::Es;
    case Shader::HwStage::Hull: return GS::HwStage::Hs;
    case Shader::HwStage::Local: return GS::HwStage::Ls;
    default: return GS::HwStage::Cs;
    }
}
static_assert(static_cast<u32>(Shader::SwStage::Fragment) == static_cast<u32>(GS::SwStage::Fragment) &&
              static_cast<u32>(Shader::SwStage::Vertex) == static_cast<u32>(GS::SwStage::Vertex) &&
              static_cast<u32>(Shader::SwStage::Compute) == static_cast<u32>(GS::SwStage::Compute) &&
              static_cast<u32>(Shader::SwStage::NumLogicalStages) == static_cast<u32>(GS::SwStage::Count));

// ---- profile

#define SSPC_PROFILE_FIELDS(X)                                                                                         \
    X(max_viewport_width) X(max_viewport_height) X(max_shared_memory_size) X(supported_spirv) X(subgroup_size)         \
    X(sparse_page_shift) X(support_int8) X(support_int16) X(support_int64) X(support_float16) X(support_float64)       \
    X(supports_denorm_behavior_independence) X(supports_rounding_mode_independence) X(support_fp16_denorm_preserve)   \
    X(support_fp16_denorm_flush) X(support_fp16_round_to_zero) X(support_fp32_denorm_preserve)                        \
    X(support_fp32_denorm_flush) X(support_fp32_round_to_zero) X(support_fp64_denorm_preserve)                        \
    X(support_fp64_denorm_flush) X(support_fp64_round_to_zero) X(support_fp16_signed_zero_inf_nan_preserve)           \
    X(support_fp32_signed_zero_inf_nan_preserve) X(support_fp64_signed_zero_inf_nan_preserve)                         \
    X(supports_image_load_store_lod) X(supports_native_cube_calc) X(supports_trinary_minmax)                          \
    X(supports_buffer_fp32_atomic_min_max) X(supports_image_fp32_atomic_min_max) X(supports_buffer_int64_atomics)    \
    X(supports_shared_int64_atomics) X(supports_workgroup_explicit_memory_layout)                                    \
    X(supports_amd_shader_explicit_vertex_parameter) X(supports_fragment_shader_barycentric)                          \
    X(supports_shader_subgroup_clock) X(has_broken_spirv_clamp) X(lower_left_origin_mode)                             \
    X(needs_manual_interpolation) X(needs_lds_barriers) X(needs_buffer_offsets) X(needs_unorm_fixup)                  \
    X(needs_clip_distance_emulation) X(supports_shader_stencil_export)

Shader::Profile ToProfile(const GS::DeviceProfile& d) {
    Shader::Profile p{};
#define X(f) p.f = d.f;
    SSPC_PROFILE_FIELDS(X)
#undef X
    return p;
}

GS::DeviceProfile FromProfile(const Shader::Profile& p) {
    GS::DeviceProfile d;
#define X(f) d.f = p.f;
    SSPC_PROFILE_FIELDS(X)
#undef X
    d.source = "bridge";
    return d;
}

// ---- runtime information from SecondSonPC's runtime parameters (the bridge's BuildRuntimeInfo, field for field)

u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    using Shader::Output;
    u32 n = 0;
    if (ctl.vs_out_misc_enable) {
        auto& misc = outputs[n++];
        misc[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc[1] = ctl.use_vtx_edge_flag ? Output::EdgeFlag
                                        : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc[2] = ctl.use_vtx_kill_flag ? Output::KillFlag
                                        : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }
    const auto dist = [&](Shader::OutputMap& o, u32 first) {
        for (u32 i = 0; i < 4; ++i) {
            const u32 k = first + i;
            o[i] = ctl.IsClipDistEnabled(k)
                       ? static_cast<Output>(static_cast<u32>(Output::ClipDist0) + k)
                       : (ctl.IsCullDistEnabled(k) ? static_cast<Output>(static_cast<u32>(Output::CullDist0) + k)
                                                   : Output::None);
        }
    };
    if (ctl.vs_out_ccdist0_enable) {
        dist(outputs[n++], 0);
    }
    if (ctl.vs_out_ccdist1_enable) {
        dist(outputs[n++], 4);
    }
    return n;
}

Shader::RuntimeInfo ToRuntime(const GS::RuntimeParams& p, const GS::DeviceProfile& prof) {
    Shader::RuntimeInfo info{};
    info.Initialize(ToHw(p.hw), static_cast<Shader::SwStage>(p.sw));
    const bool cs = p.hw == GS::HwStage::Cs;
    info.props.fp_denorm_mode32 = static_cast<AmdGpu::FpDenormMode>((p.rsrc1 >> 16) & 3);
    info.props.fp_denorm_mode16_64 = static_cast<AmdGpu::FpDenormMode>((p.rsrc1 >> 18) & 3);
    info.props.fp_round_mode32 = static_cast<AmdGpu::FpRoundMode>((p.rsrc1 >> 12) & 3);
    info.props.fp_round_mode16_64 = static_cast<AmdGpu::FpRoundMode>((p.rsrc1 >> 14) & 3);
    // the bridge: graphics (VGPRS + 1) * 4, compute VGPRS * 4 (kept: the module must equal the bridge's)
    info.props.num_allocated_vgprs = cs ? (p.rsrc1 & 0x3F) * 4 : ((p.rsrc1 & 0x3F) + 1) * 4;
    info.props.num_user_data = (p.rsrc2 >> 1) & 0x1F;
    info.props.num_input_vgprs = cs ? 0 : (p.rsrc1 >> 24) & 3;
    info.props.dx10_clamp = (p.rsrc1 >> 21) & 1;
    switch (p.hw) {
    case GS::HwStage::Vs:
        info.hw.vs.user_clip_plane_mask = p.clip_cntl & 0x3F;
        info.hw.vs.num_outputs = MapOutputs(info.hw.vs.outputs, std::bit_cast<AmdGpu::VsOutputControl>(p.vs_out_cntl));
        info.hw.vs.emulate_depth_negative_one_to_one = !prof.depth_clip_control && ((p.clip_cntl >> 19) & 1) == 0;
        info.hw.vs.clip_disable = ((p.clip_cntl >> 16) & 1) != 0 || p.rect_list != 0;
        break;
    case GS::HwStage::Ps: {
        auto& fs = info.hw.fs;
        fs.en_flags = std::bit_cast<AmdGpu::PsInput>(p.ps_input_ena);
        fs.addr_flags = std::bit_cast<AmdGpu::PsInput>(p.ps_input_addr);
        fs.num_inputs = p.num_interp;
        fs.front_face_all_bits = p.front_face_all_bits;
        fs.depth_before_shader = (p.db_shader_control >> 12) & 1;
        fs.num_samples = 1u << p.aa_samples_log2;
        fs.z_export_format = static_cast<AmdGpu::ShaderExportFormat>(p.z_format);
        const u32 d = p.db_shader_control;
        const u32 stencil = ((d >> 2) & 1) | ((d >> 1) & 1);
        fs.mrtz_mask = (d & 1) | (stencil << 1) | (((d >> 8) & 1) << 2) | (((d >> 7) & 1) << 3);
        fs.dual_source_blending = p.dual_source;
        for (u32 i = 0; i < p.num_interp && i < 32; ++i) {
            const u32 c = p.ps_input_cntl[i];
            fs.inputs[i] = {.param_index = static_cast<u16>(c & 0x1F),
                            .is_default = static_cast<u16>((c >> 5) & 1),
                            .is_flat = static_cast<u16>((c >> 10) & 1),
                            .default_value = static_cast<u16>((c >> 8) & 3)};
        }
        for (u32 cb = 0; cb < 8; ++cb) {
            const GS::ColorTargetParams& t = p.targets[cb];
            if (!t.bound) {
                continue;
            }
            AmdGpu::ColorBuffer buf{};
            buf.base_address = 1;
            buf.info.raw = t.info;
            fs.color_buffers[cb] = {
                .data_format = buf.GetDataFmt(),
                .num_format = buf.GetNumberFmt(),
                .num_conversion = buf.GetNumberConversion(),
                .export_format = static_cast<AmdGpu::ShaderExportFormat>((p.col_format >> (cb * 4)) & 0xF),
                .blend_self_scale = t.min_max_self,
                .swizzle = buf.Swizzle(),
            };
        }
        const bool ls = (p.stages_en & 3) != 0, gs = ((p.stages_en >> 5) & 1) != 0;
        fs.clip_distance_emulation =
            ((p.ps_vs_out_cntl != 0 && !ls) || (p.ps_ucp_enable != 0 && !gs)) && prof.needs_clip_distance_emulation;
        break;
    }
    case GS::HwStage::Cs:
        info.hw.cs.workgroup_size = {p.threads[0], p.threads[1], p.threads[2]};
        info.hw.cs.tgid_enable = {((p.rsrc2 >> 7) & 1) != 0, ((p.rsrc2 >> 8) & 1) != 0, ((p.rsrc2 >> 9) & 1) != 0};
        info.hw.cs.shared_memory_size = ((p.rsrc2 >> 15) & 0x1FF) * 128 * 4;
        break;
    default:
        break;
    }
    if (p.sw == GS::SwStage::Vertex) {
        info.sw.vs.step_rate_0 = p.step_rate0;
        info.sw.vs.step_rate_1 = p.step_rate1;
        info.sw.vs.vertex_sgpr_offset = static_cast<u16>(p.vertex_sgpr_offset);
        info.sw.vs.instance_sgpr_offset = static_cast<u16>(p.instance_sgpr_offset);
        info.sw.vs.tess_emulated_primitive = p.rect_list != 0 || p.quad_list != 0;
    }
    return info;
}

/// Which parts of two runtime informations differ (the bridge's operator== decides; this names them).
std::string DiffRuntime(const Shader::RuntimeInfo& a, const Shader::RuntimeInfo& b) {
    std::string d;
    auto add = [&](std::string_view what) { d += d.empty() ? std::string(what) : fmt::format(", {}", what); };
    if (a.hw_stage != b.hw_stage || a.sw_stage != b.sw_stage) add("stage");
    if (!(a.props == b.props)) {
        add(fmt::format("props (vgprs {}/{}, user data {}/{}, input vgprs {}/{}, modes)", u32(a.props.num_allocated_vgprs),
                        u32(b.props.num_allocated_vgprs), u32(a.props.num_user_data), u32(b.props.num_user_data),
                        u32(a.props.num_input_vgprs), u32(b.props.num_input_vgprs)));
    }
    if (!a.HasSameSwInfo(b.sw)) add("logical stage info (step rates, SGPR offsets, emulated primitive)");
    if (!a.HasSameHwInfo(b.hw)) {
        if (a.hw_stage == Shader::HwStage::Fragment) {
            const auto& x = a.hw.fs;
            const auto& y = b.hw.fs;
            if (!(x.en_flags == y.en_flags) || !(x.addr_flags == y.addr_flags)) add("fs input enables");
            if (x.num_inputs != y.num_inputs) add(fmt::format("fs num_inputs {}/{}", x.num_inputs, y.num_inputs));
            for (u32 i = 0; i < 8; ++i) {
                if (!(x.color_buffers[i] == y.color_buffers[i])) add(fmt::format("fs color_buffers[{}]", i));
            }
            if (x.z_export_format != y.z_export_format || x.mrtz_mask != y.mrtz_mask) add("fs depth exports");
            if (x.num_samples != y.num_samples) add("fs num_samples");
            if (x.dual_source_blending != y.dual_source_blending) add("fs dual source");
            if (x.clip_distance_emulation != y.clip_distance_emulation) add("fs clip distance emulation");
            if (x.depth_before_shader != y.depth_before_shader || x.front_face_all_bits != y.front_face_all_bits)
                add("fs depth before shader / front face");
            if (!std::equal(x.inputs.begin(), x.inputs.begin() + std::min<u32>(x.num_inputs, 32), y.inputs.begin()))
                add("fs inputs");
        } else {
            add("hardware stage info");
        }
    }
    return d;
}

// ---- reflection from the bridge's Info

template <typename T>
GS::SharpLocation Location(const Shader::SharpFetch<T>& f) {
    GS::SharpLocation l;
    l.summary = static_cast<GS::SharpLocation::Summary>(static_cast<u8>(f.summary));
    l.dwords = static_cast<u8>(Shader::SharpFetch<T>::N);
    l.load_mask = f.load_mask;
    for (size_t i = 0; i < Shader::SharpFetch<T>::N; ++i) {
        l.offsets[i] = f.offsets[i];
        l.immediates[i] = f.immediates[i];
    }
    return l;
}

GS::FetchShader FromFetch(const Shader::Gcn::FetchShaderData& f, u32 sgpr_base) {
    GS::FetchShader out;
    out.present = true;
    out.sgpr_base = sgpr_base;
    out.vertex_offset_sgpr = f.vertex_offset_sgpr;
    out.instance_offset_sgpr = f.instance_offset_sgpr;
    out.size = f.size;
    for (const auto& a : f.attributes) {
        out.attributes.push_back({.dest_vgpr = a.dest_vgpr,
                                  .num_elements = a.num_elements,
                                  .sgpr_base = a.sgpr_base,
                                  .dword_offset = a.dword_offset,
                                  .inst_offset = a.inst_offset,
                                  .instance_data = a.instance_data,
                                  .data_format = a.data_format,
                                  .num_format = a.num_format});
    }
    return out;
}

void FillReflection(const Shader::Info& info, GS::ShaderReflection& r) {
    r.hw = FromHw(info.hw_stage);
    r.sw = static_cast<GS::SwStage>(info.sw_stage);
    r.footer_hash = info.pgm_hash;
    for (const auto& b : info.buffers) {
        r.buffers.push_back({.sharp = Location(b.sharp_fetch),
                             .kind = static_cast<GS::BufferKind>(static_cast<u8>(b.buffer_type)),
                             .is_written = b.is_written,
                             .is_formatted = b.is_formatted,
                             .post_op = static_cast<GS::SharpPostOp>(static_cast<u8>(b.post_op)),
                             .post_op_dw1_mask = b.post_op_dw1_mask,
                             .used_types = static_cast<u32>(b.used_types)});
    }
    for (const auto& im : info.images) {
        r.images.push_back({.sharp = Location(im.sharp_fetch),
                            .is_depth = im.is_depth,
                            .is_atomic = im.is_atomic,
                            .is_array = im.is_array,
                            .is_written = im.is_written,
                            .is_r128 = im.is_r128,
                            .constant_mip_index = im.constant_mip_index,
                            .mip_fallback = static_cast<GS::MipFallback>(static_cast<u16>(im.mip_fallback_mode)),
                            .post_op = static_cast<GS::SharpPostOp>(static_cast<u8>(im.post_op))});
    }
    for (const auto& s : info.samplers) {
        r.samplers.push_back({.sharp = Location(s.sharp_fetch),
                              .post_op = static_cast<GS::SharpPostOp>(static_cast<u8>(s.post_op)),
                              .post_op_tsharp_dw3 = s.post_op_tsharp_dw3_off,
                              .is_depth = s.is_depth});
    }
    for (const auto& f : info.fmasks) {
        r.fmasks.push_back({.sharp_idx = f.sharp_idx});
    }
    r.flatbuf_dwords = info.srt_info.flattened_bufsize_dw;
    if (info.srt_info.walker_func != nullptr && info.srt_info.walker_func_size != 0) {
        const auto* code = reinterpret_cast<const u8*>(info.srt_info.walker_func);
        r.srt_walker.assign(code, code + info.srt_info.walker_func_size);
    }
    r.mrt_mask = info.mrt_mask;
    r.has_discard = info.has_discard;
    r.uses_dma = info.uses_dma;
    r.has_storage_images = info.has_storage_images;
    r.uses_lane_id = info.uses_lane_id;
    r.uses_shared = info.shared_types != Shader::IR::Type::Void;
    r.uses_fp16 = info.uses_fp16;
    r.uses_fp64 = info.uses_fp64;
    r.has_bitwise_xor = info.has_bitwise_xor;
    r.shared_memory_size = info.shared_memory_scratch_size;
    r.tess_consts_dword_offset = info.tess_consts_dword_offset;
}

u16 UserDataUsed(const Shader::IR::Program& program) {
    u16 mask = 0;
    for (const Shader::IR::Block* block : program.blocks) {
        for (const Shader::IR::Inst& inst : *block) {
            if (inst.GetOpcode() == Shader::IR::Opcode::GetUserData) {
                const u32 reg = static_cast<u32>(inst.Arg(0).ScalarReg());
                if (reg < 16) {
                    mask |= static_cast<u16>(1u << reg);
                }
            }
        }
    }
    return mask;
}

constexpr u32 kSetpcSentinel = 0xBE802000u; // s_setpc_b64 s[0:1]

// ---- the translation's guest reads (patch 0030)

class Reads final : public Shader::GuestReads {
public:
    explicit Reads(const GS::TranslationRequest& r) : req(r) {
        if (!req.fetch_code.empty()) {
            fetch.assign(req.fetch_code.begin(), req.fetch_code.end());
            fetch_given = true;
        }
    }

    bool Read(VAddr addr, void* dst, size_t bytes) override {
        if (!req.vertex_sharps.empty() && bytes == 16 && MatchVertexSharp(addr, dst)) {
            return true;
        }
        return req.memory != nullptr && req.memory->Read(addr, dst, bytes);
    }

    void Flatten(const u8* walker, size_t walker_size, const u32* user_data, u32* flat, size_t flat_dwords) override {
        if (!req.flatbuf.empty()) {
            if (req.flatbuf.size() != flat_dwords) {
                srt_error = fmt::format("the given flattened user data has {} dwords, the translation's {}",
                                        req.flatbuf.size(), flat_dwords);
                return;
            }
            std::copy(req.flatbuf.begin(), req.flatbuf.end(), flat);
            return;
        }
        if (walker == nullptr || walker_size == 0) {
            return;
        }
        GS::SrtProgram program;
        if (!program.Compile(std::span<const u8>(walker, walker_size), srt_error)) {
            return;
        }
        if (req.memory == nullptr) {
            srt_error = "no guest memory to walk the SRT";
            return;
        }
        GS::SrtProgram::RunStats stats;
        if (!program.Run(std::span<const u32, 16>(user_data, 16), std::span<u32>(flat, flat_dwords), *req.memory,
                         stats, srt_error)) {
            return;
        }
        srt_faults += stats.faults;
    }

    const u32* FetchShaderCode(VAddr addr) override {
        if (fetch_parsed) {
            return fetch.data();
        }
        fetch_parsed = true;
        addr &= 0xFFFFFFFFFFFFull; // the user data pair's address, as SecondSonPC's fetch cache reads it
        fetch_addr = addr;
        if (!fetch_given) {
            // read up to its s_setpc_b64 (bounded), as SecondSonPC's fetch cache does
            for (u32 have = 0; have < GS::kMaxFetchDwords;) {
                const u32 chunk = std::min<u32>(64, GS::kMaxFetchDwords - have);
                fetch.resize(have + chunk);
                if (req.memory == nullptr || !req.memory->Read(addr + have * 4ull, fetch.data() + have, chunk * 4ull)) {
                    fetch_error = fmt::format("the fetch shader at {:#x} is unreadable", addr);
                    break;
                }
                have += chunk;
                GS::FetchShader parsed;
                std::string e;
                if (GS::ParseFetchShaderCode(fetch, 0, parsed, e)) {
                    fetch.resize(parsed.size / 4);
                    fetch_error.clear();
                    break;
                }
                fetch_error = e;
                if (e.rfind("no s_setpc_b64", 0) != 0 && e.rfind("a buffer load cut", 0) != 0) {
                    break;
                }
            }
        }
        GS::FetchShader parsed;
        if (fetch_error.empty() && !GS::ParseFetchShaderCode(fetch, 0, parsed, fetch_error)) {
            // not of the driver's form: the recompiler would assert on it
        }
        if (!fetch_error.empty()) {
            fetch.clear(); // the recompiler sees an empty fetch shader; the translation is reported failed
        }
        code_size = fetch.size();
        fetch.push_back(kSetpcSentinel); // the parsers stop here whatever the code held
        return fetch.data();
    }

    /// The fetch shader's code without the sentinel.
    std::span<const u32> FetchCode() const {
        return std::span<const u32>(fetch.data(), code_size);
    }

    const GS::TranslationRequest& req;
    std::vector<u32> fetch;
    size_t code_size = 0;
    bool fetch_given = false, fetch_parsed = false;
    VAddr fetch_addr = 0;
    std::string fetch_error, srt_error;
    u32 srt_faults = 0;

private:
    bool MatchVertexSharp(VAddr addr, void* dst) {
        if (!matched_init) {
            matched_init = true;
            GS::FetchShader parsed;
            std::string e;
            if (!fetch.empty() && GS::ParseFetchShaderCode(std::span<const u32>(fetch.data(), fetch.size()), 0, parsed, e)) {
                for (const auto& a : parsed.attributes) {
                    if (a.sgpr_base + 1 < 16) {
                        const u64 table = (req.user_data[a.sgpr_base] | (u64{req.user_data[a.sgpr_base + 1]} << 32)) &
                                          0xFFFFFFFFFFFFull;
                        attr_addresses.push_back(table + u64{a.dword_offset} * 4);
                    } else {
                        attr_addresses.push_back(~0ull);
                    }
                }
            }
        }
        for (size_t i = 0; i < attr_addresses.size() && i < req.vertex_sharps.size(); ++i) {
            if (attr_addresses[i] == addr) {
                std::memcpy(dst, req.vertex_sharps[i].data(), 16);
                return true;
            }
        }
        return false;
    }
    bool matched_init = false;
    std::vector<u64> attr_addresses;
};

struct ReadsScope {
    explicit ReadsScope(Shader::GuestReads* r) {
        Shader::CurrentGuestReads() = r;
    }
    ~ReadsScope() {
        Shader::CurrentGuestReads() = nullptr;
    }
};

u64 MicrosSince(std::chrono::steady_clock::time_point t) {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count());
}

class BridgeShaderTranslator final : public GS::ShaderTranslator {
public:
    std::string_view Name() const override {
        return "shadPS4 recompiler (adapter, patch 0030)";
    }

    GS::TranslationResult Translate(const GS::TranslationRequest& req) override {
        GS::TranslationResult res;
        if (req.code.empty() || req.profile == nullptr) {
            res.error = GS::TranslationError::BadRequest;
            res.detail = "no code or no device profile";
            return res;
        }
        const bool supported = (req.hw == GS::HwStage::Ps && req.sw == GS::SwStage::Fragment) ||
                               (req.hw == GS::HwStage::Vs && req.sw == GS::SwStage::Vertex) ||
                               (req.hw == GS::HwStage::Cs && req.sw == GS::SwStage::Compute);
        if (!supported) {
            res.error = GS::TranslationError::BadRequest;
            res.detail = fmt::format("stage {} as {}: not prepared by SecondSonPC yet", GS::HwStageName(req.hw),
                                     GS::SwStageName(req.sw));
            return res;
        }
        if (req.memory == nullptr && req.flatbuf.empty()) {
            res.error = GS::TranslationError::BadRequest;
            res.detail = "neither guest memory nor a flattened user data";
            return res;
        }
        std::scoped_lock lock{mutex_}; // one translation at a time through this adapter (its pools)
        const Shader::Profile profile = ToProfile(*req.profile);
        Shader::RuntimeInfo runtime = ToRuntime(req.runtime, *req.profile);
        std::array<u32, 16> user_data = req.user_data;
        const Shader::ShaderParams params{.user_data = std::span<const u32, 16>(user_data),
                                          .code = req.code,
                                          .hash = req.footer_hash};
        Shader::Info info(ToHw(req.hw), static_cast<Shader::SwStage>(req.sw), params);
        info.pgm_base = req.address;
        Reads reads(req);
        ReadsScope scope(&reads);

        const auto t0 = std::chrono::steady_clock::now();
        const auto program = Shader::TranslateProgram(req.code, pools_, info, runtime, profile);
        res.translate_us = MicrosSince(t0);
        if (info.translation_failed) {
            res.error = GS::TranslationError::UnsupportedOpcode;
            res.detail = "the recompiler met an instruction it does not translate (named in the bridge's log)";
            return res;
        }
        if (!reads.fetch_error.empty()) {
            res.error = GS::TranslationError::FetchShader;
            res.detail = reads.fetch_error;
            return res;
        }
        if (!reads.srt_error.empty()) {
            res.error = GS::TranslationError::Srt;
            res.detail = reads.srt_error;
            return res;
        }
        res.reflection.user_data_used = UserDataUsed(program);
        Shader::Backend::Bindings binding{.unified = req.start.unified, .buffer = req.start.buffer};
        const auto t1 = std::chrono::steady_clock::now();
        res.spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime, program, binding);
        res.emit_us = MicrosSince(t1);
        if (res.spirv.empty()) {
            res.error = GS::TranslationError::Emit;
            res.detail = "no SPIR-V";
            return res;
        }
        FillReflection(info, res.reflection);
        res.reflection.start = req.start;
        res.reflection.end = {binding.unified, binding.buffer};
        res.flatbuf = info.flattened_ud_buf;
        if (info.has_fetch_shader) {
            Shader::Gcn::FetchShaderData fetch;
            if (Shader::Gcn::ParseFetchShader(info, fetch)) { // reads the hook's copy
                res.reflection.fetch = FromFetch(fetch, info.fetch_shader_sgpr_base);
                for (const auto& a : fetch.attributes) {
                    // the V# as the translation read it (before the attribute's offset and format)
                    const auto v = info.ReadUdReg<AmdGpu::Buffer>(a.sgpr_base, a.dword_offset);
                    std::array<u32, 4> raw{};
                    std::memcpy(raw.data(), &v, 16);
                    res.vertex_sharps.push_back(raw);
                }
            }
            const auto code = reads.FetchCode();
            res.fetch_code.assign(code.begin(), code.end());
        }
        return res;
    }

    bool CompareRuntime(const GS::RuntimeParams& ours, const GS::DeviceProfile& profile, std::span<const u8> reference,
                        std::string& diff) override {
        if (reference.size() != sizeof(Shader::RuntimeInfo)) {
            diff = fmt::format("reference of {} bytes, the runtime information has {}", reference.size(),
                               sizeof(Shader::RuntimeInfo));
            return false;
        }
        Shader::RuntimeInfo bridge{};
        std::memcpy(static_cast<void*>(&bridge), reference.data(), sizeof(bridge));
        const Shader::RuntimeInfo mine = ToRuntime(ours, profile);
        if (mine == bridge) {
            return true;
        }
        diff = DiffRuntime(mine, bridge);
        return false;
    }

private:
    std::mutex mutex_;
    Shader::Pools pools_;
};

BridgeShaderTranslator g_translator;

} // namespace

void InstallShaderTranslatorAdapter() {
    GS::InstallShaderTranslator(&g_translator);
}

// ---- patch 0031: the bridge's side

namespace ShaderHooks {

std::atomic<bool> g_capture{false};
std::atomic<bool> g_view{false};

void OnModuleCompiled(const Shader::Info& info, const Shader::RuntimeInfo& runtime, std::span<const u32> code,
                      size_t perm_idx, const Shader::Backend::Bindings& start, std::span<const u32> spirv,
                      const Shader::Profile& profile, const AmdGpu::Regs& regs, const AmdGpu::ComputeProgram& cs,
                      u16 vertex_sgpr_offset, u16 instance_sgpr_offset) {
    GS::CaptureWriter* writer = GS::GetCaptureWriter();
    if (writer == nullptr || !writer->IsOpen()) {
        return;
    }
    GS::CapturedModule m;
    m.hw = FromHw(info.hw_stage);
    m.sw = static_cast<GS::SwStage>(info.sw_stage);
    m.perm_idx = static_cast<u32>(perm_idx);
    m.footer_hash = info.pgm_hash;
    m.address = info.pgm_base;
    m.start = {start.unified, start.buffer};
    m.vertex_sgpr_offset = vertex_sgpr_offset;
    m.instance_sgpr_offset = instance_sgpr_offset;
    std::copy_n(info.user_data.data(), std::min<size_t>(16, info.user_data.size()), m.user_data.begin());
    m.code.assign(code.begin(), code.end());
    m.flat = info.flattened_ud_buf;
    if (info.has_fetch_shader) {
        // what the bridge's translation itself read moments ago (its thread, direct reads as it does)
        Shader::Gcn::FetchShaderData fetch;
        if (Shader::Gcn::ParseFetchShader(info, fetch)) {
            m.has_fetch = true;
            m.fetch_sgpr_base = info.fetch_shader_sgpr_base;
            const u32* fc = Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base);
            m.fetch_code.assign(fc, fc + fetch.size / 4);
            for (const auto& a : fetch.attributes) {
                const auto v = info.ReadUdReg<AmdGpu::Buffer>(a.sgpr_base, a.dword_offset);
                std::array<u32, 4> raw{};
                std::memcpy(raw.data(), &v, 16);
                m.vertex_sharps.push_back(raw);
            }
        }
    }
    m.sh.assign(regs.reg_array.begin() + 0x2C00, regs.reg_array.begin() + 0x3000);
    m.context.assign(regs.reg_array.begin() + 0xA000, regs.reg_array.begin() + 0xA400);
    m.primitive_type = regs.reg_array[0xC242];
    std::memcpy(m.cs.data(), &cs, sizeof(m.cs));
    m.spirv.assign(spirv.begin(), spirv.end());
    m.profile = FromProfile(profile);
    const auto* rb = reinterpret_cast<const u8*>(&runtime);
    m.bridge_runtime.assign(rb, rb + sizeof(runtime));
    writer->Write(m);
}

void OnPipeline(bool compute, const std::array<const Shader::Info*, 6>& infos,
                const std::array<vk::ShaderModule, 6>& modules, const std::array<Shader::RuntimeInfo, 6>& runtimes,
                const void* pipeline) {
    const GS::BridgePipelineCallback callback = GS::GetBridgePipelineCallback();
    if (callback == nullptr) {
        return;
    }
    GS::BridgePipelineView view;
    view.compute = compute;
    view.pipeline = reinterpret_cast<u64>(pipeline);
    const auto fill = [&](size_t slot, const Shader::Info* info, vk::ShaderModule module,
                          const Shader::RuntimeInfo& runtime) {
        if (info == nullptr) {
            return;
        }
        GS::BridgeStageView& s = view.stages[slot];
        s.present = true;
        s.footer_hash = info->pgm_hash;
        s.module = reinterpret_cast<u64>(static_cast<VkShaderModule>(module));
        s.flat = std::span<const u32>(info->flattened_ud_buf);
        s.runtime = std::span<const u8>(reinterpret_cast<const u8*>(&runtime), sizeof(runtime));
        s.buffers = static_cast<u32>(info->buffers.size());
        s.images = static_cast<u32>(info->images.size());
        s.samplers = static_cast<u32>(info->samplers.size());
        s.mrt_mask = info->mrt_mask;
        s.has_fetch = info->has_fetch_shader;
    };
    if (compute) {
        // the compute stage's program is in slot 0, its runtime information at the logical stage's index
        fill(static_cast<size_t>(GS::SwStage::Compute), infos[0], modules[0],
             runtimes[static_cast<size_t>(Shader::SwStage::Compute)]);
    } else {
        for (size_t s = 0; s < 6; ++s) {
            fill(s, infos[s], modules[s], runtimes[s]);
        }
    }
    callback(view);
}

} // namespace ShaderHooks

} // namespace SecondSon::Bridge
