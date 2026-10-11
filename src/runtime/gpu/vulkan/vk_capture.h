// SecondSonPC — image readback for diagnostics (P4.5; docs/PHASE4_P45_GRAPHICS_CORRECTNESS.md §A/B): a copy of an
// image recorded into the batch that uses it (stream order: exactly what that batch left there), read on the CPU once
// the batch completed, written to disk by a worker thread. Nothing waits: the game keeps its pace (the presenter's
// one-frame capture waited on a fence and read uncached memory, ~10 s a frame).
//
// Output: <path>.png for the formats a display shows (8-bit, 10-bit, half float), and <path>.raw + <path>.txt
// ("format=<VkFormat> width=<w> height=<h> row_bytes=<n> texel_bytes=<b>") for everything (exact values for tools).
// The GPU thread records and collects; the worker writes.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"
#include "runtime/gpu/vulkan/vk_resources.h"

namespace SecondSon::Gpu::Vk {

class DeviceContext;

class ImageReadback {
public:
    explicit ImageReadback(DeviceContext& ctx);
    ~ImageReadback(); ///< writes what completed, discards the rest (the device must be idle), joins the worker

    /// Records into `cb` (outside rendering; `image` in TRANSFER_SRC_OPTIMAL) a copy of mip 0 / layer `layer` of
    /// `image` (`aspect`, `width` x `height`, `texel_bytes` a texel in the copy, `format` how to read them). Written to
    /// `path` (+ .png / .raw / .txt) once submission `value` completed; `downsample` > 1: the PNG box-filtered by it (long
    /// series). false: no memory for it.
    bool Record(VkCommandBuffer cb, u64 value, VkImage image, VkFormat format, VkImageAspectFlags aspect, u32 layer,
                u32 width, u32 height, u32 texel_bytes, std::filesystem::path path, bool raw, u32 downsample = 1);
    /// Captures whose submission completed handed to the worker. GPU thread.
    void Collect(u64 completed);
    u64 Written() const { return written_.load(std::memory_order_relaxed); }

    /// 8-bit RGBA of `width` x `height` texels in `format` (the display formats, half floats clamped); false: not one
    /// of them.
    static bool ToRgba8(VkFormat format, const u8* src, u32 width, u32 height, std::vector<u8>& rgba);

private:
    struct Pending {
        u64 value = 0;
        OwnedBuffer buffer;
        VkFormat format = VK_FORMAT_UNDEFINED;
        u32 width = 0, height = 0, texel_bytes = 0, downsample = 1;
        bool raw = false;
        std::filesystem::path path;
    };
    struct Job {
        std::vector<u8> bytes;
        VkFormat format = VK_FORMAT_UNDEFINED;
        u32 width = 0, height = 0, texel_bytes = 0, downsample = 1;
        bool raw = false;
        std::filesystem::path path;
    };
    void Worker();

    DeviceContext& ctx_;
    std::deque<Pending> pending_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Job> jobs_;
    bool stop_ = false;
    std::atomic<u64> written_{0};
    std::thread worker_;
};

} // namespace SecondSon::Gpu::Vk
