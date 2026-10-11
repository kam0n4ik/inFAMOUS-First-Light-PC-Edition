// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — a guest image's layout (Phase 4, P4.4): the guest pixel formats, the GCN tile modes and where every
// mip level and slice of a T#, a colour/depth target or a VideoOut buffer lies in guest memory: what SecondSonPC's
// texture cache needs to find, upload, detile and alias a guest image. Pure functions of descriptors and registers
// (nothing is dereferenced, no Vulkan type here: the pixel format is a VkFormat value that vk_formats.h maps).
// Every value is computed as the bridge's texture cache computes it, field by field, including its integer widths
// (a u16 level/layer count, u32 sizes that wrap); the self-test (selftest_formats.cpp) compares them. Where the bridge
// stops (ASSERT/UNREACHABLE: a format it has no host format for, tile modes 27..30, sample counts outside its tables,
// a stencil-only depth target) the layout says supported = false with the reason: never a crash, never a layout
// passed off as valid.
// Ported from shadPS4's amdgpu/{pixel_format,tiling}, texture_cache/{tile.h,image_info,tile_manager} (GPL-2.0-or-later).
#pragma once

#include <array>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Gpu::Shader {
struct BufferDesc;
struct ImageDesc;
} // namespace SecondSon::Gpu::Shader

namespace SecondSon::Gpu::Cp {
class RegisterFile;
} // namespace SecondSon::Gpu::Cp

namespace SecondSon::Gpu {

// ---- guest pixel formats (Sea Islands ISA, table 8.13)

enum class DataFormat : u32 {
    FormatInvalid = 0,
    Format8 = 1,
    Format16 = 2,
    Format8_8 = 3,
    Format32 = 4,
    Format16_16 = 5,
    Format10_11_11 = 6,
    Format11_11_10 = 7,
    Format10_10_10_2 = 8,
    Format2_10_10_10 = 9,
    Format8_8_8_8 = 10,
    Format32_32 = 11,
    Format16_16_16_16 = 12,
    Format32_32_32 = 13,
    Format32_32_32_32 = 14,
    Format5_6_5 = 16,
    Format1_5_5_5 = 17,
    Format5_5_5_1 = 18,
    Format4_4_4_4 = 19,
    Format8_24 = 20,
    Format24_8 = 21,
    FormatX24_8_32 = 22,
    FormatGB_GR = 32,
    FormatBG_RG = 33,
    Format5_9_9_9 = 34,
    FormatBc1 = 35,
    FormatBc2 = 36,
    FormatBc3 = 37,
    FormatBc4 = 38,
    FormatBc5 = 39,
    FormatBc6 = 40,
    FormatBc7 = 41,
    FormatFmask8_S2_F1 = 44,
    FormatFmask8_S4_F1 = 45,
    FormatFmask8_S8_F1 = 46,
    FormatFmask8_S2_F2 = 47,
    FormatFmask8_S4_F2 = 48,
    FormatFmask8_S4_F4 = 49,
    FormatFmask16_S16_F1 = 50,
    FormatFmask16_S8_F2 = 51,
    FormatFmask32_S16_F2 = 52,
    FormatFmask32_S8_F4 = 53,
    FormatFmask32_S8_F8 = 54,
    FormatFmask64_S16_F4 = 55,
    FormatFmask64_S16_F8 = 56,
    Format4_4 = 57,
    Format6_5_5 = 58,
    Format1 = 59,
    Format1_Reversed = 60,
    Format32_As_8 = 61,
    Format32_As_8_8 = 62,
    Format32_As_32_32_32_32 = 63,
};
inline constexpr u32 kDataFormatCount = 64; ///< the T#'s 6-bit field

enum class NumberFormat : u32 {
    Unorm = 0,
    Snorm = 1,
    Uscaled = 2,
    Sscaled = 3,
    Uint = 4,
    Sint = 5,
    SnormNz = 6,
    Float = 7,
    Srgb = 9,
    Ubnorm = 10,
    UbnormNz = 11,
    Ubint = 12,
    Ubscaled = 13,
};
inline constexpr u32 kNumberFormatCount = 16; ///< the T#'s 4-bit field

enum class NumberClass : u8 { Float = 0, Sint = 1, Uint = 2 };

enum class CompSwizzle : u8 { Zero = 0, One = 1, Red = 4, Green = 5, Blue = 6, Alpha = 7 };

enum class NumberConversion : u32 {
    None = 0,
    UintToUscaled = 1,
    SintToSscaled = 2,
    UnormToUbnorm = 3,
    Sint8ToSnormNz = 4,
    Sint16ToSnormNz = 5,
    Uint32ToUnorm = 6,
    SrgbToNorm = 7,
};

/// Where r, g, b, a come from (raw 3-bit selectors: 2 and 3 select nothing, Valid() says so).
struct CompMapping {
    std::array<CompSwizzle, 4> c{CompSwizzle::Red, CompSwizzle::Green, CompSwizzle::Blue, CompSwizzle::Alpha};

