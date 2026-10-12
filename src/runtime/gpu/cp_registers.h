// SecondSonPC — the Liverpool register file as SecondSonPC's command processor keeps it (Phase 4, P4.2;
// docs/PHASE4_P42_COMMAND_PROCESSOR.md §4).
//
// The graphics registers the PM4 stream sets live in four windows of the hardware's register space (dword
// indices): config [0x2000, 0x2C00), SH [0x2C00, 0x3000), context [0xA000, 0xA400), uconfig [0xC000, 0xD000).
// A write outside them is refused (the caller reports the packet), never written somewhere else. The compute
// registers [0x2E00, 0x2E50) are not here: each queue has its own (ComputeRegisters), as the hardware keeps them
// per queue.
//
// Every register belongs to exactly one group (Group): the shader program of a stage, a stage's user data, the
// colour/depth targets, the viewport, the blend state, ... A write that changes a value (compared before the store)
// marks its group dirty and bumps the group's version; a write of the same value changes nothing. Whatever is built
// from registers (a draw's description, the pipeline key) is rebuilt from the dirty groups only, and a consumer can
// keep its own results per group version. Nothing is treated as unchanged on a partial set of registers: a group is
// all of its registers, and the groups cover every register of the windows.
#pragma once

#include <array>
#include <span>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Gpu::Cp {

/// Register spaces of SET_*_REG (the PM4 offset is relative to the space's base).
enum class Space : u8 { Config, Sh, Context, Uconfig, Count };

inline constexpr u32 kSpaceBase[] = {0x2000, 0x2C00, 0xA000, 0xC000};
inline constexpr u32 kSpaceCount[] = {0xC00, 0x400, 0x400, 0x1000};
/// The windows' registers stored one after another: config, SH, context, uconfig.
inline constexpr u32 kSpaceFirst[] = {0, 0xC00, 0x1000, 0x1400};
inline constexpr u32 kNumRegs = 0x2400;

/// Compute registers per queue: COMPUTE_DISPATCH_INITIATOR (0x2E00) .. COMPUTE_USER_DATA_15 (0x2E4F).
inline constexpr u32 kComputeBase = 0x2E00;
inline constexpr u32 kComputeCount = 0x50;

/// Registers named by the command processor (hardware register indices).
namespace Reg {
inline constexpr u32 SpiShaderPgmLoPs = 0x2C08; ///< + 1 HI, + 2 RSRC1, + 3 RSRC2, + 4 USER_DATA_0 .. + 19
inline constexpr u32 SpiShaderPgmLoVs = 0x2C48;
inline constexpr u32 SpiShaderPgmLoGs = 0x2C88;
inline constexpr u32 SpiShaderPgmLoEs = 0x2CC8;
inline constexpr u32 SpiShaderPgmLoHs = 0x2D08;
inline constexpr u32 SpiShaderPgmLoLs = 0x2D48;
inline constexpr u32 DbZInfo = 0xA010;
inline constexpr u32 CbColor0Base = 0xA318; ///< + 15 per colour target
inline constexpr u32 CbColorStride = 15;
inline constexpr u32 VgtDmaBaseHi = 0xA1F9;
inline constexpr u32 VgtDmaBase = 0xA1FA;
inline constexpr u32 VgtDrawInitiator = 0xA1FC;
inline constexpr u32 VgtIndxOffset = 0xA102;
inline constexpr u32 VgtDmaSize = 0xA29D;
inline constexpr u32 VgtDmaMaxSize = 0xA29E;
inline constexpr u32 VgtDmaIndexType = 0xA29F;
inline constexpr u32 VgtShaderStagesEn = 0xA2D5;
inline constexpr u32 VgtPrimitiveType = 0xC242;
inline constexpr u32 VgtNumIndices = 0xC24C;
inline constexpr u32 VgtNumInstances = 0xC24D;
// compute, relative to kComputeBase
inline constexpr u32 ComputeDispatchInitiator = 0x00;
inline constexpr u32 ComputeDimX = 0x01;     ///< + 1 Y, + 2 Z
inline constexpr u32 ComputePgmLo = 0x0C;    ///< + 1 HI
inline constexpr u32 ComputePgmRsrc1 = 0x12; ///< + 1 RSRC2
inline constexpr u32 ComputeUserData0 = 0x40;
} // namespace Reg

