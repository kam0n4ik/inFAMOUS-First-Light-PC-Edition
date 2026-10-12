// SecondSonPC — the shader stall trace (P4.14). See shader_trace.h.
#include "runtime/gpu/shader_trace.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/stall_trace.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::ShaderTrace {

namespace detail {
bool g_on = false;
} // namespace detail

namespace {

constexpr size_t kKinds = static_cast<size_t>(Kind::Count);

struct Rec {
    u64 t0, t1, a, b;
    u32 c, frame;
    u16 thread;
    u8 kind;
};

struct Totals {
    u64 count = 0, ticks = 0, max = 0;
};

struct State {
    std::filesystem::path dir;
    double tsc_hz = 1.0;
    std::atomic<u64> start_tsc{0}; ///< (SetOrigin may move it while the writer runs)
    u64 event_ticks = 0, frame_ticks = 0, wait_ticks = 0;
    u64 frame_ms = 50, event_ms = 10;
    // the hooks'
    std::mutex mutex;
    std::vector<Rec> pending;
    std::vector<std::string> threads; ///< slot -> "name (tid)" (only appended: the writer reads it under the mutex)
    u64 dropped = 0;
    // the writer's
    std::thread* writer = nullptr; ///< (never a joinable std::thread with static storage; joined by Stop)
    std::atomic<bool> stop{false};
    bool stopped = false;
    std::ofstream events, frames, frame_text;
    std::deque<Rec> recent; ///< events of the last seconds (the long frames' breakdown)
    std::vector<std::string> names; ///< the writer's copy of `threads`
    u32 next_frame = 0;     ///< the next presented frame to look at
    u64 prev_present = 0;
    FrameLog::GpuThreadTotals prev_totals{};
    std::array<Totals, kKinds> totals{};
    std::array<Totals, StallTrace::kEvents> waits{};
    u64 frames_seen = 0, frames_over[3] = {}; ///< >= frame threshold (50), >= 100, >= 1000 ms
    u64 frames_compile[3] = {};               ///< ... whose time was half or more translations/pipelines
    std::vector<Rec> slowest;                 ///< the slowest translations/pipelines (kept to 64)
    u64 pipe_feedback = 0, pipe_hits = 0;     ///< pipelines created with creation feedback / hits among them
    u64 longest_frame = 0;
};

State g;
thread_local int t_slot = -1;

double Ms(u64 ticks) {
    return static_cast<double>(ticks) * 1000.0 / g.tsc_hz;
}
double Sec(u64 tsc) {
    return static_cast<double>(static_cast<s64>(tsc - g.start_tsc.load(std::memory_order_relaxed))) / g.tsc_hz;
}

/// (under the mutex)
int SlotLocked(const char* role) {
    std::string name;
    if (role != nullptr) {
        name = role;
    } else {
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc != nullptr) {
            char buf[96] = {};
            WideCharToMultiByte(CP_UTF8, 0, desc, -1, buf, sizeof(buf) - 1, nullptr, nullptr);
            LocalFree(desc);
            name = buf;
        }
        if (name.empty()) {
            name = "thread";
        }
    }
    g.threads.push_back(fmt::format("{} ({})", name, GetCurrentThreadId()));
    return static_cast<int>(g.threads.size() - 1);
}

std::string_view WaitName(u32 c) {
    return c < StallTrace::kEvents ? StallTrace::EvName(static_cast<StallTrace::Ev>(c)) : "?";
}

const std::string& ThreadName(u16 slot) {
    static const std::string unknown = "?";
    return slot < g.names.size() ? g.names[slot] : unknown;
}

std::string Detail(const Rec& r) {
    switch (static_cast<Kind>(r.kind)) {
    case Kind::Translate:
    case Kind::DiskLoad:
        return fmt::format("stage {} spirv {} B", r.c, r.b);
    case Kind::PipelineCreate:
    case Kind::PipelinePrebuilt:
    case Kind::Precompile:
        return fmt::format("{}{}{}{}", (r.c & kPipeCompute) ? "compute" : "graphics",
                           (r.c & kPipeFeedback) ? ((r.c & kPipeCacheHit) ? " cache-hit" : " cache-miss") : "",
                           (r.c & kPipeWaited) ? " waited-for-worker" : "", (r.c & kPipeFailed) ? " FAILED" : "");
    case Kind::Wait:
        return std::string(WaitName(r.c));
    case Kind::DiskWrite:
        return fmt::format("{} records {} B", r.a, r.b);
    default:
        return fmt::format("{} B", r.b);
    }
}

