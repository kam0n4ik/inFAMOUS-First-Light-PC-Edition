// SecondSonPC — the backend's images (P4.4). See vk_image_cache.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_image_cache.h"

#include "runtime/gpu/page_tracker.h"
#include "runtime/gpu/vulkan/vk_buffer_cache.h"
#include "runtime/gpu/vulkan/vk_diagnostics.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/host_shaders.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_resources.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    return h;
}

bool SameImage(const ImageLayout& a, const ImageLayout& b) {
    return a.pixel_format == b.pixel_format && a.type == b.type && a.size == b.size && a.levels == b.levels &&
           a.layers == b.layers && a.num_samples == b.num_samples && a.tile_mode == b.tile_mode &&
           a.pitch == b.pitch && a.guest_size == b.guest_size;
}

/// Another image at the same address that can serve as this one through a view (the same texels: size, tiling,
/// samples; a format of the same class; at least the levels/layers asked for).
bool Serves(const ImageLayout& have, const ImageLayout& want) {
    return have.size == want.size && have.num_samples == want.num_samples && have.tile_mode == want.tile_mode &&
           have.levels >= want.levels && have.layers >= want.layers &&
           IsVulkanFormatCompatible(static_cast<VkFormat>(have.pixel_format), static_cast<VkFormat>(want.pixel_format));
}

constexpr VkAccessFlags2 kWriteAccess = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT |
                                        VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                                        VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                        VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_WRITE_BIT;

} // namespace

ImageCache::ImageCache(DeviceContext& ctx, GuestBuffers& guest) : ctx_(ctx), guest_(guest) {
    const auto& f = Fn();
    char rule[8] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_ALIAS_RULE", rule, sizeof(rule)) == 3 && std::string_view(rule) == "off") {
        alias_rule_ = false;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_ALIAS_RULE=off - the image cache's equal-address "
                               "aliasing rule is disabled for this run (a control run, not a configuration)");
    }
    const VkDescriptorSetLayoutBinding b[3] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };
    const VkDescriptorSetLayoutCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                              .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                              .bindingCount = 3,
                                              .pBindings = b};
    f.vkCreateDescriptorSetLayout(ctx_.Device(), &dci, nullptr, &detile_set_layout_);
    const VkPipelineLayoutCreateInfo pci{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &detile_set_layout_};
    f.vkCreatePipelineLayout(ctx_.Device(), &pci, nullptr, &detile_layout_);
}

ImageCache::~ImageCache() {
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    for (auto& img : images_) {
        for (auto& [k, v] : img->views) {
            f.vkDestroyImageView(dev, v, nullptr);
        }
        f.vkDestroyImage(dev, img->image, nullptr);
        f.vkFreeMemory(dev, img->memory, nullptr);
    }
    for (auto& [k, s] : samplers_) {
        f.vkDestroySampler(dev, s, nullptr);
    }
    for (auto& t : temps_) {
        f.vkDestroyBuffer(dev, t.buffer, nullptr);
        f.vkFreeMemory(dev, t.memory, nullptr);
    }
    for (auto& [k, p] : detile_pipelines_) {
        f.vkDestroyPipeline(dev, p, nullptr);
    }
    if (detile_layout_ != VK_NULL_HANDLE) {
        f.vkDestroyPipelineLayout(dev, detile_layout_, nullptr);
    }
    if (detile_set_layout_ != VK_NULL_HANDLE) {
        f.vkDestroyDescriptorSetLayout(dev, detile_set_layout_, nullptr);
    }
}

void ImageCache::Evict(u64 completed) {
    if (stats_.bytes <= budget_) {
        return;
    }
    // candidates: images uploaded from guest memory (re-uploadable), not used by a batch still in flight
    std::vector<CachedImage*> cand;
    for (const auto& img : images_) {
        if (!img->gpu_written && img->last_seq <= completed) {
            cand.push_back(img.get());
        }
    }
    std::ranges::sort(cand, [](const CachedImage* a, const CachedImage* b) { return a->last_seq < b->last_seq; });
    const u64 target = budget_ / 10 * 9;
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    std::vector<CachedImage*> gone;
    for (CachedImage* img : cand) {
        if (stats_.bytes <= target) {
            break;
        }
        for (auto& [k, v] : img->views) {
            f.vkDestroyImageView(dev, v, nullptr);
        }
        f.vkDestroyImage(dev, img->image, nullptr);
        f.vkFreeMemory(dev, img->memory, nullptr);
        const auto [lo, hi] = by_address_.equal_range(img->layout.guest_address);
        for (auto it = lo; it != hi; ++it) {
            if (it->second == img) {
                by_address_.erase(it);
                break;
            }
        }
        std::erase_if(meta_owner_, [&](const auto& e) { return e.second == img; });
        stats_.bytes -= img->bytes;
        --stats_.images;
        ++stats_.evicted;
        stats_.evicted_bytes += img->bytes;
        gone.push_back(img);
    }
    if (gone.empty()) {
        return;
    }
    std::erase_if(images_, [&](const std::unique_ptr<CachedImage>& p) {
        return std::ranges::find(gone, p.get()) != gone.end();
    });
    ++epoch_; // what Find returned (the executor's target cache and texture memo) may be gone
}

