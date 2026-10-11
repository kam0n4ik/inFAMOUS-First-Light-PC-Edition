// SecondSonPC — image readback for diagnostics (P4.5). See vk_capture.h.
#include "runtime/gpu/vulkan/vk_capture.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/png_writer.h"
#include "runtime/gpu/vulkan/vk_device_context.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

float Half(u16 h) {
    const u32 sign = (h >> 15) & 1, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    if (exp == 0) {
        return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(man), -24);
    }
    if (exp == 31) {
        return man != 0 ? 0.0f : (sign ? -1.0f : 1.0f) * 65504.0f;
    }
    return std::bit_cast<float>((sign << 31) | ((exp + 112) << 23) | (man << 13));
}

u8 Byte(float v) {
    return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace

ImageReadback::ImageReadback(DeviceContext& ctx) : ctx_(ctx) {
    worker_ = std::thread([this] { Worker(); });
}

ImageReadback::~ImageReadback() {
    for (auto& p : pending_) {
        DestroyBuffer(ctx_, p.buffer);
    }
    {
        std::scoped_lock lock{mutex_};
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool ImageReadback::Record(VkCommandBuffer cb, u64 value, VkImage image, VkFormat format, VkImageAspectFlags aspect,
                           u32 layer, u32 width, u32 height, u32 texel_bytes, std::filesystem::path path, bool raw,
                           u32 downsample) {
    if (width == 0 || height == 0 || texel_bytes == 0) {
        return false;
    }
    Pending p;
    p.value = value;
    p.format = format;
    p.width = width;
    p.height = height;
    p.texel_bytes = texel_bytes;
    p.downsample = std::max<u32>(downsample, 1);
    p.raw = raw;
    p.path = std::move(path);
    std::string error;
    // cached system memory: the CPU reads it back at memory speed (write-combined memory read byte by byte is slow)
    if (!CreateBuffer(ctx_, u64{width} * height * texel_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, p.buffer, error,
                      true, HostPlacement::Cached)) {
        SSPC_LOG(GPU, Warning, "image readback: no buffer ({})", error);
        return false;
    }
    const auto& f = Fn();
    const VkBufferImageCopy region{.imageSubresource = {aspect, 0, layer, 1}, .imageExtent = {width, height, 1}};
    f.vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p.buffer.buffer, 1, &region);
    const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                              .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                              .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    f.vkCmdPipelineBarrier2(cb, &dep);
    pending_.push_back(std::move(p));
    return true;
}

void ImageReadback::Collect(u64 completed) {
    while (!pending_.empty() && pending_.front().value <= completed) {
        Pending p = std::move(pending_.front());
        pending_.pop_front();
        Job j;
        j.format = p.format;
        j.width = p.width;
        j.height = p.height;
        j.texel_bytes = p.texel_bytes;
        j.downsample = p.downsample;
        j.raw = p.raw;
        j.path = std::move(p.path);
        const u64 bytes = u64{p.width} * p.height * p.texel_bytes;
        // host-coherent memory (CreateBuffer: HOST_VISIBLE | HOST_COHERENT [| HOST_CACHED]): no invalidation needed
        j.bytes.assign(p.buffer.mapped, p.buffer.mapped + bytes);
        DestroyBuffer(ctx_, p.buffer);
        {
            std::scoped_lock lock{mutex_};
            jobs_.push_back(std::move(j));
        }
        cv_.notify_one();
    }
}

bool ImageReadback::ToRgba8(VkFormat format, const u8* src, u32 width, u32 height, std::vector<u8>& rgba) {
    const u64 n = u64{width} * height;
    rgba.resize(n * 4);
    for (u64 i = 0; i < n; ++i) {
        u8* d = rgba.data() + i * 4;
        switch (format) {
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
            d[0] = src[i * 4 + 2], d[1] = src[i * 4 + 1], d[2] = src[i * 4 + 0];
            break;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
            d[0] = src[i * 4 + 0], d[1] = src[i * 4 + 1], d[2] = src[i * 4 + 2];
            break;
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
            u32 v;
            std::memcpy(&v, src + i * 4, 4);
            const u32 c0 = v & 0x3FF, c1 = (v >> 10) & 0x3FF, c2 = (v >> 20) & 0x3FF;
            const bool r_high = format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
            d[0] = static_cast<u8>((r_high ? c2 : c0) >> 2);
            d[1] = static_cast<u8>(c1 >> 2);
            d[2] = static_cast<u8>((r_high ? c0 : c2) >> 2);
            break;
        }
        case VK_FORMAT_R16G16B16A16_SFLOAT: {
            u16 h[4];
            std::memcpy(h, src + i * 8, 8);
            d[0] = Byte(Half(h[0])), d[1] = Byte(Half(h[1])), d[2] = Byte(Half(h[2]));
            break;
        }
        default:
            return false;
        }
        d[3] = 255;
    }
    return true;
}

void ImageReadback::Worker() {
    for (;;) {
        Job j;
        {
            std::unique_lock lock{mutex_};
            cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
            if (jobs_.empty()) {
                return; // stopped with nothing left
            }
            j = std::move(jobs_.front());
            jobs_.pop_front();
        }
        std::error_code ec;
        std::filesystem::create_directories(j.path.parent_path(), ec);
        std::vector<u8> rgba;
        bool png = ToRgba8(j.format, j.bytes.data(), j.width, j.height, rgba);
        u32 w = j.width, h = j.height;
        if (png && j.downsample > 1 && w >= j.downsample && h >= j.downsample) {
            // box filter: each output texel the mean of a downsample x downsample block
            const u32 k = j.downsample, ow = w / k, oh = h / k;
            std::vector<u8> small(u64{ow} * oh * 4);
            for (u32 y = 0; y < oh; ++y) {
                for (u32 x = 0; x < ow; ++x) {
                    u32 acc[4] = {};
                    for (u32 dy = 0; dy < k; ++dy) {
                        const u8* row = rgba.data() + (u64{y * k + dy} * w + u64{x} * k) * 4;
                        for (u32 dx = 0; dx < k * 4; ++dx) {
                            acc[dx & 3] += row[dx];
                        }
                    }
                    for (u32 c = 0; c < 4; ++c) {
                        small[(u64{y} * ow + x) * 4 + c] = static_cast<u8>(acc[c] / (k * k));
                    }
                }
            }
            rgba.swap(small);
            w = ow;
            h = oh;
        }
        png = png && WritePng(std::filesystem::path(j.path).replace_extension(".png"), w, h, rgba);
        bool raw_ok = !j.raw;
        if (j.raw || !png) {
            std::ofstream raw(std::filesystem::path(j.path).replace_extension(".raw"), std::ios::binary | std::ios::trunc);
            raw.write(reinterpret_cast<const char*>(j.bytes.data()), static_cast<std::streamsize>(j.bytes.size()));
            raw_ok = static_cast<bool>(raw);
            std::ofstream meta(std::filesystem::path(j.path).replace_extension(".txt"), std::ios::trunc);
            meta << fmt::format("format={} width={} height={} row_bytes={} texel_bytes={}\n", static_cast<int>(j.format),
                                j.width, j.height, u64{j.width} * j.texel_bytes, j.texel_bytes);
        }
        if (png || raw_ok) {
            written_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

} // namespace SecondSon::Gpu::Vk
