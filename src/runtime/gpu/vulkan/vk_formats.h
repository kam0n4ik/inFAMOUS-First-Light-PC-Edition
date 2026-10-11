// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — guest formats, descriptors and fixed-function registers as Vulkan values (Phase 4, P4.4): the host
// format of a data/number format pair, depth formats, the compatibility classes views alias by, the view (type,
// format, subresources, components) of a T# / colour target / depth target, image usage and create flags, the sampler
// of an S#, blend/depth/stencil/raster enums, colour clear values, sample counts. Inputs are raw register and
// descriptor field values; every result is what the bridge's renderer computes for them (selftest_formats.cpp compares
// them). A raw value the bridge stops on (UNREACHABLE/ASSERT) maps to Support::Invalid, or VK_FORMAT_UNDEFINED for a
// format; one it replaces by the closest Vulkan equivalent is Support::Approximate. Nothing here logs: a caller reports
// what it refuses. This header exposes Vulkan types (C API, no prototypes); image_layout.h is the Vulkan-free part.
// Ported from shadPS4's renderer_vulkan/liverpool_to_vk, texture_cache/{host_compatibility,image,image_view,sampler} (GPL-2.0-or-later).
#pragma once

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan_core.h>

#include <span>
#include <string_view>

#include "base/abi.h"
#include "runtime/gpu/image_layout.h"

namespace SecondSon::Gpu::Shader {
struct ImageDesc;
struct SamplerDesc;
} // namespace SecondSon::Gpu::Shader

