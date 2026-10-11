// SecondSonPC — host-side compute shaders (P4.4). See host_shaders.h.
#include "runtime/gpu/host_shaders.h"

#include <atomic>

namespace SecondSon::Gpu::HostShaders {

namespace {
std::atomic<std::span<const u32> (*)(u32, bool)> g_provider{nullptr};
}

std::span<const u32> Tiling(u32 num_bits, bool macro) {
    const auto provider = g_provider.load(std::memory_order_acquire);
    return provider != nullptr ? provider(num_bits, macro) : std::span<const u32>{};
}

void Install(std::span<const u32> (*provider)(u32, bool)) {
    g_provider.store(provider, std::memory_order_release);
}

} // namespace SecondSon::Gpu::HostShaders
