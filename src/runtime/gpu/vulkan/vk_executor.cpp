// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>

//
// SecondSonPC — the backend's executor (P4.4). See vk_executor.h. The per-draw state (viewports, scissors, depth,
// stencil, bias, binding conventions) follows shadPS4's renderer_vulkan/vk_rasterizer.cpp (GPL-2.0-or-later): the
// translated SPIR-V is the bridge's, so its bindings and push constants are filled the same way.
#include "runtime/gpu/vulkan/vk_executor.h"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/gnm/gnm_host.h"
#include "runtime/gpu/image_layout.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_diagnostics.h"
#include "runtime/gpu/vulkan/vk_formats.h"
#include "runtime/gpu/vulkan/vk_image_cache.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "runtime/gpu/vulkan/vk_resources.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

// context registers (indices verified against the bridge's regs.cpp layout assertions)
constexpr u32 kDbRenderControl = 0xA000;
constexpr u32 kDbHtileBase = 0xA005;
constexpr u32 kDbDepthBoundsMin = 0xA008, kDbDepthBoundsMax = 0xA009;
constexpr u32 kDbStencilClear = 0xA00A, kDbDepthClear = 0xA00B;
constexpr u32 kScreenScissorTl = 0xA00C, kScreenScissorBr = 0xA00D;
constexpr u32 kDbZInfo = 0xA010, kDbStencilInfo = 0xA011;
constexpr u32 kTaBcBase = 0xA020;
constexpr u32 kWindowOffset = 0xA080, kWindowScissorTl = 0xA081, kWindowScissorBr = 0xA082;
constexpr u32 kCbTargetMask = 0xA08E;
constexpr u32 kGenericScissorTl = 0xA090, kGenericScissorBr = 0xA091;
constexpr u32 kViewportScissor0 = 0xA094; ///< TL, BR per viewport (stride 2)
constexpr u32 kCbBlendRed = 0xA105;
constexpr u32 kDbStencilControl = 0xA10B, kDbStencilRefMask = 0xA10C, kDbStencilRefMaskBf = 0xA10D;
constexpr u32 kViewport0 = 0xA10F; ///< XSCALE, XOFFSET, YSCALE, YOFFSET, ZSCALE, ZOFFSET per viewport (stride 6)
constexpr u32 kClipUserData = 0xA16F;
constexpr u32 kDbDepthControl = 0xA200;
constexpr u32 kCbColorControl = 0xA202;
constexpr u32 kPaClClipCntl = 0xA204;
constexpr u32 kPaSuScModeCntl = 0xA205;
constexpr u32 kPaClVteCntl = 0xA206;
constexpr u32 kPaScModeCntl0 = 0xA292;
constexpr u32 kVgtMultiPrimIbResetEn = 0xA2A5;
constexpr u32 kVgtInstanceStepRate0 = 0xA2A8, kVgtInstanceStepRate1 = 0xA2A9;
constexpr u32 kPolyOffsetClamp = 0xA2DF; ///< + FRONT_SCALE, FRONT_OFFSET, BACK_SCALE, BACK_OFFSET

float F(const Cp::RegisterFile& r, u32 reg) {
    return std::bit_cast<float>(r.Get(reg));
}
s32 Lo16(u32 v) {
    return static_cast<s16>(static_cast<u16>(v & 0xFFFF));
}
s32 Hi16(u32 v) {
    return static_cast<s16>(static_cast<u16>(v >> 16));
}

constexpr u32 kQuerySlots = 1024; ///< batches in flight stay far below
constexpr u32 kProfBatches = 64, kProfPerBatch = 2048; ///< the profiler's pool: batch slots x timestamps

constexpr VkPipelineStageFlags2 kGraphicsShaders = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_TESSELLATION_CONTROL_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_TESSELLATION_EVALUATION_SHADER_BIT |
                                                   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

/// The format the displayed image is read with: the VideoOut buffer's pixel format (the bridge's ImageInfo for a
/// VideoOut buffer), when the image's texels have its size (a bit copy reinterprets them).
VkFormat DisplayFormat(u32 pixel_format, VkFormat image_format) {
    const bool four_bytes = image_format == VK_FORMAT_R8G8B8A8_UNORM || image_format == VK_FORMAT_R8G8B8A8_SRGB ||
                            image_format == VK_FORMAT_B8G8R8A8_UNORM || image_format == VK_FORMAT_B8G8R8A8_SRGB ||
                            image_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                            image_format == VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    if (!four_bytes) {
        return image_format;
    }
    switch (pixel_format) {
    case 0x80000000: // A8R8G8B8 sRGB
        return VK_FORMAT_B8G8R8A8_SRGB;
    case 0x80002200: // A8B8G8R8 sRGB
        return VK_FORMAT_R8G8B8A8_SRGB;
    case 0x88060000: // A2R10G10B10
    case 0x88000000: // A2R10G10B10 sRGB
    case 0x88740000: // A2R10G10B10 BT.2020 PQ
        return VK_FORMAT_A2R10G10B10_UNORM_PACK32;
    default:
        return image_format;
    }
}

} // namespace

std::string_view SkipName(Skip s) {
    static constexpr std::string_view kNames[] = {"not ready",     "unsupported stage", "pipeline",      "buffer",
                                                  "shader DMA",    "image",             "vertex format", "target",
                                                  "filtered mode", "meta clear",        "stream ring"};
    return static_cast<size_t>(s) < std::size(kNames) ? kNames[static_cast<size_t>(s)] : "?";
}

Executor::Executor(DeviceContext& ctx, Timeline& timeline, StreamRing& ring, GuestBuffers& guest, BufferCache& buffers,
                   ImageCache& images, PipelineFactory& pipelines, GS::ShaderFrontend& frontend, Presenter* presenter,
                   OwnedBuffer& gds, const ExecutorOptions& options)
    : ctx_(ctx), timeline_(timeline), ring_(ring), guest_(guest), buffers_(buffers), images_(images), pipelines_(pipelines),
      frontend_(frontend), presenter_(presenter), gds_(gds), options_(options) {
    images_.SetBeforeUpload([](void* self) { static_cast<Executor*>(self)->BeforeUpload(); }, this);
    buffers_.SetBeforeUpload([](void* self) { static_cast<Executor*>(self)->BeforeUpload(); }, this);
    char prof[4] = {};
    prof_ = GetEnvironmentVariableA("SSPC_GPU_PROFILE", prof, sizeof(prof)) == 1 && prof[0] == '1' &&
            ctx_.Caps().timestamp_period > 0.0f;
    if (prof_) {
        const VkQueryPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = kProfBatches * kProfPerBatch};
        if (Fn().vkCreateQueryPool(ctx_.Device(), &pi, nullptr, &prof_pool_) != VK_SUCCESS) {
            prof_ = false;
        } else {
            SSPC_LOG(GPU, Warning, "SSPC_GPU_PROFILE=1: GPU time per command (a timestamp after every command; slower)");
        }
    }
    if (ctx_.Caps().timestamp_period > 0.0f) {
        const VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                       .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                       .queryCount = 2 * kQuerySlots};
        if (Fn().vkCreateQueryPool(ctx_.Device(), &qi, nullptr, &queries_) != VK_SUCCESS) {
            queries_ = VK_NULL_HANDLE;
        }
    }
}

Executor::~Executor() {
    const auto& f = Fn();
    const VkDevice dev = ctx_.Device();
    if (queries_ != VK_NULL_HANDLE) {
        f.vkDestroyQueryPool(dev, queries_, nullptr);
    }
    if (prof_pool_ != VK_NULL_HANDLE) {
        f.vkDestroyQueryPool(dev, prof_pool_, nullptr);
    }
    DestroyBuffer(ctx_, lds_);
    DestroyBuffer(ctx_, diag_scratch_);
    for (auto& [value, b] : lds_retired_) {
        DestroyBuffer(ctx_, b);
    }
    const auto destroy = [&](Batch& b) {
        if (b.descriptors != VK_NULL_HANDLE) {
            f.vkDestroyDescriptorPool(dev, b.descriptors, nullptr);
        }
        if (b.pool != VK_NULL_HANDLE) {
            f.vkDestroyCommandPool(dev, b.pool, nullptr);
        }
    };
    if (open_) {
        f.vkEndCommandBuffer(cur_.cb);
        destroy(cur_);
    }
    for (auto& b : in_flight_) {
        destroy(b);
    }
    for (auto& b : free_) {
        destroy(b);
    }
}

void Executor::SetQueuePipe(u32 queue, u32 pipe) {
    if (queue < Cp::kNumQueues) {
        queue_pipe_[queue] = pipe;
    }
}

void Executor::Skipped(Skip why, const std::string& detail) {
    const auto i = static_cast<size_t>(why);
    if (stats_.skipped[i]++ == 0) {
        stats_.first_skip[i] = detail;
        SSPC_LOG(GPU, Warning, "native backend: first draw/dispatch not executed ({}): {}", SkipName(why), detail);
    }
}

bool Executor::Begin() {
    if (device_lost_) {
        return false; // nothing more reaches the GPU
    }
    if (open_) {
        return true;
    }
    const auto& f = Fn();
    if (!free_.empty()) {
        cur_ = std::move(free_.back());
        free_.pop_back();
        f.vkResetCommandPool(ctx_.Device(), cur_.pool, 0);
        if (cur_.descriptors != VK_NULL_HANDLE) {
            f.vkResetDescriptorPool(ctx_.Device(), cur_.descriptors, 0);
        }
    } else {
        cur_ = Batch{};
        const VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                          .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                          .queueFamilyIndex = ctx_.QueueFamily()};
        if (f.vkCreateCommandPool(ctx_.Device(), &pci, nullptr, &cur_.pool) != VK_SUCCESS) {
            return false;
        }
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = cur_.pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
        f.vkAllocateCommandBuffers(ctx_.Device(), &ai, &cur_.cb);
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192},
                                              {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},
                                              {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024},
                                              {VK_DESCRIPTOR_TYPE_SAMPLER, 4096}};
        const VkDescriptorPoolCreateInfo dci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .maxSets = 2048,
                                             .poolSizeCount = 4,
                                             .pPoolSizes = sizes};
        f.vkCreateDescriptorPool(ctx_.Device(), &dci, nullptr, &cur_.descriptors);
    }
    cur_.work.fill(0);
    cur_.queues.clear();
    cur_.value = timeline_.Next();
    g_diag_submission.store(cur_.value, std::memory_order_relaxed);
    batch_first_cmd_ = batch_last_cmd_ = 0;
    if (diag_ != nullptr && diag_->Enabled()) {
        diag_->Name(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<u64>(cur_.cb), fmt::format("batch {}", cur_.value));
    }
    const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    f.vkBeginCommandBuffer(cur_.cb, &bi);
    if (queries_ != VK_NULL_HANDLE) {
        cur_.query_slot = next_query_slot_++ % kQuerySlots;
        f.vkCmdResetQueryPool(cur_.cb, queries_, cur_.query_slot * 2, 2);
        f.vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queries_, cur_.query_slot * 2);
    }
    cur_.prof_base = ~0u;
    cur_.prof_count = 0;
    cur_.prof_keys.clear();
    if (prof_) {
        cur_.prof_base = (prof_next_batch_++ % kProfBatches) * kProfPerBatch;
        f.vkCmdResetQueryPool(cur_.cb, prof_pool_, cur_.prof_base, kProfPerBatch);
        f.vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_pool_, cur_.prof_base);
        cur_.prof_count = 1;
    }
    open_ = true;
    commands_ = 0;
    rendering_ = false;
    // a new command buffer: no dynamic state set yet - also what a draw sets only conditionally (the depth bias values
    // with bias on, the colour write masks with colour targets): not "already set" from the last buffer (validation f3)
    dyn_.valid = false;
    dyn_.bias_set = false;
    dyn_.color_masks_count = ~0u;
    dyn_vertex_valid_ = false;
    ++stats_.batches;
    return true;
}

void Executor::Note(u32 queue, u64 seq) {
    if (queue >= Cp::kNumQueues) {
        return;
    }
    if (cur_.work[queue] == 0) {
        cur_.queues.push_back(queue);
    }
    cur_.work[queue] = std::max(cur_.work[queue], seq);
}

void Executor::EndRendering() {
    if (rendering_) {
        Fn().vkCmdEndRendering(cur_.cb);
        rendering_ = false;
        render_ = RenderKey{};
    }
}

void Executor::GlobalBarrier() {
    EndRendering();
    const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    Fn().vkCmdPipelineBarrier2(cur_.cb, &dep);
    pending_writes_ = false;
    ++stats_.global_barriers;
}

