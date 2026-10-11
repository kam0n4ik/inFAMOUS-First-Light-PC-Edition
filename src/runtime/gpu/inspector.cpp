// SecondSonPC — the Performance Inspector (P4.8). See inspector.h.
#include "runtime/gpu/inspector.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <ctime>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "base/vram_monitor.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::Inspector {

namespace detail {
std::atomic<bool> g_on{false};
}

namespace {

constexpr u32 kRing = 8192; ///< frames the writer may lag (4.5 min at 30 FPS; it writes every second)

/// One frame's inspector data (GPU thread), read by the writer once ready.
struct Row {
    std::atomic<u32> n{~0u};
    std::atomic<u8> ready{0}; ///< bit 0: CpFrame, bit 1: GpuFrame
    CpTotals cp;              ///< the frame's deltas (stage_ticks and cpu_ticks already in ns)
    u64 file_reads = 0, file_bytes = 0, file_ns = 0, maps = 0, unmaps = 0;
    u64 flip_thread_ns = 0;
    GpuFrameTimes gpu;
};

struct Options {
    u32 gpu_every = 1;
    u32 sample_hz = 250;
    std::filesystem::path dir, catalog;
};

struct ThreadInfo {
    HANDLE handle = nullptr;
    std::string name;
    u64 last_cycles = 0;
    u32 seen = 0; ///< the second it was last seen in a snapshot
};

/// Stack samples: one dictionary of stacks for the run, counts per one-second window.
class StackSampler {
public:
    static constexpr u32 kDepth = 16;
    static constexpr u32 kCap = 1u << 15;
    bool Start(HANDLE gpu_thread, u32 hz, const std::filesystem::path& dir);
    void Stop();
    u64 Samples() const { return samples_; }
    u64 Dropped() const { return dropped_; }
    u64 Stacks() const { return next_id_; }

private:
    struct Entry {
        u64 hash = 0;
        u32 id = 0;
        u8 kind = 0, depth = 0; ///< kind 1: the GPU thread (unwound), 2: the flipping game thread (no unwinder)
        u64 frames[kDepth]{};
    };
    void Run();
    void Count(u8 kind, const u64* frames, u32 depth);
    void FlushWindow(double t_s);

    HANDLE gpu_ = nullptr;
    u32 period_us_ = 4000;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::vector<Entry> table_;
    std::vector<u32> counts_; ///< by table slot, this window
    std::vector<u32> touched_;
    std::vector<u32> fresh_;  ///< slots first seen this window (written to stacks.csv)
    u32 next_id_ = 0;
    u64 samples_ = 0, dropped_ = 0, failed_ = 0;
    std::ofstream stacks_, counts_out_;
    u64 exe_lo_ = 0, exe_hi_ = 0;
};

struct State {
    Options opt;
    StartInfo info;
    Catalog catalog;
    std::unique_ptr<Row[]> rows;
    CpTotals prev_cp;                        ///< GPU thread: the last CpFrame's totals
    u64 prev_file_reads = 0, prev_file_bytes = 0, prev_file_ns = 0, prev_maps = 0, prev_unmaps = 0;
    u64 prev_flip_cycles = 0;
    DWORD flip_tid_open = 0;                 ///< GPU thread: whose handle flip_handle is
    HANDLE flip_handle = nullptr;
    std::atomic<u64> file_reads{0}, file_bytes{0}, file_ns{0}, maps{0}, unmaps{0};
    std::atomic<DWORD> flip_tid{0};
    std::thread writer;
    std::atomic<bool> stop{false};
    StackSampler sampler;
    u64 tsc_freq = 1, start_tsc = 0;
    std::string start_wall;
    // the writer's
    u32 written = 0;
    u64 rows_written = 0, rows_dropped = 0, rows_without_gpu = 0, rows_without_present = 0;
    FrameLog::RowView prev_view{};
    bool have_prev_view = false;
    std::unordered_map<DWORD, ThreadInfo> threads;
    u32 second = 0;
    std::vector<std::pair<u64, u32>> markers; ///< (TSC, frame) - Ctrl+F12
    std::ofstream frames, seconds, threads_out, markers_out;
    std::string line;
    // per second aggregates of the rows written in it
    std::array<CategoryCounters, kCategories> sec_cat{};
    u32 sec_frames = 0;
    u64 last_img_res = 0, last_buf_res = 0;
};

State* g_state = nullptr;

double Us(u64 ticks) {
    return g_state != nullptr ? static_cast<double>(ticks) * 1e6 / static_cast<double>(g_state->tsc_freq) : 0.0;
}

u64 TicksToNs(u64 ticks) {
    return g_state != nullptr ? Kernel::Clock::TicksTo(ticks, 1'000'000'000ull, g_state->tsc_freq) : 0;
}

std::string JsonEscape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (const char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    return out;
}

std::string Narrow(std::wstring_view w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string ThreadName(HANDLE h) {
    PWSTR desc = nullptr;
    std::string name;
    if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr) {
        name = Narrow(desc);
        LocalFree(desc);
    }
    return name;
}

std::string EnvString(const char* name) {
    char v[512] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

/// "light[,gpu=N][,sample=Hz][,catalog=<file>]"
bool ParseOptions(std::string_view spec, Options& o, std::string& error) {
    bool mode = false;
    while (!spec.empty()) {
        const size_t comma = spec.find(',');
        const std::string_view item = spec.substr(0, comma);
        spec.remove_prefix(comma == std::string_view::npos ? spec.size() : comma + 1);
        if (item == "light" || item == "1") {
            mode = true;
        } else if (item == "deep") {
            mode = true; // P4.8.2; until then the same as light
            SSPC_LOG(GPU, Warning, "SSPC_INSPECTOR=deep: the deep capture is not implemented yet - light");
        } else if (item.starts_with("gpu=")) {
            o.gpu_every = static_cast<u32>(std::strtoul(std::string(item.substr(4)).c_str(), nullptr, 10));
        } else if (item.starts_with("sample=")) {
            o.sample_hz = std::min<u32>(static_cast<u32>(std::strtoul(std::string(item.substr(7)).c_str(), nullptr, 10)),
                                        2000);
        } else if (item.starts_with("catalog=")) {
            o.catalog = std::string(item.substr(8));
        } else if (!item.empty()) {
            error = fmt::format("unknown item '{}'", item);
            return false;
        }
    }
    if (!mode) {
        error = "no mode (light)";
    }
    return mode;
}

std::filesystem::path FindCatalog() {
    std::vector<std::filesystem::path> where;
    std::error_code ec;
    where.push_back(std::filesystem::current_path(ec) / "tools" / "perf_catalog.txt");
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH) > 0) {
        // every directory above the exe (build\bin, and the A/B copies in build\ab\<name> - P4.9: the fixed three
        // levels missed those, and their runs classified everything by the rules only)
        for (std::filesystem::path dir = std::filesystem::path(exe).parent_path(); dir.has_relative_path();
             dir = dir.parent_path()) {
            where.push_back(dir / "tools" / "perf_catalog.txt");
        }
    }
    for (const auto& p : where) {
        if (std::filesystem::exists(p, ec)) {
            return p;
        }
    }
    return {};
}

void WriteModules(const std::filesystem::path& file) {
    HMODULE mods[1024];
    DWORD bytes = 0;
    std::ofstream out(file, std::ios::trunc);
    out << "name,base,size,path\n";
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &bytes)) {
        return;
    }
    for (DWORD i = 0; i < bytes / sizeof(HMODULE) && i < 1024; ++i) {
        MODULEINFO mi{};
        wchar_t path[MAX_PATH] = {};
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi))) {
            continue;
        }
        GetModuleFileNameW(mods[i], path, MAX_PATH);
        const std::string p = Narrow(path);
        const std::string name = std::filesystem::path(path).filename().string();
        out << fmt::format("{},{:#x},{:#x},\"{}\"\n", name, reinterpret_cast<u64>(mi.lpBaseOfDll), mi.SizeOfImage, p);
    }
}

