// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — guest formats and fixed-function state as Vulkan values (P4.4). See vk_formats.h. Ported from shadPS4's
// renderer_vulkan/liverpool_to_vk.cpp, texture_cache/{image,image_view,sampler}.cpp and common/number_utils.cpp
// (GPL-2.0-or-later).
#include "runtime/gpu/vulkan/vk_formats.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>

#include "runtime/gpu/cp_registers.h"
#include "runtime/gpu/shader/shader_resources.h"

namespace SecondSon::Gpu::Vk {

namespace {

using DF = DataFormat;
using NF = NumberFormat;

float SmallFloat(u16 val, int mant_bits, double denorm_scale) {
    const int exponent0 = (val >> mant_bits) & 0x1F;
    const int mantissa = val & ((1 << mant_bits) - 1);
    float f = 0.0f;
    if (exponent0 == 0) {
        if (mantissa != 0) {
            f = static_cast<float>(denorm_scale) * static_cast<float>(mantissa);
        }
    } else if (exponent0 == 31) {
        const u32 ui = 0x7f800000u | static_cast<u32>(mantissa);
        std::memcpy(&f, &ui, 4);
    } else {
        const int exponent = exponent0 - 15;
        const float scale =
            exponent < 0 ? 1.0f / static_cast<float>(1 << -exponent) : static_cast<float>(1 << exponent);
        f = scale * (1.0f + static_cast<float>(mantissa) / static_cast<float>(1 << mant_bits));
    }
    return f;
}
float Uf11ToF32(u16 v) {
    return SmallFloat(static_cast<u16>(v & 0x7ff), 6, 1.0 / (1 << 20));
}
float Uf10ToF32(u16 v) {
    return SmallFloat(static_cast<u16>(v & 0x3ff), 5, 1.0 / (1 << 19));
}

float Uf16ToF32(u16 h) {
    const u32 sign = static_cast<u32>(h & 0x8000) << 16;
    const u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    u32 bits;
    if (exp == 0) {
        if (mant == 0) {
            bits = sign;
        } else {
            int e = -1;
            do {
                ++e;
                mant <<= 1;
            } while ((mant & 0x400) == 0);
            bits = sign | static_cast<u32>(127 - 15 - e) << 23 | (mant & 0x3FF) << 13;
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | mant << 13;
    } else {
        bits = sign | (exp + 127 - 15) << 23 | mant << 13;
    }
    return std::bit_cast<float>(bits);
}

float Unorm(u32 v, float max) {
    return float(static_cast<float>(v) * (1.0f / max));
}
float Snorm(s32 v, float max) {
    return float(static_cast<float>(v) * (1.0f / max));
}
s32 S8(u32 v) {
    return static_cast<s8>(static_cast<u8>(v & 0xff));
}
s32 S16(u32 v) {
    return static_cast<s16>(static_cast<u16>(v & 0xffff));
}

constexpr VkFormatFeatureFlags2 kBufferRead = VK_FORMAT_FEATURE_2_VERTEX_BUFFER_BIT;
constexpr VkFormatFeatureFlags2 kImageRead =
    VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
constexpr VkFormatFeatureFlags2 kImageWrite = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT |
                                              VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT;
constexpr VkFormatFeatureFlags2 kMrt = VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;

constexpr VkFormatFeatureFlags2 DataFormatFlags(DF f) {
    const u32 v = static_cast<u32>(f);
    if (v <= 12 || v == 14) { // Invalid .. 16_16_16_16, 32_32_32_32
        return kBufferRead | kImageRead | kImageWrite | kMrt;
    }
    if (v == 13) {
        return kBufferRead | kImageRead;
    }
    if (v >= 16 && v <= 19) {
        return kImageRead | kImageWrite | kMrt;
    }
    if (v >= 20 && v <= 22) {
        return kImageRead | kMrt;
    }
    if (v >= 44 && v <= 56) {
        return kImageRead | kImageWrite;
    }
    return kImageRead;
}

constexpr VkFormatFeatureFlags2 NumberFormatFlags(NF n) {
    switch (n) {
    case NF::Unorm:
    case NF::Snorm:
    case NF::Uint:
    case NF::Sint:
    case NF::Float:
        return kBufferRead | kImageRead | kImageWrite | kMrt;
    case NF::Uscaled:
    case NF::Sscaled:
    case NF::SnormNz:
        return kBufferRead | kImageRead;
    case NF::Srgb:
        return kImageRead | kMrt;
    default:
        return kImageRead;
    }
}

constexpr SurfaceFormatInfo Sf(DF d, NF n, VkFormat v) {
    return {d, n, v, DataFormatFlags(d) & NumberFormatFlags(n)};
}

constexpr std::array kSurfaceFormats{
    Sf(DF::FormatInvalid, NF::Unorm, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Snorm, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Uscaled, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Sscaled, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Uint, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Sint, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::SnormNz, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Float, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Srgb, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Ubnorm, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::UbnormNz, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Ubint, VK_FORMAT_UNDEFINED),
    Sf(DF::FormatInvalid, NF::Ubscaled, VK_FORMAT_UNDEFINED),
    Sf(DF::Format8, NF::Unorm, VK_FORMAT_R8_UNORM),
    Sf(DF::Format8, NF::Snorm, VK_FORMAT_R8_SNORM),
    Sf(DF::Format8, NF::Uint, VK_FORMAT_R8_UINT),
    Sf(DF::Format8, NF::Sint, VK_FORMAT_R8_SINT),
    Sf(DF::Format8, NF::Srgb, VK_FORMAT_R8_SRGB),
    Sf(DF::Format16, NF::Unorm, VK_FORMAT_R16_UNORM),
    Sf(DF::Format16, NF::Snorm, VK_FORMAT_R16_SNORM),
    Sf(DF::Format16, NF::Uint, VK_FORMAT_R16_UINT),
    Sf(DF::Format16, NF::Sint, VK_FORMAT_R16_SINT),
    Sf(DF::Format16, NF::Float, VK_FORMAT_R16_SFLOAT),
    Sf(DF::Format8_8, NF::Unorm, VK_FORMAT_R8G8_UNORM),
    Sf(DF::Format8_8, NF::Snorm, VK_FORMAT_R8G8_SNORM),
    Sf(DF::Format8_8, NF::Uint, VK_FORMAT_R8G8_UINT),
    Sf(DF::Format8_8, NF::Sint, VK_FORMAT_R8G8_SINT),
    Sf(DF::Format8_8, NF::Srgb, VK_FORMAT_R8G8_SRGB),
    Sf(DF::Format32, NF::Uint, VK_FORMAT_R32_UINT),
    Sf(DF::Format32, NF::Sint, VK_FORMAT_R32_SINT),
    Sf(DF::Format32, NF::Float, VK_FORMAT_R32_SFLOAT),
    Sf(DF::Format16_16, NF::Unorm, VK_FORMAT_R16G16_UNORM),
    Sf(DF::Format16_16, NF::Snorm, VK_FORMAT_R16G16_SNORM),
    Sf(DF::Format16_16, NF::Uint, VK_FORMAT_R16G16_UINT),
    Sf(DF::Format16_16, NF::Sint, VK_FORMAT_R16G16_SINT),
    Sf(DF::Format16_16, NF::Float, VK_FORMAT_R16G16_SFLOAT),
    Sf(DF::Format10_11_11, NF::Float, VK_FORMAT_B10G11R11_UFLOAT_PACK32),
    Sf(DF::Format2_10_10_10, NF::Unorm, VK_FORMAT_A2B10G10R10_UNORM_PACK32),
    Sf(DF::Format2_10_10_10, NF::Snorm, VK_FORMAT_A2B10G10R10_SNORM_PACK32),
    Sf(DF::Format2_10_10_10, NF::Uint, VK_FORMAT_A2B10G10R10_UINT_PACK32),
    Sf(DF::Format2_10_10_10, NF::Sint, VK_FORMAT_A2B10G10R10_SINT_PACK32),
    Sf(DF::Format8_8_8_8, NF::Unorm, VK_FORMAT_R8G8B8A8_UNORM),
    Sf(DF::Format8_8_8_8, NF::Snorm, VK_FORMAT_R8G8B8A8_SNORM),
    Sf(DF::Format8_8_8_8, NF::Uint, VK_FORMAT_R8G8B8A8_UINT),
    Sf(DF::Format8_8_8_8, NF::Sint, VK_FORMAT_R8G8B8A8_SINT),
    Sf(DF::Format8_8_8_8, NF::Srgb, VK_FORMAT_R8G8B8A8_SRGB),
    Sf(DF::Format32_32, NF::Uint, VK_FORMAT_R32G32_UINT),
    Sf(DF::Format32_32, NF::Sint, VK_FORMAT_R32G32_SINT),
    Sf(DF::Format32_32, NF::Float, VK_FORMAT_R32G32_SFLOAT),
    Sf(DF::Format16_16_16_16, NF::Unorm, VK_FORMAT_R16G16B16A16_UNORM),
    Sf(DF::Format16_16_16_16, NF::Snorm, VK_FORMAT_R16G16B16A16_SNORM),
    Sf(DF::Format16_16_16_16, NF::Uint, VK_FORMAT_R16G16B16A16_UINT),
    Sf(DF::Format16_16_16_16, NF::Sint, VK_FORMAT_R16G16B16A16_SINT),
    Sf(DF::Format16_16_16_16, NF::SnormNz, VK_FORMAT_R16G16B16A16_SNORM),
    Sf(DF::Format16_16_16_16, NF::Float, VK_FORMAT_R16G16B16A16_SFLOAT),
    Sf(DF::Format32_32_32, NF::Uint, VK_FORMAT_R32G32B32_UINT),
    Sf(DF::Format32_32_32, NF::Sint, VK_FORMAT_R32G32B32_SINT),
    Sf(DF::Format32_32_32, NF::Float, VK_FORMAT_R32G32B32_SFLOAT),
    Sf(DF::Format32_32_32_32, NF::Uint, VK_FORMAT_R32G32B32A32_UINT),
    Sf(DF::Format32_32_32_32, NF::Sint, VK_FORMAT_R32G32B32A32_SINT),
    Sf(DF::Format32_32_32_32, NF::Float, VK_FORMAT_R32G32B32A32_SFLOAT),
    Sf(DF::Format5_6_5, NF::Unorm, VK_FORMAT_R5G6B5_UNORM_PACK16),
    Sf(DF::Format1_5_5_5, NF::Unorm, VK_FORMAT_A1R5G5B5_UNORM_PACK16),
    Sf(DF::Format5_5_5_1, NF::Unorm, VK_FORMAT_R5G5B5A1_UNORM_PACK16),
    Sf(DF::Format4_4_4_4, NF::Unorm, VK_FORMAT_B4G4R4A4_UNORM_PACK16),
    Sf(DF::Format5_9_9_9, NF::Float, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32),
    Sf(DF::FormatBc1, NF::Unorm, VK_FORMAT_BC1_RGBA_UNORM_BLOCK),
    Sf(DF::FormatBc1, NF::Srgb, VK_FORMAT_BC1_RGBA_SRGB_BLOCK),
    Sf(DF::FormatBc2, NF::Unorm, VK_FORMAT_BC2_UNORM_BLOCK),
    Sf(DF::FormatBc2, NF::Srgb, VK_FORMAT_BC2_SRGB_BLOCK),
    Sf(DF::FormatBc3, NF::Unorm, VK_FORMAT_BC3_UNORM_BLOCK),
    Sf(DF::FormatBc3, NF::Srgb, VK_FORMAT_BC3_SRGB_BLOCK),
    Sf(DF::FormatBc4, NF::Unorm, VK_FORMAT_BC4_UNORM_BLOCK),
    Sf(DF::FormatBc4, NF::Snorm, VK_FORMAT_BC4_SNORM_BLOCK),
    Sf(DF::FormatBc5, NF::Unorm, VK_FORMAT_BC5_UNORM_BLOCK),
    Sf(DF::FormatBc5, NF::Snorm, VK_FORMAT_BC5_SNORM_BLOCK),
    Sf(DF::FormatBc6, NF::Unorm, VK_FORMAT_BC6H_UFLOAT_BLOCK),
    Sf(DF::FormatBc6, NF::Snorm, VK_FORMAT_BC6H_SFLOAT_BLOCK),
    Sf(DF::FormatBc7, NF::Unorm, VK_FORMAT_BC7_UNORM_BLOCK),
    Sf(DF::FormatBc7, NF::Srgb, VK_FORMAT_BC7_SRGB_BLOCK),
};

constexpr std::array<VkFormat, kDataFormatCount * kNumberFormatCount> kSurfaceTable = [] {
    std::array<VkFormat, kDataFormatCount * kNumberFormatCount> t{};
    for (auto& e : t) {
        e = VK_FORMAT_UNDEFINED;
    }
    for (const auto& f : kSurfaceFormats) {
        t[static_cast<u32>(f.data_format) * kNumberFormatCount + static_cast<u32>(f.number_format)] = f.vk_format;
    }
    return t;
}();

constexpr VkFormatFeatureFlags2 kDsa = VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT;
constexpr std::array kDepthFormats{
    DepthFormatInfo{0, 0, VK_FORMAT_UNDEFINED, kDsa}, DepthFormatInfo{0, 1, VK_FORMAT_D32_SFLOAT_S8_UINT, kDsa},
    DepthFormatInfo{1, 0, VK_FORMAT_D16_UNORM, kDsa}, DepthFormatInfo{1, 1, VK_FORMAT_D16_UNORM_S8_UINT, kDsa},
    DepthFormatInfo{3, 0, VK_FORMAT_D32_SFLOAT, kDsa}, DepthFormatInfo{3, 1, VK_FORMAT_D32_SFLOAT_S8_UINT, kDsa},
};

struct CompatEntry {
    VkFormat format;
    u32 cls; ///< bits per texel; 1000+ compressed, 2000+ depth: classes of their own
};
constexpr std::array kCompat{
    CompatEntry{VK_FORMAT_R8_UNORM, 8}, CompatEntry{VK_FORMAT_R8_SNORM, 8}, CompatEntry{VK_FORMAT_R8_UINT, 8},
    CompatEntry{VK_FORMAT_R8_SINT, 8}, CompatEntry{VK_FORMAT_R8_SRGB, 8}, CompatEntry{VK_FORMAT_R16_UNORM, 16},
    CompatEntry{VK_FORMAT_R16_SNORM, 16}, CompatEntry{VK_FORMAT_R16_UINT, 16}, CompatEntry{VK_FORMAT_R16_SINT, 16},
    CompatEntry{VK_FORMAT_R16_SFLOAT, 16}, CompatEntry{VK_FORMAT_R8G8_UNORM, 16},
    CompatEntry{VK_FORMAT_R8G8_SNORM, 16}, CompatEntry{VK_FORMAT_R8G8_UINT, 16}, CompatEntry{VK_FORMAT_R8G8_SINT, 16},
    CompatEntry{VK_FORMAT_R8G8_SRGB, 16}, CompatEntry{VK_FORMAT_R5G6B5_UNORM_PACK16, 16},
    CompatEntry{VK_FORMAT_A1R5G5B5_UNORM_PACK16, 16}, CompatEntry{VK_FORMAT_R5G5B5A1_UNORM_PACK16, 16},
    CompatEntry{VK_FORMAT_B4G4R4A4_UNORM_PACK16, 16}, CompatEntry{VK_FORMAT_R32_UINT, 32},
    CompatEntry{VK_FORMAT_R32_SINT, 32}, CompatEntry{VK_FORMAT_R32_SFLOAT, 32}, CompatEntry{VK_FORMAT_R16G16_UNORM, 32},
    CompatEntry{VK_FORMAT_R16G16_SNORM, 32}, CompatEntry{VK_FORMAT_R16G16_UINT, 32},
    CompatEntry{VK_FORMAT_R16G16_SINT, 32}, CompatEntry{VK_FORMAT_R16G16_SFLOAT, 32},
    CompatEntry{VK_FORMAT_B10G11R11_UFLOAT_PACK32, 32}, CompatEntry{VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 32},
    CompatEntry{VK_FORMAT_A2B10G10R10_UNORM_PACK32, 32}, CompatEntry{VK_FORMAT_A2B10G10R10_SNORM_PACK32, 32},
    CompatEntry{VK_FORMAT_A2B10G10R10_UINT_PACK32, 32}, CompatEntry{VK_FORMAT_A2B10G10R10_SINT_PACK32, 32},
    CompatEntry{VK_FORMAT_A2R10G10B10_UNORM_PACK32, 32}, CompatEntry{VK_FORMAT_R8G8B8A8_UNORM, 32},
    CompatEntry{VK_FORMAT_R8G8B8A8_SNORM, 32}, CompatEntry{VK_FORMAT_R8G8B8A8_UINT, 32},
    CompatEntry{VK_FORMAT_R8G8B8A8_SINT, 32}, CompatEntry{VK_FORMAT_R8G8B8A8_SRGB, 32},
    CompatEntry{VK_FORMAT_B8G8R8A8_UNORM, 32}, CompatEntry{VK_FORMAT_B8G8R8A8_SRGB, 32},
    CompatEntry{VK_FORMAT_R32G32_UINT, 64}, CompatEntry{VK_FORMAT_R32G32_SINT, 64},
    CompatEntry{VK_FORMAT_R32G32_SFLOAT, 64}, CompatEntry{VK_FORMAT_R16G16B16A16_UNORM, 64},
    CompatEntry{VK_FORMAT_R16G16B16A16_SNORM, 64}, CompatEntry{VK_FORMAT_R16G16B16A16_UINT, 64},
    CompatEntry{VK_FORMAT_R16G16B16A16_SINT, 64}, CompatEntry{VK_FORMAT_R16G16B16A16_SFLOAT, 64},
    CompatEntry{VK_FORMAT_R32G32B32_UINT, 96}, CompatEntry{VK_FORMAT_R32G32B32_SINT, 96},
    CompatEntry{VK_FORMAT_R32G32B32_SFLOAT, 96}, CompatEntry{VK_FORMAT_R32G32B32A32_UINT, 128},
    CompatEntry{VK_FORMAT_R32G32B32A32_SINT, 128}, CompatEntry{VK_FORMAT_R32G32B32A32_SFLOAT, 128},
    CompatEntry{VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 1001}, CompatEntry{VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 1001},
    CompatEntry{VK_FORMAT_BC2_UNORM_BLOCK, 1002}, CompatEntry{VK_FORMAT_BC2_SRGB_BLOCK, 1002},
    CompatEntry{VK_FORMAT_BC3_UNORM_BLOCK, 1003}, CompatEntry{VK_FORMAT_BC3_SRGB_BLOCK, 1003},
    CompatEntry{VK_FORMAT_BC4_UNORM_BLOCK, 1004}, CompatEntry{VK_FORMAT_BC4_SNORM_BLOCK, 1004},
    CompatEntry{VK_FORMAT_BC5_UNORM_BLOCK, 1005}, CompatEntry{VK_FORMAT_BC5_SNORM_BLOCK, 1005},
    CompatEntry{VK_FORMAT_BC6H_UFLOAT_BLOCK, 1006}, CompatEntry{VK_FORMAT_BC6H_SFLOAT_BLOCK, 1006},
    CompatEntry{VK_FORMAT_BC7_UNORM_BLOCK, 1007}, CompatEntry{VK_FORMAT_BC7_SRGB_BLOCK, 1007},
    CompatEntry{VK_FORMAT_D16_UNORM, 2001}, CompatEntry{VK_FORMAT_D32_SFLOAT, 2002},
    CompatEntry{VK_FORMAT_D16_UNORM_S8_UINT, 2003}, CompatEntry{VK_FORMAT_D32_SFLOAT_S8_UINT, 2004},
};

constexpr std::array<VkFormat, kCompat.size()> kCompatFormats = [] {
    std::array<VkFormat, kCompat.size()> r{};
    for (size_t i = 0; i < kCompat.size(); ++i) {
        r[i] = kCompat[i].format;
    }
    return r;
}();

u32 CompatClass(VkFormat f) {
    for (const auto& e : kCompat) {
        if (e.format == f) {
            return e.cls;
        }
    }
    return 0;
}

template <typename T>
Mapped<T> Ok(T v) {
    return {v, Support::Exact};
}
template <typename T>
Mapped<T> Approx(T v) {
    return {v, Support::Approximate};
}
template <typename T>
Mapped<T> Bad(T v) {
    return {v, Support::Invalid};
}

constexpr u32 kCbBase = Cp::Reg::CbColor0Base;
constexpr u32 kCbStride = Cp::Reg::CbColorStride;

} // namespace

VkFormat SurfaceFormat(u32 data_format, u32 number_format) {
    if (data_format >= kDataFormatCount || number_format >= kNumberFormatCount) {
        return VK_FORMAT_UNDEFINED;
    }
    return kSurfaceTable[data_format * kNumberFormatCount + number_format];
}

std::span<const SurfaceFormatInfo> SurfaceFormats() {
    return kSurfaceFormats;
}

std::span<const DepthFormatInfo> DepthFormats() {
    return kDepthFormats;
}

VkFormat DepthFormat(u32 z_format, u32 stencil_format) {
    for (const auto& f : kDepthFormats) {
        if (f.z_format == z_format && f.stencil_format == stencil_format) {
            return f.vk_format;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

bool IsFormatDepthCompatible(VkFormat format) {
    return format == VK_FORMAT_R32_SFLOAT || format == VK_FORMAT_R32_UINT || format == VK_FORMAT_R16_UNORM;
}

bool IsFormatStencilCompatible(VkFormat format) {
    return format == VK_FORMAT_R8_UINT || format == VK_FORMAT_R8_UNORM;
}

VkFormat PromoteFormatToDepth(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_SFLOAT:
    case VK_FORMAT_R32_UINT:
        return VK_FORMAT_D32_SFLOAT;
    case VK_FORMAT_R16_UNORM:
        return VK_FORMAT_D16_UNORM;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

bool IsDepthFormat(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

bool HasStencilAspect(VkFormat format) {
    return format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_S8_UINT;
}

bool IsVulkanFormatCompatible(VkFormat base, VkFormat view) {
    if (base == view) {
        return true;
    }
    const u32 a = CompatClass(base), b = CompatClass(view);
    return a != 0 && a == b;
}

std::span<const VkFormat> CompatibilityTableFormats() {
    return kCompatFormats;
}

VkFormat TSharpFormat(const Gpu::Shader::ImageDesc& sharp, bool is_depth, bool is_storage) {
    const DataFormat dfmt = TSharp::DataFmt(sharp);
    NumberFormat nfmt = TSharp::NumberFmt(sharp);
    if (is_storage && nfmt == NumberFormat::Srgb) {
        nfmt = NumberFormat::Unorm;
    }
    VkFormat format = SurfaceFormat(dfmt, nfmt);
    if (is_depth) {
        format = PromoteFormatToDepth(format);
    }
    return format;
}

VkFormat ColorTargetFormat(u32 cb_info) {
    return SurfaceFormat(ColorTargetDataFormat(cb_info), ColorTargetNumberFormat(cb_info));
}

Mapped<VkStencilOp> StencilOp(u32 op) {
    switch (op) {
    case 0:
        return Ok(VK_STENCIL_OP_KEEP);
    case 1:
        return Ok(VK_STENCIL_OP_ZERO);
    case 2:
        return Approx(VK_STENCIL_OP_REPLACE); // Ones
    case 3:
    case 4:
        return Ok(VK_STENCIL_OP_REPLACE);
    case 5:
        return Ok(VK_STENCIL_OP_INCREMENT_AND_CLAMP);
    case 6:
        return Ok(VK_STENCIL_OP_DECREMENT_AND_CLAMP);
    case 7:
        return Ok(VK_STENCIL_OP_INVERT);
    case 8:
        return Ok(VK_STENCIL_OP_INCREMENT_AND_WRAP);
    case 9:
        return Ok(VK_STENCIL_OP_DECREMENT_AND_WRAP);
    default:
        return op < 16 ? Approx(VK_STENCIL_OP_KEEP) : Bad(VK_STENCIL_OP_KEEP); // bitwise ops: Keep, as the bridge
    }
}

Mapped<VkCompareOp> CompareOp(u32 func) {
    static constexpr VkCompareOp kOps[8] = {VK_COMPARE_OP_NEVER,         VK_COMPARE_OP_LESS,
                                            VK_COMPARE_OP_EQUAL,         VK_COMPARE_OP_LESS_OR_EQUAL,
                                            VK_COMPARE_OP_GREATER,       VK_COMPARE_OP_NOT_EQUAL,
                                            VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    return func < 8 ? Ok(kOps[func]) : Bad(VK_COMPARE_OP_ALWAYS);
}

Mapped<bool> IsPrimitiveCulled(u32 type) {
    switch (type) {
    case 4:
    case 5:
    case 6:
    case 9:
    case 12:
    case 13:
    case 19:
    case 20:
    case 21:
        return Ok(true);
    case 0:
    case 1:
    case 2:
    case 3:
    case 10:
    case 11:
    case 17:
    case 18:
        return Ok(false);
    default:
        return Bad(true);
    }
}

Mapped<VkPrimitiveTopology> PrimitiveType(u32 type) {
    switch (type) {
    case 1:
        return Ok(VK_PRIMITIVE_TOPOLOGY_POINT_LIST);
    case 2:
        return Ok(VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    case 3:
        return Ok(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP);
    case 4:
        return Ok(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    case 5:
    case 21:
        return Ok(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN);
    case 6:
        return Ok(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    case 20:
        return Approx(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
    case 10:
        return Ok(VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY);
    case 11:
        return Ok(VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY);
    case 12:
        return Ok(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY);
    case 13:
        return Ok(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY);
    case 9:
    case 17:
    case 19:
        return Ok(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST);
    default:
        return Bad(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    }
}

Mapped<VkPolygonMode> PolygonMode(u32 mode) {
    switch (mode) {
    case 0:
        return Ok(VK_POLYGON_MODE_POINT);
    case 1:
        return Ok(VK_POLYGON_MODE_LINE);
    case 2:
        return Ok(VK_POLYGON_MODE_FILL);
    default:
        return Bad(VK_POLYGON_MODE_FILL);
    }
}

Mapped<VkCullModeFlags> CullMode(u32 mode) {
    static constexpr VkCullModeFlags kModes[4] = {VK_CULL_MODE_NONE, VK_CULL_MODE_FRONT_BIT, VK_CULL_MODE_BACK_BIT,
                                                  VK_CULL_MODE_FRONT_AND_BACK};
    return mode < 4 ? Ok(kModes[mode]) : Bad(static_cast<VkCullModeFlags>(VK_CULL_MODE_NONE));
}

Mapped<VkFrontFace> FrontFace(u32 face) {
    return face == 0 ? Ok(VK_FRONT_FACE_COUNTER_CLOCKWISE)
                     : (face == 1 ? Ok(VK_FRONT_FACE_CLOCKWISE) : Bad(VK_FRONT_FACE_CLOCKWISE));
}

Mapped<VkBlendFactor> BlendFactor(u32 factor) {
    static constexpr VkBlendFactor kFactors[21] = {VK_BLEND_FACTOR_ZERO,
                                                   VK_BLEND_FACTOR_ONE,
                                                   VK_BLEND_FACTOR_SRC_COLOR,
                                                   VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
                                                   VK_BLEND_FACTOR_SRC_ALPHA,
                                                   VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                                   VK_BLEND_FACTOR_DST_ALPHA,
                                                   VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
                                                   VK_BLEND_FACTOR_DST_COLOR,
                                                   VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
                                                   VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
                                                   VK_BLEND_FACTOR_MAX_ENUM,
                                                   VK_BLEND_FACTOR_MAX_ENUM,
                                                   VK_BLEND_FACTOR_CONSTANT_COLOR,
                                                   VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
                                                   VK_BLEND_FACTOR_SRC1_COLOR,
                                                   VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR,
                                                   VK_BLEND_FACTOR_SRC1_ALPHA,
                                                   VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA,
                                                   VK_BLEND_FACTOR_CONSTANT_ALPHA,
                                                   VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA};
    if (factor >= 21 || kFactors[factor] == VK_BLEND_FACTOR_MAX_ENUM) {
        return Bad(VK_BLEND_FACTOR_ONE);
    }
    return Ok(kFactors[factor]);
}

bool IsDualSourceBlendFactor(u32 factor) {
    return factor >= 15 && factor <= 18;
}

Mapped<VkBlendOp> BlendOp(u32 func) {
    static constexpr VkBlendOp kOps[5] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,
                                          VK_BLEND_OP_REVERSE_SUBTRACT};
    return func < 5 ? Ok(kOps[func]) : Bad(VK_BLEND_OP_ADD);
}

void NormalizeMinMaxBlend(VkBlendOp& op, VkBlendFactor& src, VkBlendFactor& dst, u32 number_format) {
    if (number_format != static_cast<u32>(NF::Unorm) && number_format != static_cast<u32>(NF::Srgb)) {
        return;
    }
    if (op != VK_BLEND_OP_MIN && op != VK_BLEND_OP_MAX) {
        return;
    }
    if ((src != VK_BLEND_FACTOR_ZERO && src != VK_BLEND_FACTOR_ONE) ||
        (dst != VK_BLEND_FACTOR_ZERO && dst != VK_BLEND_FACTOR_ONE) ||
        (src != VK_BLEND_FACTOR_ZERO && dst != VK_BLEND_FACTOR_ZERO)) {
        return;
    }
    if (op == VK_BLEND_OP_MIN) {
        src = VK_BLEND_FACTOR_ZERO;
        dst = VK_BLEND_FACTOR_ZERO;
    }
    op = VK_BLEND_OP_ADD;
}

Mapped<VkLogicOp> LogicOp(u32 rop3) {
    switch (rop3) {
    case 0x00:
        return Ok(VK_LOGIC_OP_CLEAR);
    case 0x11:
        return Ok(VK_LOGIC_OP_NOR);
    case 0x22:
        return Ok(VK_LOGIC_OP_AND_INVERTED);
    case 0x33:
        return Ok(VK_LOGIC_OP_COPY_INVERTED);
    case 0x44:
        return Ok(VK_LOGIC_OP_AND_REVERSE);
    case 0x55:
        return Ok(VK_LOGIC_OP_INVERT);
    case 0x5A:
        return Approx(VK_LOGIC_OP_XOR); // BrushXor
    case 0x66:
        return Ok(VK_LOGIC_OP_XOR);
    case 0x77:
        return Ok(VK_LOGIC_OP_NAND);
    case 0x88:
        return Ok(VK_LOGIC_OP_AND);
    case 0x99:
        return Ok(VK_LOGIC_OP_EQUIVALENT);
    case 0xAA:
        return Ok(VK_LOGIC_OP_NO_OP);
    case 0xBB:
        return Ok(VK_LOGIC_OP_OR_INVERTED);
    case 0xCC:
        return Ok(VK_LOGIC_OP_COPY);
    case 0xDD:
        return Ok(VK_LOGIC_OP_OR_REVERSE);
    case 0xEE:
        return Ok(VK_LOGIC_OP_OR);
    case 0xFF:
        return Ok(VK_LOGIC_OP_SET);
    default:
        return Bad(VK_LOGIC_OP_COPY);
    }
}

Mapped<VkSamplerAddressMode> ClampMode(u32 mode) {
    switch (mode) {
    case 0:
        return Ok(VK_SAMPLER_ADDRESS_MODE_REPEAT);
    case 1:
        return Ok(VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT);
    case 2:
        return Ok(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
    case 3:
        return Ok(VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
    case 4:
        return Approx(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    case 5:
    case 7:
        return Approx(VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE);
    case 6:
        return Ok(VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER);
    default:
        return Bad(VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }
}

Mapped<VkCompareOp> DepthCompare(u32 func) {
    return CompareOp(func);
}

Mapped<VkFilter> Filter(u32 filter) {
    if (filter > 3) {
        return Bad(VK_FILTER_NEAREST);
    }
    return Ok((filter & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
}

Mapped<VkSamplerReductionMode> FilterMode(u32 mode) {
    static constexpr VkSamplerReductionMode kModes[3] = {VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE,
                                                         VK_SAMPLER_REDUCTION_MODE_MIN, VK_SAMPLER_REDUCTION_MODE_MAX};
    return mode < 3 ? Ok(kModes[mode]) : Bad(VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE);
}

Mapped<VkSamplerMipmapMode> MipFilter(u32 filter) {
    if (filter > 2) {
        return Bad(VK_SAMPLER_MIPMAP_MODE_NEAREST);
    }
    return Ok(filter == 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST);
}

Mapped<VkBorderColor> BorderColor(u32 type) {
    static constexpr VkBorderColor kColors[4] = {VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                                 VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
                                                 VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE, VK_BORDER_COLOR_FLOAT_CUSTOM_EXT};
    return type < 4 ? Ok(kColors[type]) : Bad(VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK);
}

float SamplerLodBias(const Gpu::Shader::SamplerDesc& s) {
    const s32 raw = static_cast<s32>(s.raw[2] & 0x3FFF); // LOD_BIAS: signed 6.8
    return static_cast<float>((raw & 0x2000) ? raw - 0x4000 : raw) / 256.0f;
}

float SamplerMinLod(const Gpu::Shader::SamplerDesc& s) {
    return static_cast<float>(s.MinLod()) / 256.0f;
}

float SamplerMaxLod(const Gpu::Shader::SamplerDesc& s) {
    return static_cast<float>(s.MaxLod()) / 256.0f;
}

Mapped<float> SamplerMaxAniso(const Gpu::Shader::SamplerDesc& s) {
    const u32 r = s.MaxAniso();
    return r <= 4 ? Ok(static_cast<float>(1u << r)) : Bad(1.0f);
}

SamplerParams BuildSamplerParams(const Gpu::Shader::SamplerDesc& s, bool is_depth, const SamplerCaps& caps) {
    SamplerParams p;
    const u32 mag = s.MagFilter(), min = s.MinFilter();
    const auto fmag = Filter(mag), fmin = Filter(min);
    const auto mip = MipFilter(s.MipFilter());
    const auto cx = ClampMode(s.ClampX()), cy = ClampMode(s.ClampY()), cz = ClampMode(s.ClampZ());
    const auto cmp = DepthCompare(s.DepthCompare());
    const auto border = BorderColor(s.BorderColorType());
    const auto aniso = SamplerMaxAniso(s);
    if (!fmag.Valid() || !fmin.Valid() || !mip.Valid() || !cx.Valid() || !cy.Valid() || !cz.Valid() ||
        !cmp.Valid() || !border.Valid() || !aniso.Valid()) {
        p.supported = false;
        p.reason = "an S# field is out of range";
        return p;
    }
    p.supported = true;
    p.approximate = cx.support == Support::Approximate || cy.support == Support::Approximate ||
                    cz.support == Support::Approximate;
    const bool aniso_filter = mag >= 2 || min >= 2;
    p.anisotropy_enable = caps.anisotropy && aniso_filter;
    p.max_anisotropy = p.anisotropy_enable ? std::clamp(aniso.value, 1.0f, caps.max_anisotropy) : 1.0f;
    p.border_color = border.value;
    if (p.border_color == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT && !caps.custom_border_color) {
        p.border_color = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    }
    p.border_color_index = s.BorderColorPtr();
    p.mag_filter = fmag.value;
    p.min_filter = fmin.value;
    p.mipmap_mode = mip.value;
    p.address_u = cx.value;
    p.address_v = cy.value;
    p.address_w = cz.value;
    p.mip_lod_bias = std::min(SamplerLodBias(s), caps.max_lod_bias);
    p.compare_enable = is_depth;
    p.compare_op = cmp.value;
    p.min_lod = SamplerMinLod(s);
    p.max_lod = SamplerMaxLod(s);
    return p;
}

Mapped<VkComponentSwizzle> ComponentSwizzle(u32 selector) {
    switch (selector) {
    case 0:
        return Ok(VK_COMPONENT_SWIZZLE_ZERO);
    case 1:
        return Ok(VK_COMPONENT_SWIZZLE_ONE);
    case 4:
        return Ok(VK_COMPONENT_SWIZZLE_R);
    case 5:
        return Ok(VK_COMPONENT_SWIZZLE_G);
    case 6:
        return Ok(VK_COMPONENT_SWIZZLE_B);
    case 7:
        return Ok(VK_COMPONENT_SWIZZLE_A);
    default:
        return Bad(VK_COMPONENT_SWIZZLE_IDENTITY);
    }
}

Mapped<VkComponentMapping> ComponentMapping(const CompMapping& m) {
    const auto r = ComponentSwizzle(static_cast<u32>(m.c[0]));
    const auto g = ComponentSwizzle(static_cast<u32>(m.c[1]));
    const auto b = ComponentSwizzle(static_cast<u32>(m.c[2]));
    const auto a = ComponentSwizzle(static_cast<u32>(m.c[3]));
    const VkComponentMapping out{r.value, g.value, b.value, a.value};
    return (r.Valid() && g.Valid() && b.Valid() && a.Valid()) ? Ok(out) : Bad(out);
}

Mapped<VkComponentMapping> ViewComponents(const CompMapping& m) {
    if (m == kIdentityMapping) {
        return Ok(VkComponentMapping{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                     VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY});
    }
    return ComponentMapping(m);
}

ClearColor ColorBufferClearValue(u32 cb_info, u32 c0, u32 c1) {
    ClearColor out;
    CompMapping swizzle;
    const bool has_swizzle = ColorTargetSwizzle(cb_info, swizzle);
    const DataFormat format = static_cast<DataFormat>((cb_info >> 2) & 0x1F);
    const NumberFormat nt = ColorTargetFixedNumberFormat(cb_info);
    VkClearColorValue color{};
    const bool integer = nt == NF::Uint || nt == NF::Sint;
    if (integer) {
        color.uint32[3] = 1;
    } else {
        color.float32[3] = 1.0f;
    }
    const bool unorm = nt == NF::Unorm, snorm = nt == NF::Snorm || nt == NF::SnormNz, flt = nt == NF::Float;
    bool ok = true;
    float* f = color.float32;
    u32* u = color.uint32;
    switch (format) {
    case DF::Format8:
        if (unorm || nt == NF::Srgb) {
            f[0] = Unorm(c0 & 0xff, 255.0f);
        } else if (snorm) {
            f[0] = Snorm(S8(c0), 127.0f);
        } else if (integer) {
            u[0] = c0;
        } else {
            ok = false;
        }
        break;
    case DF::Format16:
        if (unorm) {
            f[0] = Unorm(c0 & 0xffff, 65535.0f);
        } else if (snorm) {
            f[0] = Snorm(S16(c0), 32767.0f);
        } else if (integer) {
            u[0] = c0;
        } else if (flt) {
            f[0] = Uf16ToF32(static_cast<u16>(c0));
        } else {
            ok = false;
        }
        break;
    case DF::Format8_8:
        if (unorm || nt == NF::Srgb) {
            f[0] = Unorm(c0 & 0xff, 255.0f);
            f[1] = Unorm((c0 >> 8) & 0xff, 255.0f);
        } else if (snorm) {
            f[0] = Snorm(S8(c0), 127.0f);
            f[1] = Snorm(S8(c0 >> 8), 127.0f);
        } else if (integer) {
            u[0] = c0 & 0xff;
            u[1] = (c0 >> 8) & 0xff;
        } else {
            ok = false;
        }
        break;
    case DF::Format32:
        if (integer) {
            u[0] = c0;
        } else if (flt) {
            f[0] = std::bit_cast<float>(c0);
        } else {
            ok = false;
        }
        break;
    case DF::Format16_16:
        if (unorm) {
            f[0] = Unorm(c0 & 0xffff, 65535.0f);
            f[1] = Unorm(c0 >> 16, 65535.0f);
        } else if (snorm) {
            f[0] = Snorm(S16(c0), 32767.0f);
            f[1] = Snorm(S16(c0 >> 16), 32767.0f);
        } else if (integer) {
            u[0] = c0 & 0xffff;
            u[1] = c0 >> 16;
        } else if (flt) {
            f[0] = Uf16ToF32(static_cast<u16>(c0));
            f[1] = Uf16ToF32(static_cast<u16>(c0 >> 16));
        } else {
            ok = false;
        }
        break;
    case DF::Format10_11_11:
        f[0] = Uf11ToF32(static_cast<u16>(c0 & 0x7ff));
        f[1] = Uf11ToF32(static_cast<u16>((c0 >> 11) & 0x7ff));
        f[2] = Uf10ToF32(static_cast<u16>((c0 >> 22) & 0x3ff));
        break;
    case DF::Format11_11_10:
        f[0] = Uf10ToF32(static_cast<u16>(c0 & 0x3ff));
        f[1] = Uf11ToF32(static_cast<u16>((c0 >> 10) & 0x7ff));
        f[2] = Uf11ToF32(static_cast<u16>((c0 >> 21) & 0x7ff));
        break;
    case DF::Format5_9_9_9: {
        const int exponent = static_cast<int>(c0 >> 27) - 10;
        const float scale = std::bit_cast<float>(static_cast<u32>((exponent + 127) << 23));
        f[0] = static_cast<float>(c0 & 0x1ff) * scale;
        f[1] = static_cast<float>((c0 >> 9) & 0x1ff) * scale;
        f[2] = static_cast<float>((c0 >> 18) & 0x1ff) * scale;
        break;
    }
    case DF::Format10_10_10_2:
        if (unorm) {
            f[0] = Unorm(c0 & 0x3, 3.0f);
            f[1] = Unorm((c0 >> 2) & 0x3ff, 1023.0f);
            f[2] = Unorm((c0 >> 12) & 0x3ff, 1023.0f);
            f[3] = Unorm(static_cast<u16>(c0 >> 22), 1023.0f);
        } else if (snorm) {
            f[0] = Snorm(static_cast<s8>(c0 & 0x3), 1.0f);
            f[1] = Snorm(static_cast<s16>((c0 >> 2) & 0x3ff), 511.0f);
            f[2] = Snorm(static_cast<s16>((c0 >> 12) & 0x3ff), 511.0f);
            f[3] = Snorm(static_cast<s8>(c0 >> 22), 1.0f);
        } else if (integer) {
            u[0] = c0 & 0x3;
            u[1] = (c0 >> 2) & 0x3ff;
            u[2] = (c0 >> 12) & 0x3ff;
            u[3] = c0 >> 22;
        } else {
            ok = false;
        }
        break;
    case DF::Format2_10_10_10:
        if (unorm) {
            f[0] = Unorm(c0 & 0x3ff, 1023.0f);
            f[1] = Unorm((c0 >> 10) & 0x3ff, 1023.0f);
            f[2] = Unorm((c0 >> 20) & 0x3ff, 1023.0f);
            f[3] = Unorm(c0 >> 30, 3.0f);
        } else if (snorm) {
            f[0] = Snorm(static_cast<s16>(c0 & 0x3ff), 511.0f);
            f[1] = Snorm(static_cast<s16>((c0 >> 10) & 0x3ff), 511.0f);
            f[2] = Snorm(static_cast<s16>((c0 >> 20) & 0x3ff), 511.0f);
            f[3] = Snorm(static_cast<s8>(c0 >> 30), 1.0f);
        } else if (integer) {
            u[0] = c0 & 0x3ff;
            u[1] = (c0 >> 10) & 0x3ff;
            u[2] = (c0 >> 20) & 0x3ff;
            u[3] = c0 >> 30;
        } else {
            ok = false;
        }
        break;
    case DF::Format8_8_8_8:
        if (unorm || nt == NF::Srgb) {
            for (u32 i = 0; i < 4; ++i) {
                f[i] = Unorm((c0 >> (8 * i)) & 0xff, 255.0f);
            }
        } else if (snorm) {
            for (u32 i = 0; i < 4; ++i) {
                f[i] = Snorm(S8(c0 >> (8 * i)), 127.0f);
            }
        } else if (integer) {
            for (u32 i = 0; i < 4; ++i) {
                u[i] = (c0 >> (8 * i)) & 0xff;
            }
        } else {
            ok = false;
        }
        break;
    case DF::Format32_32:
        if (integer) {
            u[0] = c0;
            u[1] = c1;
        } else if (flt) {
            f[0] = std::bit_cast<float>(c0);
            f[1] = std::bit_cast<float>(c1);
        } else {
            ok = false;
        }
        break;
    case DF::Format16_16_16_16:
        if (unorm) {
            f[0] = Unorm(c0 & 0xffff, 65535.0f);
            f[1] = Unorm(c0 >> 16, 65535.0f);
            f[2] = Unorm(c1 & 0xffff, 65535.0f);
            f[3] = Unorm(c1 >> 16, 65535.0f);
        } else if (snorm) {
            f[0] = Snorm(S16(c0), 32767.0f);
            f[1] = Snorm(S16(c0 >> 16), 32767.0f);
            f[2] = Snorm(S16(c1), 32767.0f);
            f[3] = Snorm(S16(c1 >> 16), 32767.0f);
        } else if (integer) {
            u[0] = c0 & 0xffff;
            u[1] = c0 >> 16;
            u[2] = c1 & 0xffff;
            u[3] = c1 >> 16;
        } else if (flt) {
            f[0] = Uf16ToF32(static_cast<u16>(c0));
            f[1] = Uf16ToF32(static_cast<u16>(c0 >> 16));
            f[2] = Uf16ToF32(static_cast<u16>(c1));
            f[3] = Uf16ToF32(static_cast<u16>(c1 >> 16));
        } else {
            ok = false;
        }
        break;
    case DF::Format32_32_32_32: // the bridge reads word 0 for r, g and b
        if (integer) {
            u[0] = u[1] = u[2] = c0;
            u[3] = c1;
        } else if (flt) {
            f[0] = f[1] = f[2] = std::bit_cast<float>(c0);
            f[3] = std::bit_cast<float>(c1);
        } else {
            ok = false;
        }
        break;
    case DF::Format5_6_5:
        f[0] = Unorm(c0 & 0x1f, 31.0f);
        f[1] = Unorm((c0 >> 5) & 0x3f, 63.0f);
        f[2] = Unorm(static_cast<u8>(c0 >> 11), 31.0f);
        break;
    case DF::Format1_5_5_5:
        f[0] = Unorm(c0 & 0x1f, 31.0f);
        f[1] = Unorm((c0 >> 5) & 0x1f, 31.0f);
        f[2] = Unorm((c0 >> 10) & 0x1f, 31.0f);
        f[3] = (c0 >> 15) ? 1.0f : 0.0f;
        break;
    case DF::Format5_5_5_1:
        f[0] = (c0 & 0x1) ? 1.0f : 0.0f;
        f[1] = Unorm((c0 >> 1) & 0x1f, 31.0f);
        f[2] = Unorm((c0 >> 6) & 0x1f, 31.0f);
        f[3] = Unorm((c0 >> 11) & 0x1f, 31.0f);
        break;
    case DF::Format4_4_4_4:
        f[0] = Unorm(c0 & 0xf, 15.0f);
        f[1] = Unorm((c0 >> 4) & 0xf, 15.0f);
        f[2] = Unorm((c0 >> 8) & 0xf, 15.0f);
        f[3] = Unorm(static_cast<u8>(c0 >> 12), 15.0f);
        break;
    default:
        ok = false;
        break;
    }
    if (!has_swizzle) {
        out.supported = false;
        return out;
    }
    // the MRT swizzle on the float view (as the bridge, also for integer formats)
    const std::array<float, 4> d{f[0], f[1], f[2], f[3]};
    for (u32 i = 0; i < 4; ++i) {
        switch (swizzle.c[i]) {
        case CompSwizzle::Zero:
            f[i] = 0.0f;
            break;
        case CompSwizzle::One:
            f[i] = 1.0f;
            break;
        case CompSwizzle::Red:
            f[i] = d[0];
            break;
        case CompSwizzle::Green:
            f[i] = d[1];
            break;
        case CompSwizzle::Blue:
            f[i] = d[2];
            break;
        case CompSwizzle::Alpha:
            f[i] = d[3];
            break;
        default:
            break;
        }
    }
    out.value.color = color;
    out.supported = ok;
    return out;
}

ClearColor ColorBufferClearValue(const Cp::RegisterFile& regs, u32 n) {
    const u32 base = kCbBase + n * kCbStride;
    return ColorBufferClearValue(regs.Get(base + 4), regs.Get(base + 11), regs.Get(base + 12));
}

Mapped<VkSampleCountFlagBits> NumSamples(u32 num_samples, VkSampleCountFlags supported_flags) {
    if (num_samples == 0 || num_samples > 16 || (num_samples & (num_samples - 1)) != 0) {
        return Bad(VK_SAMPLE_COUNT_1_BIT);
    }
    bool halved = false;
    while (!(supported_flags & num_samples) && num_samples > 1) {
        num_samples /= 2;
        halved = true;
    }
    const auto flags = static_cast<VkSampleCountFlagBits>(num_samples); // VK_SAMPLE_COUNT_n_BIT == n
    return halved ? Approx(flags) : Ok(flags);
}

Mapped<VkImageType> ConvertImageType(ImageType type) {
    switch (type) {
    case ImageType::Color1D:
    case ImageType::Color1DArray:
    case ImageType::Color2D:
    case ImageType::Color2DMsaa:
    case ImageType::Color2DArray:
    case ImageType::Cube:
        return Ok(VK_IMAGE_TYPE_2D);
    case ImageType::Color3D:
        return Ok(VK_IMAGE_TYPE_3D);
    default:
        return Bad(VK_IMAGE_TYPE_2D);
    }
}

Mapped<VkImageViewType> ConvertImageViewType(ImageType type) {
    switch (type) {
    case ImageType::Color1D:
    case ImageType::Color2D:
    case ImageType::Color2DMsaa:
        return Ok(VK_IMAGE_VIEW_TYPE_2D);
    case ImageType::Color1DArray:
    case ImageType::Color2DArray:
    case ImageType::Cube:
        return Ok(VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    case ImageType::Color3D:
        return Ok(VK_IMAGE_VIEW_TYPE_3D);
    default:
        return Bad(VK_IMAGE_VIEW_TYPE_2D);
    }
}

bool IsViewTypeCompatible(ImageType view_type, ImageType image_type) {
    switch (view_type) {
    case ImageType::Color1D:
    case ImageType::Color1DArray:
    case ImageType::Color2D:
    case ImageType::Color2DArray:
    case ImageType::Color2DMsaa:
    case ImageType::Color2DMsaaArray:
    case ImageType::Cube:
        return image_type == ImageType::Color1D || image_type == ImageType::Color2D ||
               image_type == ImageType::Color3D;
    case ImageType::Color3D:
        return image_type == ImageType::Color3D;
    default:
        return false;
    }
}

ImageViewDesc TSharpView(const Gpu::Shader::ImageDesc& sharp, bool is_depth, bool is_array, bool is_storage) {
    ImageViewDesc v;
    v.is_storage = is_storage;
    v.format = TSharpFormat(sharp, is_depth, is_storage);
    if (v.format == VK_FORMAT_UNDEFINED) {
        v.reason = is_depth ? "a depth read of a format that is not R32/R16" : "no host format for the T#";
        return v;
    }
    const u32 layers = TSharp::NumLayers(sharp);
    v.base_level = static_cast<u16>(sharp.BaseLevel());
    v.base_layer = static_cast<u16>(std::min<u32>(sharp.BaseArray(), layers > 0 ? layers - 1 : 0));
    v.levels = static_cast<u16>(TSharp::NumViewLevels(sharp, is_array));
    v.layers = static_cast<u16>(TSharp::NumViewLayers(sharp, is_array));
    v.type = TSharp::ViewType(sharp, is_array);
    v.min_lod = TSharp::MinLod(sharp);
    if (!is_storage) {
        v.mapping = TSharp::DstSelect(sharp);
    }
    v.supported = true;
    return v;
}

ImageViewDesc ColorTargetView(const Cp::RegisterFile& regs, u32 n) {
    ImageViewDesc v;
    const u32 base = kCbBase + n * kCbStride;
    const u32 view = regs.Get(base + 3);
    const u32 slice_start = view & 0x7FF, slice_max = (view >> 13) & 0x7FF;
    v.base_layer = static_cast<u16>(slice_start);
    v.layers = static_cast<u16>(slice_max + 1 - slice_start);
    v.type = v.layers > 1 ? ImageType::Color2DArray : ImageType::Color2D;
    v.format = ColorTargetFormat(regs.Get(base + 4));
    v.supported = v.format != VK_FORMAT_UNDEFINED;
    if (!v.supported) {
        v.reason = "no host format for the colour target";
    }
    return v;
}

ImageViewDesc DepthTargetView(const Cp::RegisterFile& regs) {
    ImageViewDesc v;
    v.format = DepthFormat(regs.Get(0xA010) & 3, regs.Get(0xA011) & 1); // DB_Z_INFO, DB_STENCIL_INFO
    v.is_storage = ((regs.Get(0xA200) >> 2) & 1) != 0;                // DB_DEPTH_CONTROL.Z_WRITE_ENABLE
    const u32 view = regs.Get(0xA002);                                  // DB_DEPTH_VIEW
    const u32 slice_start = view & 0x7FF, slice_max = (view >> 13) & 0x7FF;
    v.base_layer = static_cast<u16>(slice_start);
    v.layers = static_cast<u16>(slice_max + 1 - slice_start);
    v.type = v.layers > 1 ? ImageType::Color2DArray : ImageType::Color2D;
    v.supported = v.format != VK_FORMAT_UNDEFINED;
    if (!v.supported) {
        v.reason = "no host format for the depth target";
    }
    return v;
}

float ViewMinLod(const ImageViewDesc& view, bool min_lod_supported) {
    if (view.min_lod == 0 || !min_lod_supported) {
        return 0.0f;
    }
    const float last_level = static_cast<float>(view.base_level + view.levels - 1);
    return std::min(static_cast<float>(view.min_lod) / 256.f, last_level);
}

ViewFormat ViewFormatOf(VkFormat view_format, VkFormat image_format, VkImageAspectFlags image_aspect) {
    ViewFormat out{view_format, image_aspect};
    // a depth/stencil image is viewed only in its own format (format compatibility class, VUID-VkImageViewCreateInfo-
    // image-01761); a request for another depth format (D32 on D32S8: a shader sampling depth) takes the image's own
    // format with the depth aspect alone, as a sampled descriptor needs one aspect (VUID-VkDescriptorImageInfo-
    // imageView-01976). The image's own format (an attachment view) keeps every aspect.
    const bool other_depth_format = view_format != image_format && IsDepthFormat(view_format);
    if ((image_aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && (IsFormatDepthCompatible(view_format) || other_depth_format)) {
        out = {image_format, VK_IMAGE_ASPECT_DEPTH_BIT};
    }
    if ((image_aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && IsFormatStencilCompatible(view_format)) {
        out = {image_format, VK_IMAGE_ASPECT_STENCIL_BIT};
    }
    return out;
}

VkImageAspectFlags ImageAspect(const ImageLayout& layout) {
    if (layout.props.is_depth) {
        return VK_IMAGE_ASPECT_DEPTH_BIT | (layout.props.has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
    }
    return VK_IMAGE_ASPECT_COLOR_BIT;
}

VkImageUsageFlags ImageUsage(const ImageLayout& layout, bool feedback_loop, bool ms_storage) {
    VkImageUsageFlags usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (layout.props.is_block) {
        return usage | VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if (layout.props.is_depth) {
        return usage | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (feedback_loop) {
        usage |= VK_IMAGE_USAGE_ATTACHMENT_FEEDBACK_LOOP_BIT_EXT;
    }
    if (layout.num_samples == 1 || ms_storage) {
        usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    return usage;
}

VkFormatFeatureFlags2 FormatFeatures(VkImageUsageFlags usage) {
    VkFormatFeatureFlags2 f = 0;
    f |= (usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ? VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT : 0;
    f |= (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) ? VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT : 0;
    f |= (usage & VK_IMAGE_USAGE_SAMPLED_BIT) ? VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT : 0;
    f |= (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT : 0;
    f |= (usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ? VK_FORMAT_FEATURE_2_DEPTH_STENCIL_ATTACHMENT_BIT : 0;
    return f;
}

VkImageCreateFlags ImageCreateFlags(const ImageLayout& layout, bool view_2d_of_3d, bool block_texel_view) {
    VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (layout.props.is_volume) {
        flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
        if (view_2d_of_3d) {
            flags |= VK_IMAGE_CREATE_2D_VIEW_COMPATIBLE_BIT_EXT;
        }
    }
    if (layout.props.is_block && block_texel_view) {
        flags |= VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    }
    return flags;
}

} // namespace SecondSon::Gpu::Vk
