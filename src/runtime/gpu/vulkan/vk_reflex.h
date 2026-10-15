// SecondSonPC — NVIDIA Reflex, the Vulkan side (VK_NV_low_latency2; see upscale/reflex.h): the presenter's swapchain,
// its present markers and present ids, the executor's submissions tagged with their frame.
#pragma once

#include <vulkan/vulkan_core.h>

#include "runtime/gpu/upscale/reflex.h"

namespace SecondSon::Gpu::Reflex {

/// The presenter, once: the device, and whether VK_NV_low_latency2 and the presentId feature are enabled on it.
void Init(VkDevice device, bool available);
/// A swapchain created with VkSwapchainLatencyCreateInfoNV (latencyModeEnable) - the one Reflex works on now (replaces
/// the previous); Detach before it is destroyed. FSR frame generation's swapchain is not attached (its own presents).
void Attach(VkSwapchainKHR swapchain);
void Detach(VkSwapchainKHR swapchain);
bool Attached();
/// The presenter, around vkQueuePresentKHR of frame `flip_id` (its present carries VkPresentIdKHR = flip_id).
void PresentMarker(bool start, u64 flip_id);
/// The executor: a submission of frame `flip_id`'s work tagged with it (`storage` chained into VkSubmitInfo2's pNext;
/// nullptr when Reflex is not attached).
const void* SubmitChain(u64 flip_id, VkLatencySubmissionPresentIdNV& storage);

} // namespace SecondSon::Gpu::Reflex