void ImageCache::Invalidate(u64 addr, u64 size) {
    std::scoped_lock lock{inval_mutex_};
    pending_inval_.emplace_back(addr, size);
    has_inval_.store(true, std::memory_order_release);
}

void ImageCache::ApplyInvalidations() {
    if (!has_inval_.load(std::memory_order_acquire)) {
        return; // the common case: no lock taken
    }
    std::vector<std::pair<u64, u64>> ranges;
    {
        std::scoped_lock lock{inval_mutex_};
        has_inval_.store(false, std::memory_order_relaxed);
        if (pending_inval_.empty()) {
            return;
        }
        ranges.swap(pending_inval_);
    }
    // only the images that can overlap each range (by address; it walked every image for every range: 7.8% of the GPU
    // thread's samples, f1)
    for (const auto& [addr, size] : ranges) {
        const u64 end = addr + size;
        auto it = by_address_.lower_bound(addr > max_image_size_ ? addr - max_image_size_ : 0);
        for (; it != by_address_.end() && it->first < end; ++it) {
            CachedImage* img = it->second;
            const u64 b = img->layout.guest_address, e = b + img->layout.guest_size;
            if (b < end && addr < e && img->uploaded && !img->gpu_written) {
                img->dirty = true;
                ++stats_.invalidations;
            }
        }
    }
}

CachedImage* ImageCache::Create(const ImageLayout& layout, std::string& error) {
    const auto& f = Fn();
    const VkFormat format = static_cast<VkFormat>(layout.pixel_format);
    const auto type = ConvertImageType(layout.type);
    if (!layout.supported || format == VK_FORMAT_UNDEFINED || !type.Valid()) {
        error = fmt::format("image at {:#x}: {}", layout.guest_address,
                            !layout.supported ? std::string(layout.reason) : "no host format or type");
        return nullptr;
    }
    auto img = std::make_unique<CachedImage>();
    img->layout = layout;
    img->format = format;
    img->aspect = ImageAspect(layout);
    VkImageUsageFlags usage = ImageUsage(layout, false, false);
    const VkImageCreateFlags flags = ImageCreateFlags(layout, false, true);
    const auto samples = NumSamples(layout.num_samples, layout.props.is_depth ? ctx_.Caps().depth_sample_counts
                                                                              : ctx_.Caps().color_sample_counts);
    // the device may not take every usage for the format: drop storage, then the attachment
    VkImageFormatProperties props{};
    VkResult r = f.vkGetPhysicalDeviceImageFormatProperties(ctx_.Physical(), format, type.value,
                                                            VK_IMAGE_TILING_OPTIMAL, usage, flags, &props);
    if (r != VK_SUCCESS) {
        usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
        r = f.vkGetPhysicalDeviceImageFormatProperties(ctx_.Physical(), format, type.value, VK_IMAGE_TILING_OPTIMAL,
                                                       usage, flags, &props);
    }
    if (r != VK_SUCCESS) {
        usage &= ~(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
        r = f.vkGetPhysicalDeviceImageFormatProperties(ctx_.Physical(), format, type.value, VK_IMAGE_TILING_OPTIMAL,
                                                       usage, flags, &props);
    }
    if (r != VK_SUCCESS) {
        error = fmt::format("image at {:#x}: format {} is not supported by the device", layout.guest_address,
                            static_cast<int>(format));
        return nullptr;
    }
    img->usage = usage;
    img->create_flags = flags;
    const VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                .flags = flags,
                                .imageType = type.value,
                                .format = format,
                                .extent = {std::max(layout.size.width, 1u), std::max(layout.size.height, 1u),
                                           std::max(layout.size.depth, 1u)},
                                .mipLevels = std::max<u32>(layout.levels, 1),
                                .arrayLayers = std::max<u32>(layout.layers, 1),
                                .samples = samples.Valid() ? samples.value : VK_SAMPLE_COUNT_1_BIT,
                                .tiling = VK_IMAGE_TILING_OPTIMAL,
                                .usage = usage,
                                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (f.vkCreateImage(ctx_.Device(), &ici, nullptr, &img->image) != VK_SUCCESS) {
        error = fmt::format("vkCreateImage failed for {}x{} format {}", ici.extent.width, ici.extent.height,
                            static_cast<int>(format));
        return nullptr;
    }
    VkMemoryRequirements req{};
    f.vkGetImageMemoryRequirements(ctx_.Device(), img->image, &req);
    const s32 mt = ctx_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                   .allocationSize = req.size,
                                   .memoryTypeIndex = static_cast<u32>(std::max(mt, 0))};
    if (mt < 0 || f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &img->memory) != VK_SUCCESS) {
        f.vkDestroyImage(ctx_.Device(), img->image, nullptr);
        error = fmt::format("no video memory for a {} MiB image", req.size >> 20);
        return nullptr;
    }
    f.vkBindImageMemory(ctx_.Device(), img->image, img->memory, 0);
    img->bytes = req.size;
    img->id = next_id_++;
    ++stats_.created;
    ++stats_.images;
    stats_.bytes += req.size;
    CachedImage* raw = img.get();
    by_address_.emplace(layout.guest_address, raw);
    max_image_size_ = std::max<u64>(max_image_size_, layout.guest_size);
    ++epoch_;
    images_.push_back(std::move(img));
    return raw;
}

