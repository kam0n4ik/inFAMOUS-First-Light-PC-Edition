// SecondSonPC — SecondSonPC's own GPU backend (P4.4). See native_backend.h.
#include <windows.h>

#include "runtime/gpu/native_backend.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "base/lifecycle.h"
#include "base/log.h"
#include "runtime/gpu/backend.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/gnm/gnm_driver.h"
#include "runtime/gpu/gnm/gnm_host.h"
#include "runtime/gpu/gpu_memory.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/page_tracker.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_diagnostics.h"
#include "runtime/gpu/vulkan/vk_executor.h"
#include "runtime/gpu/vulkan/vk_image_cache.h"
#include "runtime/gpu/vulkan/vk_pipelines.h"
#include "runtime/gpu/vulkan/vk_presenter.h"
#include "runtime/gpu/vulkan/vk_resources.h"

namespace SecondSon::Gpu::Native {

namespace {

namespace GS = Gpu::Shader;

/// The command processor's view of memory: guest memory (40 bits) and VideoOut's labels (host memory).
class Memory final : public Cp::CommandMemory {
public:
    PageTracker* tracker = nullptr;
    Vk::BufferCache* buffers = nullptr; ///< its video memory copy of what the processor writes is invalidated
    Vk::Executor* executor = nullptr; ///< its writes on the GPU's timeline land before a read of them
    bool Read(u64 addr, void* dst, u64 bytes) override {
        if (VideoOut::LabelRead(addr, dst, bytes)) {
            return true;
        }
        if (executor != nullptr && executor->TimelineWritePending(addr, bytes)) {
            executor->SyncFromMemoryRead();
        }
        GuestMemory* gm = GetGuestMemory();
        if (gm == nullptr || !GuestMemory::IsValidGpuMapping(addr, bytes)) {
            return false;
        }
        gm->CopySparse(addr, static_cast<u8*>(dst), bytes);
        return true;
    }
    bool Write(u64 addr, const void* src, u64 bytes) override {
        if (VideoOut::LabelWrite(addr, src, bytes)) {
            return true;
        }
        GuestMemory* gm = GetGuestMemory();
        if (gm == nullptr || !GuestMemory::IsValidGpuMapping(addr, bytes)) {
            return false;
        }
        if (gm->TryWriteBacking(reinterpret_cast<void*>(addr), src, bytes)) {
            if (buffers != nullptr) {
                buffers->Invalidate(addr, bytes); // written past the tracker: the video memory copy is older
            }
            return true;
        }
        // memory without physical backing (flexible, anonymous): written in place when the pages are writable (a
        // texture's write-protected pages given back first: its copy is stale after this write)
        if (tracker != nullptr) {
            tracker->Disarm(addr, bytes);
        }
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
            (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) == 0 ||
            reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize < addr + bytes) {
            return false;
        }
        std::memcpy(reinterpret_cast<void*>(addr), src, bytes);
        if (buffers != nullptr) {
            buffers->Invalidate(addr, bytes);
        }
        return true;
    }
};

struct Submission {
    enum class Kind : u8 { Graphics, MapQueue, UnmapQueue, DingDong, Done } kind = Kind::Graphics;
    u64 addr = 0;
    u32 dwords = 0;
    const u32* data = nullptr;
    u32 vqid = 0, pipe = 0, ring_dwords = 0;
    u64 read_ptr = 0;
};

class Backend final : public Gnm::Host, public MemoryListener, public VideoOut::Sink {
public:
    // ---- Gnm::Host (the game's threads)
    bool SubmitGraphics(u64 addr, u32 dwords, const u32* data) override {
        Push({.kind = Submission::Kind::Graphics, .addr = addr, .dwords = dwords, .data = data});
        return true;
    }
    bool MapComputeQueue(u32 vqid, u32 pipe, u64 ring_addr, u32 ring_dwords, u64 read_ptr_addr) override {
        Push({.kind = Submission::Kind::MapQueue, .addr = ring_addr, .vqid = vqid, .pipe = pipe,
              .ring_dwords = ring_dwords, .read_ptr = read_ptr_addr});
        return true;
    }
    void UnmapComputeQueue(u32 vqid) override {
        Push({.kind = Submission::Kind::UnmapQueue, .vqid = vqid});
    }
    bool DingDong(u32 vqid, u32 next_offset_dw) override {
        Push({.kind = Submission::Kind::DingDong, .dwords = next_offset_dw, .vqid = vqid});
        return true;
    }
    void SubmitDone() override {
        Push({.kind = Submission::Kind::Done});
    }
    bool IsGpuIdle() override {
        return idle_.load(std::memory_order_acquire);
    }
    s32 SubmitEopFlip(s32 handle, u32 index, u32 mode, s64 arg) override {
        return VideoOut::SubmitEopFlip(handle, index, mode, arg);
    }
    bool BufferLabelAddress(s32 handle, u64& address) override {
        return VideoOut::BufferLabelAddress(handle, address);
    }