    static constexpr CompMapping FromRaw(u32 x, u32 y, u32 z, u32 w) {
        return {{static_cast<CompSwizzle>(x & 7), static_cast<CompSwizzle>(y & 7), static_cast<CompSwizzle>(z & 7),
                 static_cast<CompSwizzle>(w & 7)}};
    }
    constexpr bool operator==(const CompMapping&) const = default;
    /// Every selector is one of Zero/One/Red/Green/Blue/Alpha.
    bool Valid() const;
    /// A write mask (bit i: component i) through the mapping.
    u32 ApplyMask(u32 mask) const;
    /// For each of r, g, b, a: the component that reads it (Zero: none).
    CompMapping Inverse() const;
    /// The source component of component `comp` (itself for a constant selector).
    u32 Map(u32 comp) const;
};
inline constexpr CompMapping kIdentityMapping{};

DataFormat RemapDataFormat(DataFormat format);
NumberFormat RemapNumberFormat(NumberFormat format, DataFormat data_format);
CompMapping RemapSwizzle(DataFormat format, CompMapping swizzle);
/// false: SnormNz on a format that is not 8/16-bit per component (the bridge stops there); `out` = None.
bool MapNumberConversion(NumberFormat num_fmt, DataFormat data_fmt, NumberConversion& out);
NumberClass GetNumberClass(NumberFormat nfmt);
bool IsInteger(NumberFormat nfmt);
bool IsBlockCoded(DataFormat format);
bool IsFmask(DataFormat format);
/// 0 for a value that is not a format of the table (the bridge's table has -1 there, or no entry at all past 41).
u32 NumComponents(DataFormat format);
u32 NumBitsPerBlock(DataFormat format);   ///< a BC block: 64 or 128
u32 NumBitsPerElement(DataFormat format); ///< a BC texel: 4 or 8

// ---- tiling (GCN address library)

enum class PipeConfig : u32 {
    P2 = 0,
    P4_8x16 = 4,
    P4_16x16 = 5,
    P4_16x32 = 6,
    P4_32x32 = 7,
    P8_16x16_8x16 = 8,
    P8_16x32_8x16 = 9,
    P8_32x32_8x16 = 10,
    P8_16x32_16x16 = 11,
    P8_32x32_16x16 = 12,
    P8_32x32_16x32 = 13,
    P8_32x64_32x32 = 14,
    P16_32x32_8x16 = 16,
    P16_32x32_16x16 = 17,
    P16 = 18,
};

enum class MicroTileMode : u32 { Display = 0, Thin = 1, Depth = 2, Rotated = 3, Thick = 4 };

enum class MacroTileMode : u32 {
    Mode_1x4_16 = 0,
    Mode_1x2_16 = 1,
    Mode_1x1_16 = 2,
    Mode_1x1_16_Dup = 3,
    Mode_1x1_8 = 4,
    Mode_1x1_4 = 5,
    Mode_1x1_2 = 6,
    Mode_1x1_2_Dup = 7,
    Mode_1x8_16 = 8,
    Mode_1x4_16_Dup = 9,
    Mode_1x2_16_Dup = 10,
    Mode_1x1_16_Dup2 = 11,
    Mode_1x1_8_Dup = 12,
    Mode_1x1_4_Dup = 13,
    Mode_1x1_2_Dup2 = 14,
    Mode_1x1_2_Dup3 = 15,
};

enum class ArrayMode : u32 {
    ArrayLinearGeneral = 0,
    ArrayLinearAligned = 1,
    Array1DTiledThin1 = 2,
    Array1DTiledThick = 3,
    Array2DTiledThin1 = 4,
    ArrayPrtTiledThin1 = 5,
    ArrayPrt2DTiledThin1 = 6,
    Array2DTiledThick = 7,
    Array2DTiledXThick = 8,
    ArrayPrtTiledThick = 9,
    ArrayPrt2DTiledThick = 10,
    ArrayPrt3DTiledThin1 = 11,
    Array3DTiledThin1 = 12,
    Array3DTiledThick = 13,
    Array3DTiledXThick = 14,
    ArrayPrt3DTiledThick = 15,
};

/// The tiling index of a T# / CB_COLORn_ATTRIB / DB_Z_INFO (the PS4's fixed tile mode table).
enum class TileMode : u32 {
    Depth2DThin64 = 0,
    Depth2DThin128 = 1,
    Depth2DThin256 = 2,
    Depth2DThin512 = 3,
    Depth2DThin1K = 4,
    Depth1DThin = 5,
    Depth2DThinPrt256 = 6,
    Depth2DThinPrt1K = 7,
    DisplayLinearAligned = 8,
    Display1DThin = 9,
    Display2DThin = 10,
    DisplayThinPrt = 11,
    Display2DThinPrt = 12,
    Thin1DThin = 13,
    Thin2DThin = 14,
    Thin3DThin = 15,
    ThinThinPrt = 16,
    Thin2DThinPrt = 17,
    Thin3DThinPrt = 18,
    Thick1DThick = 19,
    Thick2DThick = 20,
    Thick3DThick = 21,
    ThickThickPrt = 22,
    Thick2DThickPrt = 23,
    Thick3DThickPrt = 24,
    Thick2DXThick = 25,
    Thick3DXThick = 26,
    DisplayLinearGeneral = 31,
};

/// 0..26 and 31; 27..30 are not in the table (the bridge stops on them). The functions below answer for those with
/// a fallback (linear, 0) that no layout uses: the layout builders refuse the tile mode first.
bool IsValidTileMode(u32 tile_mode);
bool IsValidMacroTileMode(u32 mode); ///< 0..15
bool IsMacroTiled(ArrayMode array_mode);
u32 GetMicroTileThickness(ArrayMode array_mode);
bool IsPrt(ArrayMode array_mode);
ArrayMode GetArrayMode(TileMode tile_mode);
MicroTileMode GetMicroTileMode(TileMode tile_mode);
PipeConfig GetPipeConfig(TileMode tile_mode);
PipeConfig GetAltPipeConfig(TileMode tile_mode);
u32 GetSampleSplit(TileMode tile_mode);
u32 GetTileSplitHw(TileMode tile_mode);
u32 GetBankWidth(MacroTileMode mode);
u32 GetBankHeight(MacroTileMode mode);
u32 GetNumBanks(MacroTileMode mode);
u32 GetMacrotileAspect(MacroTileMode mode);
u32 GetAltBankHeight(MacroTileMode mode);
u32 GetAltMacrotileAspect(MacroTileMode mode);
u32 GetAltNumBanks(MacroTileMode mode);
u32 GetPipeCount(PipeConfig pipe_cfg); ///< 0 for a configuration the PS4 does not use
u32 CalculateTileSplit(TileMode tile_mode, ArrayMode array_mode, MicroTileMode micro_tile_mode, u32 bpp);
/// false where the bridge stops: samples not a power of two up to 16, bpp outside 1..128, a mode that is not macro
/// tiled, or tiles smaller than 64 bytes (a 4-bit BC texel in a thin single-sample mode).
bool CalculateMacrotileMode(TileMode tile_mode, u32 bpp, u32 num_samples, MacroTileMode& out);

// ---- sizes (texture_cache/tile.h)

/// One mip level of an image: its bytes (all slices), its pitch and height in texels (BC: in texels, padded to the
/// block), its offset from the image's start. The same 12 bytes as the detiler's uniform block expects.
struct MipInfo {
    u32 size = 0;
    u16 pitch = 0;
    u16 height = 0;
    u32 offset = 0;
    constexpr bool operator==(const MipInfo&) const = default;
};
static_assert(sizeof(MipInfo) == 12);
inline constexpr u32 kMaxMips = 16;

/// A level's aligned size: (pitch, height) as u16 and the byte size as the bridge's size_t (truncated to u32 by the
/// caller, as there).
struct AlignedSize {
    u16 pitch = 0;
    u16 height = 0;
    u64 size = 0;
};

/// The macro tile {width, height} of a macro-tiled mode for `bpp` (8..128) and 1..8 samples. false: outside the
/// tables (tile mode > 26, bpp outside 8..255, samples 0 or > 8) — the bridge reads past its tables there.
bool GetMacroTileExtents(TileMode tile_mode, u32 bpp, u32 num_samples, bool alt, u32& width, u32& height);
/// false: bpp makes the bridge divide by zero (0, or past 0xFFFFFFF8).
bool ImageSizeLinearAligned(u32 pitch, u32 height, u32 bpp, u32 num_samples, AlignedSize& out);
AlignedSize ImageSizeMicroTiled(u32 pitch, u32 height, u32 thickness, u32 bpp, u32 num_samples);
/// false: no macro tile for these inputs (see GetMacroTileExtents, or a mode without one: the bridge asserts).
bool ImageSizeMacroTiled(u32 pitch, u32 height, u32 bpp, u32 num_samples, TileMode tile_mode, bool alt,
                         AlignedSize& out);
/// Mip 0 always is; a smaller one only while it still holds a whole macro tile. false in `ok` as above.
bool IsMacroTiledMip(u32 pitch, u32 height, u32 bpp, u32 num_samples, TileMode tile_mode, u32 mip_n, bool alt,
                     bool& ok);
/// Every level's size, pitch, height and offset; the image's size; the macro-tiled image's levels that are micro tiled
/// (bit per level). false (outputs as far as computed) for inputs no table covers. `levels` <= kMaxMips.
bool ComputeImageSize(u32 pitch, u32 height, u32 depth, u32 levels, u32 layers, u32 num_bits, u32 num_samples,
                      bool is_block, bool is_pow2, bool alt_tile, TileMode tile_mode, ArrayMode array_mode,
                      u32* out_size, u32* out_micro_tiled_mips, MipInfo* out_mips_layout);

// ---- images

enum class ImageType : u32 {
    Invalid = 0,
    Color1D = 8,
    Color2D = 9,
    Color3D = 10,
    Cube = 11,
    Color1DArray = 12,
    Color2DArray = 13,
    Color2DMsaa = 14,
    Color2DMsaaArray = 15,
};

/// The T#'s fields as the bridge's Image reads them (the frontend's ImageDesc decodes the others).
namespace TSharp {
u64 BaseAddressField(const Gpu::Shader::ImageDesc& d); ///< the 38-bit field (256-byte units)
u32 MinLod(const Gpu::Shader::ImageDesc& d);           ///< 4.8 fixed point
bool AltTileMode(const Gpu::Shader::ImageDesc& d);     ///< Neo only
ImageType Type(const Gpu::Shader::ImageDesc& d);
TileMode Tiling(const Gpu::Shader::ImageDesc& d);
bool IsTiled(const Gpu::Shader::ImageDesc& d);
DataFormat DataFmt(const Gpu::Shader::ImageDesc& d);     ///< remapped (RemapDataFormat)
NumberFormat NumberFmt(const Gpu::Shader::ImageDesc& d); ///< remapped (RemapNumberFormat)
CompMapping DstSelect(const Gpu::Shader::ImageDesc& d);  ///< remapped (RemapSwizzle)
u32 NumLayers(const Gpu::Shader::ImageDesc& d);          ///< cubes x6, pow2-padded
u32 NumLevels(const Gpu::Shader::ImageDesc& d);          ///< 1 for MSAA
u32 NumSamples(const Gpu::Shader::ImageDesc& d);         ///< MSAA: 1 << last_level
ImageType BaseType(const Gpu::Shader::ImageDesc& d);
ImageType ViewType(const Gpu::Shader::ImageDesc& d, bool is_array);
u32 NumViewLevels(const Gpu::Shader::ImageDesc& d, bool is_array);
u32 NumViewLayers(const Gpu::Shader::ImageDesc& d, bool is_array);
/// The Neo bank swizzle of an alt-tiled macro-tiled T# (0 otherwise). false: the bridge stops (CalculateMacrotileMode).
bool BankSwizzle(const Gpu::Shader::ImageDesc& d, u8& out);
} // namespace TSharp

/// A V#'s dst_sel, remapped for its data format.
CompMapping VSharpDstSelect(const Gpu::Shader::BufferDesc& d);

/// CB_COLORn_INFO: the data/number format the bridge uses for the target (remapped, SNORM_NZ read as SRGB) and the
/// MRT swizzle of its COMP_SWAP. false from the swizzle: a format with no component (the bridge reads before its
/// table there).
DataFormat ColorTargetDataFormat(u32 cb_info);
NumberFormat ColorTargetNumberFormat(u32 cb_info);
NumberFormat ColorTargetFixedNumberFormat(u32 cb_info); ///< before remapping
bool ColorTargetSwizzle(u32 cb_info, CompMapping& out);

struct ImageProps {
    bool is_volume = false;
    bool is_tiled = false;
    bool is_pow2 = false;
    bool is_block = false;
    bool is_depth = false;
    bool has_stencil = false;
    constexpr bool operator==(const ImageProps&) const = default;
};

struct ImageMeta {
    u64 cmask_addr = 0;
    u64 fmask_addr = 0;
    u64 htile_addr = 0;
    s32 htile_clear_mask = -1;
};

struct Extent3D {
    u32 width = 0;
    u32 height = 0;
    u32 depth = 0;
    constexpr bool operator==(const Extent3D&) const = default;
};

/// A guest image as the texture cache keys, creates and uploads it (the bridge's ImageInfo).
struct ImageLayout {
    /// false: this image cannot be laid out (see `reason`); the other fields are as far as computed, not a layout.
    bool supported = false;
    std::string_view reason;

