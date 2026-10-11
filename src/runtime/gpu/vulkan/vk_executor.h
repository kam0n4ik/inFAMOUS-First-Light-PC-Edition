// SecondSonPC — the backend's executor: the GPU side of SecondSonPC's command processor (Phase 4, P4.4;
// docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §D, §E). Implements Cp::GpuWork on the backend's GPU thread: every draw
// and dispatch the processor hands on is prepared by the shader frontend (P4.3), recorded into the open batch (a command
// buffer that signals the next value of the timeline), submitted at fences, flips, synchronization points or when full;
// once the GPU completed a batch, the processor learns it (CompleteWork) and writes the held end-of-pipe labels — never
// before. Work the backend cannot execute (an unsupported stage, format, resource) is counted by reason and still
// completed in order (its fences must come), never replaced by something else.
#pragma once

#include <array>
#include <deque>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/shader/gcn_program.h"
#include "runtime/gpu/vulkan/vk_api.h"
#include "runtime/gpu/vulkan/vk_pipelines.h"
#include "runtime/gpu/vulkan/vk_buffer_cache.h"
#include "runtime/gpu/vulkan/vk_resources.h"

namespace SecondSon::Gpu::Shader {
class ShaderFrontend;
}

namespace SecondSon::Gpu::Vk {

class Diagnostics;

class DeviceContext;
class Timeline;
class StreamRing;
class GuestBuffers;
class ImageCache;
class Presenter;
struct CachedImage;
struct OwnedBuffer;

enum class Skip : u8 {
    NotReady,        ///< the frontend has no module/pipeline for it (pending, failed, no program)
    Unsupported,     ///< a stage SecondSonPC does not prepare (GS/HS/...)
    Pipeline,        ///< pipeline creation failed
    Buffer,          ///< a buffer's guest memory cannot be read by the GPU
    SpecialBuffer,   ///< BDA page table / fault buffer (shader DMA): not supported yet
    Image,           ///< an image could not be made
    VertexFormat,    ///< a vertex attribute has no host format
    Target,          ///< a colour/depth target could not be made
    FilteredMode,    ///< fast clear elimination, FMASK decompression, primitive None: no draw on the hardware either
    MetaClear,       ///< a compute clear of CMASK/HTILE: recorded as a pending clear of its target
    Ring,            ///< the stream ring could not hold the draw's data
    Count,
};
std::string_view SkipName(Skip s);

struct ExecutorStats {
    u64 draws = 0, draws_executed = 0, dispatches = 0, dispatches_executed = 0;
    std::array<u64, static_cast<size_t>(Skip::Count)> skipped{};
    u64 batches = 0, flushes_fence = 0, flushes_flip = 0, flushes_sync = 0, flushes_full = 0, flushes_idle = 0;
    u64 syncs = 0, sync_wait_us = 0, flips = 0, flips_blank = 0, render_passes = 0, global_barriers = 0;
    u64 completed_batches = 0, cache_actions = 0, dma = 0, fast_clears = 0, depth_clears = 0;
    u64 indexed = 0, indirect = 0, vertex_bindings = 0, descriptor_sets = 0;
    u64 record_ns = 0; ///< CPU time recording draws/dispatches (TSC)
    u64 timeline_writes = 0, timeline_read_syncs = 0, uploads_ended_rendering = 0;
    u64 meta_writes = 0, meta_writes_xor = 0, meta_writes_reading = 0; ///< compute writes of known targets' metadata
    u64 gpu_busy_ns = 0, gpu_batches_timed = 0; ///< the GPU's time executing batches (timestamps)
    u64 submit_ns = 0;                          ///< CPU time in vkQueueSubmit2
    u64 lds_dispatches = 0, lds_max_bytes = 0;  ///< dispatches with the emulated LDS buffer
    u64 streamed = 0, streamed_bytes = 0; ///< small read-only ranges the CPU rewrote, copied into the stream ring
    u64 t_front = 0, t_bind = 0, t_range = 0, t_image = 0; ///< TSC ticks: frontend, resource binding (of it buffers, images)
    u64 t_classify = 0, t_hostptr = 0, t_alloc = 0, t_copy = 0, t_prepare = 0; ///< TSC ticks inside GuestRange
    u64 n_prepare = 0;
    u64 tw_fail_shape = 0, tw_fail_import = 0; ///< WriteOnTimeline refusals: size/alignment/address, no import
    u64 timeline_copies = 0, timeline_copy_bytes = 0, tc_refused = 0;
    u64 shadows = 0, shadow_bytes = 0, shadow_overlaps = 0, shadow_no_room = 0, shadow_too_large = 0;
    std::array<u64, 6> shadow_sizes{}; ///< written dispatch bindings: <=64 KiB, 256 KiB, 1 MiB, 4 MiB, 16 MiB, larger
    std::array<u64, 3> sync_eff{}, sync_eff_us{}; ///< waits that did wait: processor packets, frontend reads, processor reads
    std::string first_skip[static_cast<size_t>(Skip::Count)];
};

struct ExecutorOptions {
    u64 capture_flip = 0;                 ///< --gpu-capture-native-frame N: the presented frame of flip N to PNG
    std::filesystem::path capture_path;   ///< where (native_frame.png)
    u32 max_draws_per_batch = 4000;
};

class Executor final : public Cp::GpuWork {
public:
    Executor(DeviceContext& ctx, Timeline& timeline, StreamRing& ring, GuestBuffers& guest, BufferCache& buffers,
             ImageCache& images, PipelineFactory& pipelines, GS::ShaderFrontend& frontend, Presenter* presenter,
             OwnedBuffer& gds, const ExecutorOptions& options);
    ~Executor() override;

