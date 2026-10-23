// SecondSonPC R9 - the gameplay profiler. See gprof.h.
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>

#include "runtime/gpu/r9/gprof.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "engine/guest_hooks.h"
#include "runtime/kernel/clock.h"

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace SecondSon::Gpu::Gprof {

namespace {

u8 ReadMode() {
    char v[8] = {};
    if (GetEnvironmentVariableA("SSPC_R9_GPROF", v, sizeof(v)) == 0) {
        return 0;
    }
    return v[0] == '2' ? 2 : (v[0] == '1' ? 1 : 0);
}

u32 EnvU32(const char* name, u32 fallback) {
    char v[16] = {};
    if (GetEnvironmentVariableA(name, v, sizeof(v)) == 0) {
        return fallback;
    }
    return static_cast<u32>(std::strtoul(v, nullptr, 10));
}

/// Reads this process's memory without faulting (as CpuSampler: clang-cl's __try does not catch an inline load).
bool SafeRead(u64 addr, void* dst, size_t bytes) {
    SIZE_T got = 0;
    return addr != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), dst, bytes, &got) != FALSE &&
           got == bytes;
}

// the file's records: a u32 header (type in the low byte, the payload's bytes above), then the payload
enum Rec : u8 { kRecThread = 1, kRecTick, kRecSample, kRecMark, kRecCounters, kRecModule, kRecEnd };
// 2: a sample's third address is the nearest return into SecondSonPC's code; 3: up to kHostDepth of them (their count in
// the flags' high nibble) and the waiting samples (flags bit 1)
constexpr u32 kVersion = 3;
constexpr u32 kDepth = 24;              ///< eboot frames per sample (after the instruction pointer and the caller)
constexpr u32 kHostDepth = 8;           ///< SecondSonPC return addresses per sample (the stack's nearest 4 KB)
constexpr u64 kWaitEvery = 10;          ///< the watched threads that did not run: a waiting sample every 10th tick
constexpr size_t kBufBytes = 16u << 20; ///< each of the two buffers
constexpr u32 kMaxThreads = 2048;
constexpr size_t kStackWords = 2048;    ///< a sample's copy of the stack above rsp (16 KB, one read)

struct Thread {
    HANDLE h = nullptr;
    u32 tid = 0;
    u64 last = 0;       ///< its cycle counter at the last tick
    u64 stack_hi = 0;   ///< its stack's base (the TEB's), 0 unknown
    u64 ran_tick = ~u64{0}; ///< the last tick it ran in
    bool watch = false; ///< sampled while it waits too (the game's main and job threads, the GPU pipeline's)
};

/// The threads whose waits the report explains (what Game:Main, a job or a GPU thread blocks on in a hitch).
bool Watched(const std::string& name) {
    for (const char* p : {"Game:Main", "JobWorker", "SecondSonPC:Gpu", "SecondSonPC:ShaderCacheIO",
                          "SecondSonPC:VideoOutPresent", "SecondSonPC:Pipeline"}) {
        if (name.starts_with(p)) {
            return true;
        }
    }
    return false;
}