/// Preparation of shaders and pipelines on demand (what precompilation is meant to remove from frames).
bool CompileKind(u8 k) {
    const auto kind = static_cast<Kind>(k);
    return kind == Kind::Translate || kind == Kind::PipelineCreate || kind == Kind::ShaderModule ||
           kind == Kind::PipelinePrebuilt || kind == Kind::DiskLoad;
}

void KeepSlowest(const Rec& r) {
    const auto kind = static_cast<Kind>(r.kind);
    if (kind != Kind::Translate && kind != Kind::PipelineCreate && kind != Kind::Precompile &&
        kind != Kind::PipelinePrebuilt) {
        return;
    }
    g.slowest.push_back(r);
    if (g.slowest.size() > 128) {
        std::ranges::sort(g.slowest, [](const Rec& x, const Rec& y) { return x.t1 - x.t0 > y.t1 - y.t0; });
        g.slowest.resize(64);
    }
}

void TakeEvents() {
    std::vector<Rec> batch;
    {
        std::scoped_lock lock{g.mutex};
        batch.swap(g.pending);
        for (size_t i = g.names.size(); i < g.threads.size(); ++i) {
            g.names.push_back(g.threads[i]);
        }
    }
    for (const Rec& r : batch) {
        const u64 d = r.t1 >= r.t0 ? r.t1 - r.t0 : 0;
        Totals& t = g.totals[r.kind];
        ++t.count;
        t.ticks += d;
        t.max = std::max(t.max, d);
        if (static_cast<Kind>(r.kind) == Kind::Wait && r.c < StallTrace::kEvents) {
            Totals& w = g.waits[r.c];
            ++w.count;
            w.ticks += d;
            w.max = std::max(w.max, d);
        }
        if (static_cast<Kind>(r.kind) == Kind::PipelineCreate && (r.c & kPipeFeedback) != 0) {
            ++g.pipe_feedback;
            g.pipe_hits += (r.c & kPipeCacheHit) != 0 ? 1 : 0;
        }
        KeepSlowest(r);
        if (d >= g.event_ticks && g.events) {
            g.events << fmt::format("{:.3f},{:.3f},{:.3f},{},{},{},{:016x},{}\n", Sec(r.t0), Sec(r.t1), Ms(d),
                                    ThreadName(r.thread), r.frame, KindName(static_cast<Kind>(r.kind)), r.a, Detail(r));
        }
        g.recent.push_back(r);
    }
}

