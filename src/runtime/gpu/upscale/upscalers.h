// SecondSonPC — temporal upscalers of the native backend (docs/UPSCALING_FRAME_GENERATION.md): NVIDIA DLSS Super
// Resolution (sspc_dlss.dll, the NGX SDK bridge) and AMD FSR 3.1 (amd_fidelityfx_vk.dll, FidelityFX API 1.1.4). Both
// are loaded at run time from the executable's folder; without their DLLs nothing changes. They record into a command
// buffer of the backend's one queue and take the same inputs: the scene colour, depth and motion vectors at render size,
// the camera's sub-pixel jitter, and write the output image (storage, GENERAL layout).
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/upscale/upscale_settings.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Upscale {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; ///< a 2D view of mip 0 / layer 0 (colour aspect, or depth only)
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0, height = 0;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};

struct Inputs {
    Image color;  ///< SHADER_READ_ONLY_OPTIMAL
    Image depth;  ///< SHADER_READ_ONLY_OPTIMAL (hardware depth)
    Image motion; ///< SHADER_READ_ONLY_OPTIMAL, RG: current -> previous, times mv_scale = render pixels
    Image output; ///< GENERAL, storage
    u32 render_width = 0, render_height = 0; ///< the part of the inputs that holds the scene (top left)
    float jitter_x = 0, jitter_y = 0;        ///< render pixels, the offset the scene was drawn with
    float mv_scale_x = 1, mv_scale_y = 1;
    bool reset = false;
    bool hdr = true;            ///< linear HDR colour (the upscalers find the exposure themselves)
    bool depth_inverted = false;
    float frame_ms = 33.3f;
    float sharpness = 0.0f;     ///< 0..1 (FSR's RCAS; DLSS has none since 2.5.1: SecondSonPC sharpens itself)
    float camera_near = 0.1f, camera_far = 10000.0f, fov_y = 1.0f;
};

/// Vulkan functions an upscaler SDK needs: the loader's, through the backend's table.
struct DeviceInfo {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    u32 vendor_id = 0;
};

/// NVIDIA DLSS Super Resolution through sspc_dlss.dll.
class Dlss {
public:
    /// Before the instance: loads the bridge (when sspc_dlss.dll and nvngx_dlss.dll are present) and adds the instance
    /// extensions NGX asks for. `why` says why DLSS will not be available, if it will not.
    static void AppendInstanceExtensions(std::vector<std::string>& out, std::string& why);
    /// Before the device: the device extensions NGX needs on `physical` (none and `why` when DLSS cannot run there).
    static void AppendDeviceExtensions(VkInstance instance, VkPhysicalDevice physical, u32 vendor_id,
                                       std::vector<std::string>& out, std::string& why);
    /// After the device: NGX initialization. nullptr and `why` when DLSS is unavailable.
    static std::unique_ptr<Dlss> Create(const DeviceInfo& device, std::string& why);
    ~Dlss();

    /// Records the feature's creation into `cb` when its sizes / quality changed (the caller drained the GPU first if a
    /// previous feature existed: `NeedsRecreate`).
    bool NeedsRecreate(u32 in_w, u32 in_h, u32 out_w, u32 out_h, Quality q, bool hdr, bool inverted) const;
    bool CreateFeature(VkCommandBuffer cb, u32 in_w, u32 in_h, u32 out_w, u32 out_h, Quality q, bool hdr,
                       bool inverted, std::string& why);
    bool Evaluate(VkCommandBuffer cb, const Inputs& in);
    void ReleaseFeature(); ///< after the GPU is idle

private:
    Dlss() = default;
    struct Feature {
        u32 in_w = 0, in_h = 0, out_w = 0, out_h = 0;
        Quality q = Quality::Native;
        bool hdr = false, inverted = false;
    };
    bool has_feature_ = false;
    Feature feature_{};
};

/// AMD FSR 3.1 upscaling through amd_fidelityfx_vk.dll.
class Fsr {
public:
    static std::unique_ptr<Fsr> Create(const DeviceInfo& device, std::string& why);
    ~Fsr();
    bool NeedsRecreate(u32 max_in_w, u32 max_in_h, u32 out_w, u32 out_h, bool hdr, bool inverted) const;
    /// The caller drained the GPU first if a context existed.
    bool CreateContext(u32 max_in_w, u32 max_in_h, u32 out_w, u32 out_h, bool hdr, bool inverted, std::string& why);
    bool Evaluate(VkCommandBuffer cb, const Inputs& in);
    void DestroyContext(); ///< after the GPU is idle

    struct Impl;

private:
    Fsr() = default;
    std::unique_ptr<Impl> impl_;
};

/// Sub-pixel jitter (render pixels, in -0.5..0.5) of frame `index`: Halton(2,3) over the phase count FSR recommends
/// for the ratio, 8 * scale^2 (both upscalers take the same sequence).
void Jitter(u32 index, u32 render_width, u32 display_width, float& x, float& y);

/// The folder SecondSonPC.exe is in (the vendors' DLLs are looked for there).
std::wstring ExecutableDirectory();


} // namespace SecondSon::Gpu::Upscale