bool ImageCache::DetilePipeline(const TilingParams& p, VkPipeline& out, std::string& error) {
    u64 key = Mix(p.num_bits, p.macro ? 1 : 0);
    for (u32 i = 0; i < p.spec_count; ++i) {
        key = Mix(key, p.spec[i]);
    }
    if (const auto it = detile_pipelines_.find(key); it != detile_pipelines_.end()) {
        out = it->second;
        return true;
    }
    const std::span<const u32> code = HostShaders::Tiling(p.num_bits, p.macro);
    if (code.empty()) {
        error = fmt::format("no tiling kernel for {} bits ({})", p.num_bits, p.macro ? "macro" : "micro");
        return false;
    }
    const auto& f = Fn();
    const VkShaderModuleCreateInfo mci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = code.size_bytes(),
                                       .pCode = code.data()};
    VkShaderModule module = VK_NULL_HANDLE;
    if (f.vkCreateShaderModule(ctx_.Device(), &mci, nullptr, &module) != VK_SUCCESS) {
        error = "the tiling kernel's module was refused";
        return false;
    }
    std::array<VkSpecializationMapEntry, 12> entries{};
    for (u32 i = 0; i < 12; ++i) {
        entries[i] = {i, i * 4, 4};
    }
    const VkSpecializationInfo spec{.mapEntryCount = p.spec_count,
                                    .pMapEntries = entries.data(),
                                    .dataSize = p.spec_count * 4u,
                                    .pData = p.spec.data()};
    const VkComputePipelineCreateInfo ci{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = module,
                                                   .pName = "main",
                                                   .pSpecializationInfo = &spec},
                                         .layout = detile_layout_};
    const VkResult r = f.vkCreateComputePipelines(ctx_.Device(), VK_NULL_HANDLE, 1, &ci, nullptr, &out);
    f.vkDestroyShaderModule(ctx_.Device(), module, nullptr);
    if (r != VK_SUCCESS) {
        error = fmt::format("the tiling pipeline could not be created ({})", static_cast<int>(r));
        return false;
    }
    detile_pipelines_.emplace(key, out);
    return true;
}