    /// The processor whose work this executes (its registers are read at each draw; CompleteWork is called on it).
    void Attach(Cp::CommandProcessor* cp) { cp_ = cp; }
    /// The command journal, markers and the device loss report (the backend always sets it; --gpu-native-diag turns
    /// the journal on).
    void SetDiagnostics(Diagnostics* diag) { diag_ = diag; }
    /// VK_ERROR_DEVICE_LOST was seen: nothing more is recorded or submitted (the backend stops).
    bool DeviceLost() const { return device_lost_; }
    /// Compute queue -> its Gnm pipe (interrupts of RELEASE_MEM name the pipe).
    void SetQueuePipe(u32 queue, u32 pipe);

    // Cp::GpuWork (the GPU thread, inside CommandProcessor::Run)
    void Draw(const Cp::DrawDescriptor& draw) override;
    void Dispatch(const Cp::DispatchDescriptor& dispatch) override;
    void CacheAction(const Cp::CacheActionOp& op) override;
    void Flip(const Cp::FlipMarker& marker) override;
    void Fence(const Cp::FenceOp& op) override;
    void Dma(const Cp::DmaOp& op) override;
    void Interrupt(u32 queue, Cp::FenceKind kind) override;
    void Synchronize(u32 queue) override;
    bool WriteOnTimeline(u32 queue, u64 seq, u64 dst, const void* data, u64 bytes) override;
    bool CopyOnTimeline(u32 queue, u64 seq, u64 src, u64 dst, u64 bytes) override;
    /// Dense video memory for the written buffers of dispatches (see PrepareShadows); null: they write in place.
    void SetWriteShadowRing(StreamRing* ring) { shadow_ring_ = ring; }
    /// Synchronize for a CPU read of guest memory by the command processor (its own memory reads).
    void SyncFromMemoryRead() { SyncFrom(2); }

    /// A write recorded on the GPU's timeline that has not completed overlaps [addr, addr + bytes): a CPU read of that
    /// memory has to Synchronize first (the command processor's reads, the frontend's descriptor reads).
    bool TimelineWritePending(u64 addr, u64 bytes) const;

