// SecondSonPC — the Vulkan functions SecondSonPC's own backend calls (Phase 4, P4.4). The C API through a table of
// function pointers resolved from the system loader (vulkan-1.dll), the instance and the device: no global prototypes,
// no vulkan-hpp dispatcher, no VMA — nothing that could clash with the bridge's copies of them in the same executable
// (the same rule as the P4.1 foundation, vk_foundation.cpp). Internal to src/runtime/gpu/vulkan; Windows' platform
// functions (the surface) are resolved by the presenter itself.
#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

namespace SecondSon::Gpu::Vk {

#define SSPC_VKA_GLOBAL(X)                                                                                             \
    X(vkEnumerateInstanceVersion)                                                                                      \
    X(vkEnumerateInstanceExtensionProperties)                                                                          \
    X(vkEnumerateInstanceLayerProperties)                                                                              \
    X(vkCreateInstance)

#define SSPC_VKA_INSTANCE(X)                                                                                           \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceProperties2)                                                                                  \
    X(vkGetPhysicalDeviceFeatures2)                                                                                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkGetPhysicalDeviceMemoryProperties2)                                                                            \
    X(vkGetPhysicalDeviceFormatProperties)                                                                             \
    X(vkGetPhysicalDeviceImageFormatProperties)                                                                        \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)                                                                                             \
    X(vkDestroySurfaceKHR)                                                                                             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR)

/// Optional instance functions (debug utils: only with validation).
#define SSPC_VKA_INSTANCE_OPT(X)                                                                                       \
    X(vkCreateDebugUtilsMessengerEXT)                                                                                  \
    X(vkDestroyDebugUtilsMessengerEXT)

#define SSPC_VKA_DEVICE(X)                                                                                             \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkQueueWaitIdle)                                                                                                 \
    X(vkQueueSubmit2)                                                                                                  \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkResetCommandPool)                                                                                              \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkFreeCommandBuffers)                                                                                            \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkResetCommandBuffer)                                                                                            \
    X(vkGetFenceStatus)                                                                                                \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                 \
    X(vkResetFences)                                                                                                   \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkWaitSemaphores)                                                                                                \
    X(vkSignalSemaphore)                                                                                               \
    X(vkGetSemaphoreCounterValue)                                                                                      \
    X(vkCreateQueryPool)                                                                                               \
    X(vkQueueBindSparse)                                                                                               \
    X(vkDestroyQueryPool)                                                                                              \
    X(vkCmdResetQueryPool)                                                                                             \
    X(vkCmdWriteTimestamp2)                                                                                            \
    X(vkGetQueryPoolResults)                                                                                           \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindBufferMemory)                                                                                              \
    X(vkBindImageMemory)                                                                                               \
    X(vkMapMemory)                                                                                                     \
    X(vkUnmapMemory)                                                                                                   \
    X(vkCreateImageView)                                                                                               \
    X(vkDestroyImageView)                                                                                              \
    X(vkCreateSampler)                                                                                                 \
    X(vkDestroySampler)                                                                                                \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)                                                                                           \
    X(vkCreateDescriptorSetLayout)                                                                                     \
    X(vkDestroyDescriptorSetLayout)                                                                                    \
    X(vkCreateDescriptorPool)                                                                                          \
    X(vkDestroyDescriptorPool)                                                                                         \
    X(vkResetDescriptorPool)                                                                                           \
    X(vkAllocateDescriptorSets)                                                                                        \
    X(vkUpdateDescriptorSets)                                                                                          \
    X(vkCmdBindDescriptorSets)                                                                                         \
    X(vkCmdClearAttachments)                                                                                           \
    X(vkCreatePipelineLayout)                                                                                          \
    X(vkDestroyPipelineLayout)                                                                                         \
    X(vkCreateGraphicsPipelines)                                                                                       \
    X(vkCreateComputePipelines)                                                                                        \
    X(vkDestroyPipeline)                                                                                               \
    X(vkCmdPipelineBarrier2)                                                                                           \
    X(vkCmdBeginRendering)                                                                                             \
    X(vkCmdEndRendering)                                                                                               \
    X(vkCmdBindPipeline)                                                                                               \
    X(vkCmdPushDescriptorSetKHR)                                                                                       \
    X(vkCmdPushConstants)                                                                                              \
    X(vkCmdBindVertexBuffers)                                                                                          \
    X(vkCmdBindVertexBuffers2)                                                                                         \
    X(vkCmdBindIndexBuffer)                                                                                            \
    X(vkCmdDraw)                                                                                                       \
    X(vkCmdDrawIndexed)                                                                                                \
    X(vkCmdDrawIndirect)                                                                                               \
    X(vkCmdDrawIndexedIndirect)                                                                                        \
    X(vkCmdDispatch)                                                                                                   \
    X(vkCmdDispatchIndirect)                                                                                           \
    X(vkCmdSetViewportWithCount)                                                                                       \
    X(vkCmdSetScissorWithCount)                                                                                        \
    X(vkCmdSetBlendConstants)                                                                                          \
    X(vkCmdSetDepthTestEnable)                                                                                         \
    X(vkCmdSetDepthWriteEnable)                                                                                        \
    X(vkCmdSetDepthCompareOp)                                                                                          \
    X(vkCmdSetDepthBiasEnable)                                                                                         \
    X(vkCmdSetDepthBias)                                                                                               \
    X(vkCmdSetDepthBoundsTestEnable)                                                                                   \
    X(vkCmdSetDepthBounds)                                                                                             \
    X(vkCmdSetStencilTestEnable)                                                                                       \
    X(vkCmdSetStencilOp)                                                                                               \
    X(vkCmdSetStencilReference)                                                                                        \
    X(vkCmdSetStencilCompareMask)                                                                                      \
    X(vkCmdSetStencilWriteMask)                                                                                        \
    X(vkCmdSetCullMode)                                                                                                \
    X(vkCmdSetFrontFace)                                                                                               \
    X(vkCmdSetRasterizerDiscardEnable)                                                                                 \
    X(vkCmdSetLineWidth)                                                                                               \
    X(vkCmdSetPrimitiveRestartEnable)                                                                                  \
    X(vkCmdCopyBuffer)                                                                                                 \
    X(vkCmdCopyBufferToImage)                                                                                          \
    X(vkCmdCopyImageToBuffer)                                                                                          \
    X(vkCmdCopyImage)                                                                                                  \
    X(vkCmdBlitImage)                                                                                                  \
    X(vkCmdClearColorImage)                                                                                            \
    X(vkCmdClearDepthStencilImage)                                                                                     \
    X(vkCmdFillBuffer)                                                                                                 \
    X(vkCmdUpdateBuffer)                                                                                               \
    X(vkCmdResolveImage)