void Executor::Flush() {
    want_flush_ = false;
    write_run_ = false;
    run_ranges_.clear();
    if (!open_) {
        return;
    }
    const auto& f = Fn();
    if (device_lost_) { // recorded before the loss was seen: discarded
        f.vkEndCommandBuffer(cur_.cb);
        free_.push_back(std::move(cur_));
        cur_ = Batch{};
        open_ = false;
        commands_ = 0;
        rendering_ = false;
        return;
    }
    EndRendering();
    // the GPU's writes of guest buffers (video memory) copied back to the host before the batch's fences
    if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    buffers_.RecordWriteBack(cur_.cb, cur_.value);
    if (prof_) {
        ProfMark(5, "batch end: write-backs of the GPU's writes to the host");
    }
    // the batch's writes made available to the host (guest memory the CPU reads after the fence)
    const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                              .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                              .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                              .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_MEMORY_READ_BIT |
                                               VK_ACCESS_2_MEMORY_WRITE_BIT};
    const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                               .pMemoryBarriers = &mb};
    f.vkCmdPipelineBarrier2(cur_.cb, &dep);
    pending_writes_ = false;
    if (cur_.query_slot != ~0u) {
        f.vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, queries_, cur_.query_slot * 2 + 1);
    }
    f.vkEndCommandBuffer(cur_.cb);
    const VkCommandBufferSubmitInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cur_.cb};
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                       .semaphore = timeline_.Handle(),
                                       .value = cur_.value,
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    g_diag_submission.store(cur_.value, std::memory_order_relaxed);
    VkResult r;
    const u64 submit_t0 = GS::Ticks();
    {
        std::scoped_lock lock{ctx_.QueueMutex()};
        // the video memory the batch's commands need bound first (the binds signal their own timeline)
        VkSemaphore bind_sem = VK_NULL_HANDLE;
        u64 bind_value = 0;
        buffers_.SubmitBinds(bind_sem, bind_value);
        const VkSemaphoreSubmitInfo wait{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                         .semaphore = bind_sem,
                                         .value = bind_value,
                                         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        const VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                               .waitSemaphoreInfoCount = bind_sem != VK_NULL_HANDLE ? 1u : 0u,
                               .pWaitSemaphoreInfos = &wait,
                               .commandBufferInfoCount = 1,
                               .pCommandBufferInfos = &cbi,
                               .signalSemaphoreInfoCount = 1,
                               .pSignalSemaphoreInfos = &signal};
        r = f.vkQueueSubmit2(ctx_.Queue(), 1, &si, VK_NULL_HANDLE);
    }
    stats_.submit_ns += GS::TicksToNs(GS::Ticks() - submit_t0);
    if (diag_ != nullptr) {
        diag_->Submitted(cur_.value, batch_first_cmd_, batch_last_cmd_);
    }
    if (r == VK_ERROR_DEVICE_LOST) {
        const u64 value = cur_.value;
        in_flight_.push_back(std::move(cur_));
        cur_ = Batch{};
        open_ = false;
        commands_ = 0;
        OnDeviceLost("vkQueueSubmit2", value);
        return;
    }
    if (r != VK_SUCCESS) {
        SSPC_LOG(GPU, Error, "native backend: vkQueueSubmit2 failed ({})", static_cast<int>(r));
    }
    in_flight_.push_back(std::move(cur_));
    cur_ = Batch{};
    open_ = false;
    commands_ = 0;
}

void Executor::Poll() {
    const u64 completed = timeline_.Completed();
    if (timeline_.Lost()) {
        OnDeviceLost("vkGetSemaphoreCounterValue", completed + 1);
        return;
    }
    while (!in_flight_.empty() && in_flight_.front().value <= completed) {
        Batch b = std::move(in_flight_.front());
        in_flight_.pop_front();
        if (cp_ != nullptr) {
            for (const u32 q : b.queues) {
                cp_->CompleteWork(q, b.work[q]);
            }
        }
        ++stats_.completed_batches;
        if (b.query_slot != ~0u) {
            u64 ts[2] = {};
            if (Fn().vkGetQueryPoolResults(ctx_.Device(), queries_, b.query_slot * 2, 2, sizeof(ts), ts, 8,
                                           VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
                ts[1] >= ts[0]) {
                stats_.gpu_busy_ns += static_cast<u64>(static_cast<double>(ts[1] - ts[0]) * ctx_.Caps().timestamp_period);
                ++stats_.gpu_batches_timed;
            }
            b.query_slot = ~0u;
        }
        if (b.prof_base != ~0u) {
            ProfCollect(b);
        }
        free_.push_back(std::move(b));
    }
    ring_.Collect(completed);
    if (shadow_ring_ != nullptr) {
        shadow_ring_->Collect(completed);
    }
    guest_.Collect(completed);
    images_.Collect(completed);
    buffers_.Collect(completed);
    std::erase_if(lds_retired_, [&](auto& e) {
        if (e.first > completed) {
            return false;
        }
        DestroyBuffer(ctx_, e.second);
        return true;
    });
    if (!timeline_writes_.empty()) {
        std::erase_if(timeline_writes_, [&](const TimelineWrite& w) { return w.value <= completed; });
        tw_lo_ = ~0ull;
        tw_hi_ = 0;
        for (const auto& w : timeline_writes_) {
            tw_lo_ = std::min(tw_lo_, w.begin);
            tw_hi_ = std::max(tw_hi_, w.end);
        }
    }
}

std::string DescribeImage(const CachedImage& img) {
    const ImageLayout& l = img.layout;
    return fmt::format("img{}@{:#x}+{:#x} fmt {} {}x{}x{} lv {} ly {} s {} tile {} bits {}{}{}{}", img.id,
                       l.guest_address, l.guest_size, l.pixel_format, l.size.width, l.size.height, l.size.depth,
                       l.levels, l.layers, l.num_samples, static_cast<u32>(l.tile_mode), l.num_bits,
                       l.props.is_depth ? " depth" : "", img.gpu_written ? " gpu-written" : "",
                       img.alias_hits != 0 ? " ALIASED" : "");
}

void Executor::OnDeviceLost(const char* where, u64 value) {
    if (device_lost_) {
        return;
    }
    device_lost_ = true;
    std::string in_flight;
    for (const auto& b : in_flight_) {
        in_flight += fmt::format("  value {}: queues", b.value);
        for (const u32 q : b.queues) {
            in_flight += fmt::format(" {} (work {})", q, b.work[q]);
        }
        in_flight += "\n";
    }
    in_flight += fmt::format("  timeline: completed (last seen) {}, last submitted {}\n", timeline_.CompletedCached(),
                             timeline_.LastSubmitted());
    if (diag_ != nullptr) {
        diag_->DeviceLost(where, value, in_flight);
    } else {
        SSPC_LOG(GPU, Error, "native backend: VK_ERROR_DEVICE_LOST ({}, submission {})", where, value);
    }
}

void Executor::ProfMark(u64 key, const std::string& label) {
    if (!prof_ || !open_ || cur_.prof_base == ~0u || cur_.prof_count >= kProfPerBatch) {
        return;
    }
    Fn().vkCmdWriteTimestamp2(cur_.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_pool_, cur_.prof_base + cur_.prof_count);
    ++cur_.prof_count;
    cur_.prof_keys.push_back(key);
    if (auto& acc = prof_acc_[key]; acc.label.empty()) {
        acc.label = label;
    }
}

void Executor::ProfCollect(Batch& b) {
    std::vector<u64> ts(b.prof_count);
    if (b.prof_count > 1 &&
        Fn().vkGetQueryPoolResults(ctx_.Device(), prof_pool_, b.prof_base, b.prof_count, ts.size() * 8, ts.data(), 8,
                                   VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const double period = ctx_.Caps().timestamp_period;
        for (u32 i = 1; i < b.prof_count; ++i) {
            const u64 dt = ts[i] >= ts[i - 1] ? static_cast<u64>(static_cast<double>(ts[i] - ts[i - 1]) * period) : 0;
            auto& acc = prof_acc_[b.prof_keys[i - 1]];
            acc.ns += dt;
            ++acc.count;
            prof_total_ns_ += dt;
        }
    }
    b.prof_base = ~0u;
    b.prof_count = 0;
    b.prof_keys.clear();
}

void Executor::PrepareShadows(const void* plan_ptr, std::vector<VkDescriptorBufferInfo>& infos) {
    // Measured (logs\p44\diag c17-c20): a shader's stores into the sparse mirror are ~150x slower than into a dense
    // buffer on NVIDIA (the skinning dispatch 1196 us vs 7.7 us with only its outputs dense), its loads are not; a copy
    // into the mirror costs 12 us for its 420 KiB. So a dispatch writes dense shadows, copied back after it.
    static const bool off = [] {
        char v[8] = {};
        const bool o = GetEnvironmentVariableA("SSPC_DIAG_WRITE_SHADOW", v, sizeof(v)) == 3 && std::string_view(v) == "off";
        if (o) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_WRITE_SHADOW=off - dispatches write the mirror in place");
        }
        return o;
    }();
    shadows_.clear();
    if (shadow_ring_ == nullptr || off) {
        return;
    }
    const auto& plan = *static_cast<const GS::ResourceBindingPlan*>(plan_ptr);
    const u64 align = std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4);
    // the whole binding is copied in and out: a large one a shader writes a little of costs more than it saves.
    // Measured on the reference route (d1-d3): no limit 33.9 ms GPU a frame (149 MiB copied each way a frame), 1 MiB
    // 35.6 ms (the 1x1x1 dispatches' 1 MiB bindings copied), 256 KiB 29.6 ms (2 MiB a frame) - written bindings are
    // <=64 KiB, ~140 KiB (the skinning), ~1 MiB or ~16 MiB, nothing between.
    static const u64 kShadowMax = [] {
        char v[16] = {};
        return GetEnvironmentVariableA("SSPC_DIAG_SHADOW_MAX_KIB", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 10) << 10
                                                                                     : (256ull << 10);
    }();
    const size_t n = std::min(plan.buffers.size(), infos.size());
    for (size_t k = 0; k < n; ++k) {
        const auto& b = plan.buffers[k];
        VkDescriptorBufferInfo& info = infos[k];
        if (b.kind != GS::BufferKind::Guest || !b.written || b.size == 0 || info.buffer == VK_NULL_HANDLE ||
            info.range == VK_WHOLE_SIZE || info.range == 0) {
            continue;
        }
        ++stats_.shadow_sizes[info.range <= (64u << 10)    ? 0
                              : info.range <= (256u << 10) ? 1
                              : info.range <= (1u << 20)   ? 2
                              : info.range <= (4u << 20)   ? 3
                              : info.range <= (16u << 20)  ? 4
                                                           : 5];
        if (info.range > kShadowMax) {
            ++stats_.shadow_too_large;
            continue;
        }
        // another binding of the same memory would see the other copy: those stay in place
        bool overlap = false;
        for (size_t j = 0; j < plan.buffers.size() && !overlap; ++j) {
            const auto& o = plan.buffers[j];
            overlap = j != k && o.kind == GS::BufferKind::Guest && o.size != 0 && o.address < b.address + b.size &&
                      b.address < o.address + o.size;
        }
        if (overlap) {
            ++stats_.shadow_overlaps;
            continue;
        }
        StreamRing::Slice sl{};
        if (shadow_ring_->Allocate(info.range, align, cur_.value, sl) != StreamRing::Status::Ok) {
            ++stats_.shadow_no_room;
            continue;
        }
        // only the binding's own bytes are copied: the descriptor starts up to `align` - 1 bytes before them (aligned
        // down), bytes that may be another binding's - adjacent outputs (the skinning's three) copied back whole would
        // put their stale copy over the other's result (synchronization validation d4/d6: WRITE_AFTER_WRITE)
        const u64 adjust = k < buf_adjust_.size() ? buf_adjust_[k] : 0;
        if (adjust >= info.range) {
            continue;
        }
        shadows_.push_back({info.buffer, sl.buffer, info.offset + adjust, sl.offset + adjust,
                            std::min<u64>(b.size, info.range - adjust)});
        info.buffer = sl.buffer;
        info.offset = sl.offset; // aligned as the original: the shader's offset within it is the same
    }
    if (shadows_.empty()) {
        return;
    }
    // the current contents in (the shader may read what it does not overwrite); the caller ordered the earlier writes
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier();
    }
    for (const Shadow& s : shadows_) {
        const VkBufferCopy c{.srcOffset = s.home_offset, .dstOffset = s.shadow_offset, .size = s.size};
        Fn().vkCmdCopyBuffer(cur_.cb, s.home, s.shadow, 1, &c);
        ++stats_.shadows;
        stats_.shadow_bytes += s.size;
    }
    GlobalBarrier();
}

void Executor::FinishShadows() {
    if (shadows_.empty()) {
        return;
    }
    GlobalBarrier(); // the dispatch's writes before the copies
    for (const Shadow& s : shadows_) {
        const VkBufferCopy c{.srcOffset = s.shadow_offset, .dstOffset = s.home_offset, .size = s.size};
        Fn().vkCmdCopyBuffer(cur_.cb, s.shadow, s.home, 1, &c);
    }
    shadows_.clear();
    pending_writes_ = true; // a later access of the memory waits for the copies
}

void Executor::DiagDumpShader(u64 address, const std::vector<u32>* code, const std::vector<u32>& spirv) {
    // SSPC_DIAG_DUMP_SHADER=<hex address>: that program's GCN code and SPIR-V -> logs\p44\shader_<address>.{gcn,spv}
    static const u64 wanted = [] {
        char v[32] = {};
        return GetEnvironmentVariableA("SSPC_DIAG_DUMP_SHADER", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 16) : 0ull;
    }();
    if (wanted == 0 || address != wanted || dumped_shader_) {
        return;
    }
    dumped_shader_ = true;
    const auto write = [](const std::filesystem::path& path, const void* data, size_t bytes) {
        std::FILE* fp = _wfopen(path.c_str(), L"wb");
        if (fp == nullptr) {
            return false;
        }
        const bool ok = std::fwrite(data, 1, bytes, fp) == bytes;
        std::fclose(fp);
        return ok;
    };
    // next to the frame capture (logs\p44)
    const std::filesystem::path base = options_.capture_path.parent_path() / fmt::format("shader_{:x}", address);
    bool ok = true;
    if (code != nullptr) {
        ok &= write(std::filesystem::path(base).replace_extension(".gcn"), code->data(), code->size() * 4);
    }
    ok &= write(std::filesystem::path(base).replace_extension(".spv"), spirv.data(), spirv.size() * 4);
    SSPC_LOG(GPU, Warning, "DIAGNOSTIC: shader {:#x} dumped to {}.gcn/.spv ({} GCN words, {} SPIR-V words): {}", address,
             base.string(), code != nullptr ? code->size() : 0, spirv.size(), ok ? "written" : "NOT written");
}