bool ImageCache::Upload(CachedImage& img, VkCommandBuffer cb, u64 seq, std::string& error) {
    const auto& f = Fn();
    const ImageLayout& l = img.layout;
    GuestBuffers::View src{};
    GuestBuffers::Failure failure{};
    BufferCache::Range resident{};
    if (l.guest_size != 0 && buffers_ != nullptr && buffers_->Resident(l.guest_address, l.guest_size, resident)) {
        src.buffer = resident.buffer; // the GPU's copy (it may have written the texels: newer than the host's)
        src.offset = resident.offset;
    } else if (l.guest_size == 0 || !guest_.Get(l.guest_address, l.guest_size, seq, src, failure)) {
        ++stats_.import_failures;
        error = fmt::format("image at {:#x} ({} bytes): its guest memory cannot be read by the GPU", l.guest_address,
                            l.guest_size);
        return false;
    }
    if (tracker_ != nullptr) {
        tracker_->Arm(l.guest_address, l.guest_size); // a guest write from now on uploads it again
    }
    const u64 cid = diag_ != nullptr && diag_->Enabled()
                        ? diag_->Begin(cb, seq,
                                       fmt::format("upload img{}@{:#x}+{:#x} fmt {} {}x{}x{} lv {} ly {} tile {}{}",
                                                   img.id, l.guest_address, l.guest_size, l.pixel_format, l.size.width,
                                                   l.size.height, l.size.depth, l.levels, l.layers,
                                                   static_cast<u32>(l.tile_mode), img.uploaded ? " (again)" : ""))
                        : 0;
    VkBuffer copy_src = src.buffer;
    u64 copy_base = src.offset;
    if (l.IsTiled()) {
        const TilingParams p = BuildTilingParams(l, false);
        if (!p.supported) {
            error = fmt::format("image at {:#x}: no detiler ({})", l.guest_address, p.reason);
            return false;
        }
        VkPipeline pipe = VK_NULL_HANDLE;
        if (!DetilePipeline(p, pipe, error)) {
            return false;
        }
        // a device-local temporary: the linear texels, then the tiling parameters
        const u64 uoff = (static_cast<u64>(l.guest_size) + 255) & ~255ull;
        Temp t;
        t.seq = seq;
        const VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = uoff + 256,
                                     .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        if (f.vkCreateBuffer(ctx_.Device(), &bci, nullptr, &t.buffer) != VK_SUCCESS) {
            error = "detile buffer: vkCreateBuffer failed";
            return false;
        }
        VkMemoryRequirements req{};
        f.vkGetBufferMemoryRequirements(ctx_.Device(), t.buffer, &req);
        const s32 mt = ctx_.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = req.size,
                                       .memoryTypeIndex = static_cast<u32>(std::max(mt, 0))};
        if (mt < 0 || f.vkAllocateMemory(ctx_.Device(), &mai, nullptr, &t.memory) != VK_SUCCESS) {
            f.vkDestroyBuffer(ctx_.Device(), t.buffer, nullptr);
            error = "detile buffer: no video memory";
            return false;
        }
        f.vkBindBufferMemory(ctx_.Device(), t.buffer, t.memory, 0);
        temps_.push_back(t);
        f.vkCmdUpdateBuffer(cb, t.buffer, uoff, sizeof(TilingInfo), &p.info);
        VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                            .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                            .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                            .dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT};
        VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                             .pMemoryBarriers = &mb};
        f.vkCmdPipelineBarrier2(cb, &dep);
        f.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        const VkDescriptorBufferInfo in{src.buffer, src.offset, l.guest_size};
        const VkDescriptorBufferInfo outb{t.buffer, 0, l.guest_size};
        const VkDescriptorBufferInfo uni{t.buffer, uoff, sizeof(TilingInfo)};
        const VkWriteDescriptorSet w[3] = {
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &in},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &outb},
            {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 2, .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &uni},
        };
        f.vkCmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, detile_layout_, 0, 3, w);
        f.vkCmdDispatch(cb, std::max(p.dispatch_x, 1u), 1, 1);
        mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        mb.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
        mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        f.vkCmdPipelineBarrier2(cb, &dep);
        copy_src = t.buffer;
        copy_base = 0;
        ++stats_.detiles;
    } else {
        ++stats_.linear_uploads;
    }
    Transition(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_WRITE_BIT);
    std::vector<VkBufferImageCopy> copies;
    const bool is3d = l.type == ImageType::Color3D;
    for (u32 m = 0; m < l.levels && m < kMaxMips; ++m) {
        const MipInfo& mip = l.mips[m];
        VkImageAspectFlags aspect = img.aspect;
        if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT; // the stencil plane is not uploaded (targets are not uploaded at all)
        }
        copies.push_back(VkBufferImageCopy{
            .bufferOffset = copy_base + mip.offset,
            .bufferRowLength = mip.pitch,
            .bufferImageHeight = mip.height,
            .imageSubresource = {aspect, m, 0, is3d ? 1u : std::max<u32>(l.layers, 1)},
            .imageOffset = {0, 0, 0},
            .imageExtent = {std::max(l.size.width >> m, 1u), std::max(l.size.height >> m, 1u),
                            is3d ? std::max(l.size.depth >> m, 1u) : 1u}});
    }
    f.vkCmdCopyBufferToImage(cb, copy_src, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             static_cast<u32>(copies.size()), copies.data());
    if (cid != 0) {
        diag_->End(cb, cid);
    }
    img.uploaded = true;
    img.dirty = false;
    ++stats_.uploads;
    stats_.upload_bytes += l.guest_size;
    return true;
}

