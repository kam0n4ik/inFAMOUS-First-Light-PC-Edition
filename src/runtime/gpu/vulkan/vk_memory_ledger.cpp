// SecondSonPC — the device memory ledger. See vk_memory_ledger.h.
#include "runtime/gpu/vulkan/vk_memory_ledger.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

namespace SecondSon::Gpu::Vk::MemoryLedger {

namespace {

PFN_vkAllocateMemory g_allocate = nullptr;
PFN_vkFreeMemory g_free = nullptr;
u32 g_local_types = 0; ///< bit per memory type: device-local
std::mutex g_mutex;
std::unordered_map<VkDeviceMemory, u64> g_sizes; ///< the counted allocations
std::atomic<u64> g_bytes{0}, g_peak{0};

bool ImportsHost(const VkMemoryAllocateInfo* info) {
    for (auto* s = static_cast<const VkBaseInStructure*>(info->pNext); s != nullptr; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT) {
            return true;
        }
    }
    return false;
}

VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice device, const VkMemoryAllocateInfo* info,
                                        const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    const VkResult r = g_allocate(device, info, callbacks, memory);
    if (r == VK_SUCCESS && info->memoryTypeIndex < 32 && ((g_local_types >> info->memoryTypeIndex) & 1) != 0 &&
        !ImportsHost(info)) {
        std::scoped_lock lock{g_mutex};
        g_sizes[*memory] = info->allocationSize;
        const u64 now = g_bytes.fetch_add(info->allocationSize, std::memory_order_relaxed) + info->allocationSize;
        if (now > g_peak.load(std::memory_order_relaxed)) {
            g_peak.store(now, std::memory_order_relaxed);
        }
    }
    return r;
}

VKAPI_ATTR void VKAPI_CALL Free(VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks* callbacks) {
    if (memory != VK_NULL_HANDLE) {
        std::scoped_lock lock{g_mutex};
        if (const auto it = g_sizes.find(memory); it != g_sizes.end()) {
            g_bytes.fetch_sub(it->second, std::memory_order_relaxed);
            g_sizes.erase(it);
        }
    }
    g_free(device, memory, callbacks);
}

} // namespace

void Install(Api& f, const VkPhysicalDeviceMemoryProperties& memory) {
    if (g_allocate != nullptr || f.vkAllocateMemory == nullptr || f.vkFreeMemory == nullptr) {
        return;
    }
    for (u32 t = 0; t < memory.memoryTypeCount && t < 32; ++t) {
        if ((memory.memoryHeaps[memory.memoryTypes[t].heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            g_local_types |= 1u << t;
        }
    }
    g_allocate = f.vkAllocateMemory;
    g_free = f.vkFreeMemory;
    f.vkAllocateMemory = Allocate;
    f.vkFreeMemory = Free;
}

u64 Bytes() {
    return g_bytes.load(std::memory_order_relaxed);
}

u64 PeakBytes() {
    return g_peak.load(std::memory_order_relaxed);
}

} // namespace SecondSon::Gpu::Vk::MemoryLedger