/// A thread's stack base from its TEB (NT_TIB::StackBase): a sample reads the stack above rsp up to it in one go.
u64 StackBaseOf(HANDLE h) {
    struct Basic { // THREAD_BASIC_INFORMATION
        LONG exit_status;
        void* teb;
        void* client_id[2];
        ULONG_PTR affinity;
        LONG priority, base_priority;
    };
    using QueryFn = LONG(WINAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    static const auto query = reinterpret_cast<QueryFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    Basic b{};
    u64 base = 0;
    if (query == nullptr || query(h, 0 /* ThreadBasicInformation */, &b, sizeof(b), nullptr) < 0 || b.teb == nullptr ||
        !SafeRead(reinterpret_cast<u64>(b.teb) + 8, &base, sizeof(base))) {
        return 0;
    }
    return base;
}

struct MarkEntry {
    u64 tsc;
    char text[56];
};

struct Ran {
    u16 idx;
    u32 cycles;
};

class Profiler {
public:
    void Start(const StartInfo& info);
    void Stop();
    void Mark(std::string_view label);
    void SetPoll(PollFn fn, void* ctx) {
        poll_ = fn;
        poll_ctx_ = ctx;
    }

private:
    void SamplerLoop();
    void WriterLoop();
    void Refresh();
    void Tick(u64 now);
    void SampleThread(u32 idx, u64 cycles, bool waiting);
    void Handoff(bool force);
    // records go into cur_ (the sampler thread only; the stopping thread after the sampler ended)
    bool Room(size_t bytes) const {
        return cur_->size() + bytes <= kBufBytes;
    }
    void Put(const void* p, size_t n) {
        cur_->insert(cur_->end(), static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
    }
    template <class T>
    void PutV(const T& v) {
        Put(&v, sizeof(T));
    }
    void Header(Rec type, size_t payload) {
        PutV(static_cast<u32>(type | (payload << 8)));
    }

    std::filesystem::path path_;
    FILE* file_ = nullptr;
    u64 guest_lo_ = 0, guest_hi_ = 0, origin_ = 0, tsc_hz_ = 1;
    u64 text_lo_ = 0, text_hi_ = 0; ///< SecondSonPC.exe's code (.text)
    u64 code_lo_ = 0, code_hi_ = 0; ///< the eboot's executable pages (its data holds pointers that look like code)
    bool code_found_ = false;

    /// The eboot's executable range (VirtualQuery over its image; nothing suspended).
    void FindCode();
    /// `v` returns into the eboot: in its code, right after a call (E8 rel32, or FF /2 with a 0-5 byte operand) - the
    /// stack also holds data pointers into the eboot and function pointers, which are not frames.
    bool IsReturn(u64 v) const {
        if (v < code_lo_ + 7 || v >= code_hi_) {
            return false;
        }
        const u8* p = reinterpret_cast<const u8*>(v);
        const auto call_rm = [](u8 op, u8 modrm) { return op == 0xFF && ((modrm >> 3) & 7) == 2; };
        return p[-5] == 0xE8 || call_rm(p[-2], p[-1]) || call_rm(p[-3], p[-2]) || call_rm(p[-6], p[-5]) ||
               call_rm(p[-7], p[-6]);
    }
    u32 period_us_ = 1000;
    u64 min_cycles_ = 0;
    PollFn poll_ = nullptr;
    void* poll_ctx_ = nullptr;

    std::thread sampler_, writer_;
    std::atomic<bool> stop_{false};
    std::vector<Thread> threads_;
    std::vector<Ran> ran_; ///< the tick's threads that ran (reserved once)
    u64 stack_[kStackWords]{}; ///< a sample's copy of the stack (the sampler thread's)
    DWORD self_tid_ = 0;

    // two buffers: the sampler fills cur_, the writer writes out_ (handed over under mu_)
    std::vector<u8> a_, b_;
    std::vector<u8>* cur_ = &a_;
    std::vector<u8>* out_ = &b_;
    bool out_full_ = false;
    bool writer_stop_ = false;
    std::mutex mu_;
    std::condition_variable cv_;

    std::mutex mark_mu_;
    MarkEntry marks_[64]{};
    u32 mark_n_ = 0;
    u64 marks_dropped_ = 0;

    // totals (the log and the file's END record)
    u64 ticks_ = 0, samples_ = 0, sample_failed_ = 0, dropped_ = 0, suspended_ticks_ = 0, bytes_ = 0;
    u64 self_cycles_ = 0, late_ticks_ = 0;
};

Profiler g_prof;

void Profiler::Start(const StartInfo& info) {
    guest_lo_ = info.guest_lo;
    guest_hi_ = info.guest_hi;
    origin_ = info.origin_tsc;
    tsc_hz_ = std::max<u64>(Kernel::Clock::TscFrequency(), 1);
    // (mode 2 suspends the threads that ran: every 2 ms by default, every 1 ms costs twice the suspensions)
    period_us_ = std::clamp<u32>(EnvU32("SSPC_R9_GPROF_US", g_mode >= 2 ? 2000 : 1000), 250, 100000);
    const u32 pct = std::clamp<u32>(EnvU32("SSPC_R9_GPROF_MIN_PCT", 5), 0, 100);
    min_cycles_ = tsc_hz_ / 1'000'000 * period_us_ * pct / 100;
    std::error_code ec;
    std::filesystem::create_directories(info.logs_dir, ec);
    path_ = info.logs_dir / "r9_gprof.bin";
    file_ = _wfopen(path_.c_str(), L"wb");
    if (file_ == nullptr) {
        SSPC_LOG(GPU, Error, "R9 gameplay profiler: cannot write {}", path_.string());
        return;
    }
    std::setvbuf(file_, nullptr, _IOFBF, 1 << 20);
    a_.reserve(kBufBytes);
    b_.reserve(kBufBytes);
    threads_.reserve(kMaxThreads);
    ran_.reserve(kMaxThreads);
    // the header: magic, version, period, TSC rate, the frame log's origin, the eboot's and the exe's ranges, the
    // sampling threshold, the counters' names
    const HMODULE exe = GetModuleHandleW(nullptr);
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), exe, &mi, sizeof(mi));
    const u64 host_lo = reinterpret_cast<u64>(exe);
    {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
            host_lo + reinterpret_cast<const IMAGE_DOS_HEADER*>(host_lo)->e_lfanew);
        const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
        for (u32 i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            if (std::memcmp(sec[i].Name, ".text", 6) == 0) {
                text_lo_ = host_lo + sec[i].VirtualAddress;
                text_hi_ = text_lo_ + sec[i].Misc.VirtualSize;
            }
        }
    }
    std::fwrite("SSPCR9GP", 1, 8, file_);
    const u32 hdr32[2] = {kVersion, period_us_};
    std::fwrite(hdr32, sizeof(u32), 2, file_);
    const u64 hdr64[7] = {tsc_hz_, origin_, guest_lo_, guest_hi_, host_lo, host_lo + mi.SizeOfImage, min_cycles_};
    std::fwrite(hdr64, sizeof(u64), 7, file_);
    static constexpr const char* kNames[kCounters] = {"file_read_bytes", "file_read_ns", "file_reads", "guest_maps",
                                                      "guest_unmaps", "marks", "tracker_faults", "tracker_arms",
                                                      "game_flips", "cp_flips", "presents"};
    const u32 n = kCounters;
    std::fwrite(&n, sizeof(u32), 1, file_);
    for (const char* name : kNames) {
        const u16 len = static_cast<u16>(std::strlen(name));
        std::fwrite(&len, sizeof(u16), 1, file_);
        std::fwrite(name, 1, len, file_);
    }
    writer_ = std::thread([this] { WriterLoop(); });
    sampler_ = std::thread([this] { SamplerLoop(); });
    SSPC_LOG(GPU, Warning,
             "DIAGNOSTICS: SSPC_R9_GPROF={} - every thread's CPU time every {} us{} -> {} (marks: F5 calm, F6 fight, "
             "F7 traversal, F9 effects, F12 stutter)",
             g_mode, period_us_,
             g_mode >= 2 ? fmt::format(", code samples of the threads that ran >= {}% of a tick", pct) : "",
             path_.string());
}