std::string NowWall() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

void WriteSession(bool final) {
    State& s = *g_state;
    std::ofstream out(s.opt.dir / "session.json", std::ios::trunc);
    std::string env;
    if (LPWCH block = GetEnvironmentStringsW(); block != nullptr) {
        for (LPWCH p = block; *p != 0; p += wcslen(p) + 1) {
            const std::string kv = Narrow(p);
            if (kv.starts_with("SSPC_")) {
                env += fmt::format("{}\"{}\"", env.empty() ? "" : ", ", JsonEscape(kv));
            }
        }
        FreeEnvironmentStringsW(block);
    }
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::error_code ec;
    const auto exe_time = std::filesystem::last_write_time(exe, ec);
    const auto exe_size = std::filesystem::file_size(exe, ec);
    out << "{\n";
    out << fmt::format("  \"tool\": \"SecondSonPC Performance Inspector (P4.8.1)\",\n  \"mode\": \"light\",\n");
    out << fmt::format("  \"started\": \"{}\",\n  \"ended\": \"{}\",\n", s.start_wall, final ? NowWall() : "");
    out << fmt::format("  \"tsc_frequency\": {},\n  \"frame_log_start_tsc\": {},\n  \"inspector_start_tsc\": {},\n",
                       s.tsc_freq, FrameLog::StartTsc(), s.start_tsc);
    out << fmt::format("  \"device\": \"{}\",\n  \"timestamp_period_ns\": {},\n  \"fine_timing\": {},\n",
                       JsonEscape(s.info.device), s.info.timestamp_period, s.info.fine_timing ? "true" : "false");
    out << fmt::format("  \"gpu_every\": {},\n  \"sample_hz\": {},\n", s.opt.gpu_every, s.opt.sample_hz);
    out << fmt::format("  \"catalog\": \"{}\",\n  \"catalog_entries\": {},\n", JsonEscape(s.opt.catalog.string()),
                       s.catalog.Size());
    out << fmt::format("  \"exe\": \"{}\",\n  \"exe_size\": {},\n  \"exe_time\": {},\n", JsonEscape(Narrow(exe)),
                       ec ? 0 : exe_size,
                       ec ? 0 : std::chrono::duration_cast<std::chrono::seconds>(exe_time.time_since_epoch()).count());
    out << fmt::format("  \"command_line\": \"{}\",\n  \"env\": [{}],\n", JsonEscape(Narrow(GetCommandLineW())), env);
    out << fmt::format("  \"pid\": {},\n  \"categories\": [", GetCurrentProcessId());
    for (size_t i = 0; i < kCategories; ++i) {
        out << fmt::format("{}\"{}\"", i ? ", " : "", CategoryName(static_cast<Category>(i)));
    }
    out << "],\n";
    out << fmt::format("  \"frames_written\": {},\n  \"frames_dropped\": {},\n  \"frames_without_gpu_times\": {},\n"
                       "  \"frames_without_present\": {},\n",
                       s.rows_written, s.rows_dropped, s.rows_without_gpu, s.rows_without_present);
    out << fmt::format("  \"stack_samples\": {},\n  \"stack_samples_dropped\": {},\n  \"stacks\": {},\n",
                       s.sampler.Samples(), s.sampler.Dropped(), s.sampler.Stacks());
    out << fmt::format("  \"markers\": {},\n  \"final\": {}\n}}\n", s.markers.size(), final ? "true" : "false");
}

