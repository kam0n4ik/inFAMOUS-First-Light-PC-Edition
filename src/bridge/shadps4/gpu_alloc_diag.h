// SecondSonPC — SSPC-001 diagnostics: ledger of Vulkan device memory and the report of a failed
// buffer/image allocation in the temporary shadPS4 GPU bridge (Phase 0-4).
//
// Hooked into shadPS4 by patches/shadps4/0004-diag-sspc001-gpu-alloc.patch. Observation only: every
// wrapped Vulkan call is forwarded unchanged and its result is returned as is; a failed allocation
// still reaches shadPS4's ASSERT after the report. Removed together with the bridge (Phase 4).
#pragma once

#include <cstdint>
#include <string_view>

// shadPS4's Vulkan configuration (VK_NO_PROTOTYPES, beta extensions) must come before any Vulkan header.
#include "video_core/renderer_vulkan/vk_common.h"

struct VmaAllocator_T;
struct VmaAllocationCreateInfo;
struct VmaVulkanFunctions;

namespace SecondSon::GpuDiag {

// Instance::CreateAllocator, before vmaCreateAllocator: VMA gets wrapped vkAllocateMemory /
// vkFreeMemory / vkCreate{Buffer,Image} / vkBind*Memory*, and the vulkan-hpp default dispatcher gets
// wrapped vkAllocateMemory / vkFreeMemory (the sparse buffer arena allocates through it, outside VMA).
void Install(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
             VmaVulkanFunctions& functions, bool has_memory_budget);
void SetAllocator(VmaAllocator_T* allocator);

// What the current thread is allocating (guest range, shadPS4 MemoryType, debug name); read by the
// vkAllocateMemory wrapper and by the failure report. Lives for the scope of one Create call.
class ContextScope {
public:
    ContextScope(bool is_image, std::uint64_t guest_addr, std::uint64_t guest_size,
                 std::uint32_t mem_type, std::string_view name);
    ~ContextScope();
    ContextScope(const ContextScope&) = delete;
    ContextScope& operator=(const ContextScope&) = delete;
};

// The buffer-cache request being served on this thread (BufferCache::ObtainBuffer*): guest range
// that made the arena resident or needed staging. Nested scopes restore the outer one.
class RequestScope {
public:
    RequestScope(std::uint64_t guest_addr, std::uint64_t size, bool is_written, bool is_texel);
    ~RequestScope();
    RequestScope(const RequestScope&) = delete;
    RequestScope& operator=(const RequestScope&) = delete;

private:
    std::uint64_t saved_[3];
};

// Number of failed wrapped Vulkan calls on this thread so far; taken before vmaCreate* to tell a
// driver failure inside that call from a refusal by VMA itself.
std::uint64_t ThreadFailureSeq();

void ReportBufferFailure(VmaAllocator_T* allocator, const VkBufferCreateInfo& buffer_ci,
                         const VmaAllocationCreateInfo& alloc_ci, VkResult result,
                         std::uint32_t mem_type, std::uint64_t failure_seq_before);
void ReportImageFailure(VmaAllocator_T* allocator, const VkImageCreateInfo& image_ci,
                        const VmaAllocationCreateInfo& alloc_ci, VkResult result,
                        std::uint64_t failure_seq_before);

// One summary of the ledger, VMA and driver budgets (periodic with --vram-log, and on a crash).
void LogSummary(const char* reason);

} // namespace SecondSon::GpuDiag