void Profiler::Refresh() {
    // (nothing is suspended now: the snapshot, OpenThread and GetThreadDescription may allocate and lock)
    if (guest_hi_ == 0 && Engine::GuestImageEnd() != 0) {
        // the eboot is loaded after the backend started: its range goes out as the module "eboot" (the header's is 0)
        guest_lo_ = Engine::GuestImageBase();
        guest_hi_ = Engine::GuestImageEnd();
        if (Room(4 + 8 + 4 + 2 + 5)) {
            Header(kRecModule, 8 + 4 + 2 + 5);
            PutV(guest_lo_);
            PutV(static_cast<u32>(guest_hi_ - guest_lo_));
            PutV(static_cast<u16>(5));
            Put("eboot", 5);
        } else {
            ++dropped_;
        }
    }
    if (guest_hi_ != 0 && !code_found_) {
        code_found_ = true; // (once: the image is mapped whole before the game runs)
        FindCode();
    }
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || threads_.size() >= kMaxThreads ||
            std::ranges::find(threads_, static_cast<u32>(te.th32ThreadID), &Thread::tid) != threads_.end()) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                              te.th32ThreadID);
        if (h == nullptr) {
            continue;
        }
        Thread t;
        t.h = h;
        t.tid = te.th32ThreadID;
        t.stack_hi = StackBaseOf(h);
        QueryThreadCycleTime(h, &t.last); // (its time before it was found is not counted)
        std::string name;
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr) {
            for (const wchar_t* c = desc; *c != 0 && name.size() < 120; ++c) {
                name.push_back(*c < 128 && *c >= 32 ? static_cast<char>(*c) : '_');
            }
            LocalFree(desc);
        }
        t.watch = Watched(name);
        const u16 idx = static_cast<u16>(threads_.size());
        threads_.push_back(t);
        // (an exited thread keeps its handle: its counter stops, it no longer shows in the ticks)
        if (Room(4 + 2 + 4 + 2 + name.size())) {
            Header(kRecThread, 2 + 4 + 2 + name.size());
            PutV(idx);
            PutV(static_cast<u32>(t.tid));
            PutV(static_cast<u16>(name.size()));
            Put(name.data(), name.size());
        } else {
            ++dropped_;
        }
    }
    CloseHandle(snap);
}

