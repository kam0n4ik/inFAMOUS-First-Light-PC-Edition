// SecondSonPC — the guest's buffers in video memory (P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §E).
//
// Measured (logs\p44\diag\p3/p6): the GPU reading the guest's buffers straight from imported host memory (PCIe) cost
// most of the frame (the tiled lighting dispatch alone 30 ms; 0.8 ms once its inputs were in video memory). This cache
// mirrors the guest's GPU address space in one sparse buffer (offset = guest address; RTX 2060: 1 TiB sparse address
// space = 2^40), with device-local memory bound in 64 KiB blocks where the GPU touches it:
//  - a GPU read of a range uploads its pages that are not valid in video memory (a GPU copy from the host import),
//    then write-protects them (PageTracker): the guest's next CPU write invalidates them;
//  - a GPU write goes to video memory; the batch's written ranges are copied back to the host at the batch's end (before
//    its fences are written), so the CPU, the command processor and texture uploads see them after completion;
//  - writes the GPU does not see through the tracker (the command processor's DMA/WRITE_DATA, file reads, unmaps)
//    invalidate explicitly (Invalidate, any thread; applied on the GPU thread).
// Ranges that cannot be tracked (imported through the guest's own pages: no backing) are not cached: the host import.
// One thread records (the GPU thread).
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu {
class PageTracker;
}

namespace SecondSon::Gpu::Vk {

class DeviceContext;
class GuestBuffers;
class Timeline;

struct BufferCacheStats {
    u64 resident_bytes = 0, binds = 0, bind_calls = 0;
    u64 uploads = 0, upload_bytes = 0, writebacks = 0, writeback_bytes = 0;
    u64 invalidations = 0, uncached = 0, over_budget = 0, large_ranges = 0, early_writebacks = 0;
    u64 t_large = 0, t_bound = 0, t_scan = 0, t_hook = 0, t_arm = 0, t_copy = 0; ///< TSC ticks inside Prepare
    u64 n_upload_calls = 0;
    std::string first_error;
};

class BufferCache {
public:
    /// `host_wait_binds`: the sparse binds complete on the host before the batch is submitted, instead of the batch
    /// waiting for their timeline on the queue (with --gpu-native-validate: synchronization validation does not track
    /// vkQueueBindSparse's semaphore signals - a batch waiting for one stays unresolved for it and every later present
    /// is reported as WRITE_AFTER_PRESENT; logs\p44\diag c6/c7). Both orderings are valid.
    BufferCache(DeviceContext& ctx, GuestBuffers& guest, PageTracker* tracker, u64 budget_bytes, bool host_wait_binds);
    ~BufferCache(); ///< frees everything (the device must be idle)

    bool Ok() const { return buffer_ != VK_NULL_HANDLE; }

    struct Range {
        VkBuffer buffer = VK_NULL_HANDLE;
        u64 offset = 0;   ///< of the guest address in `buffer`
        u64 size = 0;     ///< bytes from offset the binding may span
        bool cached = false;
    };
    /// Called before upload commands are recorded (the executor ends rendering and orders earlier writes).
    void SetBeforeUpload(void (*hook)(void* ctx), void* ctx) {
        before_upload_ = hook;
        before_upload_ctx_ = ctx;
    }
    /// The GPU accesses guest [addr, addr + size) in commands of submission `seq` recorded into `cb`: the video memory
    /// copy, with its invalid pages uploaded first (recorded into `cb`), and a write recorded for the write-back.
    /// false: not even the host import (the caller reports the resource).
    bool Prepare(u64 addr, u64 size, bool write, VkCommandBuffer cb, u64 seq, Range& out);
    /// [addr, addr + size) is valid in video memory (a texture upload reads it there: newer than the host's).
    bool Resident(u64 addr, u64 size, Range& out);
    /// Some page of [addr, addr + size) is valid in video memory (then the host's copy may be older).
    bool AnyValid(u64 addr, u64 size);
    enum class Residency : u8 { None, Partial, All };
    /// How much of a range (up to the cached size) is valid in video memory: one pass. All: `out` set.
    Residency Classify(u64 addr, u64 size, Range& out);
    /// The guest wrote or unmapped [addr, addr + size) behind the GPU's back: the pages are uploaded again. Any thread.
    void Invalidate(u64 addr, u64 size);
    /// The GPU writes [addr, addr + size) of the host's copy in submission `seq` (a range not cached): until `seq`
    /// completes, the host's bytes there are older than the GPU's (no CPU copy may read them).
    void NoteHostWrite(u64 addr, u64 size, u64 seq);
    bool HostWritePending(u64 addr, u64 size) const;
    void Collect(u64 completed);
    /// The end of a batch: the copies of its GPU writes back to the host (`cb` outside rendering).
    void RecordWriteBack(VkCommandBuffer cb, u64 seq);
    /// Before the batch is submitted: binds the memory its commands need (vkQueueBindSparse, the queue's lock held by
    /// the caller); `wait_value` of `wait` is what the batch waits for (0: nothing to wait for).
    bool SubmitBinds(VkSemaphore& wait, u64& wait_value);