    ImageMeta meta;
    ImageProps props;
    u32 pixel_format = 0; ///< a VkFormat (vk_formats.h); VK_FORMAT_UNDEFINED = 0
    ImageType type = ImageType::Color2D;
    u16 levels = 1; ///< u16 like the bridge's SubresourceExtent: a larger count wraps there too
    u16 layers = 1;
    Extent3D size;
    u32 num_bits = 0; ///< per element; a BC block's bits for BC
    u32 num_samples = 1;
    u32 pitch = 0;
    TileMode tile_mode = TileMode::DisplayLinearAligned;
    ArrayMode array_mode = ArrayMode::ArrayLinearAligned;
    std::array<MipInfo, kMaxMips> mips{}; ///< `levels` used, the rest 0
    u32 micro_tiled_mips = 0;
    u64 guest_address = 0;
    u32 guest_size = 0;
    u8 bank_swizzle = 0;
    bool alt_tile = false;
    u64 stencil_addr = 0;
    u32 stencil_size = 0;

    bool IsTiled() const {
        return tile_mode != TileMode::DisplayLinearAligned;
    }
    /// The pitch and height in elements (BC: blocks).
    std::array<u32, 2> BlockDim() const {
        const u32 dim = props.is_block ? 2 : 0;
        return {pitch >> dim, size.height >> dim};
    }
};

/// A T# as a shader reads it. `is_depth`: the shader samples it as depth (R32/R16 promoted to D32/D16; any other
/// format is unsupported, as the bridge's PromoteFormatToDepth stops). The view flags (array, storage) do not change
/// the image (see vk_formats.h TSharpView). `neo_mode`: the process runs in Neo mode (alt tiling taken).
ImageLayout LayoutFromTSharp(const Gpu::Shader::ImageDesc& sharp, bool is_depth, bool neo_mode = false);

/// Colour target `n` (0..7) from CB_COLORn_*; `extent_hint`: the command processor's CbExtents()[n] (width in bits
/// 0..15, height in 16..31; 0: none, the size comes from the pitch/slice registers).
ImageLayout LayoutFromColorTarget(const Cp::RegisterFile& regs, u32 n, u32 extent_hint, bool neo_mode = false);

/// The depth target from DB_Z_INFO.. DB_DEPTH_SLICE, DB_DEPTH_VIEW (slices), DB_HTILE_DATA_BASE (HTILE when
/// TILE_SURFACE_ENABLE); `extent_hint`: the command processor's DbExtent(); `write_buffer`: the Z/stencil write bases
/// instead of the read bases (the bridge's depth-to-depth copy). A stencil-only target (Z format Invalid) is
/// unsupported: the bridge asserts on it.
ImageLayout LayoutFromDepthTarget(const Cp::RegisterFile& regs, u32 extent_hint, bool write_buffer = false);

/// A VideoOut buffer. `pixel_format`/`tiling_mode`: the raw SceVideoOut values (0x80000000 A8R8G8B8 sRGB, ...;
/// 0 tiled, 1 linear). The pitch comes from the width as in the bridge (linear: the width, otherwise aligned to 128
/// texels); `pitch_in_pixel` is not used for the layout. A16R16G16B16 float and unknown formats are unsupported.
ImageLayout LayoutFromVideoOut(u64 address, u32 width, u32 height, u32 pitch_in_pixel, u32 pixel_format,
                               u32 tiling_mode);

/// Formats compatible for a view (Vulkan's compatibility classes), same samples and bits.
bool IsCompatible(const ImageLayout& a, const ImageLayout& b);
/// The level of `info` that `image` is (one level, same pitch and size, at the level's address), -1: none.
s32 MipOf(const ImageLayout& image, const ImageLayout& info);
/// The slice of `info`'s level `mip` that `image` is, -1: none.
s32 SliceOf(const ImageLayout& image, const ImageLayout& info, s32 mip);

// ---- the detiler (texture_cache/tile_manager.cpp)

/// The detiler's uniform block (binding 2), byte for byte.
struct TilingInfo {
    u32 bank_swizzle = 0;
    u32 micro_tiled_mips = 0;
    u32 num_mips = 0;
    std::array<MipInfo, kMaxMips> mips{}; ///< BC: pitch and height in blocks
};
static_assert(sizeof(TilingInfo) == 204);

/// Everything to run the detiler (tiled guest bytes -> linear) or the tiler (`is_tiler`) on an image: the shader
/// variant (HostShaders::Tiling(num_bits, macro)), its specialization constants (ids 0..spec_count-1, one u32 each),
/// the uniform block, the dispatch's group count (64 threads per group, one element each).
struct TilingParams {
    bool needed = false;    ///< the image is tiled (a linear one is copied as it is)
    bool supported = false; ///< false: a tiled image the detiler has no variant/parameters for (see `reason`)
    std::string_view reason;
    u32 num_bits = 0;
    bool macro = false;
    std::array<u32, 12> spec{}; ///< micro: samples, micro tile mode, thickness, is_tiler; macro adds 8 more
    u32 spec_count = 0;         ///< 4 (micro) or 12 (macro)
    TilingInfo info;
    u32 dispatch_x = 0;
};

/// `num_mips`: the levels to (de)tile; ~0u: all of the image's (the bridge's detiler), otherwise as given up to
/// kMaxMips (its tiler passes its number of copies).
TilingParams BuildTilingParams(const ImageLayout& layout, bool is_tiler, u32 num_mips = ~0u);

} // namespace SecondSon::Gpu