void Profiler::FindCode() {
    // the first run of committed, readable, executable regions in the image (IsReturn reads the bytes before a return
    // address directly: every page of the range must stay readable)
    constexpr DWORD kExecRead = PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    u64 lo = 0, hi = 0;
    for (u64 a = guest_lo_; a < guest_hi_;) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(a), &mbi, sizeof(mbi)) == 0) {
            break;
        }
        const u64 end = std::min(reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize, guest_hi_);
        const bool code = mbi.State == MEM_COMMIT && (mbi.Protect & kExecRead) != 0 && (mbi.Protect & PAGE_GUARD) == 0;
        if (code && hi == 0) {
            lo = a;
        }
        if (code) {
            hi = end;
        } else if (hi != 0) {
            break;
        }
        a = end;
    }
    code_lo_ = lo;
    code_hi_ = hi;
    SSPC_LOG(GPU, Info, "R9 gameplay profiler: eboot code [{:#x}, {:#x}) of the image [{:#x}, {:#x})", code_lo_ - guest_lo_,
             code_hi_ - guest_lo_, u64{0}, guest_hi_ - guest_lo_);
}

void Profiler::SampleThread(u32 idx, u64 cycles, bool waiting) {
    const HANDLE target = threads_[idx].h;
    const u64 stack_hi = threads_[idx].stack_hi;
    const auto in_guest = [this](u64 v) { return v >= guest_lo_ && v < guest_hi_; };
    const u64 t0 = Kernel::Clock::ReadTsc();
    if (SuspendThread(target) == static_cast<DWORD>(-1)) {
        ++sample_failed_;
        return;
    }
    // nothing below allocates, logs or takes a lock the target may hold
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    u64 rec[2 + kHostDepth + kDepth];
    u32 n = 0, nhost = 0;
    if (GetThreadContext(target, &ctx)) {
        rec[n++] = ctx.Rip;
        // the stack above rsp in one read: up to the thread's stack base (the TEB's) when rsp is on that stack (a
        // read across the base fails whole); the game's threads run on stacks of the runtime's (not the TEB's):
        // 16 KB, else up to the end of rsp's region
        const u64 lo = ctx.Rsp;
        const u64 base = stack_hi > lo && stack_hi - lo < (u64{256} << 20) ? stack_hi : 0;
        size_t words = base != 0 ? std::min<size_t>(kStackWords, (base - lo) / 8) : kStackWords;
        if (words == 0 || !SafeRead(lo, stack_, words * 8)) {
            // up to the end of rsp's region: a shallow stack (a worker's loop and its wait) has less than 4 KB above
            // rsp, and a read across the region's end fails whole (the job threads' waits had no callers at all)
            words = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            if (base == 0 && VirtualQuery(reinterpret_cast<LPCVOID>(lo), &mbi, sizeof(mbi)) != 0 &&
                mbi.State == MEM_COMMIT) {
                const u64 end = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
                words = end > lo ? std::min<size_t>(kStackWords, (end - lo) / 8) : 0;
                if (words != 0 && !SafeRead(lo, stack_, words * 8)) {
                    words = 0;
                }
            }
        }
        const u64 hi = lo + words * 8;
        // the nearest return into the eboot (when the thread is outside it) and, before it, the nearest returns into
        // SecondSonPC's code (which of its functions the driver's or a system's code, or a wait, is under)
        u64 caller = 0;
        u64 host[kHostDepth];
        if (!in_guest(ctx.Rip)) {
            for (size_t k = 0; k < std::min<size_t>(words, 512) && caller == 0; ++k) {
                const u64 v = stack_[k];
                if (IsReturn(v)) {
                    caller = v;
                } else if (nhost < kHostDepth && v >= text_lo_ && v < text_hi_) {
                    host[nhost++] = v;
                }
            }
        }
        rec[n++] = caller;
        for (u32 k = 0; k < nhost; ++k) {
            rec[n++] = host[k];
        }
        // the game's frames: [rbp] = the caller's rbp, [rbp + 8] = the return address (from the copy when there)
        u64 rbp = ctx.Rbp;
        for (u32 k = 0; k < kDepth && rbp != 0 && (rbp & 7) == 0; ++k) {
            u64 frame[2];
            // (no rbp + 16: rbp is any value when the game uses it as a plain register, it can wrap around)
            if (rbp >= lo && rbp < hi && hi - rbp >= 16) {
                frame[0] = stack_[(rbp - lo) / 8];
                frame[1] = stack_[(rbp - lo) / 8 + 1];
            } else if ((base != 0 && (rbp < lo || rbp >= base || base - rbp < 16)) ||
                       !SafeRead(rbp, frame, sizeof(frame))) {
                break; // (off the thread's stack: not a frame)
            }
            if (!IsReturn(frame[1])) {
                break;
            }
            rec[n++] = frame[1];
            if (frame[0] <= rbp) {
                break; // the chain must go up the stack
            }
            rbp = frame[0];
        }
    }
    ResumeThread(target);
    suspended_ticks_ += Kernel::Clock::ReadTsc() - t0;
    if (n == 0) {
        ++sample_failed_;
        return;
    }
    const size_t payload = 2 + 1 + 1 + 4 + size_t{8} * n;
    if (!Room(4 + payload)) {
        ++dropped_;
        return;
    }
    Header(kRecSample, payload);
    PutV(static_cast<u16>(idx));
    PutV(static_cast<u8>(n));
    PutV(static_cast<u8>((in_guest(rec[0]) ? 1 : 0) | (waiting ? 2 : 0) | (nhost << 4)));
    PutV(static_cast<u32>(std::min<u64>(cycles, 0xffffffffu)));
    Put(rec, size_t{8} * n);
    ++samples_;
}

