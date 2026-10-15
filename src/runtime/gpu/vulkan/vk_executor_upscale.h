// SecondSonPC — the executor's upscaling state (vk_executor_upscale.cpp; docs/UPSCALING_FRAME_GENERATION.md).
#pragma once

#include <memory>

#include "runtime/gpu/upscale/upscalers.h"
#include "runtime/gpu/vulkan/vk_executor.h"

namespace SecondSon::Gpu::Vk {

/// An image the upscaler owns (device-local, one mip, one layer), its layout tracked by hand.
struct UpscaleImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE; ///< in `format` (storage too when the image has it)
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    struct View {
        VkFormat format;
        VkComponentMapping components;
        VkImageView view;
    };
    std::vector<View> views; ///< other formats / swizzles of a mutable image (sampled only)
};

struct Executor::UpscaleState {
    explicit UpscaleState(DeviceContext& c) : ctx(c) {}
    ~UpscaleState();

    DeviceContext& ctx;
    std::unique_ptr<Upscale::Dlss> dlss;
    std::unique_ptr<Upscale::Fsr> fsr;
    bool tried_dlss = false, tried_fsr = false;
    std::string dlss_why, fsr_why;

    UpscaleImage color, depth, motion, output;
    UpscaleImage sharpened; ///< the output after RCAS (DLSS with sharpness > 0)
    VkDescriptorSetLayout rcas_set = VK_NULL_HANDLE;
    VkPipelineLayout rcas_layout = VK_NULL_HANDLE;
    VkPipeline rcas_pipeline = VK_NULL_HANDLE;
    OwnedBuffer depth_staging;
    VkQueryPool queries = VK_NULL_HANDLE; ///< two timestamps per evaluation, kQuerySlots evaluations in flight
    static constexpr u32 kQuerySlots = 8;
    u64 evals = 0;

    // the frame's inputs
    CachedImage* mv_img = nullptr;    ///< the velocity pass's target (RG16F, render size)
    CachedImage* vel_depth = nullptr; ///< the depth target the velocity pass tests against: the scene's depth
    u64 scene_depth_addr = 0;         ///< learned from the last velocity pass (jitter: draws with it are scene geometry)
    bool velocity_open = false;       ///< draws of the velocity pass are being recorded
    bool snapshot_valid = false;      ///< depth and motion of this frame copied
    bool done_this_frame = false;
    bool jitter_this_frame = false; ///< the frame's scene draws were jittered (they are from the second frame on)
    u32 jitter_index = 0;
    u32 jittered_draws = 0, scene_candidates = 0; ///< this frame (diagnostics)
    float frame_jx = 0.0f, frame_jy = 0.0f; ///< the jitter this frame's scene draws got
    u64 last_eval_ticks = 0;
    u32 last_w = 0, last_h = 0, last_out_w = 0, last_out_h = 0;
    Upscale::Backend last_backend = Upscale::Backend::Off;
    Upscale::Quality last_quality = Upscale::Quality::Quality;
    bool reset = true;
    u64 dump_flip = ~0ull; ///< SSPC_UPSCALE_DUMP=<flip>: inputs and output of that flip's evaluation to logs\upscale
    float jitter_sign_x = 1.0f, jitter_sign_y = 1.0f;
    float mv_sign = -1.0f;    ///< the game's vectors are current minus previous in UV (measured): x -size
    bool depth_inverted = false;
    bool logged_first = false;
    std::string last_fail; ///< the reason logged last (logged again only when it changes)
};

} // namespace SecondSon::Gpu::Vk
