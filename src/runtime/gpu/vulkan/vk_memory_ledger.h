// SecondSonPC — the device memory ledger: every vkAllocateMemory / vkFreeMemory SecondSonPC's device makes through its
// function table, counted when the memory type is device-local and not an import of host memory. What the driver, NGX
// and the frame generation library allocate on their own is not in it (the OS's count of the process minus this).
#pragma once

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Vk::MemoryLedger {

/// Wraps the table's vkAllocateMemory / vkFreeMemory (once, after the device's functions are loaded).
void Install(Api& f, const VkPhysicalDeviceMemoryProperties& memory);
/// Device-local bytes allocated now / at most so far.
u64 Bytes();
u64 PeakBytes();

} // namespace SecondSon::Gpu::Vk::MemoryLedger