// ------------------------------------------------------------------------------------------- the writer thread

void Field(std::string& line, u64 v) {
    line += ',';
    if (v != 0) {
        line += fmt::format("{}", v);
    }
}

void FieldUs(std::string& line, double us) {
    line += ',';
    if (us >= 0.5) {
        line += fmt::format("{}", static_cast<u64>(us + 0.5));
    }
}

void WriteFramesHeader(std::ofstream& out) {
    out << "n,game_submit_us,flip_enter_us,game_flip_us,done_enter_us,done_exit_us,submits,cp_flip_us,last_unblock_us,"
           "gpu_done_us,present_us,game_idle_wait_us,cp_us,record_us,sync_us,blocked_label_us,blocked_other_us,empty_us,"
           "gpu_busy_us,draws,dispatches,batches,blocks,shaders,shader_us,pipelines,pipeline_us,module_us,"
           "pre_us,round_us,poll_us,flush_us,idle_label_us,idle_other_us,idle_empty_us,idle_gpu_us,sync_cp_us,"
           "sync_front_us,sync_read_us,syncs,submit_us,wb_kib,buf_up_kib,img_ups,img_up_kib,img_new,stream_kib,"
           "mirror_kib,barriers,rpasses,img_res_mib,buf_res_mib,file_reads,file_kib,file_us,maps,unmaps,flip_thread_us,"
           "st_front_us,st_bind_us,st_range_us,st_image_us,st_pipe_us,st_targets_us,st_vertex_us,st_layout_us,"
           "st_emit_us,st_dispatch_us,gpu_frame_us,gpu_batches,gpu_timed,gpu_overflow,gx_transfer_us,gx_flipcopy_us,"
           "gx_batchstart_us";
    for (size_t c = 0; c < kCategories; ++c) {
        const std::string_view n = CategoryName(static_cast<Category>(c));
        out << fmt::format(",d_{0},x_{0},cpu_{0}_us,gpu_{0}_us", n);
    }
    out << '\n';
}

void WriteFrameRow(State& s, u32 n, const FrameLog::RowView& v, const Row* r) {
    std::string& line = s.line;
    line.clear();
    const u64 t0 = FrameLog::StartTsc();
    const auto t = [&](u64 tsc) { return tsc > t0 ? Us(tsc - t0) : 0.0; };
    line += fmt::format("{}", n);
    FieldUs(line, t(v.game_submit));
    FieldUs(line, t(v.flip_enter));
    FieldUs(line, t(v.game_flip));
    FieldUs(line, t(v.done_enter));
    FieldUs(line, t(v.done_exit));
    Field(line, v.submits);
    FieldUs(line, t(v.cp_flip));
    FieldUs(line, t(v.last_unblock));
    FieldUs(line, t(v.gpu_done));
    FieldUs(line, t(v.present));
    const FrameLog::RowView& p = s.have_prev_view ? s.prev_view : FrameLog::RowView{};
    const auto dus = [](u64 a, u64 b) { return a >= b ? static_cast<double>(a - b) / 1e3 : 0.0; };
    const auto dn = [](u64 a, u64 b) { return a >= b ? a - b : 0; };
    const auto& tt = v.totals;
    const auto& pt = p.totals;
    FieldUs(line, dus(v.idle_wait_ns, p.idle_wait_ns));
    FieldUs(line, dus(tt.cp_ns, pt.cp_ns));
    FieldUs(line, dus(tt.record_ns, pt.record_ns));
    FieldUs(line, dus(tt.sync_ns, pt.sync_ns));
    FieldUs(line, dus(tt.blocked_label_ns, pt.blocked_label_ns));
    FieldUs(line, dus(tt.blocked_other_ns, pt.blocked_other_ns));
    FieldUs(line, dus(tt.empty_ns, pt.empty_ns));
    FieldUs(line, dus(tt.gpu_busy_ns, pt.gpu_busy_ns));
    Field(line, dn(tt.draws, pt.draws));
    Field(line, dn(tt.dispatches, pt.dispatches));
    Field(line, dn(tt.batches, pt.batches));
    Field(line, dn(tt.blocks, pt.blocks));
    Field(line, dn(tt.shaders, pt.shaders));
    FieldUs(line, dus(tt.shader_ns, pt.shader_ns));
    Field(line, dn(tt.pipelines, pt.pipelines));
    FieldUs(line, dus(tt.pipeline_ns, pt.pipeline_ns));
    FieldUs(line, dus(tt.module_ns, pt.module_ns));
    if (r == nullptr) {
        line += std::string(47 + 4 * kCategories, ',');
        line += '\n';
        return;
    }
    const CpTotals& c = r->cp;
    for (const u64 ns : {c.pre_ns, c.round_ns, c.poll_ns, c.flush_ns, c.idle_label_ns, c.idle_other_ns, c.idle_empty_ns,
                         c.idle_gpu_ns, c.sync_ns[0], c.sync_ns[1], c.sync_ns[2]}) {
        FieldUs(line, static_cast<double>(ns) / 1e3);
    }
    Field(line, c.syncs[0] + c.syncs[1] + c.syncs[2]);
    FieldUs(line, static_cast<double>(c.submit_ns) / 1e3);
    Field(line, c.wb_bytes >> 10);
    Field(line, c.buf_upload_bytes >> 10);
    Field(line, c.img_uploads);
    Field(line, c.img_upload_bytes >> 10);
    Field(line, c.img_created);
    Field(line, c.streamed_bytes >> 10);
    Field(line, c.mirrored_bytes >> 10);
    Field(line, c.barriers);
    Field(line, c.render_passes);
    Field(line, c.img_resident_bytes >> 20);
    Field(line, c.buf_resident_bytes >> 20);
    Field(line, r->file_reads);
    Field(line, r->file_bytes >> 10);
    FieldUs(line, static_cast<double>(r->file_ns) / 1e3);
    Field(line, r->maps);
    Field(line, r->unmaps);
    FieldUs(line, static_cast<double>(r->flip_thread_ns) / 1e3);
    for (const u64 ns : c.stage_ticks) {
        FieldUs(line, static_cast<double>(ns) / 1e3);
    }
    const bool gpu = (r->ready.load(std::memory_order_acquire) & 2) != 0;
    const GpuFrameTimes& g = r->gpu;
    FieldUs(line, gpu ? static_cast<double>(g.busy_ns) / 1e3 : 0.0);
    Field(line, gpu ? g.batches : 0);
    Field(line, gpu && g.timed ? 1 : 0);
    Field(line, gpu ? g.overflow : 0);
    for (const size_t k : {kGpuTransfer, kGpuFrameCopy, kGpuBatchStart}) {
        FieldUs(line, gpu && g.timed ? static_cast<double>(g.slot_ns[k]) / 1e3 : 0.0);
    }
    for (size_t k = 0; k < kCategories; ++k) {
        const CategoryCounters& cc = c.categories[k];
        Field(line, cc.draws);
        Field(line, cc.dispatches);
        FieldUs(line, static_cast<double>(cc.cpu_ticks) / 1e3); // (ns by CpFrame)
        FieldUs(line, gpu && g.timed ? static_cast<double>(g.slot_ns[k]) / 1e3 : 0.0);
        s.sec_cat[k].instances += cc.instances;
        s.sec_cat[k].vertices += cc.vertices;
        s.sec_cat[k].groups += cc.groups;
        s.sec_cat[k].draws += cc.draws;
        s.sec_cat[k].dispatches += cc.dispatches;
    }
    s.last_img_res = c.img_resident_bytes;
    s.last_buf_res = c.buf_resident_bytes;
    line += '\n';
}