CachedImage* ImageCache::Find(const ImageLayout& layout, Use use, VkCommandBuffer cb, u64 seq, std::string& error) {
    ApplyInvalidations();
    CachedImage* match = nullptr;
    const auto [lo, hi] = by_address_.equal_range(layout.guest_address);
    for (auto it = lo; it != hi; ++it) {
        if (SameImage(it->second->layout, layout)) {
            match = it->second;
            break;
        }
    }
    if (match == nullptr) {
        for (auto it = lo; it != hi; ++it) {
            if (Serves(it->second->layout, layout)) {
                match = it->second;
                ++stats_.alias_hits;
                break;
            }
        }
    }
    if (match == nullptr) {
        // the bridge's equal-address overlap (TextureCache::ResolveOverlap): the same element size and grid, and a
        // format the image can be viewed with (a depth image through its depth aspect) -> that image. Above all a
        // texture of what the GPU rendered there (G-buffer, depth, post-processing targets): those texels exist only
        // in video memory, guest memory never has them.
        // Only a sampled texture over an image the GPU wrote (a storage view needs the image's storage usage, a
        // sampled view the same sample count; CPU-written textures at a reused address are uploaded as themselves).
        const VkFormat want = static_cast<VkFormat>(layout.pixel_format);
        for (auto it = lo; it != hi && match == nullptr && use == Use::Texture && alias_rule_; ++it) {
            const ImageLayout& have = it->second->layout;
            // a compressed view of an uncompressed image is not a view in Vulkan (the bridge copies: ExpandImage)
            if (!it->second->gpu_written || have.BlockDim() != layout.BlockDim() || have.num_bits != layout.num_bits ||
                have.num_samples != layout.num_samples || have.props.is_block != layout.props.is_block ||
                (have.type == ImageType::Color3D) != (layout.type == ImageType::Color3D) ||
                have.levels < layout.levels || have.layers < layout.layers) {
                // fewer levels/layers: no view covers the request (the bridge copies the smaller image into a new one;
                // the backend has no such copy yet, the texture is its own image)
                continue;
            }
            if (layout.props.is_depth && !have.props.is_depth) {
                continue; // depth sampling of a colour image: a copy the backend does not make yet
            }
            const VkFormat hf = static_cast<VkFormat>(have.pixel_format);
            const bool viewable = IsVulkanFormatCompatible(hf, want) || (have.props.is_depth && IsFormatDepthCompatible(want)) ||
                                  (have.props.is_depth && layout.props.is_depth);
            if (viewable) {
                match = it->second;
                ++stats_.overlap_hits;
                ++match->alias_hits;
                if (diag_ != nullptr) {
                    diag_->Alias(Mix(Mix(layout.guest_address, layout.pixel_format), match->id),
                                 fmt::format("ALIAS texture {:#x}+{:#x} fmt {} type {} {}x{}x{} pitch {} bits {} block {} "
                                             "samples {} levels {} layers {} tile {} depth {} -> img{} {:#x}+{:#x} fmt {} "
                                             "type {} {}x{}x{} pitch {} bits {} block {} samples {} levels {} layers {} "
                                             "tile {} depth {} stencil {} gpu-written {} usage {:#x} flags {:#x} "
                                             "layout {}",
                                             layout.guest_address, layout.guest_size, layout.pixel_format,
                                             static_cast<u32>(layout.type), layout.size.width, layout.size.height,
                                             layout.size.depth, layout.pitch, layout.num_bits, layout.props.is_block,
                                             layout.num_samples, layout.levels, layout.layers,
                                             static_cast<u32>(layout.tile_mode), layout.props.is_depth, match->id,
                                             have.guest_address, have.guest_size, have.pixel_format,
                                             static_cast<u32>(have.type), have.size.width, have.size.height,
                                             have.size.depth, have.pitch, have.num_bits, have.props.is_block,
                                             have.num_samples, have.levels, have.layers,
                                             static_cast<u32>(have.tile_mode), have.props.is_depth,
                                             have.props.has_stencil, match->gpu_written, match->usage,
                                             match->create_flags, static_cast<int>(match->current)));
                }
            }
        }
        if (match == nullptr && use == Use::Texture) {
            for (auto it = lo; it != hi; ++it) {
                if (!it->second->gpu_written) {
                    continue;
                }
                const ImageLayout& have = it->second->layout;
                if (stats_.target_misses++ < 16) {
                    SSPC_LOG(GPU, Warning,
                             "native backend: texture at {:#x} over a GPU-written image not used: want fmt {} {}x{}x{} "
                             "pitch {} bits {} samples {} levels {} layers {} tile {} size {:#x} depth {} | have fmt {} "
                             "{}x{}x{} pitch {} bits {} samples {} levels {} layers {} tile {} size {:#x} depth {}",
                             layout.guest_address, layout.pixel_format, layout.size.width, layout.size.height,
                             layout.size.depth, layout.pitch, layout.num_bits, layout.num_samples, layout.levels,
                             layout.layers, static_cast<u32>(layout.tile_mode), layout.guest_size, layout.props.is_depth,
                             have.pixel_format, have.size.width, have.size.height, have.size.depth, have.pitch,
                             have.num_bits, have.num_samples, have.levels, have.layers, static_cast<u32>(have.tile_mode),
                             have.guest_size, have.props.is_depth);
                }
                break;
            }
        }
    }
    if (match == nullptr && use == Use::Texture) {
        // diagnostics: a new texture over memory the GPU rendered into (any overlap): those texels exist only in video
        // memory, the upload reads guest memory the GPU never wrote
        const u64 a = layout.guest_address, e = a + layout.guest_size;
        auto it = by_address_.lower_bound(a > (256ull << 20) ? a - (256ull << 20) : 0);
        for (; it != by_address_.end() && it->first < e; ++it) {
            const CachedImage* h = it->second;
            const u64 ha = h->layout.guest_address, he = ha + h->layout.guest_size;
            if (!h->gpu_written || he <= a) {
                continue;
            }
            ++stats_.overlap_misses;
            if (overlap_misses_logged_++ < 48) {
                const char* rel = ha == a                  ? "same start"
                                  : (ha < a && he >= e)   ? "texture inside target"
                                  : (a < ha && e >= he)   ? "target inside texture"
                                                          : "partial";
                SSPC_LOG(GPU, Warning,
                         "native backend: texture {:#x}+{:#x} fmt {} type {} {}x{}x{} pitch {} lv {} ly {} tile {} bits {} "
                         "block {} depth {} OVERLAPS gpu-written img{} {:#x}+{:#x} fmt {} type {} {}x{}x{} pitch {} lv {} "
                         "ly {} tile {} bits {} block {} depth {} ({}, offset {:#x})",
                         a, layout.guest_size, layout.pixel_format, static_cast<u32>(layout.type), layout.size.width,
                         layout.size.height, layout.size.depth, layout.pitch, layout.levels, layout.layers,
                         static_cast<u32>(layout.tile_mode), layout.num_bits, layout.props.is_block,
                         layout.props.is_depth, h->id, ha, h->layout.guest_size, h->layout.pixel_format,
                         static_cast<u32>(h->layout.type), h->layout.size.width, h->layout.size.height,
                         h->layout.size.depth, h->layout.pitch, h->layout.levels, h->layout.layers,
                         static_cast<u32>(h->layout.tile_mode), h->layout.num_bits, h->layout.props.is_block,
                         h->layout.props.is_depth, rel, a >= ha ? a - ha : ha - a);
            }
            break;
        }
    }
    if (match == nullptr) {
        match = Create(layout, error);
        if (match != nullptr && diag_ != nullptr && diag_->Enabled()) {
            diag_->Name(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<u64>(match->image),
                        fmt::format("img{}@{:#x}+{:#x} fmt {} {}x{}x{} lv {} ly {} s {} tile {}", match->id,
                                    layout.guest_address, layout.guest_size, layout.pixel_format, layout.size.width,
                                    layout.size.height, layout.size.depth, layout.levels, layout.layers,
                                    layout.num_samples, static_cast<u32>(layout.tile_mode)));
        }
        if (match == nullptr) {
            ++stats_.unsupported;
            if (stats_.first_unsupported.empty()) {
                stats_.first_unsupported = error;
                SSPC_LOG(GPU, Error, "native backend: {}", error);
            }
            return nullptr;
        }
    } else {
        ++stats_.hits;
    }
    match->last_seq = seq;
    const bool target = use == Use::ColorTarget || use == Use::DepthTarget;
    if (!target && !match->gpu_written && (!match->uploaded || match->dirty)) {
        stats_.reuploads += match->uploaded ? 1 : 0;
        if (before_upload_ != nullptr) {
            before_upload_(before_upload_ctx_);
        }
        if (!Upload(*match, cb, seq, error)) {
            return nullptr;
        }
    }
    if (target || use == Use::Storage) {
        match->gpu_written = true;
    }
    if (target) {
        for (const u64 meta : {layout.meta.cmask_addr, layout.meta.fmask_addr, layout.meta.htile_addr}) {
            if (meta != 0) {
                meta_owner_[meta] = match;
            }
        }
    }
    return match;
}

