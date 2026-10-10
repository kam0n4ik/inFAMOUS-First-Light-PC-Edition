// SecondSonPC — P2.5 diagnostics of the bridge's GPU write tracking (see track_diag.h).
#include "bridge/shadps4/track_diag.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <intrin.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "base/crash.h"
#include "base/log.h"

namespace SecondSon::TrackDiag {

bool g_enabled = false;

namespace {

using u32 = std::uint32_t;
using u64 = std::uint64_t;

constexpr u32 kBuckets = 2000; // 0.1 us each: 0..200 us, then one overflow bucket
constexpr u32 kPageSlots = 1u << 17;
constexpr u32 kRegionSlots = 1u << 12;
constexpr u32 kThreadSlots = 128;
constexpr u32 kProbe = 64;

struct Histogram {
    std::array<std::atomic<u64>, kBuckets + 1> b{};
};

struct Slot {
    std::atomic<u64> key{0}; // page/region number + 1; 0 = empty
    std::atomic<u64> count{0};
};

struct ThreadSlot {
    std::atomic<u32> tid{0};
    std::atomic<u64> faults{0};
    std::atomic<u64> ticks{0};
};

struct State {
    double ticks_per_us = 1.0;
    double buckets_per_tick = 1.0;
    std::atomic<u64> faults{0}, faults_write{0}, faults_read{0}, faults_unhandled{0}, faults_gpu_thread{0};
    std::atomic<u64> fault_ticks{0}, dispatch{0}, dispatch_unhandled{0}, dispatch_ticks{0}, chain_ticks{0};
    std::atomic<u64> protect_track{0}, protect_untrack{0}, pages_track{0}, pages_untrack{0}, protect_ticks{0};
    std::atomic<u64> protect_in_fault{0};
    std::atomic<u64> uploads{0}, upload_ranges{0}, upload_bytes{0}, upload_ticks{0};
    std::atomic<u64> readbacks{0}, readback_bytes{0}, readback_ticks{0}, flushes{0}, tex_calls{0}, tex_images{0};
    std::atomic<u64> page_overflow{0}, region_overflow{0}, thread_overflow{0};
    // GPU thread (one writer, the command processor; relaxed atomics for the reader).
    std::atomic<u64> barrier_calls{0}, barrier_iterations{0}, barriers{0}, barrier_partial_req{0},
        barrier_partial_state{0}, barrier_max_resources{0}, barrier_iter_big{0};
    std::atomic<u64> find_image{0}, stream_copies{0}, stream_bytes{0};
    // Stream-buffer copies seen by range (OnStreamCopyContent; guarded by stream_lock).
    struct StreamEntry {
        u64 addr = 0, size = 0, content = 0;
        u32 frame = ~0u;
    };
    std::vector<StreamEntry> stream_seen = std::vector<StreamEntry>(1u << 16);
    std::atomic_flag stream_lock = ATOMIC_FLAG_INIT;
    std::atomic<u64> stream_repeat_frame{0}, stream_repeat_frame_bytes{0}, stream_repeat_same{0},
        stream_repeat_same_bytes{0}, stream_same_any{0}, stream_same_any_bytes{0};
    Histogram handler, chain;
    std::vector<Slot> pages = std::vector<Slot>(kPageSlots);
    std::vector<Slot> regions = std::vector<Slot>(kRegionSlots);
    std::array<ThreadSlot, kThreadSlots> threads{};
};

State* g_state = nullptr;
thread_local u32 t_fault_depth = 0;

struct GpuThreadLast {
    u64 bc = 0, bi = 0, b = 0, bpr = 0, bps = 0, big = 0, fi = 0, sc = 0, sb = 0;
    u64 rf = 0, rfb = 0, rs = 0, rsb = 0, sa = 0, sab = 0;
};
GpuThreadLast g_last; // LogLoop only

inline void Add(std::atomic<u64>& a, u64 v) {
    a.fetch_add(v, std::memory_order_relaxed);
}

void Record(Histogram& h, u64 ticks) {
    const u64 i = static_cast<u64>(double(ticks) * g_state->buckets_per_tick);
    Add(h.b[std::min<u64>(i, kBuckets)], 1);
}

void Count(std::vector<Slot>& table, u64 key, std::atomic<u64>& overflow) {
    const u64 k = key + 1;
    const u32 mask = static_cast<u32>(table.size() - 1);
    u32 i = static_cast<u32>((key * 0x9E3779B97F4A7C15ull) >> 40) & mask;
    for (u32 n = 0; n < kProbe; ++n, i = (i + 1) & mask) {
        u64 cur = table[i].key.load(std::memory_order_relaxed);
        if (cur == 0 && table[i].key.compare_exchange_strong(cur, k, std::memory_order_relaxed)) {
            cur = k;
        }
        if (cur == k) {
            Add(table[i].count, 1);
            return;
        }
    }
    Add(overflow, 1);
}

ThreadSlot* ThisThread() {
    thread_local ThreadSlot* slot = nullptr;
    if (slot != nullptr) {
        return slot;
    }
    const u32 tid = GetCurrentThreadId();
    for (auto& s : g_state->threads) {
        u32 cur = s.tid.load(std::memory_order_relaxed);
        if (cur == 0 && s.tid.compare_exchange_strong(cur, tid, std::memory_order_relaxed)) {
            cur = tid;
        }
        if (cur == tid) {
            return slot = &s;
        }
    }
    Add(g_state->thread_overflow, 1);
    return nullptr;
}

double Calibrate() {
    LARGE_INTEGER f{}, a{}, b{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const u64 t0 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&b);
    const u64 t1 = __rdtsc();
    const double us = double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart);
    return double(t1 - t0) / us;
}

using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
using SetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);