/// Writes the frames that are complete (or old enough). `all`: everything up to the frame log's end (the stop).
void WriteFrames(State& s, bool all) {
    const FrameLog::Progress pr = FrameLog::GetProgress();
    constexpr u32 kLate = 120; ///< frames after which a row is written without its late parts
    while (s.written < pr.cp) {
        const u32 n = s.written;
        Row& r = s.rows[n % kRing];
        const u32 rn = r.n.load(std::memory_order_acquire);
        const u8 ready = r.ready.load(std::memory_order_acquire);
        const bool late = all || pr.cp > n + kLate;
        if (rn != n || (ready & 1) == 0) {
            if (rn != n && rn != ~0u && rn > n) {
                ++s.rows_dropped; // overwritten: the writer fell behind by the ring
                ++s.written;
                continue;
            }
            if (!late) {
                break;
            }
        }
        if (!late && ((ready & 2) == 0 || pr.present <= n)) {
            break;
        }
        FrameLog::RowView v{};
        if (!FrameLog::Read(n, v)) {
            ++s.rows_dropped;
            ++s.written;
            continue;
        }
        const bool have = rn == n && (ready & 1) != 0;
        s.rows_without_gpu += have && (ready & 2) == 0 ? 1 : 0;
        s.rows_without_present += v.present == 0 ? 1 : 0;
        WriteFrameRow(s, n, v, have ? &r : nullptr);
        s.frames << s.line;
        s.prev_view = v;
        s.have_prev_view = true;
        ++s.rows_written;
        ++s.sec_frames;
        ++s.written;
    }
    s.frames.flush();
}

void SampleThreads(State& s, double t_s) {
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(THREADENTRY32);
    const DWORD pid = GetCurrentProcessId();
    ++s.second;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        auto [it, fresh] = s.threads.try_emplace(te.th32ThreadID);
        ThreadInfo& ti = it->second;
        if (fresh) {
            ti.handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        }
        ti.seen = s.second;
        if (ti.handle == nullptr) {
            continue;
        }
        if (ti.name.empty() && (fresh || s.second % 10 == 0)) {
            ti.name = ThreadName(ti.handle);
        }
        ULONG64 cycles = 0;
        if (!QueryThreadCycleTime(ti.handle, &cycles)) {
            continue;
        }
        const u64 d = fresh ? 0 : cycles - std::min<u64>(cycles, ti.last_cycles);
        ti.last_cycles = cycles;
        const double ms = Us(d) / 1e3;
        if (ms >= 1.0) {
            std::string name = ti.name;
            std::ranges::replace(name, ',', ';');
            s.threads_out << fmt::format("{:.1f},{},{},{:.1f}\n", t_s, te.th32ThreadID, name, ms);
        }
    }
    CloseHandle(snap);
    // threads gone: their handles closed
    for (auto it = s.threads.begin(); it != s.threads.end();) {
        if (it->second.seen + 2 < s.second) {
            if (it->second.handle != nullptr) {
                CloseHandle(it->second.handle);
            }
            it = s.threads.erase(it);
        } else {
            ++it;
        }
    }
    s.threads_out.flush();
}