/// Frames presented so far that the writer has not looked at: the long ones broken down.
void TakeFrames(bool final) {
    const FrameLog::Progress p = FrameLog::GetProgress();
    // (a frame is looked at a quarter second after it was presented: its events have all been pushed by then)
    const u64 now = Kernel::Clock::ReadTsc();
    while (g.next_frame < p.present) {
        FrameLog::RowView row;
        if (!FrameLog::Read(g.next_frame, row) || row.present == 0) {
            ++g.next_frame;
            continue;
        }
        if (!final && now - row.present < static_cast<u64>(g.tsc_hz / 4)) {
            break;
        }
        const u32 n = g.next_frame++;
        const FrameLog::GpuThreadTotals prev = g.prev_totals;
        g.prev_totals = row.totals;
        const u64 from = g.prev_present;
        g.prev_present = row.present;
        if (from == 0 || row.present <= from) {
            continue;
        }
        ++g.frames_seen;
        const u64 gap = row.present - from;
        g.longest_frame = std::max(g.longest_frame, gap);
        if (gap < g.frame_ticks) {
            continue;
        }
        // what the traced threads did inside [from, present): by kind (overlap), and the longest events
        std::array<u64, kKinds> by_kind{};
        std::vector<std::pair<u64, const Rec*>> top;
        // preparation on demand per thread: the frame is compilation-bound when one thread spent half of it on that
        std::vector<u64> compile_by_thread(g.names.size() + 1, 0);
        for (const Rec& r : g.recent) {
            const u64 a = std::max(r.t0, from), b = std::min(r.t1, row.present);
            if (b <= a) {
                continue;
            }
            by_kind[r.kind] += b - a;
            top.emplace_back(b - a, &r);
            if (CompileKind(r.kind)) {
                compile_by_thread[std::min<size_t>(r.thread, compile_by_thread.size() - 1)] += b - a;
            }
        }
        const u64 compile = *std::ranges::max_element(compile_by_thread);
        const double gap_ms = Ms(gap);
        const int cls = gap_ms >= 1000.0 ? 2 : gap_ms >= 100.0 ? 1 : 0;
        const bool compile_bound = compile * 2 >= gap;
        for (int i = 0; i <= cls; ++i) {
            ++g.frames_over[i];
            g.frames_compile[i] += compile_bound ? 1 : 0;
        }
        const auto d = [&](u64 FrameLog::GpuThreadTotals::*f) {
            return static_cast<double>(row.totals.*f - prev.*f) / 1e6;
        };
        const auto dn = [&](u64 FrameLog::GpuThreadTotals::*f) { return row.totals.*f - prev.*f; };
        if (g.frames) {
            std::string line = fmt::format("{},{:.3f},{:.1f},{}", n, Sec(row.present), gap_ms,
                                           cls == 2 ? ">=1000" : cls == 1 ? ">=100" : fmt::format(">={}", g.frame_ms));
            for (size_t k = 0; k < kKinds; ++k) {
                line += fmt::format(",{:.1f}", Ms(by_kind[k]));
            }
            line += fmt::format(",{},{:.1f},{:.1f},{},{:.1f},{},{:.1f},{:.1f},{:.1f},{}\n", compile_bound ? 1 : 0,
                                d(&FrameLog::GpuThreadTotals::cp_ns), d(&FrameLog::GpuThreadTotals::record_ns),
                                dn(&FrameLog::GpuThreadTotals::shaders), d(&FrameLog::GpuThreadTotals::shader_ns),
                                dn(&FrameLog::GpuThreadTotals::pipelines), d(&FrameLog::GpuThreadTotals::pipeline_ns),
                                d(&FrameLog::GpuThreadTotals::par_wait_ns), d(&FrameLog::GpuThreadTotals::sync_ns),
                                dn(&FrameLog::GpuThreadTotals::shader_disk));
            g.frames << line;
        }
        if (cls >= 1 && g.frame_text) {
            std::ranges::sort(top, [](const auto& x, const auto& y) { return x.first > y.first; });
            g.frame_text << fmt::format(
                "frame {} at {:.3f} s: {:.1f} ms{}{}; translations {} ({:.1f} ms), pipelines {} ({:.1f} ms), processor "
                "{:.1f} ms, recording {:.1f} ms, waits for the recording thread {:.1f} ms, for the GPU {:.1f} ms\n",
                n, Sec(row.present), gap_ms, cls == 2 ? " [>= 1 s]" : "", compile_bound ? " [compilation >= half]" : "",
                dn(&FrameLog::GpuThreadTotals::shaders), d(&FrameLog::GpuThreadTotals::shader_ns),
                dn(&FrameLog::GpuThreadTotals::pipelines), d(&FrameLog::GpuThreadTotals::pipeline_ns),
                d(&FrameLog::GpuThreadTotals::cp_ns), d(&FrameLog::GpuThreadTotals::record_ns),
                d(&FrameLog::GpuThreadTotals::par_wait_ns), d(&FrameLog::GpuThreadTotals::sync_ns));
            g.frame_text << "  by kind (ms, all traced threads):";
            for (size_t k = 0; k < kKinds; ++k) {
                if (by_kind[k] != 0) {
                    g.frame_text << fmt::format(" {} {:.1f};", KindName(static_cast<Kind>(k)), Ms(by_kind[k]));
                }
            }
            g.frame_text << "\n";
            for (size_t i = 0; i < top.size() && i < 12; ++i) {
                const Rec& r = *top[i].second;
                g.frame_text << fmt::format("  {:8.1f} ms  {:<16} {:<34} key {:016x}  {}\n", Ms(top[i].first),
                                            KindName(static_cast<Kind>(r.kind)), ThreadName(r.thread), r.a, Detail(r));
            }
            if (top.size() > 12) {
                g.frame_text << fmt::format("  ... {} more events\n", top.size() - 12);
            }
        }
    }
    // events that ended well before the frames still to look at are not needed any more
    const u64 margin = static_cast<u64>(g.tsc_hz * 5);
    const u64 keep_from = g.prev_present > margin ? g.prev_present - margin : 0;
    while (!g.recent.empty() && g.recent.front().t1 < keep_from) {
        g.recent.pop_front();
    }
    if (g.recent.size() > 200000) { // (a flood of waits: keep the newest)
        g.recent.erase(g.recent.begin(), g.recent.begin() + static_cast<std::ptrdiff_t>(g.recent.size() - 100000));
    }
}

