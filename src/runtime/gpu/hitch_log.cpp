// SecondSonPC — the hitch log. See hitch_log.h.
#include "runtime/gpu/hitch_log.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "base/vram_monitor.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/vulkan/vk_memory_ledger.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::HitchLog {

namespace detail {
std::atomic<u64> g_file_bytes{0}, g_file_ns{0}, g_file_reads{0}, g_guest_maps{0};
}

namespace {

constexpr u32 kTickMs = 50;
constexpr size_t kSamples = 1200; ///< 60 s of ticks
constexpr size_t kKeepSessions = 30;
constexpr double kMiB = 1024.0 * 1024.0;

/// The process and the machine at one tick (running totals).
struct Sample {
    u64 tsc = 0;
    u64 file_bytes = 0, file_ns = 0, file_reads = 0, guest_maps = 0, tracker_faults = 0, page_faults = 0;
    u64 proc_cpu = 0; ///< 100 ns: this process's user + kernel time
    u64 sys_busy = 0; ///< 100 ns: every core's user + kernel time but idle
};

/// What a window between two samples adds up to.
struct Context {
    double file_mb = 0, file_ms = 0;
    u64 file_reads = 0, guest_maps = 0, tracker_faults = 0, page_faults = 0;
    double proc_cpu_pct = 0, other_cpu_pct = 0; ///< of the whole machine
};

/// Levels, taken once a second.
struct Levels {
    double working_set_mb = 0, private_mb = 0, ram_free_mb = 0, commit_mb = 0, commit_limit_mb = 0;
    double vram_mb = 0, vram_budget_mb = 0;
};

enum class Cause : u8 {
    Shaders, Loading, Frontend, Game, Backend, Readback, RecordWait, Recording, Uploads, Gpu, Present, Count
};
constexpr size_t kCauses = static_cast<size_t>(Cause::Count);
constexpr std::array<const char*, kCauses> kCauseNames = {
    "shader compile",  "loading",        "draw preparation", "game CPU", "backend", "GPU readback wait",
    "recording queue", "draw recording", "uploads",          "GPU",      "presentation"};

struct Hitch {
    u32 frame = 0;
    double t = 0, ms = 0;
    Cause cause = Cause::Game;
};

struct State {
    std::filesystem::path logs_dir, dir;
    FaultsFn faults = nullptr;
    void* faults_ctx = nullptr;
    double threshold_ms = 50.0;
    double freq = 1.0;  ///< TSC ticks per second
    u64 origin = 0;     ///< FrameLog's time zero (TSC)
    u64 origin_ft = 0;  ///< ... as a local FILETIME
    u32 cores = 1;
    std::ofstream txt, csv, sec;
    std::thread thread;
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    // the thread's
    std::deque<Sample> samples;
    u32 next = 0;
    bool have_prev = false;
    FrameLog::RowView prev;
    std::vector<float> intervals;
    std::vector<Hitch> hitches;
    std::vector<double> marks;
    u32 marks_seen = 0;
    Levels levels;
    Sample sec_start;
    double peak_vram_mb = 0, peak_ours_mb = 0, peak_images_mb = 0, peak_buffers_mb = 0;
    u32 over_budget_s = 0;
    u32 sec_frames = 0, sec_hitches = 0;
    double sec_worst = 0;
    u64 sec_shaders = 0, sec_pipelines = 0;
};

State* g = nullptr;
std::atomic<u32> g_marks{0};
std::atomic<u64> g_mark_tsc{0};

u64 Ft(const FILETIME& f) {
    return (static_cast<u64>(f.dwHighDateTime) << 32) | f.dwLowDateTime;
}

double Sec(u64 tsc) {
    return tsc > g->origin ? static_cast<double>(tsc - g->origin) / g->freq : 0.0;
}

/// The local time `t` seconds after the frame log's start.
std::string LocalTime(double t) {
    const u64 ft = g->origin_ft + static_cast<u64>(std::max(t, 0.0) * 1e7);
    const FILETIME f{static_cast<DWORD>(ft), static_cast<DWORD>(ft >> 32)};
    SYSTEMTIME st{};
    FileTimeToSystemTime(&f, &st);
    return fmt::format("{:02}:{:02}:{:02}.{:03}", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

Sample TakeSample() {
    Sample s;
    s.tsc = Kernel::Clock::ReadTsc();
    s.file_bytes = detail::g_file_bytes.load(std::memory_order_relaxed);
    s.file_ns = detail::g_file_ns.load(std::memory_order_relaxed);
    s.file_reads = detail::g_file_reads.load(std::memory_order_relaxed);
    s.guest_maps = detail::g_guest_maps.load(std::memory_order_relaxed);
    s.tracker_faults = g->faults != nullptr ? g->faults(g->faults_ctx) : 0;
    if (PROCESS_MEMORY_COUNTERS pm{}; GetProcessMemoryInfo(GetCurrentProcess(), &pm, sizeof(pm))) {
        s.page_faults = pm.PageFaultCount;
    }
    if (FILETIME c{}, e{}, k{}, u{}; GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
        s.proc_cpu = Ft(k) + Ft(u);
    }
    if (FILETIME i{}, k{}, u{}; GetSystemTimes(&i, &k, &u)) {
        s.sys_busy = Ft(k) + Ft(u) - std::min(Ft(i), Ft(k) + Ft(u)); // (kernel time includes idle)
    }
    return s;
}

Context Between(const Sample& a, const Sample& b) {
    const auto dn = [](u64 x, u64 y) { return x >= y ? x - y : 0; };
    Context c;
    c.file_mb = static_cast<double>(dn(b.file_bytes, a.file_bytes)) / kMiB;
    c.file_ms = static_cast<double>(dn(b.file_ns, a.file_ns)) / 1e6;
    c.file_reads = dn(b.file_reads, a.file_reads);
    c.guest_maps = dn(b.guest_maps, a.guest_maps);
    c.tracker_faults = dn(b.tracker_faults, a.tracker_faults);
    c.page_faults = dn(b.page_faults, a.page_faults);
    if (b.tsc > a.tsc) {
        const double total = static_cast<double>(b.tsc - a.tsc) / g->freq * 1e7 * g->cores;
        const u64 proc = dn(b.proc_cpu, a.proc_cpu);
        const u64 sys = dn(b.sys_busy, a.sys_busy);
        c.proc_cpu_pct = 100.0 * static_cast<double>(proc) / total;
        c.other_cpu_pct = 100.0 * static_cast<double>(dn(sys, proc)) / total;
    }
    return c;
}

/// The samples around [t0, t1] (TSC): the last one at or before t0, the first one at or after t1.
Context Window(u64 t0, u64 t1) {
    const auto& s = g->samples;
    if (s.size() < 2) {
        return {};
    }
    size_t a = 0;
    while (a + 1 < s.size() && s[a + 1].tsc <= t0) {
        ++a;
    }
    size_t b = a;
    while (b + 1 < s.size() && s[b].tsc < t1) {
        ++b;
    }
    return Between(s[a], s[b]);
}

void RefreshLevels() {
    Levels& l = g->levels;
    if (PROCESS_MEMORY_COUNTERS_EX pm{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm))) {
        l.working_set_mb = static_cast<double>(pm.WorkingSetSize) / kMiB;
        l.private_mb = static_cast<double>(pm.PrivateUsage) / kMiB;
    }
    if (PERFORMANCE_INFORMATION pi{}; GetPerformanceInfo(&pi, sizeof(pi))) {
        const double page = static_cast<double>(pi.PageSize) / kMiB;
        l.ram_free_mb = static_cast<double>(pi.PhysicalAvailable) * page;
        l.commit_mb = static_cast<double>(pi.CommitTotal) * page;
        l.commit_limit_mb = static_cast<double>(pi.CommitLimit) * page;
    }
    if (Base::VideoMemoryInfo v; Base::QueryVideoMemory(v)) {
        l.vram_mb = static_cast<double>(v.local_usage) / kMiB;
        l.vram_budget_mb = static_cast<double>(v.local_budget) / kMiB;
    }
}

/// Frame `n` (`r`), shown after frame n-1 (`p`).
void Analyze(const FrameLog::RowView& p, const FrameLog::RowView& r, u32 n) {
    const double f = 1000.0 / g->freq;
    const auto d = [f](u64 a, u64 b) { return (a >= b ? static_cast<double>(a - b) : -static_cast<double>(b - a)) * f; };
    const auto lat = [&](u64 a, u64 b) { return a != 0 && b != 0 ? d(a, b) : 0.0; };
    const auto dms = [](u64 a, u64 b) { return a >= b ? static_cast<double>(a - b) / 1e6 : 0.0; };
    const auto dn = [](u64 a, u64 b) { return a >= b ? a - b : 0; };

    const double interval = d(r.present, p.present);
    g->intervals.push_back(static_cast<float>(interval));
    ++g->sec_frames;
    g->sec_worst = std::max(g->sec_worst, interval);
    const bool tot = r.cp_flip != 0 && p.cp_flip != 0; // (the GPU thread's totals of both frames)
    const FrameLog::GpuThreadTotals& t = r.totals;
    const FrameLog::GpuThreadTotals& pt = p.totals;
    if (tot) {
        g->sec_shaders += dn(t.shaders, pt.shaders);
        g->sec_pipelines += dn(t.pipelines, pt.pipelines);
    }
    if (interval < g->threshold_ms) {
        return;
    }
    ++g->sec_hitches;

    // Where the interval went (exact: the four add up to it): the game making the frame, the game waiting for the
    // backend plus the command processor's longer latency, the GPU's longer latency, presentation's longer latency
    const double game_total = r.game_flip != 0 && p.game_flip != 0 ? d(r.game_flip, p.game_flip) : 0.0;
    const double lock_wait = tot ? dms(r.idle_wait_ns, p.idle_wait_ns) : 0.0;
    const double flip_wait = r.flip_enter != 0 && r.game_flip >= r.flip_enter ? d(r.game_flip, r.flip_enter) : 0.0;
    const double game_own = std::max(0.0, game_total - flip_wait - lock_wait);
    const double backend = (game_total - game_own) + lat(r.cp_flip, r.game_flip) - lat(p.cp_flip, p.game_flip);
    const double gpu = lat(r.gpu_done, r.cp_flip) - lat(p.gpu_done, p.cp_flip);
    const double present = lat(r.present, r.gpu_done) - lat(p.present, p.gpu_done);

    // the backend's work between the two flip markers
    const double shader_ms = tot ? dms(t.shader_ns, pt.shader_ns) + dms(t.module_ns, pt.module_ns) : 0.0;
    const double pipe_ms = tot ? dms(t.pipeline_ns, pt.pipeline_ns) : 0.0;
    const u64 shaders = tot ? dn(t.shaders, pt.shaders) : 0, disk = tot ? dn(t.shader_disk, pt.shader_disk) : 0;
    const u64 pipelines = tot ? dn(t.pipelines, pt.pipelines) : 0;
    const double sync_ms = tot ? dms(t.sync_ns, pt.sync_ns) : 0.0;
    const double record_ms = tot ? dms(t.record_ns, pt.record_ns) : 0.0;
    const double par_wait_ms = tot ? dms(t.par_wait_ns, pt.par_wait_ns) : 0.0;
    const double par_busy_ms = tot ? dms(t.par_busy_ns, pt.par_busy_ns) : 0.0;
    const double front_busy_ms = tot ? dms(t.front_busy_ns, pt.front_busy_ns) : 0.0;
    const double front_wait_ms = tot ? dms(t.front_wait_ns, pt.front_wait_ns) : 0.0;
    const double wait_game_ms = tot ? dms(t.empty_ns, pt.empty_ns) + dms(t.blocked_other_ns, pt.blocked_other_ns) : 0.0;
    const double wait_display_ms = tot ? dms(t.blocked_label_ns, pt.blocked_label_ns) : 0.0;
    const double gpu_busy_ms = tot ? dms(t.gpu_busy_ns, pt.gpu_busy_ns) : 0.0;
    const u64 img_created = tot ? dn(t.img_created, pt.img_created) : 0;
    const double img_mb = tot ? static_cast<double>(dn(t.img_upload_bytes, pt.img_upload_bytes)) / kMiB : 0.0;
    const double buf_mb = tot ? static_cast<double>(dn(t.buf_upload_bytes, pt.buf_upload_bytes)) / kMiB : 0.0;
    const double wb_mb = tot ? static_cast<double>(dn(t.writeback_bytes, pt.writeback_bytes)) / kMiB : 0.0;
    const u64 draws = tot ? dn(t.draws, pt.draws) : 0;
    const Context c = Window(p.present, r.present);
    const Levels& l = g->levels;

    const double compile = shader_ms + pipe_ms;
    Cause cause;
    if (compile >= 0.35 * interval) {
        cause = Cause::Shaders;
    } else if (interval >= 500.0 && c.file_mb >= 32.0) {
        cause = Cause::Loading;
    } else if (front_busy_ms >= 0.5 * interval && par_wait_ms >= 0.3 * interval) {
        // the frontend thread preparing draws held the queue (the game's own time then is mostly its wait for the
        // labels of frames the backend has not finished: it reads as the game's)
        cause = Cause::Frontend;
    } else {
        const double top = std::max({game_own, backend, gpu, present});
        if (top == game_own) {
            cause = c.file_ms >= 0.3 * interval || c.file_mb >= 16.0 ? Cause::Loading : Cause::Game;
        } else if (top == backend) {
            cause = sync_ms >= 0.3 * interval               ? Cause::Readback
                    : img_mb + buf_mb >= 16.0               ? Cause::Uploads
                    : par_wait_ms >= 0.3 * interval         ? Cause::RecordWait
                    : record_ms - compile >= 0.3 * interval ? Cause::Recording
                                                            : Cause::Backend;
        } else if (top == gpu) {
            cause = Cause::Gpu;
        } else {
            cause = Cause::Present;
        }
    }
    const double time = Sec(r.present);
    g->hitches.push_back({n, time, interval, cause});

    std::string notes;
    const auto note = [&notes](const std::string& s) { notes += (notes.empty() ? " | " : ", ") + s; };
    if (par_wait_ms >= 0.3 * interval) {
        note(fmt::format("command processor waited {:.0f} ms for the recording queue: recording thread busy {:.0f} ms, "
                         "frontend thread busy {:.0f} ms, frontend waiting for GPU writes {:.0f} ms",
                         par_wait_ms, par_busy_ms, front_busy_ms, front_wait_ms));
    }
    if (c.file_mb >= 1.0 || c.file_ms >= 5.0) {
        note(fmt::format("files {:.1f} MB in {:.0f} ms ({} reads)", c.file_mb, c.file_ms, c.file_reads));
    }
    if (c.other_cpu_pct >= 25.0) {
        note(fmt::format("other programs CPU {:.0f}%", c.other_cpu_pct));
    }
    if (c.page_faults >= 5000) {
        note(fmt::format("page faults {}", c.page_faults));
    }
    if (c.tracker_faults >= 100) {
        note(fmt::format("write-tracking faults {}", c.tracker_faults));
    }
    if (c.guest_maps >= 20) {
        note(fmt::format("guest memory maps {}", c.guest_maps));
    }
    if (img_created != 0 || img_mb + buf_mb >= 1.0) {
        note(fmt::format("textures +{} ({:.1f} MB), buffers {:.1f} MB, write-backs {:.1f} MB", img_created, img_mb,
                         buf_mb, wb_mb));
    }
    if (l.vram_budget_mb > 0.0 && l.vram_mb >= 0.97 * l.vram_budget_mb) {
        note(fmt::format("VRAM {:.0f}/{:.0f} MiB (at the budget)", l.vram_mb, l.vram_budget_mb));
    }
    if (l.ram_free_mb > 0.0 && l.ram_free_mb < 1024.0) {
        note(fmt::format("RAM free {:.0f} MiB", l.ram_free_mb));
    }
    g->txt << fmt::format("{}  t={:9.3f}s  frame {:7}  {:7.1f} ms  {:<17} | game {:.1f}  backend {:.1f}  gpu {:.1f}  "
                          "present {:.1f} | shaders {} ({:.1f} ms, {} from disk)  pipelines {} ({:.1f} ms)  "
                          "readback {:.1f}  recording {:.1f}  gpu busy {:.1f}  draws {}{}\n",
                          LocalTime(time), time, n, interval, kCauseNames[static_cast<size_t>(cause)], game_own,
                          backend, gpu, present, shaders, shader_ms, disk, pipelines, pipe_ms, sync_ms, record_ms,
                          gpu_busy_ms, draws, notes);
    g->txt.flush();
    g->csv << fmt::format("{},{:.3f},{},{:.2f},{},{:.2f},{:.2f},{:.2f},{:.2f},{},{:.2f},{},{},{:.2f},{:.2f},{:.2f},"
                          "{:.2f},{:.2f},{:.2f},{:.2f},{},{},{:.2f},{:.2f},{:.2f},{:.2f},{:.1f},{},{},{},{},{:.1f},"
                          "{:.1f},{:.0f},{:.0f},{:.0f},{:.2f},{:.2f},{:.2f}\n",
                          LocalTime(time), time, n, interval, kCauseNames[static_cast<size_t>(cause)], game_own,
                          backend, gpu, present, shaders, shader_ms, disk, pipelines, pipe_ms, sync_ms, record_ms,
                          par_wait_ms, wait_game_ms, wait_display_ms, gpu_busy_ms, draws, img_created, img_mb, buf_mb,
                          wb_mb, c.file_mb, c.file_ms, c.file_reads, c.page_faults, c.tracker_faults, c.guest_maps,
                          c.proc_cpu_pct, c.other_cpu_pct, l.vram_mb, l.vram_budget_mb, l.ram_free_mb, par_busy_ms,
                          front_busy_ms, front_wait_ms);
    g->csv.flush();
}

void WriteSecond(const Sample& now) {
    RefreshLevels();
    const Context c = Between(g->sec_start, now);
    const double secs = static_cast<double>(now.tsc - g->sec_start.tsc) / g->freq;
    const Levels& l = g->levels;
    const double t = Sec(now.tsc);
    const FrameLog::GpuThreadTotals& lv = g->prev.totals; // (the last frame taken: its levels)
    const double ours_mb = static_cast<double>(Vk::MemoryLedger::Bytes()) / kMiB;
    g->peak_vram_mb = std::max(g->peak_vram_mb, l.vram_mb);
    g->peak_ours_mb = std::max(g->peak_ours_mb, ours_mb);
    g->peak_images_mb = std::max(g->peak_images_mb, static_cast<double>(lv.img_resident_bytes) / kMiB);
    g->peak_buffers_mb = std::max(g->peak_buffers_mb, static_cast<double>(lv.buf_resident_bytes) / kMiB);
    g->over_budget_s += l.vram_budget_mb > 0.0 && l.vram_mb > l.vram_budget_mb ? 1 : 0;
    g->sec << fmt::format("{},{:.1f},{:.1f},{},{:.1f},{},{:.0f},{:.0f},{},{:.0f},{:.0f},{:.0f},{:.0f},{:.0f},{:.0f},{:.0f},"
                          "{:.1f},{:.1f},{},{},{},{},{},{:.0f},{:.0f},{:.0f},{:.0f},{:.0f}\n",
                          LocalTime(t).substr(0, 8), t, secs > 0.0 ? g->sec_frames / secs : 0.0, g->sec_frames,
                          g->sec_worst, g->sec_hitches, c.proc_cpu_pct, c.other_cpu_pct, c.page_faults,
                          l.working_set_mb, l.private_mb, l.ram_free_mb, l.commit_mb, l.commit_limit_mb, l.vram_mb,
                          l.vram_budget_mb, c.file_mb, c.file_ms, c.file_reads, c.guest_maps, c.tracker_faults,
                          g->sec_shaders, g->sec_pipelines, static_cast<double>(lv.img_resident_bytes) / kMiB,
                          static_cast<double>(lv.img_budget_bytes) / kMiB,
                          static_cast<double>(lv.buf_resident_bytes) / kMiB, ours_mb,
                          std::max(0.0, l.vram_mb - ours_mb));
    g->sec.flush();
    g->sec_start = now;
    g->sec_frames = g->sec_hitches = 0;
    g->sec_worst = 0.0;
    g->sec_shaders = g->sec_pipelines = 0;
}

void Tick() {
    const FrameLog::Progress pr = FrameLog::GetProgress(); // (before the sample: it covers every frame taken now)
    const Sample s = TakeSample();
    g->samples.push_back(s);
    if (g->samples.size() > kSamples) {
        g->samples.pop_front();
    }
    while (g->next < pr.present) {
        FrameLog::RowView r;
        if (!FrameLog::Read(g->next, r)) {
            g->have_prev = false;
            ++g->next;
            continue;
        }
        if (r.present == 0) {
            break; // (counted, its time not written yet: the next tick)
        }
        if (g->have_prev && g->prev.present != 0 && r.present > g->prev.present) {
            Analyze(g->prev, r, g->next);
        }
        g->prev = r;
        g->have_prev = true;
        ++g->next;
    }
    if (const u32 m = g_marks.load(std::memory_order_acquire); m != g->marks_seen) {
        g->marks_seen = m;
        const double t = Sec(g_mark_tsc.load(std::memory_order_relaxed));
        g->marks.push_back(t);
        u32 count = 0;
        const Hitch* worst = nullptr;
        for (auto it = g->hitches.rbegin(); it != g->hitches.rend() && it->t >= t - 5.0; ++it) {
            if (it->t <= t + 0.5) {
                ++count;
                worst = worst == nullptr || it->ms > worst->ms ? &*it : worst;
            }
        }
        g->txt << fmt::format("{}  t={:9.3f}s  F12 MARK: the player saw a stutter - slow frames in the 5 s before: {}",
                              LocalTime(t), t, count);
        if (worst != nullptr) {
            g->txt << fmt::format(" (worst {:.1f} ms at t={:.3f}s, {})", worst->ms, worst->t,
                                  kCauseNames[static_cast<size_t>(worst->cause)]);
        }
        g->txt << "\n";
        g->txt.flush();
    }
    if (g->sec_start.tsc == 0) {
        g->sec_start = s;
    } else if (static_cast<double>(s.tsc - g->sec_start.tsc) >= g->freq) {
        WriteSecond(s);
    }
}

void Run() {
    SetThreadDescription(GetCurrentThread(), L"SSPC:HitchLog");
    for (;;) {
        bool last = false;
        {
            std::unique_lock lock{g->mutex};
            g->cv.wait_for(lock, std::chrono::milliseconds(kTickMs), [] { return g->stop; });
            last = g->stop;
        }
        Tick();
        if (last) {
            return;
        }
    }
}

std::string Summary() {
    std::string out;
    const auto& iv = g->intervals;
    const double secs = g->samples.empty() ? 0.0 : Sec(g->samples.back().tsc);
    out += fmt::format("session {} ({:.1f} min of frames), slow frame: {:.0f} ms or more\n", g->dir.filename().string(),
                       secs / 60.0, g->threshold_ms);
    if (iv.empty()) {
        return out + "no frames shown\n";
    }
    std::vector<float> s = iv;
    std::sort(s.begin(), s.end());
    double sum = 0.0;
    for (const float v : s) {
        sum += v;
    }
    const auto pct = [&](double q) { return s[std::min(s.size() - 1, static_cast<size_t>(q * s.size()))]; };
    const size_t low_n = std::max<size_t>(1, s.size() / 100);
    double low_sum = 0.0;
    for (size_t i = s.size() - low_n; i < s.size(); ++i) {
        low_sum += s[i];
    }
    out += fmt::format("frames {}, average {:.1f} FPS, 1% low {:.1f} FPS; frame time p50 {:.1f} ms, p95 {:.1f} ms, "
                       "p99 {:.1f} ms, max {:.1f} ms\n",
                       s.size(), 1000.0 * s.size() / sum, 1000.0 * low_n / low_sum, pct(0.5), pct(0.95), pct(0.99),
                       s.back());
    const auto over = [&](double ms) { return s.end() - std::lower_bound(s.begin(), s.end(), static_cast<float>(ms)); };
    out += fmt::format("frames of 50 ms or more: {}, 100 ms: {}, 250 ms: {}, 1 s: {}\n", over(50), over(100),
                       over(250), over(1000));
    out += fmt::format("video memory: peak {:.0f} MiB of the OS's {:.0f} MiB, over it {} s; SecondSonPC's own peak "
                       "{:.0f} MiB (images {:.0f}, buffer cache {:.0f})\n",
                       g->peak_vram_mb, g->levels.vram_budget_mb, g->over_budget_s, g->peak_ours_mb,
                       g->peak_images_mb, g->peak_buffers_mb);
    std::array<u32, kCauses> count{};
    std::array<double, kCauses> total{};
    for (const Hitch& h : g->hitches) {
        ++count[static_cast<size_t>(h.cause)];
        total[static_cast<size_t>(h.cause)] += h.ms;
    }
    out += "slow frames by cause:\n";
    for (size_t i = 0; i < kCauses; ++i) {
        if (count[i] != 0) {
            out += fmt::format("  {:<17} {:5}  {:9.1f} ms\n", kCauseNames[i], count[i], total[i]);
        }
    }
    std::vector<Hitch> worst = g->hitches;
    std::sort(worst.begin(), worst.end(), [](const Hitch& a, const Hitch& b) { return a.ms > b.ms; });
    worst.resize(std::min<size_t>(worst.size(), 15));
    if (!worst.empty()) {
        out += "the worst:\n";
        for (const Hitch& h : worst) {
            out += fmt::format("  {}  t={:9.3f}s  frame {:7}  {:7.1f} ms  {}\n", LocalTime(h.t), h.t, h.frame, h.ms,
                               kCauseNames[static_cast<size_t>(h.cause)]);
        }
    }
    for (const double t : g->marks) {
        out += fmt::format("F12 mark at {} (t={:.3f}s)\n", LocalTime(t), t);
    }
    return out;
}

void PruneSessions(const std::filesystem::path& root) {
    std::error_code ec;
    std::vector<std::filesystem::path> dirs;
    for (const auto& e : std::filesystem::directory_iterator(root, ec)) {
        const std::string name = e.path().filename().string();
        if (e.is_directory(ec) && name.size() == 19 && name[4] == '-' && name[10] == '_') { // (2026-10-27_14-05-33)
            dirs.push_back(e.path());
        }
    }
    std::sort(dirs.begin(), dirs.end());
    for (size_t i = 0; i + kKeepSessions <= dirs.size(); ++i) { // (room for the new one)
        std::filesystem::remove_all(dirs[i], ec);
    }
}

} // namespace

void Mark() {
    g_mark_tsc.store(Kernel::Clock::ReadTsc(), std::memory_order_relaxed);
    g_marks.fetch_add(1, std::memory_order_release);
}

void Start(const StartInfo& info) {
    if (g != nullptr) {
        return;
    }
    if (char v[8] = {}; GetEnvironmentVariableA("SSPC_HITCH_LOG", v, sizeof(v)) > 0 && v[0] == '0') {
        SSPC_LOG(GPU, Info, "hitch log: off (SSPC_HITCH_LOG=0)");
        return;
    }
    auto* s = new State;
    if (char v[16] = {}; GetEnvironmentVariableA("SSPC_HITCH_MS", v, sizeof(v)) > 0) {
        s->threshold_ms = std::clamp(std::atof(v), 10.0, 10000.0);
    }
    s->logs_dir = info.logs_dir;
    s->faults = info.faults;
    s->faults_ctx = info.faults_ctx;
    s->freq = static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
    s->origin = FrameLog::StartTsc() != 0 ? FrameLog::StartTsc() : Kernel::Clock::ReadTsc();
    s->cores = std::max<u32>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    FILETIME utc{}, local{};
    GetSystemTimeAsFileTime(&utc);
    FileTimeToLocalFileTime(&utc, &local);
    const u64 since = static_cast<u64>(static_cast<double>(Kernel::Clock::ReadTsc() - s->origin) / s->freq * 1e7);
    s->origin_ft = Ft(local) - std::min(Ft(local), since);
    SYSTEMTIME st{};
    FileTimeToSystemTime(&local, &st);
    const std::filesystem::path root = info.logs_dir / "sessions";
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    PruneSessions(root);
    s->dir = root / fmt::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", st.wYear, st.wMonth, st.wDay, st.wHour,
                                st.wMinute, st.wSecond);
    std::filesystem::create_directories(s->dir, ec);
    s->txt.open(s->dir / "hitches.txt", std::ios::binary | std::ios::trunc);
    s->csv.open(s->dir / "hitches.csv", std::ios::binary | std::ios::trunc);
    s->sec.open(s->dir / "seconds.csv", std::ios::binary | std::ios::trunc);
    if (!s->txt || !s->csv || !s->sec) {
        SSPC_LOG(GPU, Warning, "hitch log: cannot write to {}", s->dir.string());
        delete s;
        return;
    }
    s->txt << fmt::format("Slow frames ({:.0f} ms or more after the previous one). Split of a frame's time: game = "
                          "the game making it (CPU), backend = the game waiting for SecondSonPC plus the command "
                          "processor's longer latency, gpu = the GPU's longer latency, present = presentation's. "
                          "F12 marks a stutter you saw.\n\n",
                          s->threshold_ms);
    s->csv << "time,t_s,frame,ms,cause,game_ms,backend_ms,gpu_ms,present_ms,shaders,shader_ms,shaders_from_disk,"
              "pipelines,pipeline_ms,readback_ms,record_ms,record_wait_ms,cp_wait_game_ms,cp_wait_display_ms,"
              "gpu_busy_ms,draws,textures_created,texture_mb,buffer_mb,writeback_mb,file_mb,file_ms,file_reads,"
              "page_faults,tracker_faults,guest_maps,proc_cpu_pct,other_cpu_pct,vram_mb,vram_budget_mb,ram_free_mb,"
              "record_busy_ms,front_busy_ms,front_wait_ms\n";
    s->sec << "time,t_s,fps,frames,worst_ms,slow_frames,proc_cpu_pct,other_cpu_pct,page_faults,working_set_mb,"
              "private_mb,ram_free_mb,commit_mb,commit_limit_mb,vram_mb,vram_budget_mb,file_mb,file_ms,file_reads,"
              "guest_maps,tracker_faults,shaders,pipelines,images_mb,image_budget_mb,buffer_cache_mb,"
              "sspc_vram_mb,other_vram_mb\n";
    s->intervals.reserve(1u << 16);
    g = s;
    RefreshLevels();
    g->thread = std::thread(Run);
    SSPC_LOG(GPU, Info, "hitch log: frames of {:.0f} ms or more -> {} (F12 marks a stutter)", g->threshold_ms,
             g->dir.string());
}

void Stop() {
    if (g == nullptr) {
        return;
    }
    {
        std::scoped_lock lock{g->mutex};
        g->stop = true;
    }
    g->cv.notify_all();
    if (g->thread.joinable()) {
        g->thread.join();
    }
    const std::string summary = Summary();
    std::ofstream(g->dir / "summary.txt", std::ios::binary | std::ios::trunc) << summary;
    std::error_code ec;
    std::filesystem::copy_file(g->logs_dir / "gpu_frames.csv", g->dir / "gpu_frames.csv",
                               std::filesystem::copy_options::overwrite_existing, ec);
    size_t from = 0;
    while (from < summary.size()) {
        const size_t end = summary.find('\n', from);
        SSPC_LOG(GPU, Info, "hitch log: {}", summary.substr(from, end - from));
        from = end == std::string::npos ? summary.size() : end + 1;
    }
    delete g;
    g = nullptr;
}

} // namespace SecondSon::Gpu::HitchLog