void SampleSecond(State& s, double t_s) {
    PROCESS_MEMORY_COUNTERS_EX pm{};
    PERFORMANCE_INFORMATION sys{};
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
    GetPerformanceInfo(&sys, sizeof(sys));
    Base::VideoMemoryInfo vm{};
    const bool have_vm = Base::QueryVideoMemory(vm);
    constexpr u64 kMiB = 1ull << 20;
    const u64 page = std::max<u64>(sys.PageSize, 1);
    std::string& line = s.line;
    line = fmt::format("{:.1f},{},{},{},{},{},{},{},", t_s, s.sec_frames, pm.PrivateUsage / kMiB,
                       pm.WorkingSetSize / kMiB, pm.PagefileUsage / kMiB, sys.CommitTotal * page / kMiB,
                       sys.CommitLimit * page / kMiB, sys.PhysicalAvailable * page / kMiB);
    line += have_vm ? fmt::format("{},{},{}", vm.local_usage / kMiB, vm.local_budget / kMiB, vm.nonlocal_usage / kMiB)
                    : std::string(",,");
    line += fmt::format(",{},{}", s.last_img_res / kMiB, s.last_buf_res / kMiB);
    for (size_t k = 0; k < kCategories; ++k) {
        Field(line, s.sec_cat[k].instances);
        Field(line, s.sec_cat[k].vertices);
        Field(line, s.sec_cat[k].groups);
    }
    line += '\n';
    s.seconds << line;
    s.seconds.flush();
    s.sec_cat = {};
    s.sec_frames = 0;
}

void WriterLoop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:Inspector");
    State& s = *g_state;
    auto next = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    bool hot_down = false;
    while (!s.stop.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Ctrl+F12: a marker at this moment (the player saw something worth a look)
        const bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 && (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
        if (down && !hot_down) {
            const u64 tsc = Kernel::Clock::ReadTsc();
            const u32 n = FrameLog::GetProgress().cp;
            s.markers.emplace_back(tsc, n);
            const u64 t0 = FrameLog::StartTsc();
            s.markers_out << fmt::format("{},{},{}\n", s.markers.size(), static_cast<u64>(Us(tsc - std::min(tsc, t0))), n);
            s.markers_out.flush();
            SSPC_LOG(GPU, Info, "Performance Inspector: marker {} at frame {}", s.markers.size(), n);
        }
        hot_down = down;
        if (std::chrono::steady_clock::now() < next) {
            continue;
        }
        next += std::chrono::seconds(1);
        const u64 now = Kernel::Clock::ReadTsc(), t0 = FrameLog::StartTsc();
        const double t_s = Us(now - std::min(now, t0)) / 1e6;
        WriteFrames(s, false);
        SampleSecond(s, t_s);
        SampleThreads(s, t_s);
    }
}

// ------------------------------------------------------------------------------------------- the stack sampler

/// Reads this process's memory without faulting.
bool SafeRead(u64 addr, void* dst, size_t bytes) {
    SIZE_T got = 0;
    return addr != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), dst, bytes, &got) != FALSE &&
           got == bytes;
}