void Executor::ProfDescribe(u64 key, const char* what, const void* stage_view, const std::vector<CachedImage*>& bound) {
    if (!prof_described_.insert(key).second) {
        return;
    }
    const auto& s = *static_cast<const GS::StageView*>(stage_view);
    std::string out = fmt::format("profile key {:#x} ({}) bindings:", key, what);
    for (const auto& b : s.plan.buffers) {
        if (b.kind == GS::BufferKind::Guest) {
            // where the GPU reads it: video memory (the cache's mirror), the stream ring or the host import (PCIe)
            BufferCache::Range r{};
            const BufferCache::Residency res = buffers_.Classify(b.address, b.size, r);
            const char* where = res == BufferCache::Residency::All ? "vram"
                                : !b.written && b.size <= 16 * 1024 ? "ring"
                                                                     : (res == BufferCache::Residency::Partial ? "partial"
                                                                                                                : "HOST");
            out += fmt::format(" | buf {:#x}+{:#x}{} [{}]", b.address, b.size, b.written ? " WRITTEN" : "", where);
        } else {
            out += fmt::format(" | special kind {}", static_cast<int>(b.kind));
        }
    }
    for (const CachedImage* img : bound) {
        out += fmt::format(" | img{} {:#x} fmt {} {}x{} lv {} ly {}{}{}", img->id, img->layout.guest_address,
                           img->layout.pixel_format, img->layout.size.width, img->layout.size.height, img->layout.levels,
                           img->layout.layers, img->gpu_written ? " gpu-written" : "",
                           (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) ? " storage-usage" : "");
    }
    for (const auto& v : s.plan.vertices) {
        out += fmt::format(" | vertex {:#x}+{:#x} stride {}", v.address, v.size, v.desc.Stride());
    }
    SSPC_LOG(GPU, Info, "{}", out);
}

void Executor::ProfReport() {
    if (prof_frames_ == 0 || prof_total_ns_ == 0) {
        return;
    }
    std::vector<std::pair<u64, const ProfAcc*>> v;
    for (const auto& [k, a] : prof_acc_) {
        if (a.ns != 0) {
            v.push_back({k, &a});
        }
    }
    std::ranges::sort(v, [](const auto& x, const auto& y) { return x.second->ns > y.second->ns; });
    std::string out = fmt::format("GPU profile over {} frames: {:.2f} ms GPU per frame in commands; the top:\n", prof_frames_,
                                  static_cast<double>(prof_total_ns_) / 1e6 / static_cast<double>(prof_frames_));
    for (size_t i = 0; i < v.size() && i < 30; ++i) {
        const ProfAcc& a = *v[i].second;
        out += fmt::format("  {:5.2f} ms/frame {:4.1f}% x{:6} ({:7.1f} us each)  {}\n",
                           static_cast<double>(a.ns) / 1e6 / static_cast<double>(prof_frames_),
                           100.0 * static_cast<double>(a.ns) / static_cast<double>(prof_total_ns_), a.count,
                           static_cast<double>(a.ns) / 1e3 / static_cast<double>(std::max<u64>(a.count, 1)), a.label);
    }
    // the diagnostic experiments' intervals whatever their rank (SSPC_DIAG_DUP_DISPATCH)
    for (u64 k = 6; k <= 13; ++k) {
        if (const auto it = prof_acc_.find(DiagKey(k)); it != prof_acc_.end()) {
            const ProfAcc& a = it->second;
            out += fmt::format("  (diag) {:5.3f} ms/frame x{:6} ({:7.1f} us each)  {}\n",
                               static_cast<double>(a.ns) / 1e6 / static_cast<double>(prof_frames_), a.count,
                               static_cast<double>(a.ns) / 1e3 / static_cast<double>(std::max<u64>(a.count, 1)), a.label);
        }
    }
    SSPC_LOG(GPU, Info, "{}", out);
    prof_acc_.clear();
    prof_frames_ = 0;
    prof_total_ns_ = 0;
}

u64 Executor::DiagBegin(std::string text) {
    if (diag_ == nullptr || !diag_->Enabled()) {
        return 0;
    }
    const u64 id = diag_->Begin(cur_.cb, cur_.value, std::move(text));
    if (batch_first_cmd_ == 0) {
        batch_first_cmd_ = id;
    }
    batch_last_cmd_ = id;
    return id;
}

void Executor::DiagEnd(u64 id) {
    if (id != 0) {
        diag_->End(cur_.cb, id);
    }
}

bool Executor::GuestRange(u64 addr, u64 size, bool write, u64 align, VkBuffer& buffer, u64& offset, u64& avail) {
    const u64 t0 = GS::Ticks();
    const bool ok = GuestRangeImpl(addr, size, write, align, buffer, offset, avail);
    stats_.t_range += GS::Ticks() - t0;
    return ok;
}

bool Executor::GuestRangeImpl(u64 addr, u64 size, bool write, u64 align, VkBuffer& buffer, u64& offset, u64& avail) {
    constexpr u64 kStreamMax = 16 * 1024; // the bridge's STREAM_THRESHOLD (16 KiB)
    if (!write && size <= kStreamMax) {
        BufferCache::Range r;
        u64 tt = GS::Ticks();
        const BufferCache::Residency res = buffers_.Classify(addr, size, r);
        stats_.t_classify += GS::Ticks() - tt;
        if (res == BufferCache::Residency::All) {
            buffer = r.buffer;
            offset = r.offset;
            avail = r.size;
            return true;
        }
        // nothing of it in video memory (the CPU wrote it, or it is new): the host's copy is current; a small read
        // is copied by the CPU into the stream ring (video memory) - no upload, no barrier, no break of rendering (the
        // bridge's stream buffer). The copy reads the physical backing's view (never faults).
        if (res == BufferCache::Residency::None && !buffers_.HostWritePending(addr, size) &&
            !TimelineWritePending(addr, size)) {
            StreamRing::Slice sl{};
            tt = GS::Ticks();
            const u8* host = guest_.BackingPointer(addr, size);
            if (host == nullptr) { // no window there yet: imported once (32 MiB), then the copies read its host view
                GuestBuffers::View gv{};
                GuestBuffers::Failure gfl{};
                if (guest_.Get(addr, size, cur_.value, gv, gfl)) {
                    host = guest_.BackingPointer(addr, size);
                }
            }
            stats_.t_hostptr += GS::Ticks() - tt;
            tt = GS::Ticks();
            const bool allocated = ring_.Allocate(size, std::max<u64>(align, 4), cur_.value, sl) == StreamRing::Status::Ok;
            stats_.t_alloc += GS::Ticks() - tt;
            tt = GS::Ticks();
            const bool copied =
                allocated &&
                (host != nullptr ? (std::memcpy(sl.data, host, size), true) : GS::GameMemory().Read(addr, sl.data, size));
            stats_.t_copy += GS::Ticks() - tt;
            if (copied) {
                buffer = sl.buffer;
                offset = sl.offset;
                avail = size;
                ++stats_.streamed;
                stats_.streamed_bytes += size;
                return true;
            }
        }
    }
    BufferCache::Range r;
    const u64 tp = GS::Ticks();
    const bool prepared = buffers_.Prepare(addr, size, write, cur_.cb, cur_.value, r);
    stats_.t_prepare += GS::Ticks() - tp;
    ++stats_.n_prepare;
    if (!prepared) {
        return false;
    }
    buffer = r.buffer;
    offset = r.offset;
    avail = r.size;
    return true;
}

bool Executor::TimelineWritePending(u64 addr, u64 bytes) const {
    if (timeline_writes_.empty() || addr >= tw_hi_ || addr + bytes <= tw_lo_) {
        return false;
    }
    return std::ranges::any_of(timeline_writes_,
                               [&](const TimelineWrite& w) { return addr < w.end && w.begin < addr + bytes; });
}

bool Executor::TimelineReader::Read(u64 addr, void* dst, u64 bytes) {
    if (e_.TimelineWritePending(addr, bytes)) {
        ++e_.stats_.timeline_read_syncs;
        e_.SyncFrom(1);
    } else if (e_.buffers_.GpuWritePending(addr, bytes, e_.timeline_.CompletedCached()) ||
               e_.buffers_.HostWritePending(addr, bytes)) {
        // a shader's write the host does not have yet (its batch has not completed): the frontend reads after it
        ++e_.stats_.gpu_write_read_syncs;
        e_.SyncFrom(1);
    }
    return GS::GameMemory().Read(addr, dst, bytes);
}

bool Executor::WriteOnTimeline(u32 queue, u64 seq, u64 dst, const void* data, u64 bytes) {
    // vkCmdUpdateBuffer: 4-byte aligned, up to 64 KiB; guest memory only (VideoOut's labels are host memory)
    if (bytes == 0 || bytes > 65536 || (dst & 3) != 0 || (bytes & 3) != 0 || dst >= (1ull << 40)) {
        ++stats_.tw_fail_shape;
        return false;
    }
    if (!Begin()) {
        return false;
    }
    GuestBuffers::View v{};
    GuestBuffers::Failure fl{};
    if (!guest_.Get(dst, bytes, cur_.value, v, fl)) {
        ++stats_.tw_fail_import;
        return false;
    }
    Note(queue, seq);
    ++commands_;
    // after the earlier work's accesses of this memory, before the later work's: one barrier for a run of writes to
    // different ranges (the next draw's barrier follows through pending_writes_)
    const bool overlap = std::ranges::any_of(run_ranges_, [&](const std::pair<u64, u64>& r) {
        return dst < r.second && r.first < dst + bytes;
    });
    if (!write_run_ || overlap) {
        GlobalBarrier();
        run_ranges_.clear();
    }
    const u64 cid = diag_ != nullptr && diag_->Enabled()
                        ? DiagBegin(fmt::format("timeline write q{} seq {} {:#x}+{}", queue, seq, dst, bytes))
                        : 0;
    Fn().vkCmdUpdateBuffer(cur_.cb, v.buffer, v.offset, bytes, data);
    DiagEnd(cid);
    buffers_.Invalidate(dst, bytes); // the host's copy is newer: uploaded again (after this write) at the next use
    pending_writes_ = true;
    write_run_ = true;
    run_ranges_.emplace_back(dst, dst + bytes);
    timeline_writes_.push_back({dst, dst + bytes, cur_.value});
    tw_lo_ = std::min(tw_lo_, dst);
    tw_hi_ = std::max(tw_hi_, dst + bytes);
    images_.Invalidate(dst, bytes); // textures there are uploaded again (after the write, in stream order)
    ++stats_.timeline_writes;
    return true;
}

bool Executor::CopyOnTimeline(u32 queue, u64 seq, u64 src, u64 dst, u64 bytes) {
    // guest memory only (VideoOut's labels are host memory); overlapping ranges are a memmove the CPU does
    if (bytes == 0 || src >= (1ull << 40) || dst >= (1ull << 40) || (src < dst + bytes && dst < src + bytes)) {
        ++stats_.tc_refused;
        return false;
    }
    if (!Begin()) {
        return false;
    }
    Note(queue, seq);
    ++commands_;
    write_run_ = false;
    // a copy: outside rendering, after the earlier writes (of its source too)
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier();
    }
    VkBuffer sbuf = VK_NULL_HANDLE, dbuf = VK_NULL_HANDLE;
    u64 soff = 0, doff = 0, savail = 0, davail = 0;
    if (!GuestRange(src, bytes, false, 4, sbuf, soff, savail) || !GuestRange(dst, bytes, true, 4, dbuf, doff, davail)) {
        ++stats_.tc_refused;
        return false; // nothing recorded yet: the processor copies (after Synchronize)
    }
    // the ranges' uploads (if any) ended rendering and ordered themselves; the copy after them
    EndRendering();
    const u64 cid = diag_ != nullptr && diag_->Enabled()
                        ? DiagBegin(fmt::format("timeline copy q{} seq {} {:#x} -> {:#x} ({} bytes)", queue, seq, src, dst,
                                                bytes))
                        : 0;
    if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    const VkBufferCopy region{.srcOffset = soff, .dstOffset = doff, .size = bytes};
    Fn().vkCmdCopyBuffer(cur_.cb, sbuf, dbuf, 1, &region);
    DiagEnd(cid);
    if (prof_) {
        ProfMark(4, "DMA copy on the timeline");
    }
    pending_writes_ = true; // a later read of the destination waits for the copy
    // the CPU side (the processor's reads, the frontend's descriptor reads) waits for it
    timeline_writes_.push_back({dst, dst + bytes, cur_.value});
    tw_lo_ = std::min(tw_lo_, dst);
    tw_hi_ = std::max(tw_hi_, dst + bytes);
    images_.Invalidate(dst, bytes); // textures there are uploaded again (they read the video memory copy, after it)
    ++stats_.timeline_copies;
    stats_.timeline_copy_bytes += bytes;
    return true;
}

void Executor::BeforeUpload() {
    // an upload records copies (and a detiling dispatch): outside rendering, after the writes before it
    if (rendering_) {
        ++stats_.uploads_ended_rendering;
    }
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier();
    }
}

bool Executor::WaitOldest(u64 timeout_ns) {
    if (in_flight_.empty() || device_lost_) {
        return false;
    }
    const bool done = timeline_.Wait(in_flight_.front().value, timeout_ns);
    if (!done && timeline_.Lost()) {
        OnDeviceLost("vkWaitSemaphores (oldest batch)", in_flight_.front().value);
    }
    return done;
}

void Executor::Synchronize(u32) {
    SyncFrom(0);
}