std::string ThreadName(u32 tid) {
    static const auto fn = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    std::string name;
    const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    if (h == nullptr) {
        return "(exited)";
    }
    PWSTR wide = nullptr;
    if (fn != nullptr && SUCCEEDED(fn(h, &wide)) && wide != nullptr) {
        for (const wchar_t* p = wide; *p != 0 && name.size() < 48; ++p) {
            name.push_back(*p < 128 ? static_cast<char>(*p) : '?');
        }
        LocalFree(wide);
    }
    CloseHandle(h);
    return name;
}

struct HistSnapshot {
    std::array<u64, kBuckets + 1> b{};
};

HistSnapshot Snap(const Histogram& h) {
    HistSnapshot s;
    for (u32 i = 0; i <= kBuckets; ++i) {
        s.b[i] = h.b[i].load(std::memory_order_relaxed);
    }
    return s;
}

/// "p50 a / p95 b / p99 c / p99.9 d us" of the difference now - before.
std::string Percentiles(const HistSnapshot& now, const HistSnapshot& before) {
    u64 n = 0;
    std::array<u64, kBuckets + 1> d{};
    for (u32 i = 0; i <= kBuckets; ++i) {
        d[i] = now.b[i] - before.b[i];
        n += d[i];
    }
    if (n == 0) {
        return "none";
    }
    const auto at = [&](double q) {
        const u64 want = static_cast<u64>(q * double(n - 1)) + 1;
        u64 acc = 0;
        for (u32 i = 0; i <= kBuckets; ++i) {
            acc += d[i];
            if (acc >= want) {
                return i == kBuckets ? std::string(">200") : fmt::format("{:.1f}", (i + 0.5) / 10.0);
            }
        }
        return std::string("?");
    };
    return fmt::format("p50 {} / p95 {} / p99 {} / p99.9 {} us", at(0.5), at(0.95), at(0.99), at(0.999));
}