bool StackSampler::Start(HANDLE gpu_thread, u32 hz, const std::filesystem::path& dir) {
    if (hz == 0 || gpu_thread == nullptr) {
        return false;
    }
    if (!DuplicateHandle(GetCurrentProcess(), gpu_thread, GetCurrentProcess(), &gpu_,
                         THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        gpu_ = nullptr;
        return false;
    }
    period_us_ = std::max<u32>(1'000'000 / hz, 500);
    table_.assign(kCap, Entry{});
    counts_.assign(kCap, 0);
    touched_.reserve(kCap);
    fresh_.reserve(kCap);
    const auto exe = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe + dos->e_lfanew);
    exe_lo_ = exe;
    exe_hi_ = exe + nt->OptionalHeader.SizeOfImage;
    stacks_.open(dir / "stacks.csv", std::ios::trunc);
    stacks_ << "id,thread,frames\n";
    counts_out_.open(dir / "cpu_stacks.csv", std::ios::trunc);
    counts_out_ << "t_s,thread,stack,count\n";
    stop_.store(false);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void StackSampler::Stop() {
    stop_.store(true);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (gpu_ != nullptr) {
        CloseHandle(gpu_);
        gpu_ = nullptr;
    }
}

void StackSampler::Count(u8 kind, const u64* frames, u32 depth) {
    u64 h = 0xcbf29ce484222325ull ^ kind;
    for (u32 i = 0; i < depth; ++i) {
        h = (h ^ frames[i]) * 0x100000001b3ull;
    }
    h |= 1; // 0: an empty slot
    u32 i = static_cast<u32>((h * 0x9E3779B97F4A7C15ull) >> (64 - 15));
    for (u32 probe = 0; probe < 64; ++probe, i = (i + 1) & (kCap - 1)) {
        Entry& e = table_[i];
        if (e.hash == 0) {
            e.hash = h;
            e.kind = kind;
            e.depth = static_cast<u8>(depth);
            std::copy_n(frames, depth, e.frames);
            e.id = next_id_++;
            fresh_.push_back(i);
        } else if (e.hash != h || e.kind != kind || e.depth != depth ||
                   !std::equal(frames, frames + depth, e.frames)) {
            continue;
        }
        if (counts_[i]++ == 0) {
            touched_.push_back(i);
        }
        ++samples_;
        return;
    }
    ++dropped_;
}

void StackSampler::FlushWindow(double t_s) {
    for (const u32 i : fresh_) {
        const Entry& e = table_[i];
        std::string line = fmt::format("{},{},", e.id, e.kind == 1 ? 'g' : 'm');
        for (u32 k = 0; k < e.depth; ++k) {
            line += fmt::format("{}{:x}", k ? ";" : "", e.frames[k]);
        }
        stacks_ << line << '\n';
    }
    fresh_.clear();
    for (const u32 i : touched_) {
        const Entry& e = table_[i];
        counts_out_ << fmt::format("{:.0f},{},{},{}\n", t_s, e.kind == 1 ? 'g' : 'm', e.id, counts_[i]);
        counts_[i] = 0;
    }
    touched_.clear();
    stacks_.flush();
    counts_out_.flush();
}

void StackSampler::Run() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:InspectorSampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    HANDLE flip = nullptr;
    DWORD flip_tid = 0;
    u64 frames[kDepth];
    auto window_end = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    u64 tick = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        if (timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(period_us_) * 10;
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 100);
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(period_us_));
        }
        ++tick;
        // the GPU thread with its stack (the unwinder: RtlLookupFunctionEntry/RtlVirtualUnwind, as CpuSampler)
        if (SuspendThread(gpu_) != static_cast<DWORD>(-1)) {
            // nothing below allocates, logs or takes a lock the target may hold
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            u32 n = 0;
            if (GetThreadContext(gpu_, &ctx)) {
                for (; n < kDepth && ctx.Rip != 0; ++n) {
                    DWORD64 image_base = 0;
                    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
                    // a return address: the call is the byte before it (its function, not the next one)
                    const u64 pc = n == 0 ? ctx.Rip : ctx.Rip - 1;
                    if (fe == nullptr) {
                        frames[n] = pc;
                        DWORD64 ret = 0;
                        if (ctx.Rsp == 0 || !SafeRead(ctx.Rsp, &ret, sizeof(ret))) {
                            ++n;
                            break;
                        }
                        ctx.Rip = ret;
                        ctx.Rsp += 8;
                        continue;
                    }
                    frames[n] = image_base + fe->BeginAddress; // the function, not the instruction
                    PVOID handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fe, &ctx, &handler_data, &establisher,
                                     nullptr);
                }
            }
            ResumeThread(gpu_);
            if (n != 0) {
                Count(1, frames, n);
            } else {
                ++failed_;
            }
        }
        // the game thread that flips (every other tick): its instruction pointer and the nearest return addresses
        // into the executable on its stack (guest code has no unwind data)
        if ((tick & 1) == 0 && g_state != nullptr) {
            const DWORD tid = g_state->flip_tid.load(std::memory_order_relaxed);
            if (tid != flip_tid) {
                if (flip != nullptr) {
                    CloseHandle(flip);
                }
                flip = tid != 0 ? OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid) : nullptr;
                flip_tid = tid;
            }
            if (flip != nullptr && SuspendThread(flip) != static_cast<DWORD>(-1)) {
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
                u32 n = 0;
                u64 sp[256];
                if (GetThreadContext(flip, &ctx)) {
                    frames[n++] = ctx.Rip;
                    if (SafeRead(ctx.Rsp, sp, sizeof(sp))) {
                        for (u32 k = 0; k < 256 && n < 3; ++k) {
                            if (sp[k] >= exe_lo_ && sp[k] < exe_hi_) {
                                frames[n++] = sp[k] - 1;
                            }
                        }
                    }
                }
                ResumeThread(flip);
                if (n != 0) {
                    Count(2, frames, n);
                }
            }
        }
        if (const auto now = std::chrono::steady_clock::now(); now >= window_end) {
            window_end += std::chrono::seconds(1);
            const u64 tsc = Kernel::Clock::ReadTsc(), t0 = FrameLog::StartTsc();
            FlushWindow(Us(tsc - std::min(tsc, t0)) / 1e6);
        }
    }
    const u64 tsc = Kernel::Clock::ReadTsc(), t0 = FrameLog::StartTsc();
    FlushWindow(Us(tsc - std::min(tsc, t0)) / 1e6);
    if (flip != nullptr) {
        CloseHandle(flip);
    }
    if (timer != nullptr) {
        CloseHandle(timer);
    }
}

} // namespace