/// What a register is about. The order is the bit order of the dirty mask.
enum class Group : u8 {
    // SH: shader programs (PGM_LO/HI/RSRC1/RSRC2) and user data of each graphics stage
    PsProgram,
    VsProgram,
    GsProgram,
    EsProgram,
    HsProgram,
    LsProgram,
    PsUserData,
    VsUserData,
    GsUserData,
    EsUserData,
    HsUserData,
    LsUserData,
    ShOther,
    // context
    ColorFormat,     ///< CB_COLORn_INFO/ATTRIB: formats, samples (the pipeline's attachment formats)
    ColorTarget,     ///< the rest of CB_COLORn: addresses, pitch, slice, view, CMASK/FMASK, clear words
    DepthFormat,     ///< DB_DEPTH_INFO, DB_Z_INFO, DB_STENCIL_INFO
    DepthTarget,     ///< DB_DEPTH_VIEW, HTILE base/surface, Z/stencil read/write bases, depth size/slice
    DepthBounds,     ///< DB_DEPTH_BOUNDS_MIN/MAX
    ClearValues,     ///< DB_STENCIL_CLEAR, DB_DEPTH_CLEAR
    DepthStencil,    ///< DB_DEPTH_CONTROL, DB_STENCIL_CONTROL
    StencilRef,      ///< DB_STENCILREFMASK(_BF)
    Blend,           ///< CB_BLENDn_CONTROL, CB_COLOR_CONTROL, CB_TARGET_MASK, CB_SHADER_MASK, alpha to mask
    BlendConstants,  ///< CB_BLEND_RED..ALPHA
    Viewport,        ///< PA_CL_VPORT_*, PA_SC_VPORT_ZMIN/ZMAX
    Scissor,         ///< screen/window/generic/viewport scissors, window offset, clip rects
    Raster,          ///< clip/cull/polygon mode, AA config, point/line, guard band, clip planes
    DepthBias,       ///< PA_SU_POLY_OFFSET_CLAMP/FRONT/BACK
    ShaderInterface, ///< SPI_PS_INPUT_*, SPI_VS_OUT_CONFIG, export formats, DB_SHADER_CONTROL, PA_CL_VS_OUT_CNTL
    Geometry,        ///< VGT stages, GS/tessellation, primitive restart, instance step rates, streamout
    IndexState,      ///< VGT_DMA_BASE(_HI), VGT_DMA_SIZE, VGT_DMA_MAX_SIZE, VGT_DMA_INDEX_TYPE
    DrawParams,      ///< VGT_DRAW_INITIATOR, VGT_NUM_INDICES/INSTANCES, VGT_INDX_OFFSET, min/max index
    ContextResource, ///< TA_BC_BASE (border colours)
    ContextOther,    ///< every other context register (kept in the pipeline key: unknown means it may matter)
    // uconfig
    PrimitiveType, ///< VGT_PRIMITIVE_TYPE
    UconfigOther,
    // config
    Config,
    Count,
};
static_assert(static_cast<u32>(Group::Count) <= 64);

using GroupMask = u64;
inline constexpr GroupMask Bit(Group g) {
    return GroupMask{1} << static_cast<u32>(g);
}

/// Groups whose registers decide a graphics pipeline (Vulkan pipeline key; ContextOther and ShOther conservatively).
inline constexpr GroupMask kPipelineGroups =
    Bit(Group::PsProgram) | Bit(Group::VsProgram) | Bit(Group::GsProgram) | Bit(Group::EsProgram) |
    Bit(Group::HsProgram) | Bit(Group::LsProgram) | Bit(Group::ShOther) | Bit(Group::ColorFormat) |
    Bit(Group::DepthFormat) | Bit(Group::DepthStencil) | Bit(Group::Blend) | Bit(Group::Raster) |
    Bit(Group::ShaderInterface) | Bit(Group::Geometry) | Bit(Group::ContextOther) | Bit(Group::PrimitiveType);
/// Groups a pipeline takes as dynamic state.
inline constexpr GroupMask kDynamicGroups = Bit(Group::Viewport) | Bit(Group::Scissor) | Bit(Group::BlendConstants) |
                                            Bit(Group::StencilRef) | Bit(Group::DepthBias) |
                                            Bit(Group::DepthBounds);

std::string_view GroupName(Group group);
std::string_view SpaceName(Space space);

/// The group of a register of the windows (Group::Count for one outside them).
Group GroupOf(u32 reg);

/// Flat index of a register of the windows; false outside them.
inline bool FlatIndex(u32 reg, u32& index) {
    for (u32 s = 0; s < static_cast<u32>(Space::Count); ++s) {
        if (reg >= kSpaceBase[s] && reg - kSpaceBase[s] < kSpaceCount[s]) {
            index = kSpaceFirst[s] + (reg - kSpaceBase[s]);
            return true;
        }
    }
    return false;
}
u32 RegisterOfIndex(u32 index);