void Executor::SyncFrom(u32 source) {
    // the CPU side of a packet must land after the GPU work before it: complete everything handed on so far
    ++stats_.syncs;
    if (device_lost_) {
        return;
    }
    if (!open_ && in_flight_.empty()) {
        return;
    }
    ++stats_.sync_eff[source % 3];
    const u64 t0 = GS::Ticks();
    if (open_ && commands_ > 0) {
        ++stats_.flushes_sync;
        Flush();
    }
    if (!in_flight_.empty() && !timeline_.Wait(in_flight_.back().value, 5'000'000'000ull) && timeline_.Lost()) {
        OnDeviceLost("vkWaitSemaphores (Synchronize)", in_flight_.back().value);
    }
    const u64 us = GS::TicksToNs(GS::Ticks() - t0) / 1000;
    stats_.sync_wait_us += us;
    stats_.sync_eff_us[source % 3] += us;
}

void Executor::Fence(const Cp::FenceOp& op) {
    if (op.deferred && !op.written) {
        want_flush_ = true; // the processor holds it until the GPU completes the work before it
    }
}

void Executor::Interrupt(u32 queue, Cp::FenceKind) {
    if (queue == Cp::kGraphicsQueue) {
        Gnm::RaiseInterrupt(Gnm::Interrupt::GfxEop);
    } else {
        Gnm::RaiseInterrupt(static_cast<Gnm::Interrupt>(std::min<u32>(queue_pipe_[queue], 6)));
    }
}

void Executor::Dma(const Cp::DmaOp& op) {
    ++stats_.dma;
    if (op.timeline) {
        return; // recorded by CopyOnTimeline: the copy keeps the caches coherent itself
    }
    if (op.executed && (op.kind == Cp::DmaKind::MemoryToMemory || op.kind == Cp::DmaKind::DataToMemory ||
                        op.kind == Cp::DmaKind::GdsToMemory)) {
        images_.Invalidate(op.dst, op.bytes); // the CPU side wrote guest memory images may have copies of
        buffers_.Invalidate(op.dst, op.bytes);
    }
}

void Executor::CacheAction(const Cp::CacheActionOp&) {
    // the guest's cache flushes/invalidations (EVENT_WRITE, ACQUIRE_MEM): no barrier of their own, as the bridge (it
    // ignores them too). The hazards they order are ordered here per resource: a buffer write -> a barrier before the
    // next command (pending_writes_), an image -> its tracked access, a CPU side effect -> Synchronize, the host's
    // reads -> the batch's last barrier. Synchronization validation checks this on the reference route.
    ++stats_.cache_actions;
    write_run_ = false;
}

bool Executor::PushDescriptors(const PipelineObjects& p, VkPipelineBindPoint bind, std::vector<VkWriteDescriptorSet>& w) {
    const auto& f = Fn();
    if (w.empty()) {
        return true;
    }
    if (p.push_descriptors) {
        f.vkCmdPushDescriptorSetKHR(cur_.cb, bind, p.layout, 0, static_cast<u32>(w.size()), w.data());
        return true;
    }
    const VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = cur_.descriptors,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &p.set_layout};
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (f.vkAllocateDescriptorSets(ctx_.Device(), &ai, &set) != VK_SUCCESS) {
        return false;
    }
    for (auto& x : w) {
        x.dstSet = set;
    }
    f.vkUpdateDescriptorSets(ctx_.Device(), static_cast<u32>(w.size()), w.data(), 0, nullptr);
    f.vkCmdBindDescriptorSets(cur_.cb, bind, p.layout, 0, 1, &set, 0, nullptr);
    ++stats_.descriptor_sets;
    return true;
}

bool Executor::BindStage(const void* stage_view, bool compute, PushData& push,
                         std::vector<VkWriteDescriptorSet>& writes, std::vector<VkDescriptorBufferInfo>& buffers,
                         std::vector<VkDescriptorImageInfo>& image_infos, std::vector<CachedImage*>& bound,
                         bool& writes_memory, const std::vector<CachedImage*>& targets) {
    const auto& s = *static_cast<const GS::StageView*>(stage_view);
    const GS::ResourceBindingPlan& plan = s.plan;
    const GS::ShaderReflection& refl = s.module->result.reflection;
    const u64 align = std::max<u64>(ctx_.Caps().min_storage_buffer_alignment, 4);
    const Cp::RegisterFile& regs = cp_->Registers();
    buf_adjust_.assign(plan.buffers.size(), 0);
    // buffers
    for (size_t i = 0; i < plan.buffers.size(); ++i) {
        const auto& b = plan.buffers[i];
        const u32 index = s.start.buffer + static_cast<u32>(i);
        VkDescriptorBufferInfo info{VK_NULL_HANDLE, 0, VK_WHOLE_SIZE};
        switch (b.kind) {
        case GS::BufferKind::Guest: {
            if (b.status != GS::ResourceStatus::Ok || b.size == 0 || b.address == 0) {
                break; // a null descriptor: robustness2 reads zeros (the bridge binds null for base 0 / size 0)
            }
            VkBuffer vb = VK_NULL_HANDLE;
            u64 voff = 0, vavail = 0;
            if (!GuestRange(b.address, b.size, b.written, align, vb, voff, vavail)) {
                Skipped(Skip::Buffer, fmt::format("buffer {} at {:#x} ({} bytes)", b.binding, b.address, b.size));
                return false;
            }
            const u64 aligned = voff & ~(align - 1);
            const u64 adjust = voff - aligned;
            if (index < push.buf_offsets.size()) {
                push.buf_offsets[index] = static_cast<u8>(adjust);
            }
            buf_adjust_[i] = static_cast<u32>(adjust);
            info = {vb, aligned, std::min<u64>(b.size + adjust, vavail + adjust)};
            if (b.written) {
                writes_memory = true;
                images_.Invalidate(b.address, b.size);
            }
            break;
        }
        case GS::BufferKind::Flatbuf: {
            StreamRing::Slice sl{};
            const u64 bytes = plan.flat.size() * 4;
            if (bytes == 0 || ring_.Allocate(bytes, align, cur_.value, sl) != StreamRing::Status::Ok) {
                Skipped(Skip::Ring, "flattened user data");
                return false;
            }
            std::memcpy(sl.data, plan.flat.data(), bytes);
            info = {sl.buffer, sl.offset, bytes};
            break;
        }
        case GS::BufferKind::ClipPlanes: {
            if ((regs.Get(kPaClClipCntl) & 0x3F) == 0) {
                break; // compiled without planes: never read
            }
            StreamRing::Slice sl{};
            if (ring_.Allocate(24 * 4, align, cur_.value, sl) != StreamRing::Status::Ok) {
                Skipped(Skip::Ring, "clip planes");
                return false;
            }
            for (u32 k = 0; k < 24; ++k) {
                const u32 v = regs.Get(kClipUserData + k);
                std::memcpy(sl.data + k * 4, &v, 4);
            }
            info = {sl.buffer, sl.offset, 24 * 4};
            break;
        }
        case GS::BufferKind::GdsBuffer:
            info = {gds_.buffer, 0, gds_.size};
            writes_memory |= b.written;
            break;
        case GS::BufferKind::SharedMemory: {
            // the translator's emulated LDS: each workgroup indexes its own part (the bridge: SharedMemSize() *
            // NumWorkgroups(), zeroed before the dispatch)
            const u64 per_group = std::max<u64>(refl.shared_memory_size, lds_group_bytes_);
            const u64 bytes = (std::max<u64>(per_group * (compute ? lds_groups_ : 1), 16) + 255) & ~255ull;
            const auto& f = Fn();
            if (lds_.size < bytes) {
                if (lds_.buffer != VK_NULL_HANDLE) {
                    lds_retired_.push_back({cur_.value, lds_});
                    lds_ = {};
                }
                u64 grown = 64ull << 20;
                while (grown < bytes) {
                    grown *= 2;
                }
                std::string err;
                if (!CreateBuffer(ctx_, grown, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                  false, lds_, err)) {
                    Skipped(Skip::Ring, fmt::format("emulated LDS buffer of {} bytes: {}", bytes, err));
                    return false;
                }
            }
            // earlier users of the buffer -> the zero fill -> this dispatch
            EndRendering();
            VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
            const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                       .pMemoryBarriers = &mb};
            f.vkCmdPipelineBarrier2(cur_.cb, &dep);
            f.vkCmdFillBuffer(cur_.cb, lds_.buffer, 0, bytes, 0);
            mb.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            mb.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            f.vkCmdPipelineBarrier2(cur_.cb, &dep);
            info = {lds_.buffer, 0, bytes};
            writes_memory = true;
            ++stats_.lds_dispatches;
            stats_.lds_max_bytes = std::max(stats_.lds_max_bytes, bytes);
            if (lds_logged_++ < 8) {
                SSPC_LOG(GPU, Info,
                         "native backend: emulated LDS: {} bytes ({} per workgroup x {} workgroups; translator {} bytes)",
                         bytes, per_group, compute ? lds_groups_ : 1, refl.shared_memory_size);
            }
            break;
        }
        default:
            Skipped(Skip::SpecialBuffer, "BDA page table / fault buffer (shader DMA)");
            return false;
        }
        buffers.push_back(info);
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstBinding = b.binding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                          .pBufferInfo = nullptr});
    }
    // images
    const VkPipelineStageFlags2 shader_stage = compute ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : kGraphicsShaders;
    for (const auto& im : plan.images) {
        const u32 count = std::max<u32>(im.count, 1);
        const size_t first = image_infos.size();
        bool null_image = im.status != GS::ResourceStatus::Ok;
        CachedImage* img = nullptr;
        if (!null_image) {
            const ImageLayout layout = LayoutFromTSharp(im.desc, im.depth);
            std::string err;
            const u64 ti0 = GS::Ticks();
            img = images_.Find(layout, im.storage ? ImageCache::Use::Storage : ImageCache::Use::Texture, cur_.cb,
                               cur_.value, err);
            stats_.t_image += GS::Ticks() - ti0;
            if (img == nullptr) {
                Skipped(Skip::Image, err);
                return false;
            }
        }
        for (u32 j = 0; j < count; ++j) {
            VkDescriptorImageInfo ii{VK_NULL_HANDLE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            if (img != nullptr) {
                ImageViewDesc vd = TSharpView(im.desc, im.depth, im.array, im.storage);
                if (!vd.supported) {
                    Skipped(Skip::Image, std::string(vd.reason));
                    return false;
                }
                if (count > 1) {
                    vd.base_level = static_cast<u16>(vd.base_level + j);
                    vd.levels = 1;
                }
                ii.imageView = images_.View(*img, vd, im.storage);
                const bool feedback = std::ranges::find(targets, img) != targets.end();
                ii.imageLayout = (im.storage || feedback) ? VK_IMAGE_LAYOUT_GENERAL
                                 : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                              : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            image_infos.push_back(ii);
            info_images_.push_back(img);
        }
        if (img != nullptr) {
            bound.push_back(img);
            if (im.storage) {
                writes_memory = true;
            }
        }
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstBinding = im.binding,
                          .descriptorCount = count,
                          .descriptorType = im.storage ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                       : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                          .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(first)}); // fixed up below
        (void)null_image;
        (void)shader_stage;
    }
    // samplers
    const u64 border_base = u64{regs.Get(kTaBcBase)} << 8;
    for (size_t i = 0; i < plan.samplers.size(); ++i) {
        const auto& sm = plan.samplers[i];
        const bool depth = i < refl.samplers.size() && refl.samplers[i].is_depth;
        GS::SamplerDesc desc = sm.desc;
        if (sm.status != GS::ResourceStatus::Ok) {
            desc = {};
        }
        VkSampler vs = images_.Sampler(desc, depth, border_base);
        if (vs == VK_NULL_HANDLE) {
            vs = images_.Sampler(GS::SamplerDesc{}, depth, 0); // the bridge's fallback: the default S#
        }
        const size_t first = image_infos.size();
        image_infos.push_back({vs, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL});
        info_images_.push_back(nullptr);
        writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                          .dstBinding = sm.binding,
                          .descriptorCount = 1,
                          .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                          .pImageInfo = reinterpret_cast<const VkDescriptorImageInfo*>(first)});
    }
    return true;
}