void LogLoop() {
    State& s = *g_state;
    Totals last{};
    HistSnapshot last_handler, last_chain;
    LARGE_INTEGER f{}, t_last{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t_last);
    u64 last_write = 0, last_read = 0, last_unhandled = 0, last_in_fault = 0, last_tex_calls = 0;
    for (;;) {
        Sleep(10000);
        LARGE_INTEGER t_now{};
        QueryPerformanceCounter(&t_now);
        const double dt = double(t_now.QuadPart - t_last.QuadPart) / double(f.QuadPart);
        t_last = t_now;
        const Totals now = Take();
        const HistSnapshot h = Snap(s.handler), c = Snap(s.chain);
        const double tpus = s.ticks_per_us;
        const auto ms = [tpus](u64 ticks) { return double(ticks) / tpus / 1000.0; };
        const u64 df = now.faults - last.faults;
        const u64 w = s.faults_write.load(), r = s.faults_read.load(), un = s.faults_unhandled.load();
        const u64 inf = s.protect_in_fault.load(), tc = s.tex_calls.load();
        SSPC_LOG(App, Info,
                 "track stats ({:.1f} s): faults +{} = {:.0f}/s (write +{}, read +{}, not GPU-mapped +{}); "
                 "handler {:.1f} ms = {:.2f} us mean, {}; VEH chain from first-chance {:.1f} ms, {}; bridge "
                 "dispatch {:.1f} ms",
                 dt, df, df / dt, w - last_write, r - last_read, un - last_unhandled,
                 ms(now.fault_ticks - last.fault_ticks),
                 df ? double(now.fault_ticks - last.fault_ticks) / tpus / double(df) : 0.0,
                 Percentiles(h, last_handler), ms(now.chain_ticks - last.chain_ticks), Percentiles(c, last_chain),
                 ms(now.dispatch_ticks - last.dispatch_ticks));
        SSPC_LOG(App, Info,
                 "track stats: protect calls +{} track ({} pages) / +{} untrack ({} pages, {} in fault handlers), "
                 "{:.1f} ms; uploads +{} ({:.1f} MiB, {:.1f} ms); readbacks +{} ({:.1f} MiB, {:.1f} ms incl. GPU "
                 "wait), faults on GPU-modified data +{}; texture invalidations +{} ({} images)",
                 now.protect_track - last.protect_track, now.pages_track - last.pages_track,
                 now.protect_untrack - last.protect_untrack, now.pages_untrack - last.pages_untrack,
                 inf - last_in_fault, ms(now.protect_ticks - last.protect_ticks), now.uploads - last.uploads,
                 double(now.upload_bytes - last.upload_bytes) / 1048576.0, ms(now.upload_ticks - last.upload_ticks),
                 now.readbacks - last.readbacks, double(now.readback_bytes - last.readback_bytes) / 1048576.0,
                 ms(now.readback_ticks - last.readback_ticks), now.flushes - last.flushes, tc - last_tex_calls,
                 now.tex_images - last.tex_images);
        // Cumulative views: who faults, where, how often the same page faults again.
        std::vector<std::pair<u64, ThreadSlot*>> by_thread;
        for (auto& t : s.threads) {
            if (t.tid.load() != 0 && t.faults.load() != 0) {
                by_thread.emplace_back(t.faults.load(), &t);
            }
        }
        std::sort(by_thread.begin(), by_thread.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string threads;
        for (size_t i = 0; i < std::min<size_t>(by_thread.size(), 12); ++i) {
            const ThreadSlot& t = *by_thread[i].second;
            threads += fmt::format("{}{} [{}] {} ({:.0f} ms)", i ? "; " : "", ThreadName(t.tid.load()), t.tid.load(),
                                   by_thread[i].first, ms(t.ticks.load()));
        }
        u64 distinct = 0, hist[6] = {};
        std::vector<std::pair<u64, u64>> top_pages, top_regions;
        for (const auto& p : s.pages) {
            const u64 k = p.key.load(), n = p.count.load();
            if (k == 0) {
                continue;
            }
            ++distinct;
            hist[n < 2 ? 0 : n < 10 ? 1 : n < 100 ? 2 : n < 1000 ? 3 : n < 10000 ? 4 : 5] += 1;
            top_pages.emplace_back(n, k - 1);
        }
        for (const auto& p : s.regions) {
            if (const u64 k = p.key.load(); k != 0) {
                top_regions.emplace_back(p.count.load(), k - 1);
            }
        }
        const auto top = [](std::vector<std::pair<u64, u64>>& v, size_t n, u32 shift) {
            const size_t m = std::min(n, v.size());
            std::partial_sort(v.begin(), v.begin() + m, v.end(), std::greater<>{});
            std::string out;
            for (size_t i = 0; i < m; ++i) {
                out += fmt::format("{}{:#x} {}", i ? ", " : "", v[i].second << shift, v[i].first);
            }
            return out;
        };
        SSPC_LOG(App, Info,
                 "track stats (cumulative): faults {} on {} distinct pages (faults per page 1: {}, 2-9: {}, "
                 "10-99: {}, 100-999: {}, 1000-9999: {}, >=10000: {}; table overflow {}); GPU-thread faults {}; "
                 "by thread: {}",
                 now.faults, distinct, hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], s.page_overflow.load(),
                 s.faults_gpu_thread.load(), threads);
        SSPC_LOG(App, Info, "track stats (cumulative): top 1 MiB regions {}; top pages {}", top(top_regions, 12, 20),
                 top(top_pages, 12, 12));
        const u64 bc = s.barrier_calls.load(), bi = s.barrier_iterations.load(), b = s.barriers.load();
        const u64 bpr = s.barrier_partial_req.load(), bps = s.barrier_partial_state.load();
        const u64 big = s.barrier_iter_big.load(), fi = s.find_image.load();
        const u64 sc = s.stream_copies.load(), sb = s.stream_bytes.load();
        SSPC_LOG(App, Info,
                 "track stats: GPU thread: image transitions +{} ({} partial requests, {} on images already in "
                 "per-subresource state) visiting +{} subresource states (+{} in requests of >= 64), +{} "
                 "barriers, largest image {} subresources; FindImage +{}; stream-buffer copies +{} ({:.1f} MiB)",
                 bc - g_last.bc, bpr - g_last.bpr, bps - g_last.bps, bi - g_last.bi, big - g_last.big, b - g_last.b,
                 s.barrier_max_resources.load(), fi - g_last.fi, sc - g_last.sc,
                 double(sb - g_last.sb) / 1048576.0);
        const u64 rf = s.stream_repeat_frame.load(), rfb = s.stream_repeat_frame_bytes.load();
        const u64 rs = s.stream_repeat_same.load(), rsb = s.stream_repeat_same_bytes.load();
        const u64 sa = s.stream_same_any.load(), sab = s.stream_same_any_bytes.load();
        SSPC_LOG(App, Info,
                 "track stats: stream-buffer copies repeating a range already copied in the same frame +{} "
                 "({:.1f} MiB), of them with the same content +{} ({:.1f} MiB); same content as the range's "
                 "previous copy (any frame) +{} ({:.1f} MiB)",
                 rf - g_last.rf, double(rfb - g_last.rfb) / 1048576.0, rs - g_last.rs,
                 double(rsb - g_last.rsb) / 1048576.0, sa - g_last.sa, double(sab - g_last.sab) / 1048576.0);
        g_last = {bc, bi, b, bpr, bps, big, fi, sc, sb, rf, rfb, rs, rsb, sa, sab};
        last = now;
        last_handler = h;
        last_chain = c;
        last_write = w;
        last_read = r;
        last_unhandled = un;
        last_in_fault = inf;
        last_tex_calls = tc;
    }
}

} // namespace