/// Device functions of extensions the backend enables when the device has them (nullptr otherwise).
#define SSPC_VKA_DEVICE_OPT(X)                                                                                         \
    X(vkGetMemoryHostPointerPropertiesEXT)                                                                             \
    X(vkCmdSetColorWriteMaskEXT)                                                                                       \
    X(vkCmdSetVertexInputEXT)                                                                                          \
    X(vkCreateSwapchainKHR)                                                                                            \
    X(vkDestroySwapchainKHR)                                                                                           \
    X(vkGetSwapchainImagesKHR)                                                                                         \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkQueuePresentKHR)                                                                                               \
    X(vkSetDebugUtilsObjectNameEXT)                                                                                    \
    X(vkCmdBeginDebugUtilsLabelEXT)                                                                                    \
    X(vkCmdEndDebugUtilsLabelEXT)                                                                                      \
    X(vkCmdSetCheckpointNV)                                                                                            \
    X(vkGetQueueCheckpointDataNV)                                                                                      \
    X(vkCmdWriteBufferMarkerAMD)                                                                                       \
    X(vkGetDeviceFaultInfoEXT)

#define SSPC_VKA_MEMBER(name) PFN_##name name = nullptr;

/// The table. One per process (the backend's context fills it; everything else reads it).
struct Api {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    SSPC_VKA_GLOBAL(SSPC_VKA_MEMBER)
    SSPC_VKA_INSTANCE(SSPC_VKA_MEMBER)
    SSPC_VKA_INSTANCE_OPT(SSPC_VKA_MEMBER)
    SSPC_VKA_DEVICE(SSPC_VKA_MEMBER)
    SSPC_VKA_DEVICE_OPT(SSPC_VKA_MEMBER)
};

#undef SSPC_VKA_MEMBER

/// The process's table (filled by DeviceContext::Create; zero before).
Api& Fn();

} // namespace SecondSon::Gpu::Vk
