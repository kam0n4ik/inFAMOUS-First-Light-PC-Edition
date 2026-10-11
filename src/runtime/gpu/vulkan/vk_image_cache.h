// SecondSonPC — the backend's images (Phase 4, P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §C): guest images as
// Vulkan images in video memory, keyed by their layout (image_layout.h), uploaded from the guest's memory (linear:
// copied from the imported guest buffer; tiled: detiled by the tiling kernels into scratch, then copied), render and
// depth targets (rendered by the GPU, never uploaded over), views, samplers, layout/access tracking for barriers,
// invalidation when the guest writes the memory (the memory listener) — a texture is uploaded again at its next use;
// a GPU-written image stays (its memory in guest RAM is stale until written back, which this sub-stage does not do).
//
// One thread (the backend's GPU thread) uses it; Invalidate may come from any thread (queued under a mutex).
// Nothing is substituted: an image that cannot be made (unsupported layout/format, no memory) is reported and the
// caller binds a null descriptor and counts the draw as incomplete.
#pragma once

#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/image_layout.h"
#include "runtime/gpu/vulkan/vk_api.h"
#include "runtime/gpu/vulkan/vk_formats.h"

namespace SecondSon::Gpu {
class PageTracker;
}

namespace SecondSon::Gpu::Vk {

class Diagnostics;
class BufferCache;

class DeviceContext;
class GuestBuffers;

struct CachedImage {
    u32 id = 0;
    ImageLayout layout;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageUsageFlags usage = 0;
    VkImageCreateFlags create_flags = 0;
    u32 alias_hits = 0; ///< textures over another layout that the equal-address rule gave this image
    u64 bytes = 0;
    // state for barriers (whole image)
    VkImageLayout current = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags2 stage = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    VkAccessFlags2 access = 0;
    bool uploaded = false;    ///< its guest memory was copied in (textures)
    bool gpu_written = false; ///< rendered/stored by the GPU: guest memory is no longer its source
    bool dirty = false;       ///< the guest wrote its memory since the upload
    u64 last_seq = 0;
    std::unordered_map<u64, VkImageView> views;
};

struct ImageCacheStats {
    u64 images = 0, bytes = 0, created = 0, uploads = 0, detiles = 0, linear_uploads = 0, upload_bytes = 0;
    u64 hits = 0, invalidations = 0, reuploads = 0, unsupported = 0, import_failures = 0, alias_hits = 0;
    u64 views = 0, samplers = 0, barriers = 0;
    u64 overlap_hits = 0;   ///< the bridge's equal-address rule: another image's texels through a view
    u64 target_misses = 0;  ///< a texture over a GPU-written image none of the rules accepted (uploaded instead)
    u64 overlap_misses = 0; ///< a new texture whose memory overlaps a GPU-written image (any overlap; uploaded)
    std::string first_unsupported;
};

class ImageCache {
public:
    ImageCache(DeviceContext& ctx, GuestBuffers& guest);
    ~ImageCache(); ///< destroys everything (the device must be idle)

    enum class Use : u8 { Texture, Storage, ColorTarget, DepthTarget, TransferSrc };

    /// The image for `layout` (created when new). Textures are uploaded (again, when dirty) by commands recorded into
    /// `cb` for submission `seq`; targets are not. nullptr with `error` set: unsupported or out of memory.
    CachedImage* Find(const ImageLayout& layout, Use use, VkCommandBuffer cb, u64 seq, std::string& error);
    /// A view of `image` for `view` (created once).
    VkImageView View(CachedImage& image, const ImageViewDesc& view, bool storage);
    /// Records a barrier bringing the whole image to `layout` for (`stage`, `access`); none when already there and both
    /// accesses are reads. true when a barrier was recorded.
    bool Transition(VkCommandBuffer cb, CachedImage& image, VkImageLayout layout, VkPipelineStageFlags2 stage,
                    VkAccessFlags2 access);

    /// A sampler for an S# (cached by its bits). VK_NULL_HANDLE: unsupported fields (counted).
    VkSampler Sampler(const Gpu::Shader::SamplerDesc& sharp, bool is_depth, u64 border_color_base);

    /// The guest wrote or unmapped [addr, addr + size): textures there are uploaded again at their next use. Any
    /// thread.
    void Invalidate(u64 addr, u64 size);
    /// Called before an upload records its commands (the executor ends rendering and makes earlier writes visible).
    void SetBeforeUpload(void (*hook)(void* ctx), void* ctx) {
        before_upload_ = hook;
        before_upload_ctx_ = ctx;
    }
    /// Uploads read video memory where the buffer cache holds the range (the GPU may have written it newer).
    void SetBufferCache(BufferCache* buffers) { buffers_ = buffers; }
    /// Object names, the aliasing log, upload markers (--gpu-native-diag).
    void SetDiagnostics(Diagnostics* diag) { diag_ = diag; }
    /// Write tracking: a texture's guest pages are armed when it is uploaded (P4.4).
    void SetTracker(PageTracker* tracker) { tracker_ = tracker; }
    /// Releases what completed submissions no longer need (temporary detile buffers).
    void Collect(u64 completed);

    /// The target whose CMASK/FMASK/HTILE metadata starts at `address` (registered when bound as a target), nullptr: none.
    CachedImage* MetaOwner(u64 address) const {
        const auto it = meta_owner_.find(address);
        return it != meta_owner_.end() ? it->second : nullptr;
    }
    /// The image currently at `address` written by the GPU (VideoOut buffers are render targets), nullptr: none.
    CachedImage* FindTargetAt(u64 address);

    ImageCacheStats Stats() const { return stats_; }

private:
    struct Temp {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        u64 seq = 0;
    };
    CachedImage* Create(const ImageLayout& layout, std::string& error);
    bool Upload(CachedImage& image, VkCommandBuffer cb, u64 seq, std::string& error);
    bool DetilePipeline(const TilingParams& p, VkPipeline& out, std::string& error);
    void ApplyInvalidations();

    DeviceContext& ctx_;
    PageTracker* tracker_ = nullptr;
    BufferCache* buffers_ = nullptr;
    std::unordered_map<u64, CachedImage*> meta_owner_; ///< CMASK/FMASK/HTILE address -> its target
    u64 overlap_misses_logged_ = 0;
    Diagnostics* diag_ = nullptr;
    bool alias_rule_ = true; ///< SSPC_DIAG_ALIAS_RULE=off: a diagnostic control run without the equal-address rule
    void (*before_upload_)(void*) = nullptr;
    void* before_upload_ctx_ = nullptr;
    GuestBuffers& guest_;
    std::vector<std::unique_ptr<CachedImage>> images_;
    std::multimap<u64, CachedImage*> by_address_;
    u64 max_image_size_ = 0; ///< the largest image's guest size: how far before a range an overlapping image may start
    u64 epoch_ = 0;

public:
    /// Bumped whenever the set of images changes (a new image; an eviction must bump it too): what Find returned for
    /// the same layout before is still what it returns while the epoch is the same (the executor's target cache).
    u64 Epoch() const { return epoch_; }

private:
    u32 next_id_ = 1;
    std::unordered_map<u64, VkSampler> samplers_;
    std::deque<Temp> temps_;
    // detiler
    VkDescriptorSetLayout detile_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout detile_layout_ = VK_NULL_HANDLE;
    std::unordered_map<u64, VkPipeline> detile_pipelines_;
    // invalidations from other threads
    std::mutex inval_mutex_;
    std::vector<std::pair<u64, u64>> pending_inval_;
    ImageCacheStats stats_;
};

} // namespace SecondSon::Gpu::Vk