u64 Ticks() {
    return __rdtsc();
}

void Enable() {
    if (g_state != nullptr) {
        return;
    }
    g_state = new State;
    g_state->ticks_per_us = Calibrate();
    g_state->buckets_per_tick = 10.0 / g_state->ticks_per_us;
    Base::EnableFirstChanceStamp();
    g_enabled = true;
    SSPC_LOG(App, Info, "--track-stats: GPU write tracking statistics on ({:.0f} TSC ticks per us)",
             g_state->ticks_per_us);
    std::thread([] {
        static const auto set_name = reinterpret_cast<SetThreadDescriptionFn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
        if (set_name != nullptr) {
            set_name(GetCurrentThread(), L"SecondSonPC:TrackStats");
        }
        LogLoop();
    }).detach();
}

void OnDispatch(u64 start, bool handled) {
    State& s = *g_state;
    const u64 end = __rdtsc();
    Add(s.dispatch, 1);
    Add(s.dispatch_ticks, end - start);
    if (!handled) {
        Add(s.dispatch_unhandled, 1);
    }
    const u64 first = Base::FirstChanceStamp();
    if (first != 0 && first <= start) {
        Add(s.chain_ticks, end - first);
        Record(s.chain, end - first);
    }
}

FaultScope::FaultScope(u64 addr, bool write, bool gpu_thread) : addr_{addr}, write_{write}, active_{g_enabled} {
    if (!active_) {
        return;
    }
    ++t_fault_depth;
    if (gpu_thread) {
        Add(g_state->faults_gpu_thread, 1);
    }
    start_ = __rdtsc();
}

FaultScope::~FaultScope() {
    if (!active_) {
        return;
    }
    State& s = *g_state;
    const u64 ticks = __rdtsc() - start_;
    --t_fault_depth;
    Add(s.faults, 1);
    Add(write_ ? s.faults_write : s.faults_read, 1);
    if (!handled_) {
        Add(s.faults_unhandled, 1);
    }
    Add(s.fault_ticks, ticks);
    Record(s.handler, ticks);
    if (ThreadSlot* t = ThisThread()) {
        Add(t->faults, 1);
        Add(t->ticks, ticks);
    }
    Count(s.pages, addr_ >> 12, s.page_overflow);
    Count(s.regions, addr_ >> 20, s.region_overflow);
}