    /// Submits the open batch (nothing when empty).
    void Flush();
    /// Completed batches -> CommandProcessor::CompleteWork (in order), resources released. The GPU thread, outside
    /// CommandProcessor::Run.
    void Poll();
    /// A fence was held since the last Flush (the processor waits for the GPU): the loop flushes.
    bool WantsFlush() const { return want_flush_; }
    bool HasOpenWork() const { return open_ && commands_ > 0; }
    bool Busy() const { return !in_flight_.empty(); }
    /// Waits up to `timeout_ns` for the oldest batch in flight; true when one completed.
    bool WaitOldest(u64 timeout_ns);

    const ExecutorStats& Stats() const { return stats_; }

private:
    struct Batch {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkDescriptorPool descriptors = VK_NULL_HANDLE;
        u64 value = 0;
        std::array<u64, Cp::kNumQueues> work{};
        u32 queues_mask_lo = 0; ///< queues with work (bit 0 graphics, 1.. compute; up to 31)
        u32 query_slot = ~0u;   ///< its two timestamps (GPU time of the batch)
        u32 prof_base = ~0u;    ///< SSPC_GPU_PROFILE: its first query in the profiler's pool
        u32 prof_count = 0;
        std::vector<u64> prof_keys; ///< the command key of each timestamp after the first
        std::vector<u32> queues;
    };
    struct RenderKey {
        std::array<VkImageView, 8> color{};
        u32 color_count = 0;
        VkImageView depth = VK_NULL_HANDLE;
        bool has_depth = false, has_stencil = false;
        u32 width = 0, height = 0, layers = 1;
        bool operator==(const RenderKey&) const = default;
    };

    bool Begin();
    void Note(u32 queue, u64 seq);
    void EndRendering();
    void GlobalBarrier();
    void Skipped(Skip why, const std::string& detail);
    bool BindStage(const void* stage_view, bool compute, PushData& push, std::vector<VkWriteDescriptorSet>& writes,
                   std::vector<VkDescriptorBufferInfo>& buffers, std::vector<VkDescriptorImageInfo>& images,
                   std::vector<CachedImage*>& bound, bool& writes_memory, const std::vector<CachedImage*>& targets);
    bool PushDescriptors(const PipelineObjects& p, VkPipelineBindPoint bind, std::vector<VkWriteDescriptorSet>& writes);
    void SetDynamicState(const Cp::RegisterFile& regs, const GS::GraphicsPipelineState& st, bool indexed);
    void RecordFrameCopy();
    void BeforeUpload();
    void OnDeviceLost(const char* where, u64 value);
    void SyncFrom(u32 source);
    /// A dispatch's written guest buffers moved to dense shadows: their contents copied in (recorded now, outside
    /// rendering, after the earlier writes); `infos` point at the shadows. FinishShadows copies them back after it.
    void PrepareShadows(const void* plan, std::vector<VkDescriptorBufferInfo>& infos);
    void FinishShadows();
    struct Shadow {
        VkBuffer home, shadow;
        u64 home_offset, shadow_offset, size;
    };
    std::vector<Shadow> shadows_;
    std::vector<u32> buf_adjust_; ///< BindStage: each guest buffer's offset from its aligned descriptor offset
    StreamRing* shadow_ring_ = nullptr;
    void DiagDumpShader(u64 address, const std::vector<u32>* code, const std::vector<u32>& spirv);
    bool dumped_shader_ = false;
    OwnedBuffer diag_scratch_; ///< SSPC_DIAG_DUP_DISPATCH: a dense copy of the dispatch's buffers
    u32 dense_logged_ = 0;
    static constexpr u64 DiagKey(u64 n) { return (0xD1A6ull << 32) | (n << 2) | 2; }
    /// The buffer a command accesses guest [addr, addr + size) through: video memory (the buffer cache), a stream copy
    /// for a small read-only range the CPU rewrote, or the host import. `avail`: bytes usable from `offset`.
    bool GuestRange(u64 addr, u64 size, bool write, u64 align, VkBuffer& buffer, u64& offset, u64& avail);
    bool GuestRangeImpl(u64 addr, u64 size, bool write, u64 align, VkBuffer& buffer, u64& offset, u64& avail);
    u64 DiagBegin(std::string text);
    void DiagEnd(u64 id);