    // ---- MemoryListener (the guest memory manager's threads)
    void OnUnmap(u64 addr, u64 size) override {
        tracker_.Forget(addr, size);
        if (guest_) {
            guest_->Drop(addr, size);
        }
        Invalidate(addr, size);
    }
    void OnRegister(u64, u64) override {}
    void OnMap(u64 addr, u64 size) override {
        tracker_.Forget(addr, size);
        Invalidate(addr, size);
    }
    void OnGuestWrite(u64 addr, u64 size) override {
        Invalidate(addr, size);
    }

    // ---- VideoOut::Sink (the present thread)
    bool Ready(u64 value) override {
        return value == 0 || (timeline_ && timeline_->Completed() >= value);
    }
    void Present(const VideoOut::FlipRequest& r) override {
        if (presenter_) {
            presenter_->Present(r.frame, r.flip_id);
        }
    }

    bool Start(const Options& o, std::string& error);
    void Stop();
    std::string Report();

private:
    void Push(Submission s) {
        {
            std::scoped_lock lock{mutex_};
            queue_.push_back(s);
            idle_.store(false, std::memory_order_release);
        }
        cv_.notify_one();
    }
    void Invalidate(u64 addr, u64 size) {
        if (images_) {
            images_->Invalidate(addr, size);
        }
        if (buffers_) {
            buffers_->Invalidate(addr, size);
        }
        std::scoped_lock lock{inval_mutex_};
        pending_inval_.emplace_back(addr, size);
    }
    void Loop();
    void Apply(const Submission& s);