void Profiler::Tick(u64 now) {
    // every thread's cycles since the last tick (the ones that ran only)
    ran_.clear();
    for (size_t i = 0; i < threads_.size(); ++i) {
        Thread& t = threads_[i];
        if (t.tid == self_tid_) {
            continue;
        }
        u64 c = 0;
        if (!QueryThreadCycleTime(t.h, &c)) {
            continue;
        }
        const u64 d = c - t.last;
        t.last = c;
        if (d != 0) {
            ran_.push_back({static_cast<u16>(i), static_cast<u32>(std::min<u64>(d, 0xffffffffu))});
            t.ran_tick = ticks_;
        }
    }
    const size_t payload = 8 + 2 + ran_.size() * 6;
    if (Room(4 + payload)) {
        Header(kRecTick, payload);
        PutV(now);
        PutV(static_cast<u16>(ran_.size()));
        for (const Ran& r : ran_) {
            PutV(r.idx);
            PutV(r.cycles);
        }
    } else {
        ++dropped_;
    }
    // the counters
    u64 vals[kCounters];
    for (u32 i = 0; i < kCounters; ++i) {
        vals[i] = detail::g_counters[i].load(std::memory_order_relaxed);
    }
    if (poll_ != nullptr) {
        poll_(poll_ctx_, vals + kFirstPolled);
    }
    if (Room(4 + 8 + sizeof(vals))) {
        Header(kRecCounters, 8 + sizeof(vals));
        PutV(now);
        Put(vals, sizeof(vals));
    } else {
        ++dropped_;
    }
    // the code samples (mode 2)
    if (g_mode >= 2) {
        for (const Ran& r : ran_) {
            if (r.cycles >= min_cycles_) {
                SampleThread(r.idx, r.cycles, false);
            }
        }
        // what the watched threads that did not run wait on (weight: kWaitEvery ticks of wall time each)
        if (ticks_ % kWaitEvery == 0) {
            for (u32 i = 0; i < threads_.size(); ++i) {
                if (threads_[i].watch && threads_[i].ran_tick != ticks_) {
                    SampleThread(i, 0, true);
                }
            }
        }
    }
    // the marks
    {
        std::scoped_lock lock{mark_mu_};
        for (u32 i = 0; i < mark_n_; ++i) {
            const size_t len = std::strlen(marks_[i].text);
            if (Room(4 + 8 + 2 + len)) {
                Header(kRecMark, 8 + 2 + len);
                PutV(marks_[i].tsc);
                PutV(static_cast<u16>(len));
                Put(marks_[i].text, len);
            } else {
                ++dropped_;
            }
        }
        mark_n_ = 0;
    }
    ++ticks_;
}

