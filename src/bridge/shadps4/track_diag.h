// SecondSonPC — P2.5 diagnostics: what the bridge's GPU write tracking costs (page protection + access
// violations, docs/PHASE2_P25_PERFORMANCE.md).
//
// Hooked into shadPS4 by patches/shadps4/0014-diag-gpu-write-tracking-stats.patch. Observation only:
// every hook records counters and times and changes nothing. Off unless --track-stats: then each hook
// is one load of a flag that never changes after start-up. With it on: totals and rates every 10 s
// in SecondSonPC.log ("track stats"), per-frame columns in --frame-log's frametimes.csv.
#pragma once

#include <cstdint>

namespace SecondSon::TrackDiag {

extern bool g_enabled; // written once by Enable(), before the bridge starts

inline bool Enabled() {
    return g_enabled;
}

std::uint64_t Ticks(); // time stamp counter

/// --track-stats: allocates the tables, starts the 10 s summary thread.
void Enable();

// core/signals.cpp: one access violation through the bridge's handler list (`start` = Ticks() before
// the dispatch); the time since SecondSonPC's first-chance handler saw the same fault is recorded too.
void OnDispatch(std::uint64_t start, bool handled);

// video_core/page_manager.cpp, GuestFaultSignalHandler: one tracked fault, its handling time.
class FaultScope {
public:
    FaultScope(std::uint64_t addr, bool write, bool gpu_thread);
    ~FaultScope();
    void SetHandled(bool handled) {
        handled_ = handled;
    }
    FaultScope(const FaultScope&) = delete;
    FaultScope& operator=(const FaultScope&) = delete;

private:
    std::uint64_t start_ = 0;
    std::uint64_t addr_ = 0;
    bool write_ = false;
    bool handled_ = false;
    bool active_ = false;
};

// SignalImpl::Protect: one change of tracking protection (inside a fault handler or not).
void OnProtect(std::uint64_t size, bool allow_write, std::uint64_t start);

// BufferCache: CPU-modified ranges uploaded to the GPU (SynchronizeMemory), GPU-modified data read
// back to guest memory (DownloadMemory, includes the wait for the GPU), a fault that found
// GPU-modified data (InvalidateRegion -> flush).
void OnUpload(std::uint64_t bytes, std::uint32_t ranges, std::uint64_t start);
void OnReadback(std::uint64_t bytes, std::uint64_t start);
void OnFlushRequest();

// TextureCache::InvalidateMemory: images found on the faulting page(s).
void OnTextureInvalidate(std::uint32_t images);

// GPU thread work per draw (P2.5 profile: Image::GetBarriers, TextureCache::FindImage and the
// stream-buffer copies of BufferCache::ObtainBuffer are its hottest parts):
// Image::GetBarriers: subresource states visited, barriers emitted, the image's levels*layers, whether
// the request was for part of the image, whether the image had per-subresource states already.
void OnBarriers(std::uint32_t iterations, std::uint32_t emitted, std::uint32_t resources, bool partial_request,
                bool partially_transited);
void OnFindImage();
void OnStreamCopy(std::uint64_t bytes);
// A stream-buffer copy of [addr, addr + bytes) whose content hashes to `content` in frame `frame`:
// counts copies that repeat an earlier copy of the same range in the same frame (and with the
// same content) - what a cache of stream copies could skip.
void OnStreamCopyContent(std::uint64_t addr, std::uint64_t bytes, std::uint64_t content, std::uint32_t frame);

/// Running totals (for --frame-log's per-frame deltas).
struct Totals {
    std::uint64_t faults = 0, fault_ticks = 0, dispatch_ticks = 0, chain_ticks = 0;
    std::uint64_t protect_track = 0, protect_untrack = 0, pages_track = 0, pages_untrack = 0, protect_ticks = 0;
    std::uint64_t uploads = 0, upload_bytes = 0, upload_ticks = 0;
    std::uint64_t readbacks = 0, readback_bytes = 0, readback_ticks = 0, flushes = 0, tex_images = 0;
};
Totals Take();
double TicksPerMicrosecond();

} // namespace SecondSon::TrackDiag