    const BufferCacheStats& Stats() const { return stats_; }
    VkBuffer Buffer() const { return buffer_; }

private:
    static constexpr u64 kBlockBits = 16; ///< 64 KiB residency blocks
    static constexpr u64 kPageBits = 12;  ///< 4 KiB validity pages (the tracker's)
    static constexpr u64 kPagesPerBlock = 1ull << (kBlockBits - kPageBits);
    static constexpr u64 kChunkBytes = 64ull << 20; ///< device memory allocations bound block by block
    /// Larger bindings (often a whole pool of which a draw reads a part) stay on the host import: caching them would
    /// upload and keep the whole pool. Every hot range measured on the reference route is below.
    static constexpr u64 kMaxCached = 16ull << 20;
    struct Block {
        u16 valid = 0; ///< pages whose video memory copy is current
        bool bound = false;
    };
    void ApplyInvalidations();
    bool EnsureBound(u64 first_block, u64 last_block);
    bool Upload(u64 addr, u64 size, VkCommandBuffer cb, u64 seq);
    void FlushWritten(u64 addr, u64 size, VkCommandBuffer cb, u64 seq);

    DeviceContext& ctx_;
    GuestBuffers& guest_;
    PageTracker* tracker_;
    u64 budget_;
    VkBuffer buffer_ = VK_NULL_HANDLE;
    u64 space_ = 0; ///< the mirrored guest addresses: [0, space_)
    u64 block_alignment_ = 1ull << kBlockBits;
    u32 memory_type_ = 0;
    std::vector<VkDeviceMemory> chunks_;
    u64 chunk_used_ = kChunkBytes; ///< bytes of the last chunk given out
    std::unordered_map<u64, Block> blocks_;
    std::vector<VkSparseMemoryBind> pending_binds_;
    VkSemaphore bind_timeline_ = VK_NULL_HANDLE;
    u64 bind_value_ = 0;
    std::vector<std::pair<u64, u64>> written_; ///< this batch's GPU-written ranges [begin, end)
    struct HostWrite {
        u64 begin, end, seq;
    };
    std::vector<HostWrite> host_writes_; ///< not completed
    u64 hw_lo_ = ~0ull, hw_hi_ = 0;
    std::vector<std::pair<u64, u64>> uploads_;  ///< scratch
    std::mutex inval_mutex_;
    std::atomic<bool> has_inval_{false};
    std::vector<std::pair<u64, u64>> pending_inval_;
    bool host_wait_binds_ = false; ///< the binds complete on the host (validation runs; SSPC_DIAG_BIND_HOST_WAIT=1)
    void (*before_upload_)(void*) = nullptr;
    void* before_upload_ctx_ = nullptr;
    BufferCacheStats stats_;
};

} // namespace SecondSon::Gpu::Vk