CachedImage* ImageCache::FindTargetAt(u64 address) {
    CachedImage* best = nullptr;
    const auto [lo, hi] = by_address_.equal_range(address);
    for (auto it = lo; it != hi; ++it) {
        if (it->second->gpu_written && (best == nullptr || it->second->last_seq > best->last_seq)) {
            best = it->second;
        }
    }
    return best;
}

VkImageView ImageCache::View(CachedImage& img, const ImageViewDesc& v, bool storage) {
    const ViewFormat vf = ViewFormatOf(v.format, img.format, img.aspect);
    VkImageViewType type = ConvertImageViewType(v.type).value;
    const bool image3d = img.layout.type == ImageType::Color3D;
    if (image3d) {
        type = VK_IMAGE_VIEW_TYPE_3D;
    } else if (type == VK_IMAGE_VIEW_TYPE_3D) {
        type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    }
    const u32 levels_avail = std::max<u32>(img.layout.levels, 1), layers_avail = image3d ? 1u : std::max<u32>(img.layout.layers, 1);
    const u32 base_level = std::min<u32>(v.base_level, levels_avail - 1);
    const u32 levels = std::clamp<u32>(v.levels, 1, levels_avail - base_level);
    const u32 base_layer = image3d ? 0u : std::min<u32>(v.base_layer, layers_avail - 1);
    u32 layers = image3d ? 1u : std::clamp<u32>(v.layers, 1, layers_avail - base_layer);
    if (type == VK_IMAGE_VIEW_TYPE_2D && layers > 1) {
        layers = 1;
    }
    constexpr VkComponentMapping kIdentity{VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
    const auto mapped = ViewComponents(v.mapping);
    const VkComponentMapping comps = (storage || !mapped.Valid()) ? kIdentity : mapped.value;
    if (diag_ != nullptr && img.alias_hits != 0) {
        diag_->Alias(Mix(Mix(Mix(img.id, static_cast<u64>(v.format)), static_cast<u64>(v.type)),
                         (u64{v.base_level} << 48) | (u64{v.levels} << 32) | (u64{v.base_layer} << 16) | v.layers),
                     fmt::format("VIEW of aliased img{} (image fmt {} type {} levels {} layers {} usage {:#x} flags "
                                 "{:#x}): asked fmt {} type {} levels {}+{} layers {}+{} storage {} -> view fmt {} "
                                 "aspect {:#x} type {} levels {}+{} layers {}+{}",
                                 img.id, static_cast<int>(img.format), static_cast<u32>(img.layout.type),
                                 img.layout.levels, img.layout.layers, img.usage, img.create_flags,
                                 static_cast<int>(v.format), static_cast<u32>(v.type), v.base_level, v.levels,
                                 v.base_layer, v.layers, storage, static_cast<int>(vf.format), vf.aspect,
                                 static_cast<int>(type), base_level, levels, base_layer, layers));
    }
    u64 key = Mix(static_cast<u64>(vf.format), type);
    key = Mix(key, (u64{base_level} << 48) | (u64{levels} << 32) | (u64{base_layer} << 16) | layers);
    key = Mix(key, (u64{static_cast<u32>(comps.r)} << 24) | (u64{static_cast<u32>(comps.g)} << 16) |
                       (u64{static_cast<u32>(comps.b)} << 8) | static_cast<u32>(comps.a));
    key = Mix(key, (storage ? 1u : 0u) | (u64{vf.aspect} << 8));
    if (const auto it = img.views.find(key); it != img.views.end()) {
        return it->second;
    }
    const VkImageViewUsageCreateInfo usage{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                           .usage = storage ? img.usage : (img.usage & ~VK_IMAGE_USAGE_STORAGE_BIT)};
    const VkImageViewCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .pNext = &usage,
                                   .image = img.image,
                                   .viewType = type,
                                   .format = vf.format,
                                   .components = comps,
                                   .subresourceRange = {vf.aspect, base_level, levels, base_layer, layers}};
    VkImageView view = VK_NULL_HANDLE;
    if (Fn().vkCreateImageView(ctx_.Device(), &ci, nullptr, &view) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    img.views.emplace(key, view);
    ++stats_.views;
    return view;
}

bool ImageCache::Transition(VkCommandBuffer cb, CachedImage& img, VkImageLayout layout, VkPipelineStageFlags2 stage,
                            VkAccessFlags2 access) {
    const bool prev_write = (img.access & kWriteAccess) != 0, next_write = (access & kWriteAccess) != 0;
    if (img.current == layout && !prev_write && !next_write) {
        img.stage |= stage;
        img.access |= access;
        return false;
    }
    const VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                  .srcStageMask = img.stage == 0 ? VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT : img.stage,
                                  .srcAccessMask = img.access & kWriteAccess,
                                  .dstStageMask = stage,
                                  .dstAccessMask = access,
                                  .oldLayout = img.current,
                                  .newLayout = layout,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .image = img.image,
                                  .subresourceRange = {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0,
                                                       VK_REMAINING_ARRAY_LAYERS}};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                               .pImageMemoryBarriers = &b};
    Fn().vkCmdPipelineBarrier2(cb, &dep);
    img.current = layout;
    img.stage = stage;
    img.access = access;
    ++stats_.barriers;
    return true;
}