    /// Guest memory as the frontend reads it at a draw: a write still on the GPU's timeline lands first.
    class TimelineReader final : public GS::GuestReader {
    public:
        explicit TimelineReader(Executor& e) : e_(e) {}
        bool Read(u64 addr, void* dst, u64 bytes) override;

    private:
        Executor& e_;
    };
    struct TimelineWrite {
        u64 begin = 0, end = 0, value = 0;
    };

    DeviceContext& ctx_;
    Timeline& timeline_;
    StreamRing& ring_;
    GuestBuffers& guest_;
    BufferCache& buffers_;
    ImageCache& images_;
    PipelineFactory& pipelines_;
    GS::ShaderFrontend& frontend_;
    Presenter* presenter_;
    OwnedBuffer& gds_;
    ExecutorOptions options_;
    Cp::CommandProcessor* cp_ = nullptr;
    std::array<u32, Cp::kNumQueues> queue_pipe_{};

    Batch cur_;
    bool open_ = false;
    u32 commands_ = 0;
    std::deque<Batch> in_flight_;
    std::vector<Batch> free_;
    bool rendering_ = false;
    RenderKey render_{};
    bool pending_writes_ = false; ///< storage writes since the last global barrier
    bool want_flush_ = false;
    // CMASK/HTILE addresses a compute clear marked: the target clears at its next rendering
    std::unordered_map<u64, bool> pending_meta_clear_;
    ExecutorStats stats_;
    std::vector<TimelineWrite> timeline_writes_; ///< not completed yet (pruned in Poll)
    u64 tw_lo_ = ~0ull, tw_hi_ = 0;              ///< their bounds
    bool write_run_ = false;                     ///< the commands recorded last were timeline writes
    std::vector<std::pair<u64, u64>> run_ranges_; ///< the ranges that run wrote (a write over one needs a barrier)
    TimelineReader reader_{*this};
    Diagnostics* diag_ = nullptr;
    VkQueryPool queries_ = VK_NULL_HANDLE; ///< two timestamps per batch slot
    u32 next_query_slot_ = 0;
    // SSPC_GPU_PROFILE=1: GPU time per command key (pipeline / compute shader / upload / frame copy)
    struct ProfAcc {
        u64 ns = 0, count = 0;
        std::string label;
    };
    void ProfMark(u64 key, const std::string& label);
    void ProfCollect(Batch& b);
    void ProfReport();
    /// SSPC_GPU_PROFILE: the bindings of a command key, logged once (what the hot commands read and write).
    void ProfDescribe(u64 key, const char* what, const void* stage_view, const std::vector<CachedImage*>& bound);
    std::unordered_set<u64> prof_described_;
    bool prof_ = false;

    VkQueryPool prof_pool_ = VK_NULL_HANDLE;
    u32 prof_next_batch_ = 0;
    std::unordered_map<u64, ProfAcc> prof_acc_;
    u64 prof_frames_ = 0, prof_total_ns_ = 0;
    // the translator's emulated LDS (shared memory beyond the device's): one workgroup's worth per workgroup of the
    // dispatch (the bridge: SharedMemSize() * NumWorkgroups()), in video memory, zeroed on the GPU
    OwnedBuffer lds_;
    std::vector<std::pair<u64, OwnedBuffer>> lds_retired_; ///< outgrown, until the batches using them complete
    u64 lds_groups_ = 1;
    u32 lds_group_bytes_ = 0;
    u64 lds_logged_ = 0;
    std::vector<CachedImage*> info_images_; ///< the image of each image descriptor info of the draw (nullptr: none)
    bool device_lost_ = false;
    u64 batch_first_cmd_ = 0, batch_last_cmd_ = 0;
};

} // namespace SecondSon::Gpu::Vk