namespace SecondSon::Gpu::Vk {

enum class Support : u8 {
    Exact,       ///< the guest value has this Vulkan meaning
    Approximate, ///< no Vulkan equivalent: the bridge's closest one (e.g. a bitwise stencil op as Keep)
    Invalid,     ///< not a value of the field (the bridge stops on it); `value` is a placeholder
};

template <typename T>
struct Mapped {
    T value{};
    Support support = Support::Exact;
    bool Valid() const {
        return support != Support::Invalid;
    }
};

// ---- formats

/// The host format of a data/number format pair as given (callers remap first: TSharp::DataFmt/NumberFmt,
/// ColorTargetDataFormat/NumberFormat); VK_FORMAT_UNDEFINED for a pair with none (also any value out of range).
VkFormat SurfaceFormat(u32 data_format, u32 number_format);
inline VkFormat SurfaceFormat(DataFormat data_format, NumberFormat number_format) {
    return SurfaceFormat(static_cast<u32>(data_format), static_cast<u32>(number_format));
}

struct SurfaceFormatInfo {
    DataFormat data_format;
    NumberFormat number_format;
    VkFormat vk_format;
    VkFormatFeatureFlags2 flags; ///< what the backend needs the format to support for the guest's uses
};
/// Every pair of the table (the ones with a host format, and the invalid data format).
std::span<const SurfaceFormatInfo> SurfaceFormats();

struct DepthFormatInfo {
    u32 z_format;       ///< DB_Z_INFO.FORMAT: 0 invalid, 1 Z16, 3 Z32 float
    u32 stencil_format; ///< DB_STENCIL_INFO.FORMAT: 0 invalid, 1 S8
    VkFormat vk_format;
    VkFormatFeatureFlags2 flags;
};
std::span<const DepthFormatInfo> DepthFormats();
/// VK_FORMAT_UNDEFINED for Z format 2 (reserved: the bridge asserts) and for no depth and no stencil.
VkFormat DepthFormat(u32 z_format, u32 stencil_format);

bool IsFormatDepthCompatible(VkFormat format);   ///< R32/R16 a shader reads a depth image with
bool IsFormatStencilCompatible(VkFormat format); ///< R8 a shader reads a stencil image with
/// R32 float/uint -> D32, R16 unorm -> D16; VK_FORMAT_UNDEFINED for any other (the bridge stops).
VkFormat PromoteFormatToDepth(VkFormat format);
bool IsDepthFormat(VkFormat format);
bool HasStencilAspect(VkFormat format);

/// Vulkan's format compatibility classes: `view` may view an image of format `base`. A format outside the table is
/// compatible only with itself (the bridge's table lookup throws there).
bool IsVulkanFormatCompatible(VkFormat base, VkFormat view);
/// The formats the compatibility table knows.
std::span<const VkFormat> CompatibilityTableFormats();

/// The format a T# is read with (ImageViewInfo): its remapped pair, sRGB as UNORM for a storage image, promoted to
/// depth for a depth read. VK_FORMAT_UNDEFINED: none (unsupported).
VkFormat TSharpFormat(const Gpu::Shader::ImageDesc& sharp, bool is_depth, bool is_storage);
/// The format of a colour target from CB_COLORn_INFO.
VkFormat ColorTargetFormat(u32 cb_info);

// ---- fixed-function state (raw register fields)

Mapped<VkStencilOp> StencilOp(u32 op);       ///< DB_STENCIL_CONTROL STENCILFAIL/ZPASS/ZFAIL (4 bits)
Mapped<VkCompareOp> CompareOp(u32 func);     ///< DB_DEPTH_CONTROL ZFUNC/STENCILFUNC (3 bits)
Mapped<bool> IsPrimitiveCulled(u32 type);    ///< VGT_PRIMITIVE_TYPE: face culling applies
Mapped<VkPrimitiveTopology> PrimitiveType(u32 type);
Mapped<VkPolygonMode> PolygonMode(u32 mode); ///< PA_SU_SC_MODE_CNTL POLYMODE_FRONT/BACK_PTYPE
Mapped<VkCullModeFlags> CullMode(u32 mode);  ///< CULL_FRONT | CULL_BACK << 1
Mapped<VkFrontFace> FrontFace(u32 face);
Mapped<VkBlendFactor> BlendFactor(u32 factor); ///< CB_BLENDn_CONTROL (5 bits)
bool IsDualSourceBlendFactor(u32 factor);
Mapped<VkBlendOp> BlendOp(u32 func);
/// Min/max blending with factors the hardware ignores: rewritten as an add the host gives the same result for, for
/// UNORM/sRGB targets (`number_format`: the target's remapped number format).
void NormalizeMinMaxBlend(VkBlendOp& op, VkBlendFactor& src, VkBlendFactor& dst, u32 number_format);
Mapped<VkLogicOp> LogicOp(u32 rop3); ///< CB_COLOR_CONTROL ROP3 (8 bits; the 16 logic ops and BrushXor)

// ---- samplers (S# fields)

Mapped<VkSamplerAddressMode> ClampMode(u32 mode);
Mapped<VkCompareOp> DepthCompare(u32 func);
Mapped<VkFilter> Filter(u32 filter);
Mapped<VkSamplerReductionMode> FilterMode(u32 mode);
Mapped<VkSamplerMipmapMode> MipFilter(u32 filter);
Mapped<VkBorderColor> BorderColor(u32 type);

struct SamplerCaps {
    bool anisotropy = false;      ///< samplerAnisotropy
    float max_anisotropy = 1.0f;  ///< maxSamplerAnisotropy
    bool custom_border_color = false;
    float max_lod_bias = 0.0f;    ///< maxSamplerLodBias
};

/// A VkSamplerCreateInfo's values for an S# (the bridge's Sampler). A custom border colour (border_color
/// VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) is the 4 floats at TA_BC_BASE + 16 * border_color_index, read by the caller.
struct SamplerParams {
    bool supported = false; ///< false: a field out of range (see `reason`)
    std::string_view reason;
    bool approximate = false; ///< a clamp mode replaced by the closest one
    VkFilter mag_filter = VK_FILTER_NEAREST;
    VkFilter min_filter = VK_FILTER_NEAREST;
    VkSamplerMipmapMode mipmap_mode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode address_u = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode address_v = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VkSamplerAddressMode address_w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    float mip_lod_bias = 0.0f;
    bool anisotropy_enable = false;
    float max_anisotropy = 1.0f;
    bool compare_enable = false;
    VkCompareOp compare_op = VK_COMPARE_OP_NEVER;
    float min_lod = 0.0f;
    float max_lod = 0.0f;
    VkBorderColor border_color = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    u32 border_color_index = 0;
};
/// `is_depth`: the shader samples with a depth comparison (compareEnable).
SamplerParams BuildSamplerParams(const Gpu::Shader::SamplerDesc& sampler, bool is_depth, const SamplerCaps& caps);
float SamplerLodBias(const Gpu::Shader::SamplerDesc& sampler); ///< signed 6.8
float SamplerMinLod(const Gpu::Shader::SamplerDesc& sampler);
float SamplerMaxLod(const Gpu::Shader::SamplerDesc& sampler);
Mapped<float> SamplerMaxAniso(const Gpu::Shader::SamplerDesc& sampler);

// ---- components

Mapped<VkComponentSwizzle> ComponentSwizzle(u32 selector);
Mapped<VkComponentMapping> ComponentMapping(const CompMapping& mapping);
/// A view's components: the identity as all-IDENTITY (as the bridge creates it), any other mapping selector by selector.
Mapped<VkComponentMapping> ViewComponents(const CompMapping& mapping);

// ---- clear values, samples

struct ClearColor {
    VkClearValue value{};
    /// false: a format/number type the bridge has no clear value for (it logs and clears to the value here: zero
    /// with alpha 1), or a format with no component (`value` zero).
    bool supported = false;
};
/// The fast-clear colour of a target: CB_COLORn_INFO and CB_COLORn_CLEAR_WORD0/1, swizzled by COMP_SWAP.
ClearColor ColorBufferClearValue(u32 cb_info, u32 clear_word0, u32 clear_word1);
ClearColor ColorBufferClearValue(const Cp::RegisterFile& regs, u32 n);

/// `num_samples` halved until the device supports it (Approximate then); Invalid: not 1/2/4/8/16.
Mapped<VkSampleCountFlagBits> NumSamples(u32 num_samples, VkSampleCountFlags supported_flags);

// ---- images and views

Mapped<VkImageType> ConvertImageType(ImageType type);         ///< 2D except 3D; MSAA arrays and Invalid: Invalid
Mapped<VkImageViewType> ConvertImageViewType(ImageType type); ///< arrays/cubes as 2D arrays (cubes are 2D arrays)
/// A view of `view_type` on an image of `image_type` (the bridge logs the incompatible ones and creates them anyway).
bool IsViewTypeCompatible(ImageType view_type, ImageType image_type);

/// An image view (the bridge's ImageViewInfo): what keys it and creates it.
struct ImageViewDesc {
    bool supported = false; ///< false: no format (see `reason`)
    std::string_view reason;
    ImageType type = ImageType::Color2D;
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    u16 base_level = 0;
    u16 base_layer = 0;
    u16 levels = 1;
    u16 layers = 1;
    CompMapping mapping{};
    u32 min_lod = 0; ///< the T#'s MIN_LOD (4.8)
    bool is_storage = false;