    Options options_;
    std::unique_ptr<Vk::DeviceContext> ctx_;
    std::unique_ptr<Vk::Diagnostics> diag_; ///< destroyed after everything recording into the device
    std::unique_ptr<Vk::Timeline> timeline_;
    std::unique_ptr<Vk::StreamRing> ring_;
    std::unique_ptr<Vk::StreamRing> shadow_ring_; ///< dense video memory dispatches write (Executor::PrepareShadows)
    std::unique_ptr<Vk::GuestBuffers> guest_;
    std::unique_ptr<Vk::BufferCache> buffers_; ///< the guest's buffers in video memory
    std::unique_ptr<Vk::ImageCache> images_;
    std::unique_ptr<Vk::PipelineFactory> pipelines_;
    std::unique_ptr<Vk::Presenter> presenter_;
    std::unique_ptr<GS::ShaderFrontend> frontend_;
    std::unique_ptr<Vk::Executor> executor_;
    std::unique_ptr<Cp::CommandProcessor> cp_;
    Vk::OwnedBuffer gds_;
    GS::DeviceProfile profile_;
    Memory memory_;
    PageTracker tracker_;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Submission> queue_;
    std::atomic<bool> idle_{true};
    std::atomic<bool> stop_{false};
    std::atomic<bool> device_lost_{false};
    std::thread thread_;
    bool submit_done_ = false;
    std::array<bool, Cp::kNumQueues> mapped_{};
    std::mutex inval_mutex_;
    std::vector<std::pair<u64, u64>> pending_inval_;
    u64 submissions_ = 0, cp_errors_ = 0;
    std::string first_cp_error_;
};

bool Backend::Start(const Options& o, std::string& error) {
    options_ = o;
    Vk::ContextOptions co;
    co.device_index = o.device_index;
    co.validation = o.validation;
    co.presentation = true;
    co.diagnostics = o.diagnostics;
    co.validation_log = o.logs_dir / "vk_validation.log";

    ctx_ = Vk::DeviceContext::Create(co, error);
    if (!ctx_) {
        return false;
    }
    if (!ctx_->Caps().external_memory_host) {
        error = "the device has no VK_EXT_external_memory_host (guest buffers are imported)";
        return false;
    }
    ctx_->QueryProfile(profile_);
    timeline_ = std::make_unique<Vk::Timeline>(*ctx_);
    // the stream ring: small read-only ranges and user data each command (~35 MiB a frame measured): room for 4 frames
    // in the BAR heap, so recording never waits for the GPU to free it
    Vk::HostPlacement ring_place = Vk::HostPlacement::Cached;
    u64 ring_size = 160ull << 20;
    if (char v[16] = {}; GetEnvironmentVariableA("SSPC_DIAG_RING", v, sizeof(v)) > 0 && std::string_view(v) == "bar") {
        ring_place = Vk::HostPlacement::Bar;
        ring_size = 128ull << 20;
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_RING=bar - the stream ring ({} MiB) in device-local "
                               "host-visible memory (BAR) instead of cached system memory",
                 ring_size >> 20);
    }
    ring_ = std::make_unique<Vk::StreamRing>(*ctx_, *timeline_, ring_size, ring_place);
    if (!timeline_->Ok() || !ring_->Ok()) {
        error = "the timeline or the stream ring could not be created";
        return false;
    }
    // written buffers of dispatches: ~10 MiB a frame measured (the skinning's 21 x 420 KiB first)
    shadow_ring_ = std::make_unique<Vk::StreamRing>(*ctx_, *timeline_, 128ull << 20, Vk::HostPlacement::Device);
    if (!shadow_ring_->Ok()) {
        SSPC_LOG(GPU, Warning, "native backend: no write shadow ring: dispatches write the buffer cache in place");
        shadow_ring_.reset();
    }
    {
        const auto& mem = ctx_->Memory();
        const u32 t = ring_->MemoryType();
        const VkMemoryType& mt = mem.memoryTypes[t];
        SSPC_LOG(GPU, Info, "native backend: the stream ring: {} MiB, memory type {} (flags {:#x}), heap {} ({} MiB, "
                 "flags {:#x})",
                 ring_size >> 20, t, static_cast<u32>(mt.propertyFlags), mt.heapIndex,
                 mem.memoryHeaps[mt.heapIndex].size >> 20, static_cast<u32>(mem.memoryHeaps[mt.heapIndex].flags));
    }
    if (!Vk::CreateBuffer(*ctx_, Cp::kGdsBytes,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          true, gds_, error)) {
        return false;
    }
    std::memset(gds_.mapped, 0, Cp::kGdsBytes);
    guest_ = std::make_unique<Vk::GuestBuffers>(*ctx_);
    buffers_ = std::make_unique<Vk::BufferCache>(*ctx_, *guest_, &tracker_, 2560ull << 20, o.validation);
    images_ = std::make_unique<Vk::ImageCache>(*ctx_, *guest_);
    pipelines_ = std::make_unique<Vk::PipelineFactory>(*ctx_, profile_);
    presenter_ = Vk::Presenter::Create(*ctx_, *timeline_, o.sdl_window, error);
    if (!presenter_) {
        return false;
    }
    GS::FrontendOptions fo;
    fo.async = false; // modules ready before the draw that needs them (no draw waits for a worker)
    fo.translate = true;
    fo.profile = &profile_;
    frontend_ = std::make_unique<GS::ShaderFrontend>(fo);
    Vk::ExecutorOptions eo;
    eo.capture_flip = o.capture_flip;
    eo.capture_path = o.logs_dir / "p44" / "native_frame.png";
    if (char v[16] = {}; GetEnvironmentVariableA("SSPC_DIAG_BATCH_DRAWS", v, sizeof(v)) > 0) {
        eo.max_draws_per_batch = std::max<u32>(16, static_cast<u32>(std::strtoul(v, nullptr, 10)));
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC EXPERIMENT: SSPC_DIAG_BATCH_DRAWS={} - a batch is submitted after that many "
                               "commands", eo.max_draws_per_batch);
    }
    images_->SetBufferCache(buffers_.get());
    executor_ = std::make_unique<Vk::Executor>(*ctx_, *timeline_, *ring_, *guest_, *buffers_, *images_, *pipelines_,
                                               *frontend_, presenter_.get(), gds_, eo);
    Cp::Options cpo;
    cpo.mode = Cp::Mode::Isolated;
    cpo.memory = &memory_;
    cpo.work = executor_.get();
    cpo.gds = gds_.mapped;
    cp_ = std::make_unique<Cp::CommandProcessor>(cpo);
    executor_->Attach(cp_.get());
    executor_->SetWriteShadowRing(shadow_ring_.get());
    diag_ = std::make_unique<Vk::Diagnostics>(*ctx_, o.diagnostics, o.logs_dir);
    executor_->SetDiagnostics(diag_.get());
    images_->SetDiagnostics(diag_.get());
    // one owner of the guest GPU: every resource, or none
    for (u8 r = 0; r < static_cast<u8>(Resource::Count); ++r) {
        if (!Claim(static_cast<Resource>(r), BackendKind::Sspc, &error)) {
            for (u8 k = 0; k < r; ++k) {
                Release(static_cast<Resource>(k), BackendKind::Sspc);
            }
            return false;
        }
    }
    GuestMemory* gm = GetGuestMemory();
    if (gm == nullptr || !gm->SetListener(this, BackendKind::Sspc, &error)) {
        if (gm == nullptr) {
            error = "no guest memory contract installed";
        }
        for (u8 r = 0; r < static_cast<u8>(Resource::Count); ++r) {
            Release(static_cast<Resource>(r), BackendKind::Sspc);
        }
        return false;
    }
    if (!tracker_.Start(*gm, error)) {
        gm->SetListener(nullptr, BackendKind::Sspc, nullptr);
        for (u8 r = 0; r < static_cast<u8>(Resource::Count); ++r) {
            Release(static_cast<Resource>(r), BackendKind::Sspc);
        }
        return false;
    }
    images_->SetTracker(&tracker_);
    guest_->SetTracker(&tracker_);
    memory_.tracker = &tracker_;
    memory_.buffers = buffers_.get();
    memory_.executor = executor_.get();
    Gnm::SetSdkVersion(o.sdk_version);
    Gnm::SetHost(this);
    VideoOut::SetLabelCallback([](void* ctx) { static_cast<Backend*>(ctx)->cv_.notify_one(); }, this);
    VideoOut::Start(this);
    stop_.store(false);
    thread_ = std::thread([this] { Loop(); });
    SSPC_LOG(GPU, Info, "native backend started on {} (the game's GPU work runs on SecondSonPC's Vulkan backend)",
             ctx_->Name());
    return true;
}