/// The registers of `group`, flat indices in increasing order.
std::span<const u16> GroupRegisters(Group group);

/// Context register defaults after CLEAR_STATE (the values the hardware's clear state holds for the registers the
/// PS4 driver relies on; the same table as fpPS4's pm4_pfp and the bridge's Regs::SetDefaults). Every other context
/// register is 0.
void ContextDefaults(std::span<u32, 0x400> context);

/// Every change of a RegisterFile, in order, as it is made (P4.10: the parallel command processor's journal - a second
/// register file replaying it holds the same values and group versions at the same point of the stream).
class RegisterSink {
public:
    virtual ~RegisterSink() = default;
    virtual void OnWrite(Space space, u32 offset, const u32* values, u32 count) = 0;
    virtual void OnReset() = 0;
    virtual void OnLoad(std::span<const u32> values) = 0;
};

class RegisterFile {
public:
    RegisterFile();

    /// Writes `values` to `count` consecutive registers of `space` from `offset` (SET_*_REG). false (nothing
    /// written) if the range leaves the window.
    bool Write(Space space, u32 offset, const u32* values, u32 count);
    /// One register by its hardware index; false outside the windows.
    bool WriteReg(u32 reg, u32 value);

    u32 Get(u32 reg) const {
        u32 i = 0;
        return FlatIndex(reg, i) ? values_[i] : 0;
    }
    u32 At(u32 index) const {
        return values_[index];
    }
    const std::array<u32, kNumRegs>& Values() const {
        return values_;
    }

    /// CLEAR_STATE: the context registers back to their defaults (the other spaces stay, as on the hardware).
    void ClearContext();
    /// Everything to the power-on state: context defaults, the rest 0; all groups dirty.
    void Reset();
    /// Every register from `values` (a snapshot, e.g. a trace's start); all groups dirty.
    void Load(std::span<const u32, kNumRegs> values);

    GroupMask Dirty() const {
        return dirty_;
    }
    void ClearDirty(GroupMask mask) {
        dirty_ &= ~mask;
    }
    u32 Version(Group group) const {
        return versions_[static_cast<u32>(group)];
    }
    /// Counts: register writes, writes that changed a value.
    u64 Writes() const {
        return writes_;
    }
    u64 Changes() const {
        return changes_;
    }

    /// Every write also stored into `mirror` (kNumRegs registers, flat), or none. For validation: the comparison with
    /// the bridge keeps the registers as the bridge sees them there (cp_shadow.h).
    void SetMirror(u32* mirror) {
        mirror_ = mirror;
    }
    /// Every accepted Write, Reset and Load also reported to `sink` (nullptr: none).
    void SetSink(RegisterSink* sink) {
        sink_ = sink;
    }

    /// 64-bit content hash of a group's registers, kept up to date at every change (the XOR of a 64-bit mix of each
    /// register's index and value): O(1), no pass over the group's registers when a pipeline key is built.
    u64 GroupHash(Group group) const {
        return group_hash_[static_cast<size_t>(group)];
    }
    /// The same hash computed from the values (verification).
    u64 ComputeGroupHash(Group group) const;

    static u64 RegMix(u32 index, u32 value) {
        u64 x = (u64{index} << 32) | value; // splitmix64's finaliser
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }

private:
    void Changed(GroupMask changed);
    void RehashAll();

    std::array<u32, kNumRegs> values_{};
    GroupMask dirty_ = 0;
    std::array<u32, static_cast<size_t>(Group::Count)> versions_{};
    std::array<u64, static_cast<size_t>(Group::Count)> group_hash_{};
    u64 writes_ = 0;
    u64 changes_ = 0;
    u32* mirror_ = nullptr;
    RegisterSink* sink_ = nullptr;
};

/// One queue's compute registers [0x2E00, 0x2E50).
enum class ComputeGroup : u8 { Dispatch, Program, UserData, Other, Count };

struct ComputeRegisters {
    std::array<u32, kComputeCount> values{};
    u8 dirty = 0; ///< bits of ComputeGroup
    std::array<u32, static_cast<size_t>(ComputeGroup::Count)> versions{};

    static ComputeGroup GroupOf(u32 offset);
    /// `offset` relative to kComputeBase; false (nothing written) if the range leaves [0, kComputeCount).
    bool Write(u32 offset, const u32* src, u32 count);
};

} // namespace SecondSon::Gpu::Cp