void Executor::SetDynamicState(const Cp::RegisterFile& r, const GS::GraphicsPipelineState& st, bool indexed) {
    const auto& f = Fn();
    const VkCommandBuffer cb = cur_.cb;
    const u32 clip = r.Get(kPaClClipCntl);
    const u32 vte = r.Get(kPaClVteCntl);
    // scissor: screen ∩ window ∩ generic (+ window offset)
    const u32 woff = r.Get(kWindowOffset);
    const bool enable_offset = (r.Get(kWindowScissorTl) >> 31) == 0;
    const s32 ox = enable_offset ? Lo16(woff) : 0, oy = enable_offset ? Hi16(woff) : 0;
    const u32 wtl = r.Get(kWindowScissorTl), wbr = r.Get(kWindowScissorBr);
    const u32 gtl = r.Get(kGenericScissorTl), gbr = r.Get(kGenericScissorBr);
    const u32 stl = r.Get(kScreenScissorTl), sbr = r.Get(kScreenScissorBr);
    s32 tlx = std::max({Lo16(stl), static_cast<s32>(static_cast<s16>((wtl & 0x7FFF) + ox)),
                        static_cast<s32>(static_cast<s16>((gtl & 0x7FFF) + ox))});
    s32 tly = std::max({Hi16(stl), static_cast<s32>(static_cast<s16>(((wtl >> 16) & 0x7FFF) + oy)),
                        static_cast<s32>(static_cast<s16>(((gtl >> 16) & 0x7FFF) + oy))});
    s32 brx = std::min({Lo16(sbr), static_cast<s32>(static_cast<s16>((wbr & 0x7FFF) + ox)),
                        static_cast<s32>(static_cast<s16>((gbr & 0x7FFF) + ox))});
    s32 bry = std::min({Hi16(sbr), static_cast<s32>(static_cast<s16>(((wbr >> 16) & 0x7FFF) + oy)),
                        static_cast<s32>(static_cast<s16>(((gbr >> 16) & 0x7FFF) + oy))});
    const bool vport_scissor = (r.Get(kPaScModeCntl0) >> 1) & 1;
    std::array<VkViewport, 16> vps{};
    std::array<VkRect2D, 16> scs{};
    u32 n = 0;
    for (u32 i = 0; i < 16; ++i) {
        const u32 base = kViewport0 + i * 6;
        const float xscale_raw = F(r, base + 0);
        if (xscale_raw == 0.0f) {
            continue;
        }
        const float zscale = (vte >> 4) & 1 ? F(r, base + 4) : 1.0f;
        const float zoffset = (vte >> 5) & 1 ? F(r, base + 5) : 0.0f;
        VkViewport v{};
        if (((clip >> 19) & 1) == 0) { // [-W, W]
            v.minDepth = zoffset - zscale;
            v.maxDepth = zoffset + zscale;
        } else {
            v.minDepth = zoffset;
            v.maxDepth = zoffset + zscale;
        }
        v.minDepth = std::clamp(v.minDepth, 0.0f, 1.0f);
        v.maxDepth = std::clamp(v.maxDepth, 0.0f, 1.0f);
        if ((clip >> 16) & 1) { // clipping disabled: the shader maps screen space into a 16K window
            v.x = 0.0f;
            v.y = 0.0f;
            v.width = static_cast<float>(std::min<u32>(ctx_.Caps().max_viewport_width, 16384));
            v.height = static_cast<float>(std::min<u32>(ctx_.Caps().max_viewport_height, 16384));
        } else {
            const float xscale = (vte & 1) ? xscale_raw : 1.0f;
            const float xoffset = (vte >> 1) & 1 ? F(r, base + 1) : 0.0f;
            const float yscale = (vte >> 2) & 1 ? F(r, base + 2) : 1.0f;
            const float yoffset = (vte >> 3) & 1 ? F(r, base + 3) : 0.0f;
            v.x = xoffset - xscale;
            v.y = yoffset - yscale;
            v.width = xscale * 2.0f;
            v.height = yscale * 2.0f;
        }
        s32 ax = tlx, ay = tly, bx = brx, by = bry;
        if (vport_scissor) {
            const u32 vtl = r.Get(kViewportScissor0 + i * 2), vbr = r.Get(kViewportScissor0 + i * 2 + 1);
            ax = std::max(ax, static_cast<s32>(vtl & 0x7FFF));
            ay = std::max(ay, static_cast<s32>((vtl >> 16) & 0x7FFF));
            bx = std::min(std::clamp(bx, 0, 16384), static_cast<s32>(vbr & 0x7FFF));
            by = std::min(std::clamp(by, 0, 16384), static_cast<s32>((vbr >> 16) & 0x7FFF));
        }
        ax = std::max(ax, 0);
        ay = std::max(ay, 0);
        vps[n] = v;
        scs[n] = {{ax, ay}, {static_cast<u32>(std::max(bx - ax, 0)), static_cast<u32>(std::max(by - ay, 0))}};
        ++n;
    }
    if (n == 0) {
        vps[0] = {-1.0f, -1.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        scs[0] = {{0, 0}, {1, 1}};
        n = 1;
    }
    DynShadow& sh = dyn_;
    const bool all = !sh.valid;
    sh.valid = true;
    const auto changed = [&](bool differs) {
        if (all || differs) {
            ++dyn_set_;
            return true;
        }
        ++dyn_skipped_;
        return false;
    };
    if (changed(n != sh.vp_count || std::memcmp(vps.data(), sh.vps.data(), n * sizeof(VkViewport)) != 0 ||
                std::memcmp(scs.data(), sh.scs.data(), n * sizeof(VkRect2D)) != 0)) {
        f.vkCmdSetViewportWithCount(cb, n, vps.data());
        f.vkCmdSetScissorWithCount(cb, n, scs.data());
        sh.vp_count = n;
        std::memcpy(sh.vps.data(), vps.data(), n * sizeof(VkViewport));
        std::memcpy(sh.scs.data(), scs.data(), n * sizeof(VkRect2D));
    }

    // depth / stencil
    const u32 dc = r.Get(kDbDepthControl);
    const u32 rc = r.Get(kDbRenderControl);
    const bool depth_valid = (r.Get(kDbZInfo) & 3) != 0, stencil_valid = (r.Get(kDbStencilInfo) & 1) != 0;
    const VkBool32 depth_test = ((dc >> 1) & 1) && depth_valid;
    const VkBool32 depth_write = depth_test && ((dc >> 2) & 1) && (rc & 1) == 0;
    const VkCompareOp depth_op = CompareOp((dc >> 4) & 7).value;
    if (changed(depth_test != sh.depth_test)) {
        f.vkCmdSetDepthTestEnable(cb, depth_test);
        sh.depth_test = depth_test;
    }
    if (changed(depth_write != sh.depth_write)) {
        f.vkCmdSetDepthWriteEnable(cb, depth_write);
        sh.depth_write = depth_write;
    }
    if (changed(depth_op != sh.depth_op)) {
        f.vkCmdSetDepthCompareOp(cb, depth_op);
        sh.depth_op = depth_op;
    }
    const VkBool32 bounds = (dc >> 3) & 1;
    if (changed(bounds != sh.bounds_test)) {
        f.vkCmdSetDepthBoundsTestEnable(cb, bounds);
        sh.bounds_test = bounds;
    }
    const float bmin = std::clamp(F(r, kDbDepthBoundsMin), 0.0f, 1.0f), bmax = std::clamp(F(r, kDbDepthBoundsMax), 0.0f, 1.0f);
    if (changed(bmin != sh.bounds[0] || bmax != sh.bounds[1])) {
        f.vkCmdSetDepthBounds(cb, bmin, bmax);
        sh.bounds[0] = bmin;
        sh.bounds[1] = bmax;
    }
    const u32 pc = r.Get(kPaSuScModeCntl);
    const VkBool32 bias = ((pc >> 11) & 7) != 0;
    if (changed(bias != sh.bias_enable)) {
        f.vkCmdSetDepthBiasEnable(cb, bias);
        sh.bias_enable = bias;
    }
    if (bias) {
        const bool front = (pc >> 11) & 1;
        const float offset = F(r, kPolyOffsetClamp + (front ? 2 : 4));
        const float scale = F(r, kPolyOffsetClamp + (front ? 1 : 3));
        const float b3[3] = {offset, F(r, kPolyOffsetClamp), scale / 16.0f};
        if (changed(!sh.bias_set || std::memcmp(b3, sh.bias, sizeof(b3)) != 0)) {
            f.vkCmdSetDepthBias(cb, b3[0], b3[1], b3[2]);
            std::memcpy(sh.bias, b3, sizeof(b3));
            sh.bias_set = true;
        }
    }
    const VkBool32 stencil = (dc & 1) && stencil_valid;
    if (changed(stencil != sh.stencil_test)) {
        f.vkCmdSetStencilTestEnable(cb, stencil);
        sh.stencil_test = stencil;
    }
    // the stencil faces as this draw wants them, set where they differ
    DynShadow::Face want_front{}, want_back{};
    if (stencil) {
        const u32 sc = r.Get(kDbStencilControl);
        const bool backface = (dc >> 7) & 1;
        const auto ops = [&](u32 shift) {
            return std::array<VkStencilOp, 3>{StencilOp((sc >> shift) & 0xF).value, StencilOp((sc >> (shift + 4)) & 0xF).value,
                                              StencilOp((sc >> (shift + 8)) & 0xF).value};
        };
        const auto fo = ops(0);
        const auto bo = backface ? ops(12) : fo;
        const VkCompareOp ffunc = CompareOp((dc >> 8) & 7).value;
        const VkCompareOp bfunc = backface ? CompareOp((dc >> 20) & 7).value : ffunc;
        want_front = {fo[0], fo[1], fo[2], ffunc};
        want_back = {bo[0], bo[1], bo[2], bfunc};
        const u32 front_ref = r.Get(kDbStencilRefMask), back_ref = backface ? r.Get(kDbStencilRefMaskBf) : front_ref;
        const auto uses_op_val = [&](u32 shift) {
            for (u32 k = 0; k < 3; ++k) {
                if (((sc >> (shift + 4 * k)) & 0xF) == 4) { // ReplaceOp: the op value
                    return true;
                }
            }
            return false;
        };
        const bool fop = uses_op_val(0), bop = backface ? uses_op_val(12) : fop;
        const bool clear = (rc >> 1) & 1;
        want_front.reference = fop ? (front_ref >> 24) : (front_ref & 0xFF);
        want_back.reference = bop ? (back_ref >> 24) : (back_ref & 0xFF);
        want_front.write_mask = clear ? 0 : (front_ref >> 16) & 0xFF;
        want_back.write_mask = clear ? 0 : (back_ref >> 16) & 0xFF;
        want_front.compare_mask = (front_ref >> 8) & 0xFF;
        want_back.compare_mask = (back_ref >> 8) & 0xFF;
    } else {
        want_front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0, 0, 0};
        want_back = want_front;
    }
    const auto set_face = [&](VkStencilFaceFlags face, const DynShadow::Face& w, DynShadow::Face& have) {
        if (changed(w.fail != have.fail || w.pass != have.pass || w.depth_fail != have.depth_fail ||
                    w.compare != have.compare)) {
            f.vkCmdSetStencilOp(cb, face, w.fail, w.pass, w.depth_fail, w.compare);
        }
        if (changed(w.reference != have.reference)) {
            f.vkCmdSetStencilReference(cb, face, w.reference);
        }
        if (changed(w.write_mask != have.write_mask)) {
            f.vkCmdSetStencilWriteMask(cb, face, w.write_mask);
        }
        if (changed(w.compare_mask != have.compare_mask)) {
            f.vkCmdSetStencilCompareMask(cb, face, w.compare_mask);
        }
        have = w;
    };
    set_face(VK_STENCIL_FACE_FRONT_BIT, want_front, sh.front);
    set_face(VK_STENCIL_FACE_BACK_BIT, want_back, sh.back);
    // primitive state
    const bool rect_quad = st.prim_type == 17 || st.prim_type == 19;
    const auto topo = PrimitiveType(st.prim_type).value;
    const bool list = topo == VK_PRIMITIVE_TOPOLOGY_POINT_LIST || topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST ||
                      topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST ||
                      topo == VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY ||
                      topo == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    const bool restart = (r.Get(kVgtMultiPrimIbResetEn) & 1) && indexed &&
                         (ctx_.Caps().list_restart || !list) && (ctx_.Caps().patch_list_restart || !rect_quad);
    if (changed(static_cast<VkBool32>(restart) != sh.restart)) {
        f.vkCmdSetPrimitiveRestartEnable(cb, restart);
        sh.restart = restart;
    }
    const VkBool32 discard = (clip >> 22) & 1;
    if (changed(discard != sh.discard)) {
        f.vkCmdSetRasterizerDiscardEnable(cb, discard);
        sh.discard = discard;
    }
    const auto culled = IsPrimitiveCulled(st.prim_type);
    const VkCullModeFlags cull = culled.value ? CullMode(pc & 3).value : VK_CULL_MODE_NONE;
    if (changed(cull != sh.cull)) {
        f.vkCmdSetCullMode(cb, cull);
        sh.cull = cull;
    }
    const VkFrontFace front_face = FrontFace((pc >> 2) & 1).value;
    if (changed(front_face != sh.front_face)) {
        f.vkCmdSetFrontFace(cb, front_face);
        sh.front_face = front_face;
    }
    if (changed(sh.line_width != 1.0f)) {
        f.vkCmdSetLineWidth(cb, 1.0f);
        sh.line_width = 1.0f;
    }
    const float bc[4] = {F(r, kCbBlendRed), F(r, kCbBlendRed + 1), F(r, kCbBlendRed + 2), F(r, kCbBlendRed + 3)};
    if (changed(std::memcmp(bc, sh.blend, sizeof(bc)) != 0)) {
        f.vkCmdSetBlendConstants(cb, bc);
        std::memcpy(sh.blend, bc, sizeof(bc));
    }
    if (ctx_.Caps().color_write_mask_dynamic) {
        const u32 num_color = static_cast<u32>(std::bit_width(st.mrt_mask & 0xFFu));
        if (num_color > 0) {
            std::array<VkColorComponentFlags, 8> masks{};
            for (u32 i = 0; i < num_color; ++i) {
                masks[i] = st.targets[i].write_mask & 0xF;
            }
            if (changed(num_color != sh.color_masks_count || masks != sh.color_masks)) {
                f.vkCmdSetColorWriteMaskEXT(cb, 0, num_color, masks.data());
                sh.color_masks_count = num_color;
                sh.color_masks = masks;
            }
        }
    }
}

