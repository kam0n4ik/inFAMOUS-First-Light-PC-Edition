// SecondSonPC — a stage's runtime parameters from SecondSonPC's register file (P4.3). See runtime_params.h.
#include "runtime/gpu/shader/runtime_params.h"

namespace SecondSon::Gpu::Shader {

namespace {

constexpr u32 kSampleCoverageEna = 1u << 14; // SPI_PS_INPUT_ENA/ADDR.SAMPLE_COVERAGE_ENA

bool IsSrc1Factor(u32 factor) {
    return factor >= 15 && factor <= 18; // SRC1_COLOR, INV_SRC1_COLOR, SRC1_ALPHA, INV_SRC1_ALPHA
}

} // namespace

StageConfig DecodeStages(u32 stages_en) {
    switch (stages_en) { // the whole register, as the bridge compares it (LS/HS/ES/GS/VS_EN, DYNAMIC_HS)
    case 0x00:
        return StageConfig::Vs;
    case 0xB0:
        return StageConfig::EsGs;
    case 0x45:
        return StageConfig::LsHs;
    case 0xAD:
        return StageConfig::LsHsEsGs;
    default:
        return StageConfig::Unknown;
    }
}

bool BuildGraphicsRuntime(const Cp::RegisterFile& regs, HwStage hw, SwStage sw, u32 vertex_sgpr_offset,
                          u32 instance_sgpr_offset, RuntimeParams& out) {
    out = {};
    out.hw = hw;
    out.sw = sw;
    const auto stage = static_cast<u32>(hw);
    if (stage >= 6) {
        return false;
    }
    out.rsrc1 = regs.Get(Reg::kPgmLo[stage] + 2) & Rsrc1TranslationMask(hw);
    out.rsrc2 = regs.Get(Reg::kPgmLo[stage] + 3) & Rsrc2TranslationMask(hw);
    const u32 prim = regs.Get(Reg::VgtPrimitiveType);
    switch (hw) {
    case HwStage::Vs:
        if (sw != SwStage::Vertex) {
            return false; // a VS translated as the tessellation evaluation stage (LS/HS): not prepared yet
        }
        out.step_rate0 = regs.Get(Reg::VgtInstanceStepRate0);
        out.step_rate1 = regs.Get(Reg::VgtInstanceStepRate1);
        out.rect_list = prim == kPrimRectList ? 1 : 0;
        out.quad_list = prim == kPrimQuadList ? 1 : 0;
        out.vertex_sgpr_offset = vertex_sgpr_offset;
        out.instance_sgpr_offset = instance_sgpr_offset;
        out.vs_out_cntl = regs.Get(Reg::PaClVsOutCntl) & 0x3FFFFFFu;
        out.clip_cntl = regs.Get(Reg::PaClClipCntl) & kClipCntlMask;
        return true;
    case HwStage::Ps: {
        out.ps_input_ena = regs.Get(Reg::SpiPsInputEna) & 0xFFFF;
        out.ps_input_addr = regs.Get(Reg::SpiPsInputAddr) & 0xFFFF;
        out.num_interp = regs.Get(Reg::SpiPsInControl) & 0x3F;
        out.front_face_all_bits = (regs.Get(Reg::SpiBarycCntl) >> 24) & 1;
        out.db_shader_control = regs.Get(Reg::DbShaderControl) & kDbShaderMask;
        out.z_format = regs.Get(Reg::SpiShaderZFormat) & 0xF;
        out.col_format = regs.Get(Reg::SpiShaderColFormat);
        if ((out.ps_input_ena & out.ps_input_addr & kSampleCoverageEna) != 0) {
            out.aa_samples_log2 = regs.Get(Reg::PaScAaConfig) & 7;
        }
        const u32 blend0 = regs.Get(Reg::CbBlend0Control);
        if ((blend0 >> 30) & 1) {
            bool dual = IsSrc1Factor(blend0 & 0x1F) || IsSrc1Factor((blend0 >> 8) & 0x1F);
            if ((blend0 >> 29) & 1) { // separate alpha blend
                dual |= IsSrc1Factor((blend0 >> 16) & 0x1F) || IsSrc1Factor((blend0 >> 24) & 0x1F);
            }
            out.dual_source = dual ? 1 : 0;
        }
        out.stages_en = regs.Get(Reg::VgtShaderStagesEn) & 0x1FF;
        out.ps_vs_out_cntl = regs.Get(Reg::PaClVsOutCntl) & 0xFF;
        out.ps_ucp_enable = regs.Get(Reg::PaClClipCntl) & 0x3F;
        for (u32 i = 0; i < out.num_interp && i < 32; ++i) {
            out.ps_input_cntl[i] = regs.Get(Reg::SpiPsInputCntl0 + i) & kPsInputCntlMask;
        }
        // colour targets, as the pipeline's first pass sees them (writes disabled by CB_COLOR_CONTROL: none bound)
        const bool writes = ((regs.Get(Reg::CbColorControl) >> 4) & 7) != 0;
        const u32 target_mask = regs.Get(Reg::CbTargetMask);
        for (u32 cb = 0; cb < 8 && writes; ++cb) {
            const u32 base = regs.Get(Reg::CbColor0Base + cb * Reg::CbColorStride);
            const u32 info = regs.Get(Reg::CbColor0Base + cb * Reg::CbColorStride + Reg::CbColorInfoOffset);
            const u32 mask = (target_mask >> (cb * 4)) & 0xF;
            if (base == 0 || ((info >> 2) & 0x1F) == 0 || mask == 0) {
                continue;
            }
            ColorTargetParams& t = out.targets[cb];
            t.bound = 1;
            t.info = info & kColorInfoMask;
            const u32 bc = regs.Get(Reg::CbBlend0Control + cb);
            const u32 func = (bc >> 5) & 7;
            t.min_max_self = ((bc >> 30) & 1) != 0 && ((info >> 16) & 1) == 0 && (func == 2 || func == 3) &&
                                     (bc & 0x1F) == 2 && ((bc >> 8) & 0x1F) == 8
                                 ? 1
                                 : 0;
        }
        return true;
    }
    default:
        return false; // GS/ES/HS/LS
    }
}

void BuildComputeRuntime(std::span<const u32, Cp::kComputeCount> cs, RuntimeParams& out) {
    out = {};
    out.hw = HwStage::Cs;
    out.sw = SwStage::Compute;
    out.rsrc1 = cs[Reg::ComputePgmRsrc1] & Rsrc1TranslationMask(HwStage::Cs);
    out.rsrc2 = cs[Reg::ComputePgmRsrc2] & Rsrc2TranslationMask(HwStage::Cs);
    for (u32 i = 0; i < 3; ++i) {
        out.threads[i] = cs[Reg::ComputeNumThreadX + i] & 0xFFFF;
    }
}

} // namespace SecondSon::Gpu::Shader