void WriterLoop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:ShaderTrace");
    while (!g.stop.load(std::memory_order_acquire)) {
        Sleep(250);
        TakeEvents();
        TakeFrames(false);
        g.events.flush();
        g.frames.flush();
        g.frame_text.flush();
    }
}

void WriteSummary() {
    std::ofstream o(g.dir / "shader_stall_summary.txt", std::ios::trunc);
    if (!o) {
        return;
    }
    o << fmt::format("shader stall trace (P4.14): thresholds event {} ms, frame {} ms, waits from {:.1f} ms\n",
                     g.event_ms, g.frame_ms, Ms(g.wait_ticks));
    o << fmt::format("frames presented {}; >= {} ms {}, >= 100 ms {}, >= 1000 ms {}; longest {:.1f} ms\n", g.frames_seen,
                     g.frame_ms, g.frames_over[0], g.frames_over[1], g.frames_over[2], Ms(g.longest_frame));
    o << fmt::format("  of them a thread spent half or more on translations/pipelines: {}, {}, {}\n",
                     g.frames_compile[0], g.frames_compile[1], g.frames_compile[2]);
    o << fmt::format("pipelines created on demand with creation feedback {}: application pipeline cache hits {}, "
                     "misses {}\n",
                     g.pipe_feedback, g.pipe_hits, g.pipe_feedback - g.pipe_hits);
    o << "totals by kind (count, ms, max ms):\n";
    for (size_t k = 0; k < kKinds; ++k) {
        const Totals& t = g.totals[k];
        if (t.count != 0) {
            o << fmt::format("  {:<16} {:>8} {:>12.1f} {:>10.1f}\n", KindName(static_cast<Kind>(k)), t.count,
                             Ms(t.ticks), Ms(t.max));
        }
    }
    o << "forwarded waits >= the minimum (count, ms, max ms):\n";
    for (size_t k = 0; k < g.waits.size(); ++k) {
        const Totals& t = g.waits[k];
        if (t.count != 0) {
            o << fmt::format("  {:<16} {:>8} {:>12.1f} {:>10.1f}\n", WaitName(static_cast<u32>(k)), t.count,
                             Ms(t.ticks), Ms(t.max));
        }
    }
    std::ranges::sort(g.slowest, [](const Rec& x, const Rec& y) { return x.t1 - x.t0 > y.t1 - y.t0; });
    o << "slowest translations / pipelines:\n";
    for (size_t i = 0; i < g.slowest.size() && i < 40; ++i) {
        const Rec& r = g.slowest[i];
        o << fmt::format("  {:8.1f} ms at {:8.3f} s  {:<16} key {:016x}  frame {}  {}  {}\n", Ms(r.t1 - r.t0), Sec(r.t0),
                         KindName(static_cast<Kind>(r.kind)), r.a, r.frame, ThreadName(r.thread), Detail(r));
    }
    o << fmt::format("events dropped: {}\n", g.dropped);
}

} // namespace

const char* KindName(Kind kind) {
    static constexpr const char* kNames[] = {"Translate",        "DiskLoad",   "DiskWrite", "ShaderModule",
                                             "PipelineCreate",   "PipelinePrebuilt", "Precompile", "CacheLoad",
                                             "CacheSave",        "Wait"};
    static_assert(std::size(kNames) == kKinds);
    const size_t i = static_cast<size_t>(kind);
    return i < kKinds ? kNames[i] : "?";
}

void detail::Push(Kind kind, u64 t0, u64 t1, u64 a, u64 b, u32 c) {
    const u32 frame = StallTrace::CurrentFrame();
    std::scoped_lock lock{g.mutex};
    if (t_slot < 0) {
        t_slot = SlotLocked(nullptr);
    }
    if (g.pending.size() >= (1u << 20)) {
        ++g.dropped; // (the writer stopped or stalls: never grow without bound)
        return;
    }
    g.pending.push_back(Rec{t0, t1, a, b, c, frame, static_cast<u16>(t_slot), static_cast<u8>(kind)});
}

void NameThread(const char* role) {
    if (!detail::g_on || t_slot >= 0) {
        return;
    }
    std::scoped_lock lock{g.mutex};
    t_slot = SlotLocked(role);
}

