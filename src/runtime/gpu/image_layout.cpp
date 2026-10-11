// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — a guest image's layout (Phase 4, P4.4). See image_layout.h.
// Ported from shadPS4's amdgpu/{pixel_format,tiling}, texture_cache/{tile.h,image_info,tile_manager} (GPL-2.0-or-later).
#include "runtime/gpu/image_layout.h"

#include <algorithm>
#include <bit>

#include "runtime/gpu/cp_registers.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/vulkan/vk_formats.h"

namespace SecondSon::Gpu {

namespace {

// ---- pixel formats

constexpr u32 kFormatTableSize = 42; // the table ends after BC7

constexpr std::array<s8, kFormatTableSize> kNumComponents = {
    0, 1, 1, 2, 1, 2, 3, 3, 4, 4, 4, 2, 4, 3, 4, 0, // 0..15
    3, 4, 4, 4, 2, 2, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 16..31
    3, 3, 4, 4, 4, 4, 1, 2, 3, 4,                   // 32..41
};

constexpr std::array<s16, kFormatTableSize> kBitsPerBlock = {
    0,  8,  16, 16, 32, 32, 32, 32, 32, 32, 32, 64, 64, 96, 128, -1, // 0..15
    16, 16, 16, 16, 32, 32, 64, -1, -1, -1, -1, -1, -1, -1, -1,  -1, // 16..31
    16, 16, 32, 64, 128, 128, 64, 128, 128, 128,                     // 32..41
};

constexpr std::array<s16, kFormatTableSize> kBitsPerElement = {
    0,  8,  16, 16, 32, 32, 32, 32, 32, 32, 32, 64, 64, 96, 128, -1, // 0..15
    16, 16, 16, 16, 32, 32, 64, -1, -1, -1, -1, -1, -1, -1, -1,  -1, // 16..31
    16, 16, 32, 4,  8,  8,  4,  8,  8,  8,                           // 32..41
};

template <typename T>
u32 FormatTableValue(const std::array<T, kFormatTableSize>& table, DataFormat format) {
    const u32 index = static_cast<u32>(format);
    return index < table.size() && table[index] > 0 ? static_cast<u32>(table[index]) : 0;
}

// ---- tiling tables (tiling.cpp's switches, one row per value)

constexpr u32 kMicroTileSize = 8;
constexpr u32 kDramRowSize = 1024;

struct TileModeRow {
    bool valid;
    ArrayMode array_mode;
    MicroTileMode micro_mode;
    PipeConfig pipe;
    PipeConfig alt_pipe;
    u8 sample_split;
    u16 tile_split_hw;
};

constexpr TileModeRow Row(ArrayMode a, MicroTileMode m, PipeConfig p, u8 sample_split, u16 tile_split_hw) {
    return {true, a, m, p, PipeConfig::P16_32x32_8x16, sample_split, tile_split_hw};
}

using AM = ArrayMode;
using MM = MicroTileMode;
using PC = PipeConfig;

constexpr std::array<TileModeRow, 32> kTileModes = {
    Row(AM::Array2DTiledThin1, MM::Depth, PC::P8_32x32_16x16, 1, 64),      // 0 Depth2DThin64
    Row(AM::Array2DTiledThin1, MM::Depth, PC::P8_32x32_16x16, 1, 128),     // 1 Depth2DThin128
    Row(AM::Array2DTiledThin1, MM::Depth, PC::P8_32x32_16x16, 1, 256),     // 2 Depth2DThin256
    Row(AM::Array2DTiledThin1, MM::Depth, PC::P8_32x32_16x16, 1, 512),     // 3 Depth2DThin512
    Row(AM::Array2DTiledThin1, MM::Depth, PC::P8_32x32_16x16, 1, 1024),    // 4 Depth2DThin1K
    Row(AM::Array1DTiledThin1, MM::Depth, PC::P8_32x32_16x16, 1, 64),      // 5 Depth1DThin
    Row(AM::ArrayPrt2DTiledThin1, MM::Depth, PC::P8_32x32_16x16, 1, 256),  // 6 Depth2DThinPrt256
    Row(AM::ArrayPrt2DTiledThin1, MM::Depth, PC::P8_32x32_16x16, 1, 1024), // 7 Depth2DThinPrt1K
    Row(AM::ArrayLinearAligned, MM::Display, PC::P8_32x32_16x16, 1, 64),   // 8 DisplayLinearAligned
    Row(AM::Array1DTiledThin1, MM::Display, PC::P8_32x32_16x16, 1, 64),    // 9 Display1DThin
    Row(AM::Array2DTiledThin1, MM::Display, PC::P8_32x32_16x16, 2, 64),    // 10 Display2DThin
    Row(AM::ArrayPrtTiledThin1, MM::Display, PC::P8_32x32_8x16, 2, 64),    // 11 DisplayThinPrt
    Row(AM::ArrayPrt2DTiledThin1, MM::Display, PC::P8_32x32_16x16, 2, 64), // 12 Display2DThinPrt
    Row(AM::Array1DTiledThin1, MM::Thin, PC::P8_32x32_16x16, 1, 64),       // 13 Thin1DThin
    Row(AM::Array2DTiledThin1, MM::Thin, PC::P8_32x32_16x16, 2, 64),       // 14 Thin2DThin
    Row(AM::Array3DTiledThin1, MM::Thin, PC::P8_32x32_8x16, 2, 64),        // 15 Thin3DThin
    Row(AM::ArrayPrtTiledThin1, MM::Thin, PC::P8_32x32_8x16, 2, 64),       // 16 ThinThinPrt
    Row(AM::ArrayPrt2DTiledThin1, MM::Thin, PC::P8_32x32_16x16, 2, 64),    // 17 Thin2DThinPrt
    Row(AM::ArrayPrt3DTiledThin1, MM::Thin, PC::P8_32x32_16x16, 2, 64),    // 18 Thin3DThinPrt
    Row(AM::Array1DTiledThick, MM::Thick, PC::P8_32x32_16x16, 1, 64),      // 19 Thick1DThick
    Row(AM::Array2DTiledThick, MM::Thick, PC::P8_32x32_16x16, 1, 64),      // 20 Thick2DThick
    Row(AM::Array3DTiledThick, MM::Thick, PC::P8_32x32_8x16, 1, 64),       // 21 Thick3DThick
    Row(AM::ArrayPrtTiledThick, MM::Thick, PC::P8_32x32_8x16, 1, 64),      // 22 ThickThickPrt
    Row(AM::ArrayPrt2DTiledThick, MM::Thick, PC::P8_32x32_16x16, 1, 64),   // 23 Thick2DThickPrt
    Row(AM::ArrayPrt3DTiledThick, MM::Thick, PC::P8_32x32_8x16, 1, 64),    // 24 Thick3DThickPrt
    Row(AM::Array2DTiledXThick, MM::Thick, PC::P8_32x32_16x16, 1, 64),     // 25 Thick2DXThick
    Row(AM::Array3DTiledXThick, MM::Thick, PC::P8_32x32_8x16, 1, 64),      // 26 Thick3DXThick
    TileModeRow{},                                                         // 27
    TileModeRow{},                                                         // 28
    TileModeRow{},                                                         // 29
    TileModeRow{},                                                         // 30
    TileModeRow{true, AM::ArrayLinearGeneral, MM::Display, PC::P2, PC::P2, 1, 64}, // 31 DisplayLinearGeneral
};

const TileModeRow& TileRow(TileMode tile_mode) {
    static constexpr TileModeRow kNone{false, AM::ArrayLinearGeneral, MM::Display, PC::P2, PC::P2, 1, 64};
    const u32 index = static_cast<u32>(tile_mode);
    return index < kTileModes.size() && kTileModes[index].valid ? kTileModes[index] : kNone;
}

struct ArrayModeRow {
    bool macro;
    u8 thickness;
    bool prt;
};
constexpr std::array<ArrayModeRow, 16> kArrayModes = {{
    {false, 1, false}, // ArrayLinearGeneral
    {false, 1, false}, // ArrayLinearAligned
    {false, 1, false}, // Array1DTiledThin1
    {false, 4, false}, // Array1DTiledThick
    {true, 1, false},  // Array2DTiledThin1
    {true, 1, true},   // ArrayPrtTiledThin1
    {true, 1, true},   // ArrayPrt2DTiledThin1
    {true, 4, false},  // Array2DTiledThick
    {true, 8, false},  // Array2DTiledXThick
    {true, 4, true},   // ArrayPrtTiledThick
    {true, 4, true},   // ArrayPrt2DTiledThick
    {true, 1, true},   // ArrayPrt3DTiledThin1
    {true, 1, false},  // Array3DTiledThin1
    {true, 4, false},  // Array3DTiledThick
    {true, 8, false},  // Array3DTiledXThick
    {true, 4, true},   // ArrayPrt3DTiledThick
}};

const ArrayModeRow& ArrayRow(ArrayMode array_mode) {
    static constexpr ArrayModeRow kNone{false, 1, false};
    const u32 index = static_cast<u32>(array_mode);
    return index < kArrayModes.size() ? kArrayModes[index] : kNone;
}

struct MacroModeRow {
    u8 bank_width;
    u8 bank_height;
    u8 num_banks;
    u8 aspect;
    u8 alt_bank_height;
    u8 alt_aspect;
    u8 alt_num_banks;
};
constexpr std::array<MacroModeRow, 16> kMacroModes = {{
    {1, 4, 16, 4, 4, 2, 8},  // Mode_1x4_16
    {1, 2, 16, 2, 4, 2, 8},  // Mode_1x2_16
    {1, 1, 16, 2, 2, 1, 8},  // Mode_1x1_16
    {1, 1, 16, 2, 2, 1, 8},  // Mode_1x1_16_Dup
    {1, 1, 8, 1, 1, 1, 8},   // Mode_1x1_8
    {1, 1, 4, 1, 1, 1, 8},   // Mode_1x1_4
    {1, 1, 2, 1, 1, 1, 4},   // Mode_1x1_2
    {1, 1, 2, 1, 1, 1, 2},   // Mode_1x1_2_Dup
    {1, 8, 16, 4, 4, 2, 16}, // Mode_1x8_16
    {1, 4, 16, 4, 4, 2, 8},  // Mode_1x4_16_Dup
    {1, 2, 16, 2, 2, 1, 8},  // Mode_1x2_16_Dup
    {1, 1, 16, 2, 1, 1, 8},  // Mode_1x1_16_Dup2
    {1, 1, 8, 1, 1, 1, 4},   // Mode_1x1_8_Dup
    {1, 1, 4, 1, 1, 1, 4},   // Mode_1x1_4_Dup
    {1, 1, 2, 1, 1, 1, 2},   // Mode_1x1_2_Dup2
    {1, 1, 2, 1, 1, 1, 2},   // Mode_1x1_2_Dup3
}};

const MacroModeRow& MacroRow(MacroTileMode mode) {
    static constexpr MacroModeRow kNone{0, 0, 0, 0, 0, 0, 0};
    const u32 index = static_cast<u32>(mode);
    return index < kMacroModes.size() ? kMacroModes[index] : kNone;
}

// ---- macro tile extents (tile.h): [samples log2][tile mode * 5 + log2(bpp) - 3]

struct TileExtent {
    u16 width;
    u16 height;
};
using ExtentTable = std::array<TileExtent, 27 * 5>;

// clang-format off
constexpr std::array<ExtentTable, 8> kMacroTileExtents = {{
    // x1
    {{
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 00
        {256, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 01
        {256, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 02
        {256, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 03
        {256, 128}, {128, 128}, {128, 64}, {128, 64}, {64, 64}, // 04
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 05
        {256, 256}, {256, 128}, {128, 128}, {128, 128}, {128, 128}, // 06
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}, // 07
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 08
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 09
        {256, 128}, {128, 128}, {128, 64}, {128, 64}, {64, 64}, // 0A
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}, // 0B
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}, // 0C
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 0D
        {256, 128}, {128, 128}, {128, 64}, {128, 64}, {64, 64}, // 0E
        {256, 128}, {128, 128}, {128, 64}, {128, 64}, {64, 64}, // 0F
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}, // 10
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}, // 11
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {64, 64}, // 12
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 13
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 14
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 15
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 16
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 17
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 18
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 19
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 1A
    }},
    // x2
    {{
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 00
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 01
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 02
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 03
        {128, 128}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 04
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 05
        {256, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 06
        {256, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 07
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 08
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 09
        {128, 128}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0A
        {256, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 0B
        {256, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 0C
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 0D
        {128, 128}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0E
        {128, 128}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0F
        {256, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 10
        {256, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 11
        {256, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 12
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 13
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 14
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 15
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 16
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 17
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 18
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 19
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 1A
    }},
    // x4
    {{
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 00
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 01
        {128, 64}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 02
        {128, 64}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 03
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 04
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 05
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 06
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 07
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 08
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 09
        {128, 64}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0A
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 0B
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 0C
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 0D
        {128, 64}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0E
        {128, 64}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0F
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 10
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 11
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 12
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 13
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 14
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 15
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 16
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 17
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 18
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 19
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 1A
    }},
    // x8
    {{
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 00
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 01
        {128, 64}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 02
        {128, 64}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 03
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 04
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 05
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 06
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 07
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 08
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 09
        {128, 64}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0A
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 0B
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 0C
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 0D
        {128, 64}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0E
        {128, 64}, {128, 64}, {128, 64}, {64, 64}, {64, 64}, // 0F
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 10
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 11
        {128, 128}, {128, 128}, {128, 64}, {64, 64}, {64, 64}, // 12
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 13
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 14
        {128, 64}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 15
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 16
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 17
        {128, 128}, {128, 64}, {64, 64}, {64, 64}, {64, 64}, // 18
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 19
        {128, 64}, {64, 64}, {64, 64}, {64, 64}, {64, 64}, // 1A
    }},
    // alt_x1
    {{
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 00
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 01
        {256, 128}, {256, 128}, {128, 128}, {128, 128}, {128, 128}, // 02
        {256, 128}, {256, 128}, {128, 128}, {128, 128}, {128, 128}, // 03
        {256, 128}, {256, 128}, {128, 128}, {128, 128}, {128, 64}, // 04
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 05
        {256, 256}, {256, 128}, {128, 128}, {128, 128}, {128, 128}, // 06
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {128, 32}, // 07
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 08
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 09
        {256, 128}, {256, 128}, {128, 128}, {128, 128}, {128, 64}, // 0A
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {128, 32}, // 0B
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {128, 32}, // 0C
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 0D
        {256, 128}, {256, 128}, {128, 128}, {128, 128}, {128, 64}, // 0E
        {256, 128}, {256, 128}, {128, 128}, {128, 128}, {128, 64}, // 0F
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {128, 32}, // 10
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {128, 32}, // 11
        {256, 256}, {256, 128}, {128, 128}, {128, 64}, {128, 32}, // 12
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 13
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 14
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 15
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 16
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 17
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 18
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 19
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 1A
    }},
    // alt_x2
    {{
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 00
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 01
        {256, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 02
        {256, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 03
        {256, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 04
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 05
        {256, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 06
        {256, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 07
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 08
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 09
        {256, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0A
        {256, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 0B
        {256, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 0C
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 0D
        {256, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0E
        {256, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0F
        {256, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 10
        {256, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 11
        {256, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 12
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 13
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 14
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 15
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 16
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 17
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 18
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 19
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 1A
    }},
    // alt_x4
    {{
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 00
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 01
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 02
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 03
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 04
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 05
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 06
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 07
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 08
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 09
        {128, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0A
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 0B
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 0C
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 0D
        {128, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0E
        {128, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0F
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 10
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 11
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 12
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 13
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 14
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 15
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 16
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 17
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 18
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 19
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 1A
    }},
    // alt_x8
    {{
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 00
        {256, 128}, {256, 128}, {256, 128}, {256, 128}, {256, 128}, // 01
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 02
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 03
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 04
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 05
        {128, 128}, {128, 128}, {128, 128}, {128, 128}, {128, 128}, // 06
        {128, 64}, {128, 32}, {128, 32}, {128, 32}, {128, 32}, // 07
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 08
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 09
        {128, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0A
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 0B
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 0C
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 0D
        {128, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0E
        {128, 128}, {128, 128}, {128, 128}, {128, 64}, {128, 64}, // 0F
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 10
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 11
        {128, 128}, {128, 128}, {128, 64}, {128, 32}, {128, 32}, // 12
        {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, // 13
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 14
        {128, 128}, {128, 128}, {128, 64}, {128, 64}, {128, 64}, // 15
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 16
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 17
        {128, 128}, {128, 64}, {128, 32}, {128, 32}, {128, 32}, // 18
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 19
        {128, 128}, {128, 64}, {128, 64}, {128, 64}, {128, 64}, // 1A
    }},
}};
// clang-format on

// ---- registers

constexpr u32 kDbDepthView = 0xA002;
constexpr u32 kDbHtileDataBase = 0xA005;
constexpr u32 kDbStencilInfo = 0xA011;
constexpr u32 kDbZReadBase = 0xA012;
constexpr u32 kDbStencilReadBase = 0xA013;
constexpr u32 kDbZWriteBase = 0xA014;
constexpr u32 kDbStencilWriteBase = 0xA015;
constexpr u32 kDbDepthSize = 0xA016;
constexpr u32 kDbDepthSlice = 0xA017;

/// CB_COLORn_* (15 registers per target).
struct ColorRegs {
    u32 base, pitch, slice, view, info, attrib, cmask, cmask_slice, fmask, fmask_slice, clear0, clear1;

    static ColorRegs Read(const Cp::RegisterFile& regs, u32 n) {
        const u32 r = Cp::Reg::CbColor0Base + n * Cp::Reg::CbColorStride;
        return {regs.Get(r),     regs.Get(r + 1), regs.Get(r + 2), regs.Get(r + 3),  regs.Get(r + 4),  regs.Get(r + 5),
                regs.Get(r + 7), regs.Get(r + 8), regs.Get(r + 9), regs.Get(r + 10), regs.Get(r + 11), regs.Get(r + 12)};
    }
    u32 Format() const { return (info >> 2) & 0x1F; }
    bool LinearGeneral() const { return (info >> 7) & 1; }
    bool FastClear() const { return (info >> 13) & 1; }
    bool Compression() const { return (info >> 14) & 1; }
    bool AltTileMode() const { return info >> 31; }
    u32 TileIndex() const { return attrib & 0x1F; }
    u32 NumSamples() const { return 1u << ((attrib >> 15) & 3); } // NUM_FRAGMENTS
    u32 Pitch() const { return ((pitch & 0x7FF) + 1) << 3; }
    u32 SliceTileMax() const { return slice & 0x3FFFFF; }
    u32 Height() const { return (SliceTileMax() + 1) * 64 / Pitch(); }
    u32 SliceStart() const { return view & 0x7FF; }
    u32 NumSlices() const { return ((view >> 13) & 0x7FF) + 1; }
    TileMode Tiling() const { return LinearGeneral() ? TileMode::DisplayLinearGeneral : TileMode(TileIndex()); }
};

ImageLayout& Refuse(ImageLayout& l, std::string_view reason) {
    l.supported = false;
    l.reason = reason;
    return l;
}

bool IsTiledMode(TileMode t) {
    return t != TileMode::DisplayLinearAligned && t != TileMode::DisplayLinearGeneral;
}

} // namespace

// ---- pixel formats

bool CompMapping::Valid() const {
    return std::ranges::all_of(c, [](CompSwizzle s) {
        const u32 v = static_cast<u32>(s);
        return v <= 1 || (v >= 4 && v <= 7);
    });
}

u32 CompMapping::Map(u32 comp) const {
    const u32 swizzled = static_cast<u32>(c[comp & 3]);
    return swizzled >= 4 && swizzled <= 7 ? swizzled - 4 : comp;
}

u32 CompMapping::ApplyMask(u32 mask) const {
    u32 out = 0;
    for (u32 i = 0; i < 4; ++i) {
        out |= ((mask >> i) & 1) << Map(i);
    }
    return out;
}

CompMapping CompMapping::Inverse() const {
    const auto inverse = [this](CompSwizzle target) {
        static constexpr CompSwizzle kBy[4] = {CompSwizzle::Red, CompSwizzle::Green, CompSwizzle::Blue,
                                               CompSwizzle::Alpha};
        for (u32 i = 0; i < 4; ++i) {
            if (c[i] == target) {
                return kBy[i];
            }
        }
        return CompSwizzle::Zero;
    };
    return {{inverse(CompSwizzle::Red), inverse(CompSwizzle::Green), inverse(CompSwizzle::Blue),
             inverse(CompSwizzle::Alpha)}};
}

DataFormat RemapDataFormat(DataFormat format) {
    switch (format) {
    case DataFormat::Format11_11_10:
        return DataFormat::Format10_11_11;
    case DataFormat::Format10_10_10_2:
        return DataFormat::Format2_10_10_10;
    case DataFormat::Format5_5_5_1:
        return DataFormat::Format1_5_5_5;
    default:
        return format;
    }
}

NumberFormat RemapNumberFormat(NumberFormat format, DataFormat data_format) {
    switch (format) {
    case NumberFormat::Unorm:
        switch (data_format) {
        case DataFormat::Format32:
        case DataFormat::Format32_32:
        case DataFormat::Format32_32_32:
        case DataFormat::Format32_32_32_32:
            return NumberFormat::Uint;
        default:
            return format;
        }
    case NumberFormat::Srgb:
        switch (data_format) {
        case DataFormat::FormatBc4:
        case DataFormat::FormatBc5:
        case DataFormat::FormatBc6:
            return NumberFormat::Unorm; // no sRGB host variant (non-colour / HDR data)
        default:
            return format;
        }
    case NumberFormat::Uscaled:
        return NumberFormat::Uint;
    case NumberFormat::Sscaled:
    case NumberFormat::SnormNz:
        return NumberFormat::Sint;
    case NumberFormat::Ubnorm:
        return NumberFormat::Unorm;
    case NumberFormat::Float:
        // 8-bit float: the stencil of a depth-stencil image, passed through as uint
        return data_format == DataFormat::Format8 ? NumberFormat::Uint : format;
    default:
        return format;
    }
}

CompMapping RemapSwizzle(DataFormat format, CompMapping swizzle) {
    const auto [r, g, b, a] = swizzle.c;
    switch (format) {
    case DataFormat::Format1_5_5_5:
    case DataFormat::Format11_11_10:
    case DataFormat::Format5_6_5:
        return {{b, g, r, a}};
    case DataFormat::Format10_10_10_2:
        return {{a, b, g, r}};
    case DataFormat::Format4_4_4_4:
        return {{g, b, a, r}};
    case DataFormat::Format8:
    case DataFormat::Format16:
    case DataFormat::Format32: {
        // the single channel is R on the host: a selector of A reads R
        CompMapping result = swizzle;
        for (CompSwizzle& s : result.c) {
            if (s == CompSwizzle::Alpha) {
                s = CompSwizzle::Red;
            }
        }
        return result;
    }
    default:
        return swizzle;
    }
}

bool MapNumberConversion(NumberFormat num_fmt, DataFormat data_fmt, NumberConversion& out) {
    out = NumberConversion::None;
    switch (num_fmt) {
    case NumberFormat::Unorm:
        switch (data_fmt) {
        case DataFormat::Format32:
        case DataFormat::Format32_32:
        case DataFormat::Format32_32_32:
        case DataFormat::Format32_32_32_32:
            out = NumberConversion::Uint32ToUnorm;
            break;
        default:
            break;
        }
        return true;
    case NumberFormat::Srgb:
        if (data_fmt == DataFormat::FormatBc6) {
            out = NumberConversion::SrgbToNorm;
        }
        return true;
    case NumberFormat::Uscaled:
        out = NumberConversion::UintToUscaled;
        return true;
    case NumberFormat::Sscaled:
        out = NumberConversion::SintToSscaled;
        return true;
    case NumberFormat::Ubnorm:
        out = NumberConversion::UnormToUbnorm;
        return true;
    case NumberFormat::SnormNz:
        switch (data_fmt) {
        case DataFormat::Format8:
        case DataFormat::Format8_8:
        case DataFormat::Format8_8_8_8:
            out = NumberConversion::Sint8ToSnormNz;
            return true;
        case DataFormat::Format16:
        case DataFormat::Format16_16:
        case DataFormat::Format16_16_16_16:
            out = NumberConversion::Sint16ToSnormNz;
            return true;
        default:
            return false;
        }
    default:
        return true;
    }
}

NumberClass GetNumberClass(NumberFormat nfmt) {
    switch (nfmt) {
    case NumberFormat::Sint:
        return NumberClass::Sint;
    case NumberFormat::Uint:
        return NumberClass::Uint;
    default:
        return NumberClass::Float;
    }
}

bool IsInteger(NumberFormat nfmt) {
    return nfmt == NumberFormat::Sint || nfmt == NumberFormat::Uint;
}

bool IsBlockCoded(DataFormat format) {
    return format >= DataFormat::FormatBc1 && format <= DataFormat::FormatBc7;
}

bool IsFmask(DataFormat format) {
    return format >= DataFormat::FormatFmask8_S2_F1 && format <= DataFormat::FormatFmask64_S16_F8;
}

u32 NumComponents(DataFormat format) {
    return FormatTableValue(kNumComponents, format);
}

u32 NumBitsPerBlock(DataFormat format) {
    return FormatTableValue(kBitsPerBlock, format);
}

u32 NumBitsPerElement(DataFormat format) {
    return FormatTableValue(kBitsPerElement, format);
}

// ---- tiling

bool IsValidTileMode(u32 tile_mode) {
    return tile_mode < kTileModes.size() && kTileModes[tile_mode].valid;
}

bool IsValidMacroTileMode(u32 mode) {
    return mode < kMacroModes.size();
}

bool IsMacroTiled(ArrayMode array_mode) {
    return ArrayRow(array_mode).macro;
}

u32 GetMicroTileThickness(ArrayMode array_mode) {
    return ArrayRow(array_mode).thickness;
}

bool IsPrt(ArrayMode array_mode) {
    return ArrayRow(array_mode).prt;
}

ArrayMode GetArrayMode(TileMode tile_mode) {
    return TileRow(tile_mode).array_mode;
}

MicroTileMode GetMicroTileMode(TileMode tile_mode) {
    return TileRow(tile_mode).micro_mode;
}

PipeConfig GetPipeConfig(TileMode tile_mode) {
    return TileRow(tile_mode).pipe;
}

PipeConfig GetAltPipeConfig(TileMode tile_mode) {
    return TileRow(tile_mode).alt_pipe;
}

u32 GetSampleSplit(TileMode tile_mode) {
    return TileRow(tile_mode).sample_split;
}

u32 GetTileSplitHw(TileMode tile_mode) {
    return TileRow(tile_mode).tile_split_hw;
}

u32 GetBankWidth(MacroTileMode mode) {
    return MacroRow(mode).bank_width;
}

u32 GetBankHeight(MacroTileMode mode) {
    return MacroRow(mode).bank_height;
}

u32 GetNumBanks(MacroTileMode mode) {
    return MacroRow(mode).num_banks;
}

u32 GetMacrotileAspect(MacroTileMode mode) {
    return MacroRow(mode).aspect;
}

u32 GetAltBankHeight(MacroTileMode mode) {
    return MacroRow(mode).alt_bank_height;
}

u32 GetAltMacrotileAspect(MacroTileMode mode) {
    return MacroRow(mode).alt_aspect;
}

u32 GetAltNumBanks(MacroTileMode mode) {
    return MacroRow(mode).alt_num_banks;
}

u32 GetPipeCount(PipeConfig pipe_cfg) {
    switch (pipe_cfg) {
    case PipeConfig::P2:
        return 2;
    case PipeConfig::P8_32x32_8x16:
    case PipeConfig::P8_32x32_16x16:
        return 8;
    case PipeConfig::P16_32x32_8x16:
        return 16;
    default:
        return 0;
    }
}

u32 CalculateTileSplit(TileMode tile_mode, ArrayMode array_mode, MicroTileMode micro_tile_mode, u32 bpp) {
    const u32 sample_split = GetSampleSplit(tile_mode);
    const u32 tile_split_hw = GetTileSplitHw(tile_mode);
    const u32 tile_thickness = GetMicroTileThickness(array_mode);
    const u32 tile_bytes_1x = (bpp * kMicroTileSize * kMicroTileSize * tile_thickness + 7) / 8;
    const u32 color_tile_split = std::max(256u, sample_split * tile_bytes_1x);
    const u32 tile_split = micro_tile_mode == MicroTileMode::Depth ? tile_split_hw : color_tile_split;
    return std::min(kDramRowSize, tile_split);
}

bool CalculateMacrotileMode(TileMode tile_mode, u32 bpp, u32 num_samples, MacroTileMode& out) {
    out = MacroTileMode::Mode_1x4_16;
    if (!IsValidTileMode(static_cast<u32>(tile_mode)) || !std::has_single_bit(num_samples) || num_samples > 16 ||
        bpp < 1 || bpp > 128) {
        return false;
    }
    const ArrayMode array_mode = GetArrayMode(tile_mode);
    if (!IsMacroTiled(array_mode)) {
        return false;
    }
    const MicroTileMode micro_tile_mode = GetMicroTileMode(tile_mode);
    const u32 tile_thickness = GetMicroTileThickness(array_mode);
    const u32 tile_bytes_1x = bpp * kMicroTileSize * kMicroTileSize * tile_thickness / 8;
    const u32 tile_split = CalculateTileSplit(tile_mode, array_mode, micro_tile_mode, bpp);
    const u32 tile_bytes = std::min(tile_split, num_samples * tile_bytes_1x);
    // tiles under 64 bytes give index -1: a PRT mode wraps to 7, any other is no mode
    const u32 index = static_cast<u32>(std::bit_width(tile_bytes / 64)) - 1;
    const u32 mode = IsPrt(array_mode) ? index + 8 : index;
    if (mode >= kMacroModes.size()) {
        return false;
    }
    out = static_cast<MacroTileMode>(mode);
    return true;
}

// ---- sizes

bool GetMacroTileExtents(TileMode tile_mode, u32 bpp, u32 num_samples, bool alt, u32& width, u32& height) {
    width = 0;
    height = 0;
    const u32 row = static_cast<u32>(tile_mode);
    if (num_samples == 0 || num_samples > 8 || row > 26 || bpp < 8 || bpp > 255) {
        return false;
    }
    const u32 samples_log = static_cast<u32>(std::bit_width(num_samples)) - 1;
    const u32 column = static_cast<u32>(std::bit_width(bpp)) - 4; // 8, 16, 32, 64 (96), 128
    const TileExtent& e = kMacroTileExtents[(alt ? 4 : 0) + samples_log][row * 5 + column];
    width = e.width;
    height = e.height;
    return true;
}

bool ImageSizeLinearAligned(u32 pitch, u32 height, u32 bpp, u32 num_samples, AlignedSize& out) {
    out = {};
    const u32 bytes = (bpp + 7) / 8;
    if (bytes == 0) {
        return false;
    }
    const u32 pitch_align = std::max(8u, 64u / bytes);
    u32 pitch_aligned = (pitch + pitch_align - 1) & ~(pitch_align - 1);
    const u32 height_aligned = height;
    u64 log_sz = pitch_aligned * height_aligned * num_samples; // u32 product, as the bridge
    const u32 slice_align = std::max(64u, 256u / bytes);
    // A slice-aligned pitch is found within slice_align steps for every real bpp; a cap instead of a hang otherwise.
    for (u32 steps = 0; log_sz % slice_align; ++steps) {
        if (steps > 4096) {
            return false;
        }
        pitch_aligned += pitch_align;
        log_sz = pitch_aligned * height_aligned * num_samples;
    }
    out = {static_cast<u16>(pitch_aligned), static_cast<u16>(height_aligned), (log_sz * bpp + 7) / 8};
    return true;
}

AlignedSize ImageSizeMicroTiled(u32 pitch, u32 height, u32 thickness, u32 bpp, u32 num_samples) {
    constexpr u32 kAlign = kMicroTileSize;
    u32 pitch_aligned = (pitch + kAlign - 1) & ~(kAlign - 1);
    const u32 height_aligned = (height + kAlign - 1) & ~(kAlign - 1);
    u64 log_sz = (pitch_aligned * height_aligned * bpp * num_samples + 7) / 8; // u32 arithmetic, as the bridge
    while ((log_sz * thickness) % 256) {
        pitch_aligned += kAlign;
        log_sz = (pitch_aligned * height_aligned * bpp * num_samples + 7) / 8;
    }
    return {static_cast<u16>(pitch_aligned), static_cast<u16>(height_aligned), log_sz};
}

bool ImageSizeMacroTiled(u32 pitch, u32 height, u32 bpp, u32 num_samples, TileMode tile_mode, bool alt,
                         AlignedSize& out) {
    out = {};
    u32 pitch_align = 0;
    u32 height_align = 0;
    if (!GetMacroTileExtents(tile_mode, bpp, num_samples, alt, pitch_align, height_align) || pitch_align == 0 ||
        height_align == 0) {
        return false;
    }
    const u32 pitch_aligned = (pitch + pitch_align - 1) & ~(pitch_align - 1);
    const u32 height_aligned = (height + height_align - 1) & ~(height_align - 1);
    const u32 log_sz = pitch_aligned * height_aligned * num_samples;
    out = {static_cast<u16>(pitch_aligned), static_cast<u16>(height_aligned), (log_sz * bpp + 7) / 8}; // u32, as there
    return true;
}

bool IsMacroTiledMip(u32 pitch, u32 height, u32 bpp, u32 num_samples, TileMode tile_mode, u32 mip_n, bool alt,
                     bool& ok) {
    ok = true;
    if (mip_n == 0) {
        return true;
    }
    u32 pitch_align = 0;
    u32 height_align = 0;
    if (!GetMacroTileExtents(tile_mode, bpp, num_samples, alt, pitch_align, height_align)) {
        ok = false;
        return false;
    }
    return pitch >= pitch_align && height >= height_align;
}

bool ComputeImageSize(u32 pitch, u32 height, u32 depth, u32 levels, u32 layers, u32 num_bits, u32 num_samples,
                      bool is_block, bool is_pow2, bool alt_tile, TileMode tile_mode, ArrayMode array_mode,
                      u32* out_size, u32* out_micro_tiled_mips, MipInfo* out_mips_layout) {
    const u32 thickness = GetMicroTileThickness(array_mode);
    const bool macro = IsMacroTiled(array_mode);
    *out_size = 0;
    *out_micro_tiled_mips = 0;
    if (levels > kMaxMips) {
        return false;
    }
    for (u32 mip = 0; mip < levels; ++mip) {
        u32 mip_w = pitch >> mip;
        u32 mip_h = height >> mip;
        if (is_block) {
            mip_w = (mip_w + 3) / 4;
            mip_h = (mip_h + 3) / 4;
        }
        mip_w = std::max(mip_w, 1u);
        mip_h = std::max(mip_h, 1u);
        u32 mip_d = std::max(depth >> mip, 1u);
        if (is_pow2) {
            mip_w = std::bit_ceil(mip_w);
            mip_h = std::bit_ceil(mip_h);
            mip_d = std::bit_ceil(mip_d);
        }

        MipInfo& mip_info = out_mips_layout[mip];
        u32 mip_thickness = 1;
        AlignedSize aligned;
        if (array_mode == ArrayMode::ArrayLinearAligned) {
            if (!ImageSizeLinearAligned(mip_w, mip_h, num_bits, num_samples, aligned)) {
                return false;
            }
        } else {
            bool ok = true;
            const bool macro_mip =
                macro && IsMacroTiledMip(mip_w, mip_h, num_bits, num_samples, tile_mode, mip, alt_tile, ok);
            if (!ok) {
                return false;
            }
            if (macro_mip) {
                mip_thickness = thickness;
                if (!ImageSizeMacroTiled(mip_w, mip_h, num_bits, num_samples, tile_mode, alt_tile, aligned)) {
                    return false;
                }
            } else {
                mip_thickness = std::min(thickness, 4u);
                aligned = ImageSizeMicroTiled(mip_w, mip_h, mip_thickness, num_bits, num_samples);
                if (macro) {
                    *out_micro_tiled_mips |= 1u << mip;
                }
            }
        }
        mip_info.pitch = aligned.pitch;
        mip_info.height = aligned.height;
        mip_info.size = static_cast<u32>(aligned.size);
        if (is_block) {
            mip_info.pitch = std::max<u16>(static_cast<u16>(mip_info.pitch * 4), 32);
            mip_info.height = std::max<u16>(static_cast<u16>(mip_info.height * 4), 32);
        }
        u32 num_slices = mip_d * layers;
        num_slices += (0u - num_slices) & (mip_thickness - 1);
        mip_info.size *= num_slices;
        mip_info.offset = *out_size;
        *out_size += mip_info.size;
    }
    return true;
}

// ---- T#, V#, CB

namespace TSharp {

u64 BaseAddressField(const Gpu::Shader::ImageDesc& d) {
    return d.raw[0] | (u64{d.raw[1] & 0x3Fu} << 32);
}

u32 MinLod(const Gpu::Shader::ImageDesc& d) {
    return (d.raw[1] >> 8) & 0xFFF;
}

bool AltTileMode(const Gpu::Shader::ImageDesc& d) {
    return (d.raw[6] >> 24) & 1;
}

ImageType Type(const Gpu::Shader::ImageDesc& d) {
    return static_cast<ImageType>(d.Type());
}

TileMode Tiling(const Gpu::Shader::ImageDesc& d) {
    return static_cast<TileMode>(d.TilingIndex());
}

bool IsTiled(const Gpu::Shader::ImageDesc& d) {
    return IsTiledMode(Tiling(d));
}

DataFormat DataFmt(const Gpu::Shader::ImageDesc& d) {
    return RemapDataFormat(static_cast<DataFormat>(d.DataFormat()));
}

NumberFormat NumberFmt(const Gpu::Shader::ImageDesc& d) {
    return RemapNumberFormat(static_cast<NumberFormat>(d.NumFormat()), static_cast<DataFormat>(d.DataFormat()));
}

CompMapping DstSelect(const Gpu::Shader::ImageDesc& d) {
    return RemapSwizzle(static_cast<DataFormat>(d.DataFormat()),
                        CompMapping::FromRaw(d.DstSel(0), d.DstSel(1), d.DstSel(2), d.DstSel(3)));
}

u32 NumLayers(const Gpu::Shader::ImageDesc& d) {
    u32 slices = d.Depth(); // DEPTH + 1: layers of an array
    const ImageType type = Type(d);
    if (type == ImageType::Color3D) {
        slices = 1; // the volume's depth
    } else if (type == ImageType::Cube) {
        slices *= 6; // whole cubes
    }
    if (d.Pow2Pad()) {
        slices = std::bit_ceil(slices);
    }
    return slices;
}

namespace {
bool IsMsaa(ImageType type) {
    return type == ImageType::Color2DMsaa || type == ImageType::Color2DMsaaArray;
}
} // namespace

u32 NumLevels(const Gpu::Shader::ImageDesc& d) {
    return IsMsaa(Type(d)) ? 1 : d.LastLevel() + 1;
}

u32 NumSamples(const Gpu::Shader::ImageDesc& d) {
    return IsMsaa(Type(d)) ? 1u << d.LastLevel() : 1;
}

ImageType BaseType(const Gpu::Shader::ImageDesc& d) {
    const ImageType type = Type(d);
    if (type == ImageType::Color1DArray) {
        return ImageType::Color1D;
    }
    if (type == ImageType::Color2DArray || IsMsaa(type) || type == ImageType::Cube) {
        return ImageType::Color2D;
    }
    return type;
}

ImageType ViewType(const Gpu::Shader::ImageDesc& d, bool is_array) {
    const ImageType type = Type(d);
    if (!is_array) {
        if (type == ImageType::Color1DArray) {
            return ImageType::Color1D;
        }
        if (type == ImageType::Color2DArray) {
            return ImageType::Color2D;
        }
        if (type == ImageType::Color2DMsaaArray) {
            return ImageType::Color2DMsaa;
        }
    }
    return type;
}

u32 NumViewLevels(const Gpu::Shader::ImageDesc& d, bool is_array) {
    if (IsMsaa(ViewType(d, is_array))) {
        return 1;
    }
    const u32 max_level = std::min<u32>(d.LastLevel() + 1, NumLevels(d));
    return max_level > d.BaseLevel() ? max_level - d.BaseLevel() : 1;
}

u32 NumViewLayers(const Gpu::Shader::ImageDesc& d, bool is_array) {
    switch (ViewType(d, is_array)) {
    case ImageType::Color1D:
    case ImageType::Color2D:
    case ImageType::Color2DMsaa:
    case ImageType::Color3D:
        return 1;
    default: {
        const u32 max_array = std::min<u32>(d.LastArray() + 1, NumLayers(d));
        return max_array > d.BaseArray() ? max_array - d.BaseArray() : 1;
    }
    }
}

bool BankSwizzle(const Gpu::Shader::ImageDesc& d, u8& out) {
    out = 0;
    const TileMode tile_mode = Tiling(d);
    if (!IsValidTileMode(static_cast<u32>(tile_mode))) {
        return false;
    }
    const DataFormat dfmt = DataFmt(d);
    if (!AltTileMode(d) || dfmt == DataFormat::FormatInvalid || !IsMacroTiled(GetArrayMode(tile_mode))) {
        return true;
    }
    MacroTileMode macro_tile_mode{};
    if (!CalculateMacrotileMode(tile_mode, NumBitsPerElement(dfmt), NumSamples(d), macro_tile_mode)) {
        return false;
    }
    const u64 banks = GetAltNumBanks(macro_tile_mode);
    out = static_cast<u8>((((banks - 1) << 4) & BaseAddressField(d)) >> 4);
    return true;
}

} // namespace TSharp

CompMapping VSharpDstSelect(const Gpu::Shader::BufferDesc& d) {
    return RemapSwizzle(static_cast<DataFormat>(d.DataFormat()),
                        CompMapping::FromRaw(d.DstSel(0), d.DstSel(1), d.DstSel(2), d.DstSel(3)));
}

NumberFormat ColorTargetFixedNumberFormat(u32 cb_info) {
    // CB number types differ from T#'s in one value: 6 is sRGB here
    const auto number_type = static_cast<NumberFormat>((cb_info >> 8) & 7);
    return number_type == NumberFormat::SnormNz ? NumberFormat::Srgb : number_type;
}

DataFormat ColorTargetDataFormat(u32 cb_info) {
    return RemapDataFormat(static_cast<DataFormat>((cb_info >> 2) & 0x1F));
}

NumberFormat ColorTargetNumberFormat(u32 cb_info) {
    return RemapNumberFormat(ColorTargetFixedNumberFormat(cb_info), static_cast<DataFormat>((cb_info >> 2) & 0x1F));
}

bool ColorTargetSwizzle(u32 cb_info, CompMapping& out) {
    using S = CompSwizzle;
    // [COMP_SWAP][components - 1]: standard, alternate, standard reverse, alternate reverse
    static constexpr std::array<std::array<CompMapping, 4>, 4> kMrtSwizzles{{
        {{{{S::Red, S::Zero, S::Zero, S::Zero}},
          {{S::Red, S::Green, S::Zero, S::Zero}},
          {{S::Red, S::Green, S::Blue, S::Zero}},
          {{S::Red, S::Green, S::Blue, S::Alpha}}}},
        {{{{S::Green, S::Zero, S::Zero, S::Zero}},
          {{S::Red, S::Alpha, S::Zero, S::Zero}},
          {{S::Red, S::Green, S::Alpha, S::Zero}},
          {{S::Blue, S::Green, S::Red, S::Alpha}}}},
        {{{{S::Blue, S::Zero, S::Zero, S::Zero}},
          {{S::Green, S::Red, S::Zero, S::Zero}},
          {{S::Blue, S::Green, S::Red, S::Zero}},
          {{S::Alpha, S::Blue, S::Green, S::Red}}}},
        {{{{S::Alpha, S::Zero, S::Zero, S::Zero}},
          {{S::Alpha, S::Red, S::Zero, S::Zero}},
          {{S::Alpha, S::Green, S::Red, S::Zero}},
          {{S::Alpha, S::Red, S::Green, S::Blue}}}},
    }};
    const auto format = static_cast<DataFormat>((cb_info >> 2) & 0x1F);
    const u32 components = NumComponents(format);
    if (components == 0) {
        out = kIdentityMapping;
        return false;
    }
    out = RemapSwizzle(format, kMrtSwizzles[(cb_info >> 11) & 3][components - 1]);
    return true;
}

// ---- layouts

ImageLayout LayoutFromTSharp(const Gpu::Shader::ImageDesc& sharp, bool is_depth, bool neo_mode) {
    ImageLayout l;
    const u32 tiling = sharp.TilingIndex();
    if (!IsValidTileMode(tiling)) {
        return Refuse(l, "T#: tiling index 27..30 is not a tile mode");
    }
    const DataFormat data_fmt = TSharp::DataFmt(sharp);
    l.tile_mode = static_cast<TileMode>(tiling);
    l.array_mode = GetArrayMode(l.tile_mode);
    l.pixel_format = Vk::SurfaceFormat(data_fmt, TSharp::NumberFmt(sharp));
    if (l.pixel_format == VK_FORMAT_UNDEFINED) {
        return Refuse(l, "T#: data/number format without a host format");
    }
    if (is_depth) {
        l.pixel_format = Vk::PromoteFormatToDepth(static_cast<VkFormat>(l.pixel_format));
        if (l.pixel_format == VK_FORMAT_UNDEFINED) {
            return Refuse(l, "T#: depth read of a format that is not R32/R16");
        }
    }
    l.type = TSharp::BaseType(sharp);
    l.props.is_volume = l.type == ImageType::Color3D;
    l.props.is_tiled = IsTiledMode(l.tile_mode);
    l.props.is_pow2 = sharp.Pow2Pad();
    l.props.is_block = IsBlockCoded(data_fmt);
    l.props.is_depth = is_depth;
    l.size.width = sharp.Width();
    l.size.height = sharp.Height();
    l.size.depth = l.props.is_volume ? sharp.Depth() : 1;
    l.pitch = sharp.Pitch();
    l.levels = static_cast<u16>(TSharp::NumLevels(sharp));
    l.layers = static_cast<u16>(TSharp::NumLayers(sharp));
    l.num_samples = TSharp::NumSamples(sharp);
    l.num_bits = NumBitsPerBlock(data_fmt);
    l.guest_address = sharp.Base();
    if (TSharp::AltTileMode(sharp)) {
        if (!TSharp::BankSwizzle(sharp, l.bank_swizzle)) {
            return Refuse(l, "T#: alt tiling with no macro tile mode for this format/sample count");
        }
        l.alt_tile = neo_mode;
    }
    // UpdateSize. (The bridge's precomputed table for square power-of-two BCn Thin1DThin images holds exactly what
    // ComputeImageSize computes for them.)
    if (l.array_mode == ArrayMode::ArrayLinearGeneral) {
        return Refuse(l, "T#: linear general (tiling index 31) has no layout");
    }
    if (!ComputeImageSize(l.pitch, l.size.height, l.size.depth, l.levels, l.layers, l.num_bits, l.num_samples,
                          l.props.is_block, l.props.is_pow2, l.alt_tile, l.tile_mode, l.array_mode, &l.guest_size,
                          &l.micro_tiled_mips, l.mips.data())) {
        return Refuse(l, "T#: sample count or element size outside the tile tables");
    }
    l.supported = true;
    return l;
}

ImageLayout LayoutFromColorTarget(const Cp::RegisterFile& regs, u32 n, u32 extent_hint, bool neo_mode) {
    ImageLayout l;
    if (n >= 8) {
        return Refuse(l, "colour target: index past 7");
    }
    const ColorRegs cb = ColorRegs::Read(regs, n);
    l.tile_mode = cb.Tiling();
    if (!IsValidTileMode(static_cast<u32>(l.tile_mode))) {
        return Refuse(l, "colour target: tile mode 27..30 is not a tile mode");
    }
    l.props.is_tiled = IsTiledMode(l.tile_mode);
    l.array_mode = GetArrayMode(l.tile_mode);
    const DataFormat data_fmt = ColorTargetDataFormat(cb.info);
    l.pixel_format = Vk::SurfaceFormat(data_fmt, ColorTargetNumberFormat(cb.info));
    if (l.pixel_format == VK_FORMAT_UNDEFINED) {
        return Refuse(l, "colour target: format/number type without a host format");
    }
    l.num_samples = cb.NumSamples();
    l.num_bits = NumBitsPerBlock(data_fmt);
    l.type = ImageType::Color2D;
    const bool hint = extent_hint != 0;
    l.size.width = hint ? (extent_hint & 0xFFFF) : cb.Pitch();
    l.size.height = hint ? (extent_hint >> 16) : cb.Height();
    l.size.depth = 1;
    l.pitch = cb.Pitch();
    l.layers = static_cast<u16>(cb.NumSlices());
    l.meta.cmask_addr = cb.FastClear() ? u64{cb.cmask} << 8 : 0;
    l.meta.fmask_addr = cb.Compression() ? u64{cb.fmask} << 8 : 0;
    l.guest_address = (u64{cb.base} << 8) | (cb.LinearGeneral() ? (cb.SliceStart() & 0xFF) : 0);
    if (l.props.is_tiled) {
        const u32 bytes_per_element = NumBitsPerBlock(static_cast<DataFormat>(cb.Format())) / 8u;
        const u32 slice_size = bytes_per_element * (cb.SliceTileMax() + 1) * 64u * l.num_samples;
        l.guest_size = slice_size * l.layers;
        l.mips[0] = {l.guest_size, static_cast<u16>(l.pitch), static_cast<u16>(cb.Height()), 0};
    } else {
        AlignedSize aligned;
        if (!ImageSizeLinearAligned(l.pitch, l.size.height, l.num_bits, l.num_samples, aligned)) {
            return Refuse(l, "colour target: no linear size for this element size");
        }
        l.guest_size = static_cast<u32>(aligned.size);
        l.guest_size *= l.layers;
        l.mips[0] = {l.guest_size, static_cast<u16>(l.pitch), static_cast<u16>(l.size.height), 0};
    }
    l.alt_tile = neo_mode && cb.AltTileMode();
    l.supported = true;
    return l;
}

ImageLayout LayoutFromDepthTarget(const Cp::RegisterFile& regs, u32 extent_hint, bool write_buffer) {
    ImageLayout l;
    const u32 z_info = regs.Get(Cp::Reg::DbZInfo);
    const u32 z_format = z_info & 3;
    const u32 stencil_format = regs.Get(kDbStencilInfo) & 1;
    const u32 depth_size = regs.Get(kDbDepthSize);
    const u32 slice_tile_max = regs.Get(kDbDepthSlice) & 0x3FFFFF;
    const u32 view = regs.Get(kDbDepthView);
    l.tile_mode = static_cast<TileMode>((z_info >> 20) & 7); // the depth modes 0..7
    l.array_mode = GetArrayMode(l.tile_mode);
    l.pixel_format = Vk::DepthFormat(z_format, stencil_format);
    l.type = ImageType::Color2D;
    l.props.is_tiled = IsTiledMode(l.tile_mode);
    l.props.is_depth = true;
    l.props.has_stencil = stencil_format != 0;
    l.num_samples = 1u << ((z_info >> 2) & 3);
    l.num_bits = z_format == 3 ? 32 : 16;
    const u32 pitch = ((depth_size & 0x7FF) + 1) << 3;
    const u32 height = (((depth_size >> 11) & 0x7FF) + 1) << 3;
    const bool hint = extent_hint != 0;
    l.size.width = hint ? (extent_hint & 0xFFFF) : pitch;
    l.size.height = hint ? (extent_hint >> 16) : height;
    l.size.depth = 1;
    l.pitch = pitch;
    l.layers = static_cast<u16>(((view >> 13) & 0x7FF) + 1);
    l.meta.htile_addr = (z_info >> 29) & 1 ? u64{regs.Get(kDbHtileDataBase)} << 8 : 0;
    l.stencil_addr = u64{regs.Get(write_buffer ? kDbStencilWriteBase : kDbStencilReadBase)} << 8;
    l.guest_address = u64{regs.Get(write_buffer ? kDbZWriteBase : kDbZReadBase)} << 8;
    if (l.pixel_format == VK_FORMAT_UNDEFINED) {
        return Refuse(l, "depth target: Z format 2 (reserved) or no depth and no stencil");
    }
    if (z_format == 0) {
        return Refuse(l, "depth target: stencil only (no Z format), which the bridge cannot size");
    }
    // the depth modes are all tiled
    const u32 bytes_per_element = l.num_bits >> 3;
    l.guest_size = (slice_tile_max + 1) * 64 * bytes_per_element * l.num_samples * l.layers;
    l.mips[0] = {l.guest_size, static_cast<u16>(pitch), static_cast<u16>(height), 0};
    l.stencil_size = l.guest_size / (l.num_bits >> 3); // one byte per sample, the depth's tiling
    l.supported = true;
    return l;
}

ImageLayout LayoutFromVideoOut(u64 address, u32 width, u32 height, u32 pitch_in_pixel, u32 pixel_format,
                               u32 tiling_mode) {
    (void)pitch_in_pixel;
    ImageLayout l;
    constexpr u32 kTile = 0;
    constexpr u32 kLinear = 1;
    l.props.is_tiled = tiling_mode == kTile;
    l.tile_mode = l.props.is_tiled ? TileMode::Display2DThin : TileMode::DisplayLinearAligned;
    l.array_mode = GetArrayMode(l.tile_mode);
    switch (pixel_format) {
    case 0x80002200: // A8B8G8R8 sRGB
    case 0x80000000: // A8R8G8B8 sRGB: RGBA here, BGRA in the frame's view
        l.pixel_format = VK_FORMAT_R8G8B8A8_SRGB;
        break;
    case 0x88060000: // A2R10G10B10
    case 0x88000000: // A2R10G10B10 sRGB
    case 0x88740000: // A2R10G10B10 BT.2020 PQ
        l.pixel_format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
        break;
    default:
        return Refuse(l, "VideoOut: pixel format the bridge does not present (A16R16G16B16 float or unknown)");
    }
    l.num_samples = 1;
    l.type = ImageType::Color2D;
    l.size = {width, height, 1};
    l.pitch = tiling_mode == kLinear ? width : (width + 127) & ~127u;
    l.num_bits = 32;
    l.guest_address = address;
    if (!ComputeImageSize(l.pitch, l.size.height, l.size.depth, l.levels, l.layers, l.num_bits, l.num_samples, false,
                          false, false, l.tile_mode, l.array_mode, &l.guest_size, &l.micro_tiled_mips, l.mips.data())) {
        return Refuse(l, "VideoOut: no layout");
    }
    l.supported = true;
    return l;
}

bool IsCompatible(const ImageLayout& a, const ImageLayout& b) {
    const auto fa = static_cast<VkFormat>(a.pixel_format);
    const auto fb = static_cast<VkFormat>(b.pixel_format);
    return (Vk::IsVulkanFormatCompatible(fa, fb) || Vk::IsVulkanFormatCompatible(fb, fa)) &&
           a.num_samples == b.num_samples && a.num_bits == b.num_bits;
}

s32 MipOf(const ImageLayout& image, const ImageLayout& info) {
    if (!IsCompatible(image, info) || info.array_mode != image.array_mode || image.levels != 1) {
        return -1;
    }
    const u32 info_dim = info.props.is_block ? 2 : 0;
    const u32 this_dim = image.props.is_block ? 2 : 0;
    s32 mip = -1;
    const u32 levels = std::min<u32>(info.levels, kMaxMips);
    for (u32 m = 0; m < levels; ++m) {
        const MipInfo& level = info.mips[m];
        const u64 mip_base = info.guest_address + level.offset;
        const u64 mip_end = mip_base + level.size;
        const u32 slice_size = info.layers != 0 ? level.size / info.layers : 0;
        if (image.guest_address >= mip_base && image.guest_address < mip_end && slice_size != 0 &&
            (image.guest_address - mip_base) % slice_size == 0 &&
            (image.pitch >> this_dim) == static_cast<u32>(level.pitch >> info_dim)) {
            mip = static_cast<s32>(m);
            break;
        }
    }
    if (mip < 0) {
        return -1;
    }
    // the levels' 2D block dimensions agree
    const u32 mip_w = std::max(info.size.width >> (mip + info_dim), 1u);
    const u32 mip_h = std::max(info.size.height >> (mip + info_dim), 1u);
    const u32 this_w = std::max(image.size.width >> this_dim, 1u);
    const u32 this_h = std::max(image.size.height >> this_dim, 1u);
    if (this_w != mip_w || this_h != mip_h) {
        return -1;
    }
    const u32 mip_d = std::max(info.size.depth >> mip, 1u);
    if (info.type == ImageType::Color3D && image.type == ImageType::Color2D) {
        if (image.layers != mip_d) { // a 2D array copied into a 3D level: one layer per slice
            return -1;
        }
    } else if (image.type != info.type) {
        return -1;
    }
    return mip;
}

s32 SliceOf(const ImageLayout& image, const ImageLayout& info, s32 mip) {
    if (!IsCompatible(image, info) || image.type != info.type || mip < 0 || mip >= static_cast<s32>(kMaxMips)) {
        return -1;
    }
    const u32 info_dim = info.props.is_block ? 2 : 0;
    const u32 mip_w = std::max(info.size.width >> (mip + info_dim), 1u);
    const u32 mip_h = std::max(info.size.height >> (mip + info_dim), 1u);
    const u32 mip_p = std::max<u32>(info.mips[mip].pitch >> info_dim, 1u);
    const u32 this_dim = image.props.is_block ? 2 : 0;
    const u32 this_w = std::max(image.size.width >> this_dim, 1u);
    const u32 this_h = std::max(image.size.height >> this_dim, 1u);
    const u32 this_p = std::max(image.pitch >> this_dim, 1u);
    if (this_w != mip_w || this_h != mip_h || this_p != mip_p) {
        return -1;
    }
    const u32 slice_size = info.layers != 0 ? info.mips[mip].size / info.layers : 0;
    if (slice_size == 0 || image.guest_size % slice_size != 0) {
        return -1;
    }
    const u64 addr_diff = image.guest_address - (info.guest_address + info.mips[mip].offset);
    if (image.guest_size == 0 || addr_diff % image.guest_size != 0) {
        return -1;
    }
    return static_cast<s32>(addr_diff / image.guest_size);
}

// ---- the detiler

TilingParams BuildTilingParams(const ImageLayout& layout, bool is_tiler, u32 num_mips) {
    TilingParams p;
    if (!layout.supported) {
        p.reason = "the image has no layout";
        return p;
    }
    p.needed = layout.props.is_tiled;
    if (!p.needed) {
        p.supported = true; // copied as it is
        return p;
    }
    switch (layout.num_bits) {
    case 8:
    case 16:
    case 32:
    case 64:
    case 96:
    case 128:
        break;
    default:
        p.reason = "no detiler for this element size";
        return p;
    }
    p.num_bits = layout.num_bits;
    const MicroTileMode micro_tile_mode = GetMicroTileMode(layout.tile_mode);
    p.macro = IsMacroTiled(layout.array_mode);
    p.spec[0] = layout.num_samples;
    p.spec[1] = static_cast<u32>(micro_tile_mode);
    p.spec[2] = GetMicroTileThickness(layout.array_mode);
    p.spec[3] = is_tiler ? 1 : 0;
    if (p.macro) {
        MacroTileMode macro_tile_mode{};
        if (!CalculateMacrotileMode(layout.tile_mode, layout.num_bits, layout.num_samples, macro_tile_mode)) {
            p.reason = "no macro tile mode for this element size/sample count";
            return p;
        }
        p.spec[4] = static_cast<u32>(layout.array_mode);
        p.spec[5] = static_cast<u32>(GetPipeConfig(layout.tile_mode));
        p.spec[6] = GetBankWidth(macro_tile_mode);
        p.spec[7] = GetBankHeight(macro_tile_mode);
        p.spec[8] = GetNumBanks(macro_tile_mode);
        p.spec[9] = static_cast<u32>(std::bit_width(p.spec[8])) - 1;
        p.spec[10] = CalculateTileSplit(layout.tile_mode, layout.array_mode, micro_tile_mode, layout.num_bits);
        p.spec[11] = GetMacrotileAspect(macro_tile_mode);
    }
    p.spec_count = p.macro ? 12 : 4;

    p.info.bank_swizzle = layout.bank_swizzle;
    p.info.micro_tiled_mips = layout.micro_tiled_mips;
    p.info.num_mips = num_mips == ~0u ? layout.levels : std::min(num_mips, kMaxMips);
    const u32 count = std::min(p.info.num_mips, kMaxMips);
    for (u32 mip = 0; mip < count; ++mip) {
        MipInfo& mip_info = p.info.mips[mip];
        mip_info = layout.mips[mip];
        if (layout.props.is_block) { // in blocks
            mip_info.pitch = std::max<u16>(static_cast<u16>((mip_info.pitch + 3) / 4), 1);
            mip_info.height = std::max<u16>(static_cast<u16>((mip_info.height + 3) / 4), 1);
        }
    }
    p.dispatch_x = (layout.guest_size / (layout.num_bits / 8)) / 64;
    p.supported = true;
    return p;
}

} // namespace SecondSon::Gpu