void Backend::Apply(const Submission& s) {
    switch (s.kind) {
    case Submission::Kind::Graphics:
        ++submissions_;
        cp_->SubmitGraphics(s.addr, s.dwords, s.data);
        break;
    case Submission::Kind::MapQueue:
        if (s.vqid < Cp::kNumQueues && cp_->MapComputeQueue(s.vqid, s.addr, s.ring_dwords, s.read_ptr)) {
            mapped_[s.vqid] = true;
            executor_->SetQueuePipe(s.vqid, s.pipe);
        }
        break;
    case Submission::Kind::UnmapQueue:
        if (s.vqid < Cp::kNumQueues) {
            cp_->UnmapComputeQueue(s.vqid);
            mapped_[s.vqid] = false;
        }
        break;
    case Submission::Kind::DingDong:
        ++submissions_;
        cp_->DingDong(s.vqid, s.dwords);
        break;
    case Submission::Kind::Done:
        submit_done_ = true;
        break;
    }
}

void Backend::Loop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:GpuNative");
    std::deque<Submission> local;
    auto rate_t0 = std::chrono::steady_clock::now();
    u64 rate_flips = 0, rate_draws = 0, rate_record = 0, rate_wait = 0, rate_tw = 0, rate_gpu = 0, rate_submit = 0;
    u64 rate_front = 0, rate_bind = 0, rate_range = 0, rate_image = 0, rate_streamed = 0, rate_streamed_bytes = 0;
    u64 rate_pipe = 0, rate_targets = 0, rate_vertex = 0, rate_layout = 0, rate_emit = 0, rate_dispatch = 0;
    u64 rate_ring_waits = 0, rate_classify = 0, rate_hostptr = 0, rate_alloc = 0, rate_copy = 0, rate_prepare = 0;
    u64 rate_nprep = 0, rate_tl = 0, rate_tb = 0, rate_ts = 0, rate_th = 0, rate_ta = 0, rate_tc = 0, rate_nu = 0;
    u64 prof_cp = 0, prof_poll = 0, prof_flush = 0, prof_idle = 0; // ns since the last rate line
    using Clock = std::chrono::steady_clock;
    const auto ns_since = [](Clock::time_point t) {
        return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
    };
    while (!stop_.load(std::memory_order_acquire)) {
        {
            std::unique_lock lock{mutex_};
            local.swap(queue_);
        }
        for (const auto& s : local) {
            Apply(s);
        }
        local.clear();
        {
            std::vector<std::pair<u64, u64>> inval;
            {
                std::scoped_lock lock{inval_mutex_};
                inval.swap(pending_inval_);
            }
            for (const auto& [a, sz] : inval) {
                frontend_->Invalidate(a, sz);
            }
        }
        // the guest's writes to tracked pages, before the work it submitted after them
        tracker_.Drain([&](u64 a, u64 sz) {
            images_->Invalidate(a, sz);
            buffers_->Invalidate(a, sz);
            frontend_->Invalidate(a, sz);
        });
        bool progress = false, blocked = false, pending = false;
        const auto cp_t0 = Clock::now();
        for (u32 q = 0; q < Cp::kNumQueues; ++q) {
            if (q != Cp::kGraphicsQueue && !mapped_[q]) {
                continue;
            }
            for (u32 i = 0; i < 4096; ++i) {
                const Cp::StepStatus st = cp_->Step(q);
                if (st == Cp::StepStatus::Packet) {
                    progress = true;
                    continue;
                }
                if (st == Cp::StepStatus::Blocked) {
                    blocked = true;
                } else if (st == Cp::StepStatus::Faulted) {
                    if (cp_errors_++ == 0) {
                        first_cp_error_ = cp_->LastError().Text();
                        SSPC_LOG(GPU, Error, "native backend: the command processor stopped a submission: {}",
                                 first_cp_error_);
                    }
                }
                break;
            }
            pending |= cp_->PendingSubmissions(q) != 0;
        }
        prof_cp += ns_since(cp_t0);
        const auto poll_t0 = Clock::now();
        executor_->Poll(); // completed batches: the processor writes its held labels now
        prof_poll += ns_since(poll_t0);
        if (executor_->DeviceLost()) {
            // VK_ERROR_DEVICE_LOST: the report is written, nothing more is submitted; the process ends with an
            // explicit status (a run with a lost device is not a successful run)
            device_lost_.store(true, std::memory_order_release);
            SSPC_LOG(GPU, Error, "native backend: stopped after VK_ERROR_DEVICE_LOST; the process exits with code 5");
            std::thread([] {
                Lifecycle::Exit(5, "native GPU backend: VK_ERROR_DEVICE_LOST (logs\\gpu_device_lost.txt)");
            }).detach();
            return;
        }
        if (const auto now = std::chrono::steady_clock::now(); now - rate_t0 >= std::chrono::seconds(10)) {
            const auto& s = executor_->Stats();
            const double secs = std::chrono::duration<double>(now - rate_t0).count();
            const double frames = std::max<double>(1.0, static_cast<double>(s.flips - rate_flips));
            const auto per_frame = [&](double ns) { return ns / 1e6 / frames; };
            SSPC_LOG(GPU, Info,
                     "native backend: {:.1f} flips/s, {:.0f} draws/s over the last {:.1f} s: recording {} ms, waiting "
                     "for the GPU {} ms, writes on the timeline {} | per frame (ms): command processor {:.1f} (of it "
                     "recording {:.1f}, waiting for the GPU {:.1f}), submit {:.1f}, poll {:.1f}, flush {:.1f}, idle {:.1f}; "
                     "GPU busy {:.1f}",
                     static_cast<double>(s.flips - rate_flips) / secs, static_cast<double>(s.draws - rate_draws) / secs,
                     secs, (s.record_ns - rate_record) / 1'000'000, (s.sync_wait_us - rate_wait) / 1000,
                     s.timeline_writes - rate_tw, per_frame(static_cast<double>(prof_cp)),
                     per_frame(static_cast<double>(s.record_ns - rate_record)),
                     per_frame(static_cast<double>(s.sync_wait_us - rate_wait) * 1000.0),
                     per_frame(static_cast<double>(s.submit_ns - rate_submit)), per_frame(static_cast<double>(prof_poll)),
                     per_frame(static_cast<double>(prof_flush)), per_frame(static_cast<double>(prof_idle)),
                     per_frame(static_cast<double>(s.gpu_busy_ns - rate_gpu)));
            SSPC_LOG(GPU, Info,
                     "native backend: recording per frame (ms): frontend {:.1f}, binding {:.1f} (buffers {:.1f}, images "
                     "{:.1f}); streamed {:.0f} small ranges ({:.1f} MiB) per frame; stream ring waits {}",
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_front - rate_front))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_bind - rate_bind))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_range - rate_range))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_image - rate_image))),
                     static_cast<double>(s.streamed - rate_streamed) / frames,
                     static_cast<double>(s.streamed_bytes - rate_streamed_bytes) / 1048576.0 / frames,
                     ring_->Waits() - rate_ring_waits);
            rate_ring_waits = ring_->Waits();
            SSPC_LOG(GPU, Info,
                     "native backend: draw stages per frame (ms): pipeline {:.1f}, targets {:.1f}, vertex/index {:.1f}, "
                     "barriers/layouts {:.1f}, commands {:.1f}; dispatches whole {:.1f}",
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_pipe - rate_pipe))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_targets - rate_targets))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_vertex - rate_vertex))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_layout - rate_layout))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_emit - rate_emit))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_dispatch - rate_dispatch))));
            rate_pipe = s.t_pipe;
            rate_targets = s.t_targets;
            rate_vertex = s.t_vertex;
            rate_layout = s.t_layout;
            rate_emit = s.t_emit;
            rate_dispatch = s.t_dispatch;
            SSPC_LOG(GPU, Info,
                     "native backend: buffers per frame (ms): classify {:.1f}, host pointer {:.1f}, ring {:.1f}, copy {:.1f}, "
                     "cache prepare {:.1f} ({:.0f} calls)",
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_classify - rate_classify))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_hostptr - rate_hostptr))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_alloc - rate_alloc))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_copy - rate_copy))),
                     per_frame(static_cast<double>(GS::TicksToNs(s.t_prepare - rate_prepare))),
                     static_cast<double>(s.n_prepare - rate_nprep) / frames);
            rate_classify = s.t_classify;
            rate_hostptr = s.t_hostptr;
            rate_alloc = s.t_alloc;
            rate_copy = s.t_copy;
            rate_prepare = s.t_prepare;
            rate_nprep = s.n_prepare;
            const auto& bc = buffers_->Stats();
            SSPC_LOG(GPU, Info,
                     "native backend: cache prepare per frame (ms): large ranges {:.1f}, bind {:.1f}, scan {:.1f}, upload "
                     "hook {:.1f}, write-protect {:.1f}, copies {:.1f} ({:.0f} uploading calls)",
                     per_frame(static_cast<double>(GS::TicksToNs(bc.t_large - rate_tl))),
                     per_frame(static_cast<double>(GS::TicksToNs(bc.t_bound - rate_tb))),
                     per_frame(static_cast<double>(GS::TicksToNs(bc.t_scan - rate_ts))),
                     per_frame(static_cast<double>(GS::TicksToNs(bc.t_hook - rate_th))),
                     per_frame(static_cast<double>(GS::TicksToNs(bc.t_arm - rate_ta))),
                     per_frame(static_cast<double>(GS::TicksToNs(bc.t_copy - rate_tc))),
                     static_cast<double>(bc.n_upload_calls - rate_nu) / frames);
            rate_tl = bc.t_large;
            rate_tb = bc.t_bound;
            rate_ts = bc.t_scan;
            rate_th = bc.t_hook;
            rate_ta = bc.t_arm;
            rate_tc = bc.t_copy;
            rate_nu = bc.n_upload_calls;
            rate_streamed_bytes = s.streamed_bytes;
            rate_front = s.t_front;
            rate_bind = s.t_bind;
            rate_range = s.t_range;
            rate_image = s.t_image;
            rate_streamed = s.streamed;
            rate_gpu = s.gpu_busy_ns;
            rate_submit = s.submit_ns;
            prof_cp = prof_poll = prof_flush = prof_idle = 0;
            rate_record = s.record_ns;
            rate_wait = s.sync_wait_us;
            rate_tw = s.timeline_writes;
            rate_t0 = now;
            rate_flips = s.flips;
            rate_draws = s.draws;
        }
        if (executor_->WantsFlush() || (!progress && executor_->HasOpenWork())) {
            const auto flush_t0 = Clock::now();
            executor_->Flush();
            prof_flush += ns_since(flush_t0);
        }
        if (progress) {
            continue;
        }
        const bool queue_empty = [&] {
            std::scoped_lock lock{mutex_};
            return queue_.empty();
        }();
        if (submit_done_ && queue_empty && !pending && !blocked && !executor_->Busy() &&
            !executor_->HasOpenWork()) {
            submit_done_ = false;
            idle_.store(true, std::memory_order_release);
            Gnm::RaiseInterrupt(Gnm::Interrupt::GpuIdle);
        }
        const auto idle_t0 = Clock::now();
        if (executor_->Busy()) {
            executor_->WaitOldest(1'000'000); // 1 ms: completion unblocks labels
        } else {
            std::unique_lock lock{mutex_};
            cv_.wait_for(lock, std::chrono::milliseconds(1), [&] { return !queue_.empty() || stop_.load(); });
        }
        prof_idle += ns_since(idle_t0);
    }
}