VkSampler ImageCache::Sampler(const Gpu::Shader::SamplerDesc& s, bool is_depth, u64 border_color_base) {
    SamplerCaps caps;
    caps.anisotropy = true;
    caps.max_anisotropy = 16.0f;
    caps.custom_border_color = ctx_.Caps().custom_border_color;
    caps.max_lod_bias = 15.0f;
    const SamplerParams p = BuildSamplerParams(s, is_depth, caps);
    if (!p.supported) {
        return VK_NULL_HANDLE;
    }
    std::array<float, 4> border{};
    if (p.border_color == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT) {
        if (auto* mem = GetGuestMemory(); mem != nullptr && border_color_base != 0) {
            mem->CopySparse(border_color_base + u64{p.border_color_index} * 16, reinterpret_cast<u8*>(border.data()), 16);
        }
    }
    u64 key = Mix(Mix(s.raw[0] | (u64{s.raw[1]} << 32), s.raw[2] | (u64{s.raw[3]} << 32)), is_depth ? 1 : 0);
    u32 bits[4];
    std::memcpy(bits, border.data(), 16);
    key = Mix(Mix(key, bits[0] | (u64{bits[1]} << 32)), bits[2] | (u64{bits[3]} << 32));
    if (const auto it = samplers_.find(key); it != samplers_.end()) {
        return it->second;
    }
    VkSamplerCustomBorderColorCreateInfoEXT custom{.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
                                                   .format = VK_FORMAT_R32G32B32A32_SFLOAT};
    std::memcpy(custom.customBorderColor.float32, border.data(), 16);
    const VkSamplerCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                 .pNext = p.border_color == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ? &custom : nullptr,
                                 .magFilter = p.mag_filter,
                                 .minFilter = p.min_filter,
                                 .mipmapMode = p.mipmap_mode,
                                 .addressModeU = p.address_u,
                                 .addressModeV = p.address_v,
                                 .addressModeW = p.address_w,
                                 .mipLodBias = p.mip_lod_bias,
                                 .anisotropyEnable = p.anisotropy_enable ? VK_TRUE : VK_FALSE,
                                 .maxAnisotropy = p.max_anisotropy,
                                 .compareEnable = p.compare_enable ? VK_TRUE : VK_FALSE,
                                 .compareOp = p.compare_op,
                                 .minLod = p.min_lod,
                                 .maxLod = p.max_lod,
                                 .borderColor = p.border_color,
                                 .unnormalizedCoordinates = VK_FALSE};
    VkSampler sampler = VK_NULL_HANDLE;
    if (Fn().vkCreateSampler(ctx_.Device(), &ci, nullptr, &sampler) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    samplers_.emplace(key, sampler);
    ++stats_.samplers;
    return sampler;
}

void ImageCache::Collect(u64 completed) {
    while (!temps_.empty() && temps_.front().seq <= completed) {
        Fn().vkDestroyBuffer(ctx_.Device(), temps_.front().buffer, nullptr);
        Fn().vkFreeMemory(ctx_.Device(), temps_.front().memory, nullptr);
        temps_.pop_front();
    }
    Evict(completed);
}

} // namespace SecondSon::Gpu::Vk
