// SecondSonPC — AMD FSR 3.1 frame generation for the native backend's presenter (docs/UPSCALING_FRAME_GENERATION.md).
//
// The FidelityFX frame generation swapchain (amd_fidelityfx_vk.dll) replaces the presenter's swapchain: the presenter
// keeps drawing each frame the game rendered into the swapchain's image and presents it; FFX generates a frame between
// it and the previous one (from the game's depth and motion vectors, the frame without the HUD) and presents both,
// paced over the interval between the game's frames. The game's simulation, timing and frame rate are untouched: only
// what reaches the display changes. Frames without the game's scene inputs (menus, movies, loading screens, a reset)
// are presented alone (generation off for them).
//
// Integration follows shadPS4-Bloodborne-DLSS-FSR (IFreemz, vk_bb_fsr_frame_gen.cpp, GPL-2.0-or-later): the same
// FidelityFX API calls and queue plan.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Upscale {

class FsrFrameGen {
public:
    struct Queue {
        VkQueue queue = VK_NULL_HANDLE;
        u32 family = ~0u;
    };
    struct Queues {
        Queue game;    ///< the backend's queue (its submissions under the context's queue lock)
        Queue present; ///< another queue that can present
        Queue acquire; ///< another queue (any)
        Queue compute; ///< an async compute queue (optional)
    };
    /// Loads amd_fidelityfx_vk.dll and keeps the device. nullptr and `why` when it cannot run.
    static std::unique_ptr<FsrFrameGen> Create(VkPhysicalDevice physical, VkDevice device, const Queues& queues,
                                               PFN_vkGetDeviceProcAddr gdpa, std::string& why);
    ~FsrFrameGen();

    // the swapchain functions, in place of Vulkan's
    VkResult CreateSwapchain(const VkSwapchainCreateInfoKHR& info, VkSwapchainKHR& out);
    void DestroySwapchain(VkSwapchainKHR swapchain); ///< (the generation context first: it presents to it)
    VkResult GetImages(VkSwapchainKHR swapchain, std::vector<VkImage>& images);
    VkResult Acquire(VkSwapchainKHR swapchain, u64 timeout, VkSemaphore semaphore, VkFence fence, u32* index);
    VkResult Present(VkQueue queue, const VkPresentInfoKHR& info);

    struct Inputs {
        VkImage depth = VK_NULL_HANDLE;  ///< R32F, render size (in SHADER_READ_ONLY_OPTIMAL)
        VkImage motion = VK_NULL_HANDLE; ///< RG16F, render size: x mv_scale = render pixels, current -> previous
        u32 depth_w = 0, depth_h = 0, motion_w = 0, motion_h = 0;
        u32 render_w = 0, render_h = 0;
        float jitter_x = 0, jitter_y = 0, mv_scale_x = 1, mv_scale_y = 1;
        float camera_near = 0.1f, camera_far = 10000.0f, fov_y = 1.0f;
        bool depth_inverted = true;
        bool reset = false;
        VkImage hudless = VK_NULL_HANDLE; ///< the frame without the HUD at the backbuffer's size (SHADER_READ_ONLY)
        VkFormat hudless_format = VK_FORMAT_UNDEFINED;
    };
    /// Present thread, before presenting a game frame: `inputs` null for a frame without them (generation off).
    /// Records the preparation into `cb` (submitted before the present).
    void SetFrame(VkCommandBuffer cb, const Inputs* inputs, VkExtent2D backbuffer, float frame_ms);
    /// Frames shown since the last call, generated ones included (1 without generation).
    u32 TakeShown();
    bool Generating() const { return generating_; }

    struct Impl;

private:
    FsrFrameGen() = default;
    std::unique_ptr<Impl> impl_;
    bool generating_ = false;
};

} // namespace SecondSon::Gpu::Upscale