bool Start(const StartInfo& info) {
    const std::string spec = EnvString("SSPC_INSPECTOR");
    if (spec.empty() || g_state != nullptr) {
        return false;
    }
    Options opt;
    std::string error;
    if (!ParseOptions(spec, opt, error)) {
        SSPC_LOG(GPU, Error, "SSPC_INSPECTOR='{}': {} - the Performance Inspector stays off", spec, error);
        return false;
    }
    auto s = std::make_unique<State>();
    s->info = info;
    s->tsc_freq = std::max<u64>(Kernel::Clock::TscFrequency(), 1);
    s->start_tsc = Kernel::Clock::ReadTsc();
    s->start_wall = NowWall();
    if (const std::string dir = EnvString("SSPC_INSPECTOR_DIR"); !dir.empty()) {
        opt.dir = dir;
    } else {
        std::string stamp = s->start_wall;
        std::ranges::replace(stamp, ':', '-');
        std::ranges::replace(stamp, 'T', '_');
        opt.dir = info.logs_dir / "performance_inspector" / stamp;
    }
    std::error_code ec;
    std::filesystem::create_directories(opt.dir, ec);
    if (ec) {
        SSPC_LOG(GPU, Error, "Performance Inspector: cannot create {}: {} - off", opt.dir.string(), ec.message());
        return false;
    }
    if (opt.catalog.empty()) {
        if (const std::string c = EnvString("SSPC_INSPECTOR_CATALOG"); !c.empty()) {
            opt.catalog = c;
        } else {
            opt.catalog = FindCatalog();
        }
    }
    if (!opt.catalog.empty()) {
        std::string cerr;
        if (!s->catalog.Load(opt.catalog, cerr)) {
            SSPC_LOG(GPU, Warning, "Performance Inspector: catalog {}: {}", opt.catalog.string(), cerr);
        } else if (!cerr.empty()) {
            SSPC_LOG(GPU, Warning, "Performance Inspector: catalog {}: {}", opt.catalog.string(), cerr);
        }
    } else {
        SSPC_LOG(GPU, Warning, "Performance Inspector: no effect catalog (tools\\perf_catalog.txt): every command is "
                               "classified by the rules only");
    }
    if (info.timestamp_period <= 0.0) {
        opt.gpu_every = 0;
    }
    s->opt = opt;
    s->rows = std::make_unique<Row[]>(kRing);
    s->frames.open(opt.dir / "frames.csv", std::ios::trunc);
    WriteFramesHeader(s->frames);
    s->seconds.open(opt.dir / "seconds.csv", std::ios::trunc);
    s->seconds << "t_s,frames,private_mib,working_set_mib,process_commit_mib,system_commit_mib,system_limit_mib,"
                  "physical_available_mib,vram_local_mib,vram_budget_mib,vram_nonlocal_mib,own_images_mib,own_buffers_mib";
    for (size_t k = 0; k < kCategories; ++k) {
        s->seconds << fmt::format(",i_{0},v_{0},w_{0}", CategoryName(static_cast<Category>(k)));
    }
    s->seconds << '\n';
    s->threads_out.open(opt.dir / "threads.csv", std::ios::trunc);
    s->threads_out << "t_s,tid,name,cpu_ms\n";
    s->markers_out.open(opt.dir / "markers.csv", std::ios::trunc);
    s->markers_out << "marker,t_us,frame\n";
    WriteModules(opt.dir / "modules.csv");
    g_state = s.release();
    WriteSession(false);
    g_state->writer = std::thread(WriterLoop);
    detail::g_on.store(true, std::memory_order_release);
    SSPC_LOG(GPU, Warning,
             "Performance Inspector (P4.8) ON: {} - GPU category timestamps every {} frame(s), stack samples {} Hz, "
             "catalog {} ({} programs); Ctrl+F12 marks a moment",
             opt.dir.string(), opt.gpu_every, opt.sample_hz, opt.catalog.string(), g_state->catalog.Size());
    return true;
}

void SetGpuThread(HANDLE thread) {
    if (g_state != nullptr && g_state->sampler.Start(thread, g_state->opt.sample_hz, g_state->opt.dir)) {
        SSPC_LOG(GPU, Info, "Performance Inspector: the GPU thread is sampled at {} Hz", g_state->opt.sample_hz);
    }
}

void StopSampling() {
    if (g_state != nullptr) {
        g_state->sampler.Stop();
    }
}

void Stop() {
    State* s = g_state;
    if (s == nullptr) {
        return;
    }
    detail::g_on.store(false, std::memory_order_release);
    s->sampler.Stop();
    s->stop.store(true, std::memory_order_release);
    if (s->writer.joinable()) {
        s->writer.join();
    }
    WriteFrames(*s, true);
    const u64 now = Kernel::Clock::ReadTsc(), t0 = FrameLog::StartTsc();
    SampleSecond(*s, Us(now - std::min(now, t0)) / 1e6);
    WriteModules(s->opt.dir / "modules.csv"); // (the modules loaded since)
    WriteSession(true);
    SSPC_LOG(GPU, Info, "Performance Inspector: {} frames written ({} dropped), {} stack samples -> {}",
             s->rows_written, s->rows_dropped, s->sampler.Samples(), s->opt.dir.string());
    for (auto& [tid, ti] : s->threads) {
        if (ti.handle != nullptr) {
            CloseHandle(ti.handle);
        }
    }
    if (s->flip_handle != nullptr) {
        CloseHandle(s->flip_handle);
    }
    // the state stays (other threads may still call a hook after the flag): it is small
}

const Catalog* GetCatalog() {
    return g_state != nullptr ? &g_state->catalog : nullptr;
}