    bool operator==(const ImageViewDesc& o) const {
        return supported == o.supported && type == o.type && format == o.format && base_level == o.base_level &&
               base_layer == o.base_layer && levels == o.levels && layers == o.layers && mapping == o.mapping &&
               min_lod == o.min_lod && is_storage == o.is_storage;
    }
};
/// The view a shader reads/writes a T# through (`is_array`: the instruction addresses layers; `is_storage`: written).
ImageViewDesc TSharpView(const Gpu::Shader::ImageDesc& sharp, bool is_depth, bool is_array, bool is_storage);
ImageViewDesc ColorTargetView(const Cp::RegisterFile& regs, u32 n);
/// The depth target's view (storage = DB_DEPTH_CONTROL.Z_WRITE_ENABLE, layers from DB_DEPTH_VIEW).
ImageViewDesc DepthTargetView(const Cp::RegisterFile& regs);
/// VK_EXT_image_view_min_lod's minLod for a view (0: none, also without the extension).
float ViewMinLod(const ImageViewDesc& view, bool min_lod_supported);

struct ViewFormat {
    VkFormat format;
    VkImageAspectFlags aspect;
};
/// The format and aspect a view of an image uses: R32/R16 on a depth image (R8 on a stencil image) take the image's
/// own format and the depth (stencil) aspect.
ViewFormat ViewFormatOf(VkFormat view_format, VkFormat image_format, VkImageAspectFlags image_aspect);

VkImageAspectFlags ImageAspect(const ImageLayout& layout);
/// The usage an image is created with (`feedback_loop`: VK_EXT_attachment_feedback_loop_layout; `ms_storage`:
/// shaderStorageImageMultisample).
VkImageUsageFlags ImageUsage(const ImageLayout& layout, bool feedback_loop, bool ms_storage);
/// The format features `usage` needs (storage left out on purpose, as the bridge: it is always requested).
VkFormatFeatureFlags2 FormatFeatures(VkImageUsageFlags usage);
/// MUTABLE_FORMAT | EXTENDED_USAGE, 2D-array (and 2D-view, `view_2d_of_3d`) compatible for volumes, block texel view
/// compatible for BC (`block_texel_view`).
VkImageCreateFlags ImageCreateFlags(const ImageLayout& layout, bool view_2d_of_3d, bool block_texel_view);

} // namespace SecondSon::Gpu::Vk