void Executor::Draw(const Cp::DrawDescriptor& d) {
    const u64 t0 = GS::Ticks();
    ++stats_.draws;
    const auto& f = Fn();
    const Cp::RegisterFile& regs = cp_->Registers();
    const u32 mode = (regs.Get(kCbColorControl) >> 4) & 7;
    const bool filtered = mode == 2 || mode == 5 || d.primitive_type == 0; // EliminateFastClear, FmaskDecompress, None
    // the frontend reads the guest's descriptors now (a write still on the GPU's timeline lands first: before this draw
    // is noted, so that flush completes only the earlier work)
    const u64 tf0 = GS::Ticks();
    const GS::DrawPreparation* prepared = filtered ? nullptr : &frontend_.PrepareDraw(d, regs, reader_);
    stats_.t_front += GS::Ticks() - tf0;
    if (!Begin()) {
        return;
    }
    Note(d.queue, d.seq);
    ++commands_;
    write_run_ = false;
    if (filtered) {
        Skipped(Skip::FilteredMode, fmt::format("CB_COLOR_CONTROL mode {}, primitive {}", mode, d.primitive_type));
        return;
    }
    const GS::DrawPreparation& prep = *prepared;
    if (!prep.ready) {
        Skipped(prep.unsupported ? Skip::Unsupported : Skip::NotReady,
                fmt::format("draw at {:#x}: VS {}, PS {}", d.packet_addr,
                            GS::StageStatusName(frontend_.Stage(GS::HwStage::Vs).status),
                            GS::StageStatusName(frontend_.Stage(GS::HwStage::Ps).status)));
        return;
    }
    u64 ts = GS::Ticks();
    const auto stage = [&](u64& acc) {
        const u64 now = GS::Ticks();
        acc += now - ts;
        ts = now;
    };
    const GS::GraphicsPipelineState* st = frontend_.Pipelines().Graphics(prep.pipeline);
    const GS::StageView& vs = frontend_.Stage(GS::HwStage::Vs);
    const GS::StageView& ps = frontend_.Stage(GS::HwStage::Ps);
    const bool has_ps = st != nullptr && st->ps_module != 0 && ps.module != nullptr;
    std::string err;
    const PipelineObjects* pipe = st == nullptr ? nullptr
                                                : pipelines_.Graphics(prep.pipeline, *st, vs.module, vs.plan,
                                                                      has_ps ? ps.module : nullptr,
                                                                      has_ps ? &ps.plan : nullptr, err);
    if (pipe == nullptr) {
        Skipped(Skip::Pipeline, err.empty() ? "no pipeline state" : err);
        return;
    }
    stage(stats_.t_pipe);

    // targets
    std::vector<CachedImage*>& targets = scratch_.targets;
    targets.clear();
    RenderKey key;
    std::array<CachedImage*, 8> color_img{};
    const u32 target_mask = regs.Get(kCbTargetMask);
    std::array<bool, 8> color_clear{};
    CachedImage* depth_img = nullptr;
    const u32 dc = regs.Get(kDbDepthControl);
    bool depth_clear = false;
    // the last draw's targets when nothing they are built from changed (e1: 9.9 ms a frame, ~1.8 us a draw, rebuilding
    // the same targets draw after draw)
    TargetInputs tin{};
    tin.versions = {regs.Version(Cp::Group::ColorFormat), regs.Version(Cp::Group::ColorTarget),
                    regs.Version(Cp::Group::DepthFormat), regs.Version(Cp::Group::DepthTarget)};
    tin.mode = mode;
    tin.target_mask = target_mask;
    tin.depth_control = dc & 7; // stencil, depth test, depth write (the depth view)
    tin.mrt_mask = st->mrt_mask;
    tin.cb_extent = d.cb_extent;
    tin.db_extent = d.db_extent;
    tin.epoch = images_.Epoch();
    if (target_cache_.valid && target_cache_.in == tin) {
        ++stats_.target_cache_hits;
        targets = target_cache_.targets;
        key = target_cache_.key;
        color_img = target_cache_.color_img;
        depth_img = target_cache_.depth_img;
        for (CachedImage* img : targets) {
            img->last_seq = cur_.value; // what Find records for a target
        }
        if (!pending_meta_clear_.empty()) {
            const auto take = [&](u64 meta) {
                auto it = meta != 0 ? pending_meta_clear_.find(meta) : pending_meta_clear_.end();
                if (it == pending_meta_clear_.end() || !it->second) {
                    return false;
                }
                it->second = false;
                return true;
            };
            for (u32 i = 0; i < 8; ++i) {
                color_clear[i] = color_img[i] != nullptr && take(target_cache_.cmask[i]);
            }
            depth_clear = depth_img != nullptr && take(target_cache_.htile);
        }
    } else {
    ++stats_.target_cache_misses;
    target_cache_.valid = false;
    target_cache_.cmask.fill(0);
    target_cache_.htile = 0;
    key.width = ctx_.Caps().max_framebuffer_width;
    key.height = ctx_.Caps().max_framebuffer_height;
    key.layers = 0xFFFF;
    const u32 num_color = static_cast<u32>(std::bit_width(st->mrt_mask & 0xFFu));
    for (u32 i = 0; i < num_color; ++i) {
        const u32 base = Cp::Reg::CbColor0Base + i * Cp::Reg::CbColorStride;
        const u32 info = regs.Get(base + 4);
        if (mode == 0 || ((st->mrt_mask >> i) & 1) == 0 || ((info >> 2) & 0x1F) == 0 || regs.Get(base) == 0 ||
            ((target_mask >> (i * 4)) & 0xF) == 0) {
            continue;
        }
        const ImageLayout layout = LayoutFromColorTarget(regs, i, d.cb_extent[i]);
        CachedImage* img = images_.Find(layout, ImageCache::Use::ColorTarget, cur_.cb, cur_.value, err);
        if (img == nullptr) {
            Skipped(Skip::Target, err);
            return;
        }
        const ImageViewDesc vd = ColorTargetView(regs, i);
        key.color[i] = images_.View(*img, vd, false);
        if (key.color[i] == VK_NULL_HANDLE) {
            Skipped(Skip::Target, "colour target view");
            return;
        }
        key.width = std::min(key.width, std::max(img->layout.size.width, 1u));
        key.height = std::min(key.height, std::max(img->layout.size.height, 1u));
        key.layers = std::min<u32>(key.layers, std::max<u32>(vd.layers, 1));
        color_img[i] = img;
        targets.push_back(img);
        const u64 cmask = u64{regs.Get(base + 7)} << 8;
        target_cache_.cmask[i] = cmask;
        if (cmask != 0) {
            if (auto it = pending_meta_clear_.find(cmask); it != pending_meta_clear_.end() && it->second) {
                color_clear[i] = true;
                it->second = false;
            }
        }
    }
    key.color_count = num_color;
    const bool depth_valid = (regs.Get(kDbZInfo) & 3) != 0, stencil_valid = (regs.Get(kDbStencilInfo) & 1) != 0;
    if ((((dc >> 1) & 1) && depth_valid) || ((dc & 1) && stencil_valid)) {
        const ImageLayout layout = LayoutFromDepthTarget(regs, d.db_extent);
        depth_img = images_.Find(layout, ImageCache::Use::DepthTarget, cur_.cb, cur_.value, err);
        if (depth_img == nullptr) {
            Skipped(Skip::Target, err);
            return;
        }
        key.depth = images_.View(*depth_img, DepthTargetView(regs), false);
        key.has_depth = depth_valid;
        key.has_stencil = stencil_valid && depth_img->layout.props.has_stencil;
        key.width = std::min(key.width, std::max(depth_img->layout.size.width, 1u));
        key.height = std::min(key.height, std::max(depth_img->layout.size.height, 1u));
        targets.push_back(depth_img);
        const u64 htile = u64{regs.Get(kDbHtileBase)} << 8;
        target_cache_.htile = htile;
        if (htile != 0) {
            if (auto it = pending_meta_clear_.find(htile); it != pending_meta_clear_.end() && it->second) {
                depth_clear = true;
                it->second = false;
            }
        }
    }
    if (key.layers == 0xFFFF) {
        key.layers = 1;
    }
    if (targets.empty()) {
        key.width = std::max<u32>(1, std::min<u32>(key.width, 16384));
        key.height = std::max<u32>(1, std::min<u32>(key.height, 16384));
    }
    target_cache_.valid = true;
    target_cache_.in = tin;
    target_cache_.in.epoch = images_.Epoch(); // a target just created bumped it: the next draw's inputs carry the new one
    target_cache_.targets = targets;
    target_cache_.key = key;
    target_cache_.color_img = color_img;
    target_cache_.depth_img = depth_img;
    }
    stage(stats_.t_targets);
    // resources
    PushData push{};
    const u32 vte = regs.Get(kPaClVteCntl);
    push.xscale = (vte & 1) ? F(regs, kViewport0) : 1.0f;
    push.xoffset = (vte >> 1) & 1 ? F(regs, kViewport0 + 1) : 0.0f;
    push.yscale = (vte >> 2) & 1 ? F(regs, kViewport0 + 2) : 1.0f;
    push.yoffset = (vte >> 3) & 1 ? F(regs, kViewport0 + 3) : 0.0f;
    // the command's descriptor lists: reused (their capacity kept) - per-command vectors were 14% of the GPU thread's
    // samples in the heap (f1)
    std::vector<VkWriteDescriptorSet>& writes = scratch_.writes;
    std::vector<VkDescriptorBufferInfo>& buf_infos = scratch_.buf_infos;
    std::vector<VkDescriptorImageInfo>& img_infos = scratch_.img_infos;
    std::vector<CachedImage*>& bound = scratch_.bound;
    writes.clear();
    buf_infos.clear();
    img_infos.clear();
    bound.clear();
    bool writes_memory = false;
    info_images_.clear();
    const u64 tb0 = GS::Ticks();
    if (has_ps && !BindStage(&ps, false, push, writes, buf_infos, img_infos, bound, writes_memory, targets)) {
        return;
    }
    if (!BindStage(&vs, false, push, writes, buf_infos, img_infos, bound, writes_memory, targets)) {
        return;
    }
    stats_.t_bind += GS::Ticks() - tb0;
    ts = GS::Ticks();
    // vertex inputs
    std::vector<VkVertexInputBindingDescription2EXT>& vbind = scratch_.vbind;
    std::vector<VkVertexInputAttributeDescription2EXT>& vattr = scratch_.vattr;
    std::vector<VkBuffer>& vbufs = scratch_.vbufs;
    std::vector<VkDeviceSize>& voffs = scratch_.voffs;
    vbind.clear();
    vattr.clear();
    vbufs.clear();
    voffs.clear();
    for (const auto& v : vs.plan.vertices) {
        const u32 slot = static_cast<u32>(vattr.size());
        // the format the fetch shader loads with: a typed load's (MTBUF) own format overrides the V#'s, as the
        // translated shader declares the input with it (the bridge's VertexAttribute::GetSharp; VUID-...-Input-08734)
        // and remapped as the bridge's AmdGpu::Buffer::GetNumberFmt() (RemapNumberFormat: SnormNz/Sscaled -> Sint,
        // Uscaled -> Uint, ...; the shader converts those), the number class the shader's input was declared with
        const u32 dfmt = v.fetch.data_format != 0 ? v.fetch.data_format : static_cast<u32>(v.desc.DataFormat());
        const u32 raw_nfmt = v.fetch.data_format != 0 ? v.fetch.num_format : static_cast<u32>(v.desc.NumFormat());
        const u32 nfmt = static_cast<u32>(
            RemapNumberFormat(static_cast<NumberFormat>(raw_nfmt), static_cast<DataFormat>(dfmt)));
        const VkFormat fmt = SurfaceFormat(dfmt, nfmt);
        if (fmt == VK_FORMAT_UNDEFINED) {
            Skipped(Skip::VertexFormat, fmt::format("vertex attribute {} format {}/{}", v.attribute, dfmt, nfmt));
            return;
        }
        const u32 inst = v.fetch.instance_data;
        const u32 divisor = inst == 1 ? regs.Get(kVgtInstanceStepRate0) : (inst == 2 ? regs.Get(kVgtInstanceStepRate1) : 1);
        vattr.push_back({.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                         .location = slot,
                         .binding = slot,
                         .format = fmt,
                         .offset = 0});
        vbind.push_back({.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
                         .binding = slot,
                         .stride = v.desc.Stride(),
                         .inputRate = inst == 0 ? VK_VERTEX_INPUT_RATE_VERTEX : VK_VERTEX_INPUT_RATE_INSTANCE,
                         .divisor = inst == 0 ? 1u : std::max<u32>(divisor, 1)});
        if (v.status == GS::ResourceStatus::Ok && v.address != 0 && v.size != 0) {
            VkBuffer vb = VK_NULL_HANDLE;
            u64 voff = 0, vavail = 0;
            if (!GuestRange(v.address, v.size, false, 4, vb, voff, vavail)) {
                Skipped(Skip::Buffer, fmt::format("vertex buffer at {:#x}", v.address));
                return;
            }
            vbufs.push_back(vb);
            voffs.push_back(voff);
        } else {
            vbufs.push_back(VK_NULL_HANDLE);
            voffs.push_back(0);
        }
    }
    // index buffer
    VkBuffer ibuf = VK_NULL_HANDLE;
    u64 ioff = 0;
    const bool idx32 = (d.index_type & 3) == 1;
    if (d.indexed) {
        const u32 isize = idx32 ? 4 : 2;
        const u64 addr = d.index_base + u64{d.index_offset} * isize;
        const u64 count = d.indirect ? d.max_index_size : std::min(d.index_count, d.max_index_size);
        if (count != 0) {
            u64 iavail = 0;
            if (!GuestRange(addr, count * isize, false, isize, ibuf, ioff, iavail)) {
                Skipped(Skip::Buffer, fmt::format("index buffer at {:#x}", addr));
                return;
            }
        }
    }
    VkBuffer argbuf = VK_NULL_HANDLE;
    u64 argoff = 0;
    if (d.indirect) {
        u64 aavail = 0;
        if (!GuestRange(d.args_addr, d.indexed ? 20 : 16, false, 4, argbuf, argoff, aavail)) {
            Skipped(Skip::Buffer, fmt::format("indirect arguments at {:#x}", d.args_addr));
            return;
        }
    }

    stage(stats_.t_vertex);
    // barriers and layouts (outside rendering)
    if (pending_writes_) {
        GlobalBarrier();
    }
    const bool feedback_any = std::ranges::any_of(bound, [&](CachedImage* b) {
        return std::ranges::find(targets, b) != targets.end();
    });
    bool need_transition = false;
    const auto want = [&](CachedImage* img, VkImageLayout layout) { return img->current != layout; };
    for (u32 i = 0; i < 8; ++i) {
        if (color_img[i] != nullptr &&
            want(color_img[i], feedback_any ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)) {
            need_transition = true;
        }
    }
    // a draw that samples its own depth attachment: one layout valid for both uses (VUID-vkCmdDraw-imageLayout-00344)
    const bool depth_feedback = depth_img != nullptr && std::ranges::find(bound, depth_img) != bound.end();
    const VkImageLayout depth_layout =
        depth_feedback ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    if (depth_img != nullptr && want(depth_img, depth_layout)) {
        need_transition = true;
    }
    const auto texture_layout = [](const CachedImage* img) {
        const bool storage = (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) && img->gpu_written &&
                             img->current == VK_IMAGE_LAYOUT_GENERAL;
        return storage ? VK_IMAGE_LAYOUT_GENERAL
               : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };
    for (CachedImage* img : bound) {
        // a texture in another layout (rendered, uploaded, stored) needs its barrier outside rendering; one already
        // read in its layout does not (storage writes since are ordered by pending_writes_' global barrier)
        if (std::ranges::find(targets, img) == targets.end() && img->current != texture_layout(img)) {
            need_transition = true;
            break;
        }
    }
    if (need_transition || !rendering_ || !(key == render_)) {
        EndRendering();
        for (u32 i = 0; i < 8; ++i) {
            if (color_img[i] != nullptr) {
                images_.Transition(cur_.cb, *color_img[i],
                                   feedback_any ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                                   VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            }
        }
        if (depth_img != nullptr) {
            images_.Transition(cur_.cb, *depth_img, depth_layout,
                               VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                               VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                   VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        }
        for (u32 k = 0; k < bound.size(); ++k) {
            CachedImage* img = bound[k];
            if (std::ranges::find(targets, img) != targets.end()) {
                continue;
            }
            const bool storage = (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) && img->gpu_written && img->current == VK_IMAGE_LAYOUT_GENERAL;
            images_.Transition(cur_.cb, *img,
                               storage ? VK_IMAGE_LAYOUT_GENERAL
                               : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               kGraphicsShaders, VK_ACCESS_2_SHADER_READ_BIT);
        }
    }
    // the descriptors' image layouts are the images' current ones (VUID-vkCmdDraw-None-09600)
    for (size_t i = 0; i < img_infos.size() && i < info_images_.size(); ++i) {
        if (info_images_[i] != nullptr && img_infos[i].imageView != VK_NULL_HANDLE) {
            img_infos[i].imageLayout = info_images_[i]->current;
        }
    }
    for (auto& w : writes) {
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            continue;
        }
        w.pImageInfo = img_infos.data() + reinterpret_cast<size_t>(w.pImageInfo);
    }
    {
        size_t bi = 0;
        for (auto& w : writes) {
            if (w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                w.pBufferInfo = &buf_infos[bi++];
            }
        }
    }
    stage(stats_.t_layout);
    if (!rendering_) {
        std::array<VkRenderingAttachmentInfo, 8> colors{};
        for (u32 i = 0; i < key.color_count; ++i) {
            colors[i] = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                         .imageView = key.color[i],
                         .imageLayout = feedback_any ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                         .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (color_clear[i] && key.color[i] != VK_NULL_HANDLE) {
                colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
                colors[i].clearValue = ColorBufferClearValue(regs, i).value;
                ++stats_.fast_clears;
            }
        }
        VkRenderingAttachmentInfo depth{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                        .imageView = key.depth,
                                        .imageLayout = depth_layout,
                                        .loadOp = depth_clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        depth.clearValue.depthStencil = {std::clamp(F(regs, kDbDepthClear), 0.0f, 1.0f), regs.Get(kDbStencilClear) & 0xFF};
        const VkRenderingInfo ri{.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                 .renderArea = {{0, 0}, {key.width, key.height}},
                                 .layerCount = key.layers,
                                 .colorAttachmentCount = key.color_count,
                                 .pColorAttachments = colors.data(),
                                 .pDepthAttachment = key.has_depth ? &depth : nullptr,
                                 .pStencilAttachment = key.has_stencil ? &depth : nullptr};
        f.vkCmdBeginRendering(cur_.cb, &ri);
        rendering_ = true;
        render_ = key;
        ++stats_.render_passes;
        if (depth_clear) {
            ++stats_.depth_clears;
        }
    }
    // the hardware's depth/stencil clear: DB_RENDER_CONTROL clear bits clear the drawn area (the scissor)
    const u32 rc = regs.Get(kDbRenderControl);
    if (depth_img != nullptr && (rc & 3) != 0 && !depth_clear) {
        VkClearAttachment ca{.aspectMask = 0};
        if ((rc & 1) && key.has_depth && ((dc >> 1) & 1) && ((dc >> 2) & 1)) {
            ca.aspectMask |= VK_IMAGE_ASPECT_DEPTH_BIT;
        }
        if ((rc & 2) && key.has_stencil) {
            ca.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
        if (ca.aspectMask != 0) {
            ca.clearValue.depthStencil = {std::clamp(F(regs, kDbDepthClear), 0.0f, 1.0f),
                                          regs.Get(kDbStencilClear) & 0xFF};
            const VkClearRect rect{{{0, 0}, {key.width, key.height}}, 0, key.layers};
            f.vkCmdClearAttachments(cur_.cb, 1, &ca, 1, &rect);
            ++stats_.depth_clears;
        }
    }
    f.vkCmdBindPipeline(cur_.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
    if (!PushDescriptors(*pipe, VK_PIPELINE_BIND_POINT_GRAPHICS, writes)) {
        Skipped(Skip::Pipeline, "descriptor set allocation");
        return;
    }
    f.vkCmdPushConstants(cur_.cb, pipe->layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(PushData), &push);
    SetDynamicState(regs, *st, d.indexed);
    const auto same_input = [](const auto& a, const auto& b) {
        return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
    };
    if (!dyn_vertex_valid_ || !same_input(vbind, dyn_.vbind) || !same_input(vattr, dyn_.vattr)) {
        f.vkCmdSetVertexInputEXT(cur_.cb, static_cast<u32>(vbind.size()), vbind.data(), static_cast<u32>(vattr.size()),
                                 vattr.data());
        dyn_.vbind = vbind;
        dyn_.vattr = vattr;
        dyn_vertex_valid_ = true;
        ++dyn_set_;
    } else {
        ++dyn_skipped_;
    }
    if (!vbufs.empty()) {
        f.vkCmdBindVertexBuffers(cur_.cb, 0, static_cast<u32>(vbufs.size()), vbufs.data(), voffs.data());
        stats_.vertex_bindings += vbufs.size();
    }
    // the fetch shader's vertex/instance offsets (the bridge's GetDrawOffsets)
    const GS::FetchShader& fetch = vs.module->result.reflection.fetch;
    const auto& vs_ud = d.user_data[static_cast<size_t>(Cp::Stage::Vs)];
    u32 vertex_offset = d.vgt_index_offset;
    u32 instance_offset = 0;
    if (fetch.present) {
        if (vertex_offset == 0 && fetch.vertex_offset_sgpr >= 0 && fetch.vertex_offset_sgpr < 16) {
            vertex_offset = vs_ud[fetch.vertex_offset_sgpr];
        }
        if (fetch.instance_offset_sgpr >= 0 && fetch.instance_offset_sgpr < 16) {
            instance_offset = vs_ud[fetch.instance_offset_sgpr];
        }
    }
    const u32 instances = std::max<u32>(d.instance_count, 1);
    u64 cid = 0;
    if (diag_ != nullptr && diag_->Enabled()) {
        std::string text = fmt::format("draw q{} seq {} packet {:#x} pipeline {} vs {:#x} ps {:#x} prim {}{}{} count {} "
                                       "instances {} render {}x{}x{}",
                                       d.queue, d.seq, d.packet_addr, prep.pipeline, vs.address,
                                       has_ps ? ps.address : 0, d.primitive_type, d.indexed ? " indexed" : "",
                                       d.indirect ? " indirect" : "", d.index_count, d.instance_count, key.width,
                                       key.height, key.layers);
        for (u32 i = 0; i < 8; ++i) {
            if (color_img[i] != nullptr) {
                text += fmt::format(" | rt{} {}", i, DescribeImage(*color_img[i]));
            }
        }
        if (depth_img != nullptr) {
            text += fmt::format(" | depth {}", DescribeImage(*depth_img));
        }
        for (const CachedImage* img : bound) {
            text += fmt::format(" | tex {}", DescribeImage(*img));
        }
        cid = DiagBegin(std::move(text));
    }
    if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    if (d.indexed) {
        if (ibuf != VK_NULL_HANDLE) {
            f.vkCmdBindIndexBuffer(cur_.cb, ibuf, ioff, idx32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16);
        }
        ++stats_.indexed;
    }
    if (d.indirect) {
        ++stats_.indirect;
        if (d.indexed) {
            if (ibuf != VK_NULL_HANDLE) {
                f.vkCmdDrawIndexedIndirect(cur_.cb, argbuf, argoff, 1, 20);
            }
        } else {
            f.vkCmdDrawIndirect(cur_.cb, argbuf, argoff, 1, 16);
        }
    } else if (d.indexed) {
        if (ibuf != VK_NULL_HANDLE) {
            f.vkCmdDrawIndexed(cur_.cb, std::min(d.index_count, d.max_index_size), instances, 0,
                               static_cast<s32>(vertex_offset), instance_offset);
        }
    } else {
        f.vkCmdDraw(cur_.cb, d.index_count, instances, vertex_offset, instance_offset);
    }
    DiagEnd(cid);
    stage(stats_.t_emit);
    if (prof_) {
        ProfMark((u64{prep.pipeline} << 2) | 0,
                 fmt::format("draw pipeline {} vs {:#x} ps {:#x} prim {} target {}x{} rt0 fmt {} depth {}", prep.pipeline,
                             vs.address, has_ps ? ps.address : 0, d.primitive_type, key.width, key.height,
                             color_img[0] != nullptr ? color_img[0]->layout.pixel_format : 0, depth_img != nullptr));
        ProfDescribe((u64{prep.pipeline} << 2) | 0, "draw: vertex stage", &vs, bound);
        if (has_ps) {
            ProfDescribe((u64{prep.pipeline} << 2) | 0x8000000000000000ull, "draw: pixel stage", &ps, bound);
        }
    }
    ++stats_.draws_executed;
    if (writes_memory) {
        pending_writes_ = true;
    }
    stats_.record_ns += GS::TicksToNs(GS::Ticks() - t0);
    if (commands_ >= options_.max_draws_per_batch) {
        ++stats_.flushes_full;
        Flush();
    }
}

void Executor::Dispatch(const Cp::DispatchDescriptor& d) {
    const u64 t0 = GS::Ticks();
    ++stats_.dispatches;
    const auto& f = Fn();
    const Cp::ComputeRegisters* cs = cp_->Compute(d.queue);
    const u64 tf0 = GS::Ticks();
    const GS::DrawPreparation* prepared =
        cs == nullptr ? nullptr
                      : &frontend_.PrepareDispatch(d, std::span<const u32, Cp::kComputeCount>(cs->values), reader_);
    stats_.t_front += GS::Ticks() - tf0;
    if (!Begin()) {
        return;
    }
    Note(d.queue, d.seq);
    ++commands_;
    write_run_ = false;
    if (cs == nullptr) {
        Skipped(Skip::NotReady, "no compute registers for the queue");
        return;
    }
    const GS::DrawPreparation& prep = *prepared;
    if (!prep.ready) {
        Skipped(prep.unsupported ? Skip::Unsupported : Skip::NotReady,
                fmt::format("dispatch at {:#x}: CS {}", d.packet_addr,
                            GS::StageStatusName(frontend_.Stage(GS::HwStage::Cs).status)));
        return;
    }
    const GS::StageView& csv = frontend_.Stage(GS::HwStage::Cs);
    lds_groups_ = std::max<u64>(1, u64{cs->values[Cp::Reg::ComputeDimX]} * cs->values[Cp::Reg::ComputeDimX + 1] *
                                       cs->values[Cp::Reg::ComputeDimX + 2]);
    lds_group_bytes_ = ((d.shader.rsrc2 >> 15) & 0x1FFu) * 512u; // COMPUTE_PGM_RSRC2.LDS_SIZE, 128-dword units
    // a compute clear of a target's CMASK/HTILE (no address arithmetic): the target clears at its next rendering
    const GS::ShaderReflection& refl = csv.module->result.reflection;
    {
        // diagnostics: compute writes/reads of the metadata of targets the image cache knows (the bridge's IsMeta)
        bool reads_meta = false, writes_meta = false;
        u64 meta_addr = 0;
        for (const auto& b : csv.plan.buffers) {
            if (b.kind == GS::BufferKind::Guest && b.status == GS::ResourceStatus::Ok &&
                images_.MetaOwner(b.address) != nullptr) {
                if (b.written) {
                    writes_meta = true;
                    meta_addr = b.address;
                } else {
                    reads_meta = true;
                }
            }
        }
        if (writes_meta) {
            ++stats_.meta_writes;
            stats_.meta_writes_xor += refl.has_bitwise_xor ? 1 : 0;
            stats_.meta_writes_reading += reads_meta ? 1 : 0;
            if (stats_.meta_writes <= 24) {
                const CachedImage* owner = images_.MetaOwner(meta_addr);
                SSPC_LOG(GPU, Info,
                         "native backend: dispatch {:#x} (cs {:#x}, groups {}x{}x{}) writes metadata {:#x} of img{} "
                         "{:#x} fmt {} {}x{} depth {} (xor {}, reads metadata {})",
                         d.packet_addr, csv.address, d.groups[0], d.groups[1], d.groups[2], meta_addr, owner->id,
                         owner->layout.guest_address, owner->layout.pixel_format, owner->layout.size.width,
                         owner->layout.size.height, owner->layout.props.is_depth, refl.has_bitwise_xor, reads_meta);
            }
        }
    }
    if (!refl.has_bitwise_xor) {
        for (const auto& b : csv.plan.buffers) {
            if (b.kind == GS::BufferKind::Guest && b.written && b.status == GS::ResourceStatus::Ok) {
                // registered when targets bound: the CMASK/HTILE addresses seen so far
                const Cp::RegisterFile& regs = cp_->Registers();
                bool meta = false;
                for (u32 i = 0; i < 8 && !meta; ++i) {
                    meta = (u64{regs.Get(Cp::Reg::CbColor0Base + i * Cp::Reg::CbColorStride + 7)} << 8) == b.address;
                }
                meta = meta || (u64{regs.Get(kDbHtileBase)} << 8) == b.address;
                if (meta && b.address != 0) {
                    pending_meta_clear_[b.address] = true;
                    Skipped(Skip::MetaClear, fmt::format("compute write of metadata at {:#x}", b.address));
                    return;
                }
            }
        }
    }
    std::string err;
    const PipelineObjects* pipe = pipelines_.Compute(*csv.module, csv.plan, err);
    DiagDumpShader(csv.address, csv.program != nullptr ? &csv.program->code : nullptr, csv.module->result.spirv);
    if (pipe == nullptr) {
        Skipped(Skip::Pipeline, err);
        return;
    }
    PushData push{};
    // the command's descriptor lists: reused (their capacity kept) - per-command vectors were 14% of the GPU thread's
    // samples in the heap (f1)
    std::vector<VkWriteDescriptorSet>& writes = scratch_.writes;
    std::vector<VkDescriptorBufferInfo>& buf_infos = scratch_.buf_infos;
    std::vector<VkDescriptorImageInfo>& img_infos = scratch_.img_infos;
    std::vector<CachedImage*>& bound = scratch_.bound;
    writes.clear();
    buf_infos.clear();
    img_infos.clear();
    bound.clear();
    bool writes_memory = false;
    info_images_.clear();
    const std::vector<CachedImage*> none;
    const u64 tb0 = GS::Ticks();
    if (!BindStage(&csv, true, push, writes, buf_infos, img_infos, bound, writes_memory, none)) {
        return;
    }
    stats_.t_bind += GS::Ticks() - tb0;
    VkBuffer argbuf = VK_NULL_HANDLE;
    u64 argoff = 0;
    if (d.indirect) {
        u64 aavail = 0;
        if (!GuestRange(d.indirect_addr, 12, false, 4, argbuf, argoff, aavail)) {
            Skipped(Skip::Buffer, fmt::format("indirect dispatch arguments at {:#x}", d.indirect_addr));
            return;
        }
    }
    EndRendering();
    if (pending_writes_) {
        GlobalBarrier();
    }
    for (CachedImage* img : bound) {
        const bool storage = (img->usage & VK_IMAGE_USAGE_STORAGE_BIT) && img->gpu_written;
        images_.Transition(cur_.cb, *img,
                           storage ? VK_IMAGE_LAYOUT_GENERAL
                           : img->layout.props.is_depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                                        : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           storage ? (VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT)
                                   : VK_ACCESS_2_SHADER_READ_BIT);
    }
    for (auto& w : writes) {
        if (w.descriptorType != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            w.pImageInfo = img_infos.data() + reinterpret_cast<size_t>(w.pImageInfo);
        }
    }
    size_t bi = 0;
    for (auto& w : writes) {
        if (w.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            w.pBufferInfo = &buf_infos[bi++];
        }
    }
    // the descriptors' image layouts are the images' current ones (after the transitions above)
    for (size_t i = 0; i < img_infos.size() && i < info_images_.size(); ++i) {
        if (info_images_[i] != nullptr && img_infos[i].imageView != VK_NULL_HANDLE) {
            img_infos[i].imageLayout = info_images_[i]->current;
        }
    }
    PrepareShadows(&csv.plan, buf_infos);
    f.vkCmdBindPipeline(cur_.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe->pipeline);
    if (!PushDescriptors(*pipe, VK_PIPELINE_BIND_POINT_COMPUTE, writes)) {
        FinishShadows();
        Skipped(Skip::Pipeline, "descriptor set allocation");
        return;
    }
    f.vkCmdPushConstants(cur_.cb, pipe->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PushData), &push);
    u64 cid = 0;
    if (diag_ != nullptr && diag_->Enabled()) {
        std::string text = fmt::format("dispatch q{} seq {} packet {:#x} cs {:#x} groups {}x{}x{}{}", d.queue, d.seq,
                                       d.packet_addr, csv.address, d.groups[0], d.groups[1], d.groups[2],
                                       d.indirect ? " indirect" : "");
        for (const CachedImage* img : bound) {
            text += fmt::format(" | image {}", DescribeImage(*img));
        }
        cid = DiagBegin(std::move(text));
    }
    if (prof_) {
        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
    }
    if (d.indirect) {
        f.vkCmdDispatchIndirect(cur_.cb, argbuf, argoff);
    } else {
        f.vkCmdDispatch(cur_.cb, d.groups[0], d.groups[1], d.groups[2]);
    }
    DiagEnd(cid);
    if (prof_) {
        ProfMark((csv.address << 2) | 1, fmt::format("dispatch cs {:#x} groups {}x{}x{}{}", csv.address, d.groups[0],
                                                     d.groups[1], d.groups[2], d.indirect ? " indirect" : ""));
        ProfDescribe((csv.address << 2) | 1, "dispatch", &csv, bound);
        // SSPC_DIAG_DUP_DISPATCH=<hex cs address> (with SSPC_GPU_PROFILE): the dispatch once more right after itself
        // (same inputs, same outputs) and an empty interval - whether its time is its own execution or a stall
        static const u64 dup = [] {
            char v[32] = {};
            return GetEnvironmentVariableA("SSPC_DIAG_DUP_DISPATCH", v, sizeof(v)) > 0 ? std::strtoull(v, nullptr, 16)
                                                                                      : 0ull;
        }();
        if (dup != 0 && csv.address == dup && !d.indirect) {
            GlobalBarrier();
            ProfMark(3, "before a command: its barriers, uploads, render pass changes");
            ProfMark(DiagKey(6), "DIAG: an empty interval (timestamp to timestamp)");
            f.vkCmdDispatch(cur_.cb, d.groups[0], d.groups[1], d.groups[2]);
            ProfMark(DiagKey(7), "DIAG: the same dispatch again, right after itself");
            f.vkCmdDispatch(cur_.cb, 1, 1, 1);
            ProfMark(DiagKey(8), "DIAG: the same program, one workgroup");
            // its buffers copied into one dense device-local buffer (not the sparse mirror, not the ring): the same
            // dispatch reading and writing there (the guest's data untouched)
            std::string serr;
            if (diag_scratch_.buffer == VK_NULL_HANDLE) {
                CreateBuffer(ctx_, 16ull << 20,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                 VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             false, diag_scratch_, serr);
            }
            if (diag_scratch_.buffer == VK_NULL_HANDLE && dense_logged_++ == 0) {
                SSPC_LOG(GPU, Warning, "DIAGNOSTIC: no dense scratch buffer: {}", serr);
            }
            // 9: every buffer dense; 10: only the read ones (the written stay in the mirror); 11: only the written
            for (u32 variant = 9; variant <= 11 && diag_scratch_.buffer != VK_NULL_HANDLE; ++variant) {
                GlobalBarrier(); // the copies after the dispatches above
                std::vector<VkDescriptorBufferInfo> dup_infos = buf_infos;
                u64 at = 0;
                bool fits = true;
                for (size_t k = 0; k < dup_infos.size(); ++k) {
                    auto& bi = dup_infos[k];
                    if (bi.buffer == VK_NULL_HANDLE || bi.range == VK_WHOLE_SIZE || bi.range == 0) {
                        continue;
                    }
                    const bool written = k < csv.plan.buffers.size() && csv.plan.buffers[k].written;
                    if ((variant == 10 && written) || (variant == 11 && !written)) {
                        continue;
                    }
                    at = (at + 255) & ~255ull;
                    if (at + bi.range > diag_scratch_.size) {
                        fits = false;
                        break;
                    }
                    const VkBufferCopy c{.srcOffset = bi.offset, .dstOffset = at, .size = bi.range};
                    f.vkCmdCopyBuffer(cur_.cb, bi.buffer, diag_scratch_.buffer, 1, &c);
                    bi.buffer = diag_scratch_.buffer;
                    bi.offset = at;
                    at += bi.range;
                }
                if (dense_logged_++ == 0) {
                    SSPC_LOG(GPU, Warning, "DIAGNOSTIC: dense copy of the dispatch's {} buffers: {} bytes, fits {}",
                             dup_infos.size(), at, fits);
                }
                if (fits) {
                    GlobalBarrier();
                    std::vector<VkWriteDescriptorSet> dup_writes = writes;
                    for (auto& w : dup_writes) {
                        if (w.pBufferInfo != nullptr) {
                            w.pBufferInfo = dup_infos.data() + (w.pBufferInfo - buf_infos.data());
                        }
                    }
                    if (PushDescriptors(*pipe, VK_PIPELINE_BIND_POINT_COMPUTE, dup_writes)) {
                        ProfMark(3, "before a command: its barriers, uploads, render pass changes");
                        f.vkCmdDispatch(cur_.cb, d.groups[0], d.groups[1], d.groups[2]);
                        ProfMark(DiagKey(variant), variant == 9    ? "DIAG: the same dispatch, all its buffers in a dense copy"
                                          : variant == 10 ? "DIAG: the same dispatch, read buffers dense, written in the mirror"
                                                          : "DIAG: the same dispatch, written buffers dense, read in the mirror");
                        if (variant == 11) {
                            // the dense outputs copied back over the mirror's (the same values): a copy's writes
                            GlobalBarrier();
                            ProfMark(3, "before a command: its barriers, uploads, render pass changes");
                            u32 copies = 0;
                            for (size_t k = 0; k < dup_infos.size() && k < csv.plan.buffers.size(); ++k) {
                                if (csv.plan.buffers[k].written && dup_infos[k].buffer == diag_scratch_.buffer) {
                                    const VkBufferCopy c{.srcOffset = dup_infos[k].offset,
                                                         .dstOffset = buf_infos[k].offset,
                                                         .size = dup_infos[k].range};
                                    f.vkCmdCopyBuffer(cur_.cb, diag_scratch_.buffer, buf_infos[k].buffer, 1, &c);
                                    ++copies;
                                }
                            }
                            ProfMark(DiagKey(12), "DIAG: the written buffers copied from the dense copy into the mirror");
                            // the same bytes through a shader-free fill-free path: dense -> dense
                            ProfMark(3, "before a command: its barriers, uploads, render pass changes");
                            for (size_t k = 0; k < dup_infos.size() && k < csv.plan.buffers.size(); ++k) {
                                if (csv.plan.buffers[k].written && dup_infos[k].buffer == diag_scratch_.buffer) {
                                    const VkBufferCopy c{.srcOffset = dup_infos[k].offset,
                                                         .dstOffset = (8ull << 20) + dup_infos[k].offset,
                                                         .size = dup_infos[k].range};
                                    f.vkCmdCopyBuffer(cur_.cb, diag_scratch_.buffer, diag_scratch_.buffer, 1, &c);
                                }
                            }
                            ProfMark(DiagKey(13), "DIAG: the same copies dense -> dense");
                            (void)copies;
                        }
                    }
                }
            }
        }
    }
    FinishShadows();
    ++stats_.dispatches_executed;
    if (writes_memory) {
        pending_writes_ = true;
    }
    stats_.t_dispatch += GS::Ticks() - t0;
    stats_.record_ns += GS::TicksToNs(GS::Ticks() - t0);
}

void Executor::Flip(const Cp::FlipMarker&) {
    ++stats_.flips;
    if (prof_ && ++prof_frames_ >= 300) {
        ProfReport();
    }
    write_run_ = false;
    VideoOut::BufferInfo buf;
    s32 index = -1;
    s32 frame = -1;
    const bool has = VideoOut::NextFlipBuffer(buf, index);
    if (has && presenter_ != nullptr && Begin()) {
        EndRendering();
        CachedImage* img = images_.FindTargetAt(buf.address);
        std::string err;
        if (img == nullptr) {
            // not rendered by the GPU (a CPU-drawn buffer): from guest memory
            const ImageLayout layout = LayoutFromVideoOut(buf.address, buf.width, buf.height, buf.pitch,
                                                          buf.pixel_format, buf.tiling_mode);
            img = images_.Find(layout, ImageCache::Use::Texture, cur_.cb, cur_.value, err);
        }
        if (img != nullptr) {
            frame = presenter_->AcquireFrame(250);
            if (frame >= 0) {
                if (pending_writes_) {
                    GlobalBarrier();
                }
                images_.Transition(cur_.cb, *img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                const u32 w = std::min(std::max(buf.width, 1u), img->layout.size.width);
                const u32 h = std::min(std::max(buf.height, 1u), img->layout.size.height);
                // the scanout reads the bytes in the VideoOut buffer's pixel format (the bridge views the image with
                // it): A8R8G8B8 is B,G,R,A in memory, whatever format the colour target was created with
                const VkFormat shown = DisplayFormat(buf.pixel_format, img->format);
                const u64 cid = DiagBegin(fmt::format("flip copy {} as fmt {} {}x{} -> frame {}", DescribeImage(*img),
                                                      static_cast<int>(shown), w, h, frame));
                if (!presenter_->RecordCopy(cur_.cb, frame, img->image, shown, w, h)) {
                    presenter_->ReleaseFrame(frame);
                    frame = -1;
                }
                DiagEnd(cid);
                ProfMark(2, "flip copy (frame to the presenter)");
            }
        } else if (stats_.flips_blank == 0) {
            SSPC_LOG(GPU, Warning, "native backend: the displayed buffer {:#x} has no image: {}", buf.address, err);
        }
        ++commands_;
    }
    if (frame < 0) {
        ++stats_.flips_blank;
    }
    if (options_.capture_flip != 0 && presenter_ != nullptr && VideoOut::NextFlipId() == options_.capture_flip) {
        presenter_->RequestCapture(options_.capture_flip, options_.capture_path);
    }
    const u64 value = open_ ? cur_.value : timeline_.LastSubmitted();
    ++stats_.flushes_flip;
    Flush();
    VideoOut::OnFlipMarker(value, frame);
    frontend_.NewFrame();
}

} // namespace SecondSon::Gpu::Vk