bool WriteFile(std::string_view name, const std::string& text) {
    if (g_state == nullptr) {
        return false;
    }
    std::ofstream out(g_state->opt.dir / std::string(name), std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

u32 GpuEvery() {
    return g_state != nullptr ? g_state->opt.gpu_every : 0;
}

void CpFrame(u32 n, const CpTotals& t) {
    State* s = g_state;
    if (s == nullptr || !On()) {
        return;
    }
    Row& r = s->rows[n % kRing];
    r.ready.store(0, std::memory_order_relaxed);
    r.n.store(~0u, std::memory_order_release);
    const CpTotals& p = s->prev_cp;
    CpTotals& d = r.cp;
    const auto dn = [](u64 a, u64 b) { return a >= b ? a - b : 0; };
    d.pre_ns = dn(t.pre_ns, p.pre_ns);
    d.round_ns = dn(t.round_ns, p.round_ns);
    d.poll_ns = dn(t.poll_ns, p.poll_ns);
    d.flush_ns = dn(t.flush_ns, p.flush_ns);
    d.idle_label_ns = dn(t.idle_label_ns, p.idle_label_ns);
    d.idle_other_ns = dn(t.idle_other_ns, p.idle_other_ns);
    d.idle_empty_ns = dn(t.idle_empty_ns, p.idle_empty_ns);
    d.idle_gpu_ns = dn(t.idle_gpu_ns, p.idle_gpu_ns);
    for (size_t i = 0; i < 3; ++i) {
        d.sync_ns[i] = dn(t.sync_ns[i], p.sync_ns[i]);
        d.syncs[i] = dn(t.syncs[i], p.syncs[i]);
    }
    d.submit_ns = dn(t.submit_ns, p.submit_ns);
    d.wb_bytes = dn(t.wb_bytes, p.wb_bytes);
    d.buf_upload_bytes = dn(t.buf_upload_bytes, p.buf_upload_bytes);
    d.img_uploads = dn(t.img_uploads, p.img_uploads);
    d.img_upload_bytes = dn(t.img_upload_bytes, p.img_upload_bytes);
    d.img_created = dn(t.img_created, p.img_created);
    d.streamed_bytes = dn(t.streamed_bytes, p.streamed_bytes);
    d.mirrored_bytes = dn(t.mirrored_bytes, p.mirrored_bytes);
    d.barriers = dn(t.barriers, p.barriers);
    d.render_passes = dn(t.render_passes, p.render_passes);
    d.img_resident_bytes = t.img_resident_bytes;
    d.buf_resident_bytes = t.buf_resident_bytes;
    for (size_t i = 0; i < d.stage_ticks.size(); ++i) {
        d.stage_ticks[i] = TicksToNs(dn(t.stage_ticks[i], p.stage_ticks[i]));
    }
    for (size_t k = 0; k < kCategories; ++k) {
        const CategoryCounters& a = t.categories[k];
        const CategoryCounters& b = p.categories[k];
        d.categories[k] = {dn(a.draws, b.draws),           dn(a.instances, b.instances),
                           dn(a.vertices, b.vertices),     dn(a.dispatches, b.dispatches),
                           dn(a.groups, b.groups),         TicksToNs(dn(a.cpu_ticks, b.cpu_ticks))};
    }
    s->prev_cp = t;
    const u64 fr = s->file_reads.load(std::memory_order_relaxed), fb = s->file_bytes.load(std::memory_order_relaxed),
              fn = s->file_ns.load(std::memory_order_relaxed), mp = s->maps.load(std::memory_order_relaxed),
              um = s->unmaps.load(std::memory_order_relaxed);
    r.file_reads = dn(fr, s->prev_file_reads);
    r.file_bytes = dn(fb, s->prev_file_bytes);
    r.file_ns = dn(fn, s->prev_file_ns);
    r.maps = dn(mp, s->prev_maps);
    r.unmaps = dn(um, s->prev_unmaps);
    s->prev_file_reads = fr;
    s->prev_file_bytes = fb;
    s->prev_file_ns = fn;
    s->prev_maps = mp;
    s->prev_unmaps = um;
    // the flipping game thread's CPU time this frame (cycles at the TSC's rate)
    r.flip_thread_ns = 0;
    if (const DWORD tid = s->flip_tid.load(std::memory_order_relaxed); tid != 0) {
        if (tid != s->flip_tid_open) {
            if (s->flip_handle != nullptr) {
                CloseHandle(s->flip_handle);
            }
            s->flip_handle = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
            s->flip_tid_open = tid;
            s->prev_flip_cycles = 0;
        }
        ULONG64 cycles = 0;
        if (s->flip_handle != nullptr && QueryThreadCycleTime(s->flip_handle, &cycles)) {
            r.flip_thread_ns = s->prev_flip_cycles != 0 ? TicksToNs(dn(cycles, s->prev_flip_cycles)) : 0;
            s->prev_flip_cycles = cycles;
        }
    }
    r.gpu = {};
    r.n.store(n, std::memory_order_release);
    r.ready.store(1, std::memory_order_release);
}

void GpuFrame(u32 n, const GpuFrameTimes& times) {
    State* s = g_state;
    if (s == nullptr || !On()) {
        return;
    }
    Row& r = s->rows[n % kRing];
    if (r.n.load(std::memory_order_acquire) != n) {
        return; // (its CpFrame was not recorded: the inspector started in between)
    }
    r.gpu = times;
    r.ready.fetch_or(2, std::memory_order_acq_rel);
}

void FileRead(u64 bytes, u64 ns) {
    if (State* s = g_state; s != nullptr && On()) {
        s->file_reads.fetch_add(1, std::memory_order_relaxed);
        s->file_bytes.fetch_add(bytes, std::memory_order_relaxed);
        s->file_ns.fetch_add(ns, std::memory_order_relaxed);
    }
}

void FlipThread() {
    if (State* s = g_state; s != nullptr && On()) {
        const DWORD tid = GetCurrentThreadId();
        if (s->flip_tid.load(std::memory_order_relaxed) != tid) {
            s->flip_tid.store(tid, std::memory_order_relaxed);
        }
    }
}

void GuestMap(bool unmap) {
    if (State* s = g_state; s != nullptr && On()) {
        (unmap ? s->unmaps : s->maps).fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace SecondSon::Gpu::Inspector