bool Start(const std::filesystem::path& logs_dir) {
    char v[128] = {};
    const DWORD len = GetEnvironmentVariableA("SSPC_SHADER_STALL_TRACE", v, sizeof(v));
    if (len == 0 || len >= sizeof(v) || v[0] == '0') {
        return false;
    }
    u64 event_ms = 10, frame_ms = 50;
    double wait_ms = 1.0;
    const std::string_view s(v, len);
    if (!s.starts_with("1")) {
        SSPC_LOG(GPU, Warning, "SSPC_SHADER_STALL_TRACE='{}' is not 1[,event=ms][,frame=ms][,wait=ms]: off", s);
        return false;
    }
    for (size_t pos = s.find(','); pos != std::string_view::npos; pos = s.find(',', pos + 1)) {
        const size_t end = s.find(',', pos + 1);
        const std::string kv(s.substr(pos + 1, end == std::string_view::npos ? std::string_view::npos : end - pos - 1));
        if (kv.starts_with("event=")) {
            event_ms = std::strtoull(kv.c_str() + 6, nullptr, 10);
        } else if (kv.starts_with("frame=")) {
            frame_ms = std::max<u64>(std::strtoull(kv.c_str() + 6, nullptr, 10), 1);
        } else if (kv.starts_with("wait=")) {
            wait_ms = std::strtod(kv.c_str() + 5, nullptr);
        } else {
            SSPC_LOG(GPU, Warning, "SSPC_SHADER_STALL_TRACE: unknown option '{}' (ignored)", kv);
        }
    }
    g.dir = logs_dir;
    g.tsc_hz = static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
    g.start_tsc = FrameLog::StartTsc() != 0 ? FrameLog::StartTsc() : Kernel::Clock::ReadTsc();
    g.event_ms = event_ms;
    g.frame_ms = frame_ms;
    g.event_ticks = static_cast<u64>(static_cast<double>(event_ms) * g.tsc_hz / 1000.0);
    g.frame_ticks = static_cast<u64>(static_cast<double>(frame_ms) * g.tsc_hz / 1000.0);
    g.wait_ticks = static_cast<u64>(wait_ms * g.tsc_hz / 1000.0);
    g.events.open(logs_dir / "shader_stall_events.csv", std::ios::trunc);
    g.frames.open(logs_dir / "shader_stall_frames.csv", std::ios::trunc);
    g.frame_text.open(logs_dir / "shader_stall_frames.txt", std::ios::trunc);
    g.events << "t0_s,t1_s,ms,thread,frame,kind,key,detail\n";
    std::string header = "frame,present_s,gap_ms,class";
    for (size_t k = 0; k < kKinds; ++k) {
        header += fmt::format(",{}_ms", KindName(static_cast<Kind>(k)));
    }
    header += ",compile_bound,cp_ms,record_ms,shaders,shader_ms,pipelines,pipeline_ms,par_wait_ms,gpu_sync_ms,"
              "shader_disk\n";
    g.frames << header;
    g.pending.reserve(4096);
    detail::g_on = true;
    g.writer = new std::thread(WriterLoop);
    SSPC_LOG(GPU, Warning,
             "DIAGNOSTIC: SSPC_SHADER_STALL_TRACE={} - translations, pipelines and waits >= {:.1f} ms traced (events >= "
             "{} ms, frames >= {} ms -> logs\\shader_stall_*)",
             s, wait_ms, event_ms, frame_ms);
    return true;
}

void SetOrigin(u64 tsc) {
    if (detail::g_on && tsc != 0) {
        g.start_tsc.store(tsc, std::memory_order_relaxed); // (rows written meanwhile may use either origin)
    }
}

u64 WaitMinTicks() {
    return g.wait_ticks;
}

void Stop() {
    if (!detail::g_on || g.stopped) {
        return;
    }
    g.stopped = true;
    g.stop.store(true, std::memory_order_release);
    if (g.writer != nullptr && g.writer->joinable()) {
        g.writer->join();
    }
    TakeEvents();
    TakeFrames(true);
    WriteSummary();
    g.events.close();
    g.frames.close();
    g.frame_text.close();
    SSPC_LOG(GPU, Info,
             "shader stall trace: {} frames, >= {} ms {}, >= 100 ms {}, >= 1000 ms {} (a thread on translations/pipelines "
             ">= half of it: {}, {}, {}) -> {}",
             g.frames_seen, g.frame_ms, g.frames_over[0], g.frames_over[1], g.frames_over[2], g.frames_compile[0],
             g.frames_compile[1], g.frames_compile[2], (g.dir / "shader_stall_summary.txt").string());
}

} // namespace SecondSon::Gpu::ShaderTrace
