// SecondSonPC — a stage's runtime parameters from SecondSonPC's register file (Phase 4, P4.3;
// docs/PHASE4_P43_SHADER_TRANSLATION.md §5.3).
//
// The registers a program's translation depends on besides its code: the program's RSRC settings, the vertex
// stage's step rates and primitive type, the clipper and VS output controls, the fragment stage's inputs, export
// formats and colour targets, the compute stage's group size. Read from the register file the command processor
// keeps (P4.2) — the hardware's semantics, not the bridge's — as hardware fields (shader_types.h, RuntimeParams).
// The groups they live in are listed (kRuntimeGroups) so a caller rebuilds them only when one of those changed.
#pragma once

#include "base/abi.h"
#include "runtime/gpu/cp_registers.h"
#include "runtime/gpu/shader/shader_types.h"

namespace SecondSon::Gpu::Shader {

/// Register indices the runtime parameters read (the hardware's; the adapter checks them against the bridge's layout).
namespace Reg {
inline constexpr u32 CbTargetMask = 0xA08E;
inline constexpr u32 SpiPsInputCntl0 = 0xA191; ///< .. 0xA1B0
inline constexpr u32 SpiPsInputEna = 0xA1B3;
inline constexpr u32 SpiPsInputAddr = 0xA1B4;
inline constexpr u32 SpiPsInControl = 0xA1B6;
inline constexpr u32 SpiBarycCntl = 0xA1B8;
inline constexpr u32 SpiShaderZFormat = 0xA1C4;
inline constexpr u32 SpiShaderColFormat = 0xA1C5;
inline constexpr u32 CbBlend0Control = 0xA1E0; ///< .. 0xA1E7
inline constexpr u32 CbColorControl = 0xA202;
inline constexpr u32 DbShaderControl = 0xA203;
inline constexpr u32 PaClClipCntl = 0xA204;
inline constexpr u32 PaClVsOutCntl = 0xA207;
inline constexpr u32 VgtInstanceStepRate0 = 0xA2A8;
inline constexpr u32 VgtInstanceStepRate1 = 0xA2A9;
inline constexpr u32 VgtShaderStagesEn = 0xA2D5;
inline constexpr u32 PaScAaConfig = 0xA2F8;
inline constexpr u32 CbColor0Base = 0xA318; ///< + 15 per target
inline constexpr u32 CbColorInfoOffset = 4;
inline constexpr u32 CbColorStride = 15;
inline constexpr u32 VgtPrimitiveType = 0xC242;
/// SPI_SHADER_PGM_LO of each graphics stage (PS, VS, GS, ES, HS, LS): + 2 RSRC1, + 3 RSRC2, + 4 USER_DATA_0.
inline constexpr u32 kPgmLo[6] = {0x2C08, 0x2C48, 0x2C88, 0x2CC8, 0x2D08, 0x2D48};
// compute, relative to COMPUTE_DISPATCH_INITIATOR (0x2E00)
inline constexpr u32 ComputeNumThreadX = 0x07;
inline constexpr u32 ComputePgmLo = 0x0C;
inline constexpr u32 ComputePgmRsrc1 = 0x12;
inline constexpr u32 ComputePgmRsrc2 = 0x13;
inline constexpr u32 ComputeUserData0 = 0x40;
} // namespace Reg

inline constexpr u32 kPrimRectList = 17, kPrimQuadList = 19;

/// The register groups (P4.2) the graphics runtime parameters read: a draw whose changed groups miss all of them keeps
/// the previous draw's parameters.
inline constexpr Cp::GroupMask kRuntimeGroups =
    Cp::Bit(Cp::Group::PsProgram) | Cp::Bit(Cp::Group::VsProgram) | Cp::Bit(Cp::Group::GsProgram) |
    Cp::Bit(Cp::Group::EsProgram) | Cp::Bit(Cp::Group::HsProgram) | Cp::Bit(Cp::Group::LsProgram) |
    Cp::Bit(Cp::Group::ColorFormat) | Cp::Bit(Cp::Group::ColorTarget) | Cp::Bit(Cp::Group::Blend) |
    Cp::Bit(Cp::Group::Raster) | Cp::Bit(Cp::Group::ShaderInterface) | Cp::Bit(Cp::Group::Geometry) |
    Cp::Bit(Cp::Group::ContextOther) | Cp::Bit(Cp::Group::PrimitiveType);

/// The logical stages VGT_SHADER_STAGES_EN enables.
enum class StageConfig : u8 { Vs, EsGs, LsHs, LsHsEsGs, Unknown };
StageConfig DecodeStages(u32 stages_en);

/// The runtime parameters of graphics stage `hw` translated as `sw`. `vertex_sgpr_offset`/`instance_sgpr_offset`:
/// an indirect draw's (0 otherwise). false: a stage SecondSonPC does not prepare yet (GS/ES/HS/LS: none in the
/// game's scenes so far, docs §4.4).
bool BuildGraphicsRuntime(const Cp::RegisterFile& regs, HwStage hw, SwStage sw, u32 vertex_sgpr_offset,
                          u32 instance_sgpr_offset, RuntimeParams& out);

/// The compute stage's runtime parameters from a queue's compute registers (COMPUTE_*, 0x50 dwords).
void BuildComputeRuntime(std::span<const u32, Cp::kComputeCount> cs, RuntimeParams& out);

} // namespace SecondSon::Gpu::Shader