void Profiler::Handoff(bool force) {
    std::unique_lock lock{mu_};
    if (out_full_) {
        if (!force) {
            return; // (the writer is behind: cur_ keeps filling - a full one drops records, counted)
        }
        cv_.wait(lock, [this] { return !out_full_; });
    }
    bytes_ += cur_->size();
    std::swap(cur_, out_);
    cur_->clear();
    out_full_ = true;
    cv_.notify_all();
}

void Profiler::SamplerLoop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:R9Gprof");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    self_tid_ = GetCurrentThreadId();
    u64 self_start = 0;
    QueryThreadCycleTime(GetCurrentThread(), &self_start);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == nullptr) {
        timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    const u64 period_ticks = tsc_hz_ / 1'000'000 * period_us_;
    const u64 refresh_ticks = tsc_hz_;     // the threads found again every second
    const u64 handoff_ticks = tsc_hz_ / 2; // the writer gets the records every half second
    Refresh();
    u64 last_refresh = Kernel::Clock::ReadTsc();
    u64 last_handoff = last_refresh;
    u64 due = last_refresh + period_ticks;
    while (!stop_.load(std::memory_order_acquire)) {
        u64 now = Kernel::Clock::ReadTsc();
        if (now < due) {
            LARGE_INTEGER rel;
            rel.QuadPart = -static_cast<LONGLONG>((due - now) * 10'000'000 / tsc_hz_); // 100 ns units, relative
            if (timer != nullptr && rel.QuadPart < 0 && SetWaitableTimer(timer, &rel, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, INFINITE);
            } else {
                Sleep(1);
            }
            now = Kernel::Clock::ReadTsc();
        }
        if (now > due + period_ticks) {
            ++late_ticks_;
            due = now; // (a stalled sampler does not burst to catch up)
        }
        due += period_ticks;
        if (now - last_refresh >= refresh_ticks) {
            Refresh();
            last_refresh = now;
        }
        Tick(now);
        if (now - last_handoff >= handoff_ticks || cur_->size() > kBufBytes / 2) {
            Handoff(false);
            last_handoff = now;
        }
    }
    QueryThreadCycleTime(GetCurrentThread(), &self_cycles_);
    self_cycles_ -= self_start;
    if (timer != nullptr) {
        CloseHandle(timer);
    }
}

