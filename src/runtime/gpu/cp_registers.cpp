// SecondSonPC — the command processor's register file (P4.2). See cp_registers.h.
#include "runtime/gpu/cp_registers.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <vector>

namespace SecondSon::Gpu::Cp {

namespace {

constexpr u32 Idx(u32 reg) {
    for (u32 s = 0; s < static_cast<u32>(Space::Count); ++s) {
        if (reg >= kSpaceBase[s] && reg - kSpaceBase[s] < kSpaceCount[s]) {
            return kSpaceFirst[s] + (reg - kSpaceBase[s]);
        }
    }
    return kNumRegs;
}

// The register → group table: every register of the windows has exactly one group. Context register names after
// the Sea Islands register reference (the hardware's), indices as the bridge's Regs static_asserts confirm.
constexpr std::array<u8, kNumRegs> MakeGroups() {
    std::array<u8, kNumRegs> t{};
    const auto set = [&t](u32 first, u32 last, Group g) {
        for (u32 r = first; r <= last; ++r) {
            t[Idx(r)] = static_cast<u8>(g);
        }
    };
    set(0x2000, 0x2BFF, Group::Config);
    set(0x2C00, 0x2FFF, Group::ShOther);
    constexpr u32 kStages[] = {Reg::SpiShaderPgmLoPs, Reg::SpiShaderPgmLoVs, Reg::SpiShaderPgmLoGs,
                               Reg::SpiShaderPgmLoEs, Reg::SpiShaderPgmLoHs, Reg::SpiShaderPgmLoLs};
    for (u32 s = 0; s < 6; ++s) {
        set(kStages[s], kStages[s] + 3, static_cast<Group>(static_cast<u32>(Group::PsProgram) + s));
        set(kStages[s] + 4, kStages[s] + 19, static_cast<Group>(static_cast<u32>(Group::PsUserData) + s));
    }
    set(0xA000, 0xA3FF, Group::ContextOther);
    set(0xA002, 0xA002, Group::DepthTarget); // DB_DEPTH_VIEW
    set(0xA005, 0xA005, Group::DepthTarget); // DB_HTILE_DATA_BASE
    set(0xA008, 0xA009, Group::DepthBounds);
    set(0xA00A, 0xA00B, Group::ClearValues);
    set(0xA00C, 0xA00D, Group::Scissor);     // PA_SC_SCREEN_SCISSOR_TL/BR
    set(0xA00F, 0xA011, Group::DepthFormat); // DB_DEPTH_INFO, DB_Z_INFO, DB_STENCIL_INFO
    set(0xA012, 0xA017, Group::DepthTarget); // read/write bases, DB_DEPTH_SIZE, DB_DEPTH_SLICE
    set(0xA020, 0xA021, Group::ContextResource);
    set(0xA080, 0xA08B, Group::Scissor);    // window offset, window scissor, clip rects
    set(0xA08C, 0xA08C, Group::Raster);     // PA_SC_EDGERULE
    set(0xA08D, 0xA08D, Group::Scissor);    // PA_SU_HARDWARE_SCREEN_OFFSET
    set(0xA08E, 0xA08F, Group::Blend);      // CB_TARGET_MASK, CB_SHADER_MASK
    set(0xA090, 0xA0B3, Group::Scissor);    // generic and viewport scissors
    set(0xA0B4, 0xA0D3, Group::Viewport);   // PA_SC_VPORT_ZMIN/ZMAX
    set(0xA100, 0xA102, Group::DrawParams); // VGT_MAX/MIN_VTX_INDX, VGT_INDX_OFFSET
    set(0xA103, 0xA103, Group::Geometry);   // VGT_MULTI_PRIM_IB_RESET_INDX
    set(0xA105, 0xA108, Group::BlendConstants);
    set(0xA10B, 0xA10B, Group::DepthStencil); // DB_STENCIL_CONTROL
    set(0xA10C, 0xA10D, Group::StencilRef);
    set(0xA10F, 0xA16E, Group::Viewport); // PA_CL_VPORT_*
    set(0xA16F, 0xA186, Group::Raster);   // PA_CL_UCP_*
    set(0xA191, 0xA1C5, Group::ShaderInterface);
    set(0xA1E0, 0xA1E7, Group::Blend); // CB_BLENDn_CONTROL
    set(0xA1F9, 0xA1FA, Group::IndexState);
    set(0xA1FC, 0xA1FC, Group::DrawParams);
    set(0xA200, 0xA200, Group::DepthStencil);    // DB_DEPTH_CONTROL
    set(0xA201, 0xA201, Group::Raster);          // DB_EQAA
    set(0xA202, 0xA202, Group::Blend);           // CB_COLOR_CONTROL
    set(0xA203, 0xA203, Group::ShaderInterface); // DB_SHADER_CONTROL
    set(0xA204, 0xA206, Group::Raster);          // PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL
    set(0xA207, 0xA207, Group::ShaderInterface); // PA_CL_VS_OUT_CNTL
    set(0xA208, 0xA208, Group::Raster);          // PA_CL_NANINF_CNTL
    set(0xA280, 0xA283, Group::Raster);          // point size/min-max, line control, line stipple
    set(0xA284, 0xA291, Group::Geometry);        // output path, tessellation, GS mode/on-chip
    set(0xA292, 0xA293, Group::Raster);          // PA_SC_MODE_CNTL_0/1
    set(0xA294, 0xA29C, Group::Geometry);
    set(0xA29D, 0xA29F, Group::IndexState); // VGT_DMA_SIZE, VGT_DMA_MAX_SIZE, VGT_DMA_INDEX_TYPE
    set(0xA2A0, 0xA2DB, Group::Geometry);
    set(0xA2A2, 0xA2A2, Group::DrawParams);  // VGT_DMA_NUM_INSTANCES
    set(0xA2AF, 0xA2AF, Group::DepthTarget); // DB_HTILE_SURFACE
    set(0xA2DC, 0xA2DC, Group::Blend);       // DB_ALPHA_TO_MASK
    set(0xA2DE, 0xA2DE, Group::Raster);      // PA_SU_POLY_OFFSET_DB_FMT_CNTL
    set(0xA2DF, 0xA2E3, Group::DepthBias);
    set(0xA2E4, 0xA2EF, Group::Geometry); // GS instances, streamout
    set(0xA2F0, 0xA317, Group::Raster);   // centroid priority, AA config/sample locations/mask, guard band
    for (u32 n = 0; n < 8; ++n) {
        const u32 b = Reg::CbColor0Base + n * Reg::CbColorStride;
        set(b, b + Reg::CbColorStride - 1, Group::ColorTarget);
        set(b + 4, b + 5, Group::ColorFormat); // CB_COLORn_INFO, CB_COLORn_ATTRIB
    }
    set(0xC000, 0xCFFF, Group::UconfigOther);
    set(Reg::VgtPrimitiveType, Reg::VgtPrimitiveType, Group::PrimitiveType);
    set(Reg::VgtNumIndices, Reg::VgtNumInstances, Group::DrawParams);
    return t;
}

constexpr std::array<u8, kNumRegs> kGroups = MakeGroups();

// The clear state (fpPS4 pm4_pfp, as the bridge's Regs::SetDefaults): context offset 0x80.. and single registers.
constexpr u32 kDefaultsAt80[] = {
    0x00000000u, 0x80000000u, 0x40004000u, 0xdeadbeefu, 0x00000000u, 0x40004000u, 0x00000000u, 0x40004000u,
    0x00000000u, 0x40004000u, 0x00000000u, 0x40004000u, 0xaa99aaaau, 0x00000000u, 0xdeadbeefu, 0xdeadbeefu,
    0x80000000u, 0x40004000u, 0x00000000u, 0x00000000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u,
    0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u,
    0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u,
    0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u,
    0x80000000u, 0x40004000u, 0x80000000u, 0x40004000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u,
    0x00000000u, 0x3f800000u, 0x00000000u, 0x3f800000u, 0x2a00161au,
};
struct DefaultValue {
    u32 offset;
    u32 value;
};
constexpr DefaultValue kDefaultsSingle[] = {
    {0x000d, 0x40004000u}, {0x01b6, 0x00000002u}, {0x0204, 0x00090000u}, {0x0205, 0x00000004u},
    {0x0295, 0x00000100u}, {0x0296, 0x00000080u}, {0x0297, 0x00000002u}, {0x02aa, 0x00001000u},
    {0x02f7, 0x00001000u}, {0x02f9, 0x00000005u}, {0x02fa, 0x3f800000u}, {0x02fb, 0x3f800000u},
    {0x02fc, 0x3f800000u}, {0x02fd, 0x3f800000u}, {0x0316, 0x0000000eu}, {0x0317, 0x00000010u},
};

using GroupLists = std::array<std::vector<u16>, static_cast<size_t>(Group::Count)>;

const GroupLists& Lists() {
    static const GroupLists lists = [] {
        GroupLists l;
        for (u32 i = 0; i < kNumRegs; ++i) {
            l[kGroups[i]].push_back(static_cast<u16>(i));
        }
        return l;
    }();
    return lists;
}

constexpr GroupMask kAllGroups = ~GroupMask{0} >> (64 - static_cast<u32>(Group::Count));

} // namespace

std::string_view GroupName(Group group) {
    static constexpr std::string_view kNames[] = {
        "ps-program",   "vs-program",       "gs-program",    "es-program",       "hs-program",    "ls-program",
        "ps-user-data", "vs-user-data",     "gs-user-data",  "es-user-data",     "hs-user-data",  "ls-user-data",
        "sh-other",     "color-format",     "color-target",  "depth-format",     "depth-target",  "depth-bounds",
        "clear-values", "depth-stencil",    "stencil-ref",   "blend",            "blend-consts",  "viewport",
        "scissor",      "raster",           "depth-bias",    "shader-interface", "geometry",      "index-state",
        "draw-params",  "context-resource", "context-other", "primitive-type",   "uconfig-other", "config"};
    static_assert(std::size(kNames) == static_cast<size_t>(Group::Count));
    return group < Group::Count ? kNames[static_cast<size_t>(group)] : "?";
}

std::string_view SpaceName(Space space) {
    static constexpr std::string_view kNames[] = {"config", "sh", "context", "uconfig"};
    return space < Space::Count ? kNames[static_cast<size_t>(space)] : "?";
}

Group GroupOf(u32 reg) {
    const u32 i = Idx(reg);
    return i < kNumRegs ? static_cast<Group>(kGroups[i]) : Group::Count;
}

u32 RegisterOfIndex(u32 index) {
    for (u32 s = static_cast<u32>(Space::Count); s-- > 0;) {
        if (index >= kSpaceFirst[s]) {
            return kSpaceBase[s] + (index - kSpaceFirst[s]);
        }
    }
    return 0;
}

std::span<const u16> GroupRegisters(Group group) {
    if (group >= Group::Count) {
        return {};
    }
    return Lists()[static_cast<size_t>(group)];
}

void ContextDefaults(std::span<u32, 0x400> context) {
    std::fill(context.begin(), context.end(), 0u);
    std::copy(std::begin(kDefaultsAt80), std::end(kDefaultsAt80), context.begin() + 0x80);
    for (const auto& d : kDefaultsSingle) {
        context[d.offset] = d.value;
    }
}

RegisterFile::RegisterFile() {
    Reset();
}

void RegisterFile::Changed(GroupMask changed) {
    dirty_ |= changed;
    while (changed != 0) {
        ++versions_[std::countr_zero(changed)];
        changed &= changed - 1;
    }
}

bool RegisterFile::Write(Space space, u32 offset, const u32* values, u32 count) {
    const u32 s = static_cast<u32>(space);
    if (s >= static_cast<u32>(Space::Count) || offset > kSpaceCount[s] || count > kSpaceCount[s] - offset) {
        return false;
    }
    const u32 first = kSpaceFirst[s] + offset;
    u32* dst = values_.data() + first;
    const u8* groups = kGroups.data() + first;
    GroupMask changed = 0;
    u32 n = 0;
    for (u32 i = 0; i < count; ++i) {
        const u32 v = values[i];
        if (dst[i] != v) {
            group_hash_[groups[i]] ^= RegMix(first + i, dst[i]) ^ RegMix(first + i, v);
            dst[i] = v;
            changed |= GroupMask{1} << groups[i];
            ++n;
        }
    }
    if (mirror_ != nullptr) {
        std::memcpy(mirror_ + first, values, size_t{count} * sizeof(u32));
    }
    if (sink_ != nullptr && (changed != 0 || !sink_changes_only_)) {
        sink_->OnWrite(space, offset, values, count);
    }
    writes_ += count;
    if (changed != 0) {
        changes_ += n;
        Changed(changed);
    }
    return true;
}

bool RegisterFile::WriteReg(u32 reg, u32 value) {
    for (u32 s = 0; s < static_cast<u32>(Space::Count); ++s) {
        if (reg >= kSpaceBase[s] && reg - kSpaceBase[s] < kSpaceCount[s]) {
            return Write(static_cast<Space>(s), reg - kSpaceBase[s], &value, 1);
        }
    }
    return false;
}

void RegisterFile::ClearContext() {
    std::array<u32, 0x400> defaults;
    ContextDefaults(defaults);
    Write(Space::Context, 0, defaults.data(), 0x400);
}

void RegisterFile::RehashAll() {
    group_hash_.fill(0);
    for (u32 i = 0; i < kNumRegs; ++i) {
        group_hash_[kGroups[i]] ^= RegMix(i, values_[i]);
    }
}

void RegisterFile::Reset() {
    values_.fill(0);
    ContextDefaults(std::span<u32, 0x400>(values_.data() + kSpaceFirst[static_cast<u32>(Space::Context)], 0x400));
    if (mirror_ != nullptr) {
        std::memcpy(mirror_, values_.data(), sizeof(values_));
    }
    if (sink_ != nullptr) {
        sink_->OnReset();
    }
    RehashAll();
    Changed(kAllGroups);
}

void RegisterFile::Load(std::span<const u32, kNumRegs> values) {
    std::copy(values.begin(), values.end(), values_.begin());
    if (mirror_ != nullptr) {
        std::memcpy(mirror_, values_.data(), sizeof(values_));
    }
    if (sink_ != nullptr) {
        sink_->OnLoad(values);
    }
    RehashAll();
    Changed(kAllGroups);
}

u64 RegisterFile::ComputeGroupHash(Group group) const {
    u64 h = 0;
    for (const u16 i : GroupRegisters(group)) {
        h ^= RegMix(i, values_[i]);
    }
    return h;
}

ComputeGroup ComputeRegisters::GroupOf(u32 offset) {
    if (offset <= 0x06) {
        return ComputeGroup::Dispatch; // initiator, dims, start
    }
    if ((offset >= 0x07 && offset <= 0x09) || offset == Reg::ComputePgmLo || offset == Reg::ComputePgmLo + 1 ||
        offset == Reg::ComputePgmRsrc1 || offset == Reg::ComputePgmRsrc1 + 1) {
        return ComputeGroup::Program; // thread group size, program, resources
    }
    if (offset >= Reg::ComputeUserData0 && offset < Reg::ComputeUserData0 + 16) {
        return ComputeGroup::UserData;
    }
    return ComputeGroup::Other;
}

bool ComputeRegisters::Write(u32 offset, const u32* src, u32 count) {
    if (offset > kComputeCount || count > kComputeCount - offset) {
        return false;
    }
    u8 changed = 0;
    for (u32 i = 0; i < count; ++i) {
        u32& r = values[offset + i];
        if (r != src[i]) {
            r = src[i];
            changed |= static_cast<u8>(1u << static_cast<u32>(GroupOf(offset + i)));
        }
    }
    dirty |= changed;
    for (u32 g = 0; g < static_cast<u32>(ComputeGroup::Count); ++g) {
        if (changed & (1u << g)) {
            ++versions[g];
        }
    }
    return true;
}

} // namespace SecondSon::Gpu::Cp