void OnProtect(u64 size, bool allow_write, u64 start) {
    State& s = *g_state;
    Add(s.protect_ticks, __rdtsc() - start);
    const u64 pages = (size + 4095) >> 12;
    if (allow_write) {
        Add(s.protect_untrack, 1);
        Add(s.pages_untrack, pages);
    } else {
        Add(s.protect_track, 1);
        Add(s.pages_track, pages);
    }
    if (t_fault_depth != 0) {
        Add(s.protect_in_fault, 1);
    }
}

void OnUpload(u64 bytes, u32 ranges, u64 start) {
    State& s = *g_state;
    Add(s.uploads, 1);
    Add(s.upload_ranges, ranges);
    Add(s.upload_bytes, bytes);
    Add(s.upload_ticks, __rdtsc() - start);
}

void OnReadback(u64 bytes, u64 start) {
    State& s = *g_state;
    Add(s.readbacks, 1);
    Add(s.readback_bytes, bytes);
    Add(s.readback_ticks, __rdtsc() - start);
}

void OnFlushRequest() {
    Add(g_state->flushes, 1);
}

void OnTextureInvalidate(u32 images) {
    Add(g_state->tex_calls, 1);
    Add(g_state->tex_images, images);
}

void OnBarriers(u32 iterations, u32 emitted, u32 resources, bool partial_request, bool partially_transited) {
    State& s = *g_state;
    Add(s.barrier_calls, 1);
    Add(s.barrier_iterations, iterations);
    Add(s.barriers, emitted);
    if (partial_request) {
        Add(s.barrier_partial_req, 1);
    }
    if (partially_transited) {
        Add(s.barrier_partial_state, 1);
    }
    if (iterations >= 64) {
        Add(s.barrier_iter_big, iterations);
    }
    if (resources > s.barrier_max_resources.load(std::memory_order_relaxed)) {
        s.barrier_max_resources.store(resources, std::memory_order_relaxed);
    }
}

void OnFindImage() {
    Add(g_state->find_image, 1);
}

void OnStreamCopy(u64 bytes) {
    Add(g_state->stream_copies, 1);
    Add(g_state->stream_bytes, bytes);
}

void OnStreamCopyContent(u64 addr, u64 bytes, u64 content, u32 frame) {
    State& s = *g_state;
    while (s.stream_lock.test_and_set(std::memory_order_acquire)) {
        _mm_pause();
    }
    auto& e = s.stream_seen[((addr ^ (bytes << 40)) * 0x9E3779B97F4A7C15ull) >> 48];
    if (e.addr == addr && e.size == bytes) {
        if (e.content == content) {
            Add(s.stream_same_any, 1);
            Add(s.stream_same_any_bytes, bytes);
        }
        if (e.frame == frame) {
            Add(s.stream_repeat_frame, 1);
            Add(s.stream_repeat_frame_bytes, bytes);
            if (e.content == content) {
                Add(s.stream_repeat_same, 1);
                Add(s.stream_repeat_same_bytes, bytes);
            }
        }
    }
    e = {addr, bytes, content, frame};
    s.stream_lock.clear(std::memory_order_release);
}

Totals Take() {
    Totals t;
    if (g_state == nullptr) {
        return t;
    }
    const State& s = *g_state;
    const auto ld = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    t.faults = ld(s.faults);
    t.fault_ticks = ld(s.fault_ticks);
    t.dispatch_ticks = ld(s.dispatch_ticks);
    t.chain_ticks = ld(s.chain_ticks);
    t.protect_track = ld(s.protect_track);
    t.protect_untrack = ld(s.protect_untrack);
    t.pages_track = ld(s.pages_track);
    t.pages_untrack = ld(s.pages_untrack);
    t.protect_ticks = ld(s.protect_ticks);
    t.uploads = ld(s.uploads);
    t.upload_bytes = ld(s.upload_bytes);
    t.upload_ticks = ld(s.upload_ticks);
    t.readbacks = ld(s.readbacks);
    t.readback_bytes = ld(s.readback_bytes);
    t.readback_ticks = ld(s.readback_ticks);
    t.flushes = ld(s.flushes);
    t.tex_images = ld(s.tex_images);
    return t;
}

double TicksPerMicrosecond() {
    return g_state != nullptr ? g_state->ticks_per_us : 1.0;
}

} // namespace SecondSon::TrackDiag