void Profiler::WriterLoop() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:R9GprofWriter");
    std::unique_lock lock{mu_};
    while (true) {
        cv_.wait(lock, [this] { return out_full_ || writer_stop_; });
        if (out_full_) {
            std::vector<u8>* out = out_;
            lock.unlock();
            std::fwrite(out->data(), 1, out->size(), file_);
            std::fflush(file_);
            lock.lock();
            out_->clear();
            out_full_ = false;
            cv_.notify_all();
            continue;
        }
        break; // writer_stop_
    }
}

void Profiler::Stop() {
    if (file_ == nullptr) {
        return;
    }
    stop_.store(true, std::memory_order_release);
    if (sampler_.joinable()) {
        sampler_.join();
    }
    // the modules (the report attributes the samples outside the eboot and the exe: driver, system), then the totals
    HMODULE mods[1024];
    DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &need)) {
        for (DWORD i = 0; i < std::min<DWORD>(need / sizeof(HMODULE), 1024); ++i) {
            MODULEINFO mi{};
            char name[MAX_PATH] = {};
            if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof(mi)) ||
                GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, sizeof(name)) == 0) {
                continue;
            }
            const size_t len = std::strlen(name);
            if (Room(4 + 8 + 4 + 2 + len)) {
                Header(kRecModule, 8 + 4 + 2 + len);
                PutV(reinterpret_cast<u64>(mi.lpBaseOfDll));
                PutV(static_cast<u32>(mi.SizeOfImage));
                PutV(static_cast<u16>(len));
                Put(name, len);
            }
        }
    }
    const u64 totals[8] = {ticks_, samples_, sample_failed_, dropped_, suspended_ticks_, self_cycles_, late_ticks_,
                           marks_dropped_};
    if (Room(4 + sizeof(totals))) {
        Header(kRecEnd, sizeof(totals));
        Put(totals, sizeof(totals));
    }
    Handoff(true);
    {
        std::unique_lock lock{mu_};
        cv_.wait(lock, [this] { return !out_full_; });
        writer_stop_ = true;
        cv_.notify_all();
    }
    if (writer_.joinable()) {
        writer_.join();
    }
    std::fclose(file_);
    file_ = nullptr;
    for (Thread& t : threads_) {
        CloseHandle(t.h);
    }
    const double hz = static_cast<double>(tsc_hz_);
    SSPC_LOG(GPU, Info,
             "R9 gameplay profiler: {} ticks ({} late), {} threads, {} code samples ({} failed), {} records dropped, "
             "{:.1f} MiB; its own thread {:.2f} s CPU, threads suspended {:.3f} s in all -> {}",
             ticks_, late_ticks_, threads_.size(), samples_, sample_failed_, dropped_,
             static_cast<double>(bytes_) / (1 << 20), static_cast<double>(self_cycles_) / hz,
             static_cast<double>(suspended_ticks_) / hz, path_.string());
}

void Profiler::Mark(std::string_view label) {
    const u64 now = Kernel::Clock::ReadTsc();
    std::scoped_lock lock{mark_mu_};
    if (mark_n_ >= std::size(marks_)) {
        ++marks_dropped_;
        return;
    }
    MarkEntry& m = marks_[mark_n_++];
    m.tsc = now;
    const size_t len = std::min(label.size(), sizeof(m.text) - 1);
    std::memcpy(m.text, label.data(), len);
    m.text[len] = 0;
}

} // namespace

const u8 g_mode = ReadMode();

namespace detail {
std::atomic<u64> g_counters[kCounters];
}

void SetPoll(PollFn fn, void* ctx) {
    g_prof.SetPoll(fn, ctx);
}

void Start(const StartInfo& info) {
    if (On()) {
        g_prof.Start(info);
    }
}

void Stop() {
    if (On()) {
        g_prof.Stop();
    }
}

void Mark(std::string_view label) {
    if (!On()) {
        return;
    }
    Add(Ctr::Marks, 1);
    g_prof.Mark(label);
    SSPC_LOG(GPU, Info, "R9 mark: {}", label);
}

} // namespace SecondSon::Gpu::Gprof