void Backend::Stop() {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    VideoOut::Stop();
    Gnm::SetHost(nullptr);
    if (frontend_) {
        frontend_->Stop();
    }
    if (ctx_) {
        std::scoped_lock lock{ctx_->QueueMutex()};
        Vk::Fn().vkDeviceWaitIdle(ctx_->Device());
    }
    std::error_code ec;
    std::filesystem::create_directories(options_.logs_dir, ec);
    std::ofstream(options_.logs_dir / "gpu_native.txt", std::ios::trunc) << Report();
}

std::string Backend::Report() {
    std::string out = device_lost_.load() ? "STATUS: FAILED - VK_ERROR_DEVICE_LOST (logs\\gpu_device_lost.txt)\n"
                                          : "STATUS: running/stopped normally\n";
    out += fmt::format("SecondSonPC native GPU backend (P4.4)\ndevice: {}\nsubmissions: {}, "
                                  "command processor errors: {} {}\n",
                                  ctx_ ? ctx_->Name() : "-", submissions_, cp_errors_, first_cp_error_);
    if (executor_) {
        const auto& s = executor_->Stats();
        out += fmt::format("draws {} (executed {}), dispatches {} (executed {}), batches {}, completed {}, flips {} "
                           "(blank {}), syncs {} ({} us waiting), render passes {}, global barriers {}, fast clears {}, "
                           "depth clears {}, metadata writes {} (xor {}, reading metadata {}), cache actions {}\n",
                           s.draws, s.draws_executed, s.dispatches, s.dispatches_executed, s.batches,
                           s.completed_batches, s.flips, s.flips_blank, s.syncs, s.sync_wait_us, s.render_passes,
                           s.global_barriers, s.fast_clears, s.depth_clears, s.meta_writes, s.meta_writes_xor, s.meta_writes_reading, s.cache_actions);
        for (size_t i = 0; i < s.skipped.size(); ++i) {
            if (s.skipped[i] != 0) {
                out += fmt::format("  not executed ({}): {} — first: {}\n", Vk::SkipName(static_cast<Vk::Skip>(i)),
                                   s.skipped[i], s.first_skip[i]);
            }
        }
    }
    if (images_) {
        const auto i = images_->Stats();
        out += fmt::format("images {} ({} MiB), uploads {} (detiled {}, linear {}, {} MiB), overlap hits {}, target misses {}, overlap misses {}, unsupported {} {}\n",
                           i.images, i.bytes >> 20, i.uploads, i.detiles, i.linear_uploads, i.upload_bytes >> 20,
                           i.overlap_hits, i.target_misses, i.overlap_misses, i.unsupported, i.first_unsupported);
    }
    if (guest_) {
        const auto g = guest_->GetStats();
        out += fmt::format("guest buffer windows {} ({} MiB; through the backing {}, the guest's pages {}), imports {}, failures {} {}\n", g.windows, g.bytes >> 20, g.backing_windows, g.guest_windows,
                           g.imports, g.import_failures, g.first_failure);
    }
    if (cp_ && executor_) {
        const auto& c = cp_->Stats();
        const auto& s = executor_->Stats();
        out += fmt::format("target cache: hits {}, misses {}\n", s.target_cache_hits, s.target_cache_misses);
        out += fmt::format("write shadows {} ({} MiB; left in place: overlapping {}, no room {}, too large {}); written "
                           "dispatch bindings <=64 KiB {}, <=256 KiB {}, <=1 MiB {}, <=4 MiB {}, <=16 MiB {}, larger {}\n",
                           s.shadows, s.shadow_bytes >> 20, s.shadow_overlaps, s.shadow_no_room, s.shadow_too_large,
                           s.shadow_sizes[0], s.shadow_sizes[1], s.shadow_sizes[2], s.shadow_sizes[3],
                           s.shadow_sizes[4], s.shadow_sizes[5]);
        out += fmt::format("timeline copies {} ({} MiB, refused {})\n", s.timeline_copies, s.timeline_copy_bytes >> 20,
                           s.tc_refused);
        out += fmt::format("waits that waited: processor packets {} ({} ms), frontend reads {} ({} ms), processor memory "
                           "reads {} ({} ms)\n",
                           s.sync_eff[0], s.sync_eff_us[0] / 1000, s.sync_eff[1], s.sync_eff_us[1] / 1000, s.sync_eff[2],
                           s.sync_eff_us[2] / 1000);
        out += fmt::format("command processor waits for the GPU: WRITE_DATA {} (timeline refusals: shape {}, import {}), "
                           "DMA by kind [m2m {}, data2mem {}, m2gds {}, data2gds {}, gds2mem {}]; m2m sizes "
                           "<=256 B {}, <=4 KiB {}, <=64 KiB {}, larger {}\n",
                           c.sync_write_data, s.tw_fail_shape, s.tw_fail_import, c.sync_dma[0], c.sync_dma[1],
                           c.sync_dma[2], c.sync_dma[3], c.sync_dma[4], c.sync_dma_m2m_size[0], c.sync_dma_m2m_size[1],
                           c.sync_dma_m2m_size[2], c.sync_dma_m2m_size[3]);
    }
    if (buffers_) {
        const auto& b = buffers_->Stats();
        out += fmt::format("buffer cache: {} resident {} MiB ({} binds in {} calls), uploads {} ({} MiB), write-backs {} "
                           "({} MiB), invalidations {}, uncached {} (large ranges {}, early write-backs {}), over budget {} {}\n",
                           buffers_->Ok() ? "on," : "OFF,", b.resident_bytes >> 20, b.binds, b.bind_calls, b.uploads,
                           b.upload_bytes >> 20, b.writebacks, b.writeback_bytes >> 20, b.invalidations, b.uncached, b.large_ranges, b.early_writebacks,
                           b.over_budget, b.first_error);
    }
    if (pipelines_) {
        const auto& p = pipelines_->Stats();
        out += fmt::format("pipelines: graphics {}, compute {}, modules {}, failures {} {}\n", p.graphics, p.compute,
                           p.modules, p.failures, p.first_failure);
    }
    {
        const auto t = tracker_.GetStats();
        out += fmt::format("write tracking: armed pages {}, arms {}, disarms {}, write faults {} (raced {}), never-armed blocks {}, forgotten "
                           "pages {}\n",
                           t.armed_pages, t.arms, t.disarms, t.faults, t.faults_raced, t.never_armed_blocks, t.forgotten);
    }
    if (presenter_) {
        const auto p = presenter_->GetStats();
        out += fmt::format("presented {} (black {}), copies {}, captures {}, swapchain recreated {}{}\n", p.presented,
                           p.black, p.copies, p.captures, p.swapchain_recreated, p.device_lost ? ", DEVICE LOST" : "");
    }
    return out;
}

Backend* g_backend = nullptr;

} // namespace

bool Start(const Options& options, std::string& error) {
    if (g_backend != nullptr) {
        error = "the native backend is already running";
        return false;
    }
    static Backend backend; // lives for the process: the memory listener slot must never dangle
    if (!backend.Start(options, error)) {
        return false;
    }
    g_backend = &backend;
    return true;
}

void Stop() {
    if (g_backend != nullptr) {
        g_backend->Stop();
    }
}

bool Running() {
    return g_backend != nullptr;
}

std::string Report() {
    return g_backend != nullptr ? g_backend->Report() : std::string("the native backend is not running\n");
}

} // namespace SecondSon::Gpu::Native
