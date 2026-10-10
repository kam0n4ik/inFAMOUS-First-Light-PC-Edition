// SecondSonPC — the process's exception dispatcher (see exceptions.h).
#include "runtime/cpu/exceptions.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <intrin.h>
#include <windows.h>

#include "base/crash.h"
#include "base/log.h"
#include "runtime/cpu/fs_patcher.h"
#include "runtime/cpu/tcb_slot.h"

#ifndef SSPC_GIT_REV
#define SSPC_GIT_REV "unknown"
#endif

namespace SecondSon::Cpu::Exceptions {

namespace {

// ------------------------------------------------------------------------------------- guest code

constexpr u32 kMaxRanges = 256; // two per module (code, trampolines)

struct Range {
    u64 begin = 0;
    u64 end = 0;
    u64* red_zone = nullptr; // 1 bit per 16-byte granule: a function that keeps data below rsp
    char name[48]{};
};

std::array<Range, kMaxRanges> g_ranges;
std::atomic<u32> g_range_count{0};
SRWLOCK g_range_lock = SRWLOCK_INIT; // registration only; lookups go by the published count

const Range* FindRange(u64 address) {
    const u32 count = g_range_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        const Range& r = g_ranges[i];
        if (address >= r.begin && address < r.end) {
            return &r;
        }
    }
    return nullptr;
}

// ------------------------------------------------------------------------------------ statistics

constexpr u32 kOwners = static_cast<u32>(Owner::Count);

std::atomic<u64> g_av{0}, g_traps{0}, g_illegal{0}, g_other_guest{0}, g_passed_other{0}, g_nested{0}, g_execute{0};
std::array<std::atomic<u64>, kOwners> g_av_by{}, g_traps_by{}, g_illegal_by{};
std::atomic<u64> g_guest_resolved{0}, g_xstate_over{0}, g_in_rz_fn{0}, g_exposed{0};
std::array<std::atomic<u64>, 8> g_first_exposed{};

// Dispatcher + bridge handler time of the access violations the bridge resolved: logarithmic buckets
// with 4 linear steps per power of two.
constexpr u32 kBuckets = 4 * 48;
std::array<std::atomic<u64>, kBuckets> g_hist{};
std::atomic<u64> g_bridge_cycles{0}, g_bridge_max{0};

u32 Bucket(u64 cycles) {
    if (cycles < 8) {
        return static_cast<u32>(cycles);
    }
    const u32 msb = 63 - static_cast<u32>(__lzcnt64(cycles));
    return std::min(4 * (msb - 1) + static_cast<u32>((cycles >> (msb - 2)) & 3), kBuckets - 1);
}

u64 BucketValue(u32 index) {
    if (index < 8) {
        return index;
    }
    const u32 msb = index / 4 + 1;
    return (4ull + (index & 3)) << (msb - 2);
}

void NoteBridgeTime(u64 cycles) {
    g_hist[Bucket(cycles)].fetch_add(1, std::memory_order_relaxed);
    g_bridge_cycles.fetch_add(cycles, std::memory_order_relaxed);
    for (u64 max = g_bridge_max.load(std::memory_order_relaxed);
         cycles > max && !g_bridge_max.compare_exchange_weak(max, cycles, std::memory_order_relaxed);) {
    }
}

void Count(std::array<std::atomic<u64>, kOwners>& by, Owner owner) {
    by[static_cast<u32>(owner)].fetch_add(1, std::memory_order_relaxed);
}

u64 g_install_tsc = 0;
LARGE_INTEGER g_install_qpc{};

// ------------------------------------------------------------------------------------- red zone

/// The kernel builds the exception frame right below the faulting rsp. Its extended-state (XSAVE) area
/// ends there, and the AVX part (the upper halves of ymm0-15) is written only when that state is in use:
/// then it lies over the 128 bytes the System V ABI lets a leaf function keep below rsp (measured on
/// Windows 10 19045: 24-56 bytes below rsp stay intact with AVX in use, 280-312 without; the self-test
/// NativeExceptions(red zone) repeats the measurement).
bool XStateOverRedZone(const CONTEXT* ctx) {
    DWORD64 mask = 0;
    if (!GetXStateFeaturesMask(const_cast<CONTEXT*>(ctx), &mask) || (mask & XSTATE_MASK_AVX) == 0) {
        return false;
    }
    DWORD length = 0;
    const auto* avx = static_cast<const u8*>(LocateXStateFeature(const_cast<CONTEXT*>(ctx), XSTATE_AVX, &length));
    if (avx == nullptr) {
        return false;
    }
    const u64 lo = reinterpret_cast<u64>(avx);
    const u64 hi = lo + length;
    return lo < ctx->Rsp && hi > ctx->Rsp - 128;
}

/// A resolved exception whose rip is guest code: what the kernel's frame may have done to its red zone.
void NoteGuestResolved(const CONTEXT* ctx) {
    const Range* r = FindRange(ctx->Rip);
    if (r == nullptr) {
        return;
    }
    g_guest_resolved.fetch_add(1, std::memory_order_relaxed);
    bool in_fn = false;
    if (r->red_zone != nullptr) {
        const u64 granule = (ctx->Rip - r->begin) >> 4;
        in_fn = (r->red_zone[granule >> 6] >> (granule & 63)) & 1;
    }
    const bool over = XStateOverRedZone(ctx); // parses CONTEXT_EX only: no system call, no lock
    if (over) {
        g_xstate_over.fetch_add(1, std::memory_order_relaxed);
    }
    if (in_fn) {
        g_in_rz_fn.fetch_add(1, std::memory_order_relaxed);
    }
    if (over && in_fn) {
        const u64 n = g_exposed.fetch_add(1, std::memory_order_relaxed);
        if (n < g_first_exposed.size()) {
            g_first_exposed[n].store(ctx->Rip, std::memory_order_relaxed);
        }
    }
}

// ------------------------------------------------------------------------------------- dispatcher

Options g_options;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_pass_everything{false};
std::atomic<GuestSignalDelivery> g_signal_delivery{nullptr};
std::atomic<ThreadDescriber> g_describer{nullptr};
std::atomic<void (*)()> g_fatal_hook{nullptr};
PVOID g_handle = nullptr;

thread_local u32 t_depth = 0;                       // inside an owner's handler on this thread
thread_local EXCEPTION_POINTERS* t_outer = nullptr; // ... handling this exception

/// Sets the thread's handler nesting for a scope and puts it back on every way out.
class NestingScope {
public:
    NestingScope(u32 depth, EXCEPTION_POINTERS* outer) : depth_{t_depth}, outer_{t_outer} {
        t_depth = depth;
        t_outer = outer;
    }
    ~NestingScope() {
        t_depth = depth_;
        t_outer = outer_;
    }
    NestingScope(const NestingScope&) = delete;
    NestingScope& operator=(const NestingScope&) = delete;

private:
    u32 depth_;
    EXCEPTION_POINTERS* outer_;
};

/// Runs an owner's handler; an exception it raises reaches the dispatcher as nested.
template <typename F>
bool InHandler(EXCEPTION_POINTERS* info, F&& handler) {
    NestingScope scope{t_depth + 1, info};
    return handler();
}

/// The bridge's handler was re-entrant (its own vectored handler): an access violation inside an owner's
/// handler is dispatched again, to this depth; deeper, or any other exception there, is fatal.
constexpr u32 kMaxNesting = 4;

bool OnFatalReporter();                         // the fatal path's reporter thread, while it reports
[[noreturn]] void EmergencyExit(const char* why); // the minimal report without locks, then the end

// FreeBSD signal numbers and codes the guest sees.
constexpr s32 kSigIll = 4, kSigTrap = 5, kSigFpe = 8, kSigBus = 10, kSigSegv = 11;
constexpr s32 kSegvMaperr = 1, kSegvAccerr = 2, kBusAdrerr = 2, kIllIllopc = 1, kIllPrvopc = 5, kTrapBrkpt = 1,
              kTrapTrace = 2, kFpeIntovf = 1, kFpeIntdiv = 2, kFpeFltdiv = 3, kFpeFltovf = 4, kFpeFltund = 5,
              kFpeFltres = 6, kFpeFltinv = 7, kFpeFltsub = 8;

bool InPeImage(u64 address) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != 0 && mbi.Type == MEM_IMAGE;
}

/// No owner resolved it. Guest code: its signal, else a fatal report (the frame walk through guest code,
/// which has no unwind data, would never reach a handler). Host code in a PE image: passed on to its SEH
/// and C++ handlers and the unhandled exception filter. Other code (generated by the host, no unwind data
/// either): fatal.
LONG Unresolved(EXCEPTION_POINTERS* info, std::array<std::atomic<u64>, kOwners>* by, s32 signal, s32 code,
                u64 address, const char* reason, bool execute = false) {
    const u64 rip = info->ContextRecord->Rip;
    // An execute fault's rip is the bad target, in no range: it is the guest's when the thread runs guest
    // code (its TCB is set: both thread managers set it before entering the guest).
    const bool guest = IsGuestCode(rip) || (execute && Cpu::CurrentTcb() != nullptr);
    if (!guest && InPeImage(rip)) {
        if (by != nullptr) {
            Count(*by, Owner::PassedOn);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (guest) {
        if (const auto deliver = g_signal_delivery.load(std::memory_order_acquire)) {
            // The guest's handler runs on this thread; exceptions it raises are ordinary ones.
            bool delivered = false;
            {
                NestingScope scope{0, nullptr};
                delivered = deliver(info, signal, code, address);
            }
            if (delivered) {
                if (by != nullptr) {
                    Count(*by, Owner::GuestSignal);
                }
                return EXCEPTION_CONTINUE_EXECUTION;
            }
        }
    }
    if (by != nullptr) {
        Count(*by, Owner::Fatal);
    }
    Fatal(info, guest ? reason : "exception in generated host code (no unwind data)");
}

LONG OnAccessViolation(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    const u64 access = rec->NumberParameters >= 2 ? rec->ExceptionInformation[0] : 0;
    const u64 address = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
    const u64 rip = info->ContextRecord->Rip;
    g_av.fetch_add(1, std::memory_order_relaxed);
    Base::StampFirstChance(); // --track-stats: the bridge measures the whole chain from here
    // 1. SecondSonPC's CPU patcher: an FS access reads the displacement (FS base 0), near 0 or, negative,
    //    above the user address space. GPU tracking faults (guest memory from 0x2'0000'0000) never enter.
    if (g_options.cpu_patcher && (address < 0x10000 || address >= 0xFFFF800000000000ull) &&
        InHandler(info, [info] { return Cpu::HandlePatcherAccessViolation(info); })) {
        Count(g_av_by, Owner::CpuPatcher);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    // 2. An execute fault: recorded (SSPC-002), never anyone's to resolve.
    if (access == 8 || address == rip) {
        g_execute.fetch_add(1, std::memory_order_relaxed);
        Base::RecordExecuteFault(info);
        return Unresolved(info, &g_av_by, kSigSegv, kSegvMaperr, address,
                          "execute fault (jump/call/return to a bad address)", true);
    }
    // 3. The graphics bridge: GPU write/read tracking, the SRT walker.
    if (const auto bridge = g_options.bridge.access_violation) {
        const u64 t0 = __rdtsc();
        if (InHandler(info, [&] { return bridge(info, address); })) {
            NoteBridgeTime(__rdtsc() - t0);
            Count(g_av_by, Owner::Bridge);
            NoteGuestResolved(info->ContextRecord);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    MEMORY_BASIC_INFORMATION mbi{};
    const bool committed = VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) != 0 &&
                           mbi.State == MEM_COMMIT;
    return Unresolved(info, &g_av_by, kSigSegv, committed ? kSegvAccerr : kSegvMaperr, address,
                      committed ? "access violation (protection) nobody resolved"
                                : "access violation (unmapped memory) nobody resolved");
}

LONG OnTrap(EXCEPTION_POINTERS* info, bool misreported) {
    g_traps.fetch_add(1, std::memory_order_relaxed);
    if (g_options.cpu_patcher &&
        InHandler(info, [&] { return Cpu::HandlePatcherTrap(info, misreported); })) {
        Count(g_traps_by, Owner::CpuPatcher);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (const auto bridge = g_options.bridge.patch_trap) {
        if (InHandler(info, [&] { return bridge(info, misreported); })) {
            Count(g_traps_by, Owner::Bridge);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    const u64 rip = info->ContextRecord->Rip;
    return Unresolved(info, &g_traps_by, misreported ? kSigIll : kSigTrap, misreported ? kIllPrvopc : kTrapBrkpt, rip,
                      misreported ? "privileged instruction" : "breakpoint (int3) not placed by a patcher");
}

LONG OnIllegal(EXCEPTION_POINTERS* info) {
    g_illegal.fetch_add(1, std::memory_order_relaxed);
    if (g_options.cpu_patcher && InHandler(info, [info] { return Cpu::HandleIllegalInstruction(info); })) {
        Count(g_illegal_by, Owner::CpuPatcher);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (const auto bridge = g_options.bridge.illegal_instruction) {
        if (InHandler(info, [&] { return bridge(info); })) {
            Count(g_illegal_by, Owner::Bridge);
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return Unresolved(info, &g_illegal_by, kSigIll, kIllIllopc, info->ContextRecord->Rip,
                      "illegal instruction (not emulated)");
}

/// Traps only the guest's own code has an answer for: its signal or a fatal report; host code passes on.
LONG OnOther(EXCEPTION_POINTERS* info, s32 signal, s32 code, const char* reason) {
    const u64 rip = info->ContextRecord->Rip;
    if (!IsGuestCode(rip)) {
        g_passed_other.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_other_guest.fetch_add(1, std::memory_order_relaxed);
    return Unresolved(info, nullptr, signal, code, rip, reason);
}

LONG WINAPI Dispatch(EXCEPTION_POINTERS* info) {
    if (g_pass_everything.load(std::memory_order_relaxed)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    // A fault while the reporter writes a report: no second report, the minimal line and the end.
    if (((code & 0xF0000000u) == 0xC0000000u || code == EXCEPTION_BREAKPOINT || code == EXCEPTION_SINGLE_STEP) &&
        OnFatalReporter()) {
        EmergencyExit("fault on the fatal reporter thread");
    }
    if (t_depth != 0) {
        // Inside an owner's handler: a C++ exception there is that handler's business; an access violation is
        // dispatched again (the bridge's own handler was re-entrant), to kMaxNesting; any other fault is fatal
        // (the first exception's handling cannot be finished).
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
            if (t_depth < kMaxNesting) {
                break;
            }
            [[fallthrough]];
        case EXCEPTION_BREAKPOINT:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_IN_PAGE_ERROR:
            g_nested.fetch_add(1, std::memory_order_relaxed);
            Fatal(info, "exception inside an exception handler");
        default:
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return OnAccessViolation(info);
    case EXCEPTION_BREAKPOINT:
        return OnTrap(info, false);
    case EXCEPTION_PRIV_INSTRUCTION:
        return OnTrap(info, true);
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return OnIllegal(info);
    case EXCEPTION_SINGLE_STEP:
        return OnOther(info, kSigTrap, kTrapTrace, "single step");
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return OnOther(info, kSigFpe, kFpeIntdiv, "integer division by zero");
    case EXCEPTION_INT_OVERFLOW:
        return OnOther(info, kSigFpe, kFpeIntovf, "integer overflow");
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        return OnOther(info, kSigFpe, kFpeFltdiv, "floating-point division by zero");
    case EXCEPTION_FLT_OVERFLOW:
        return OnOther(info, kSigFpe, kFpeFltovf, "floating-point overflow");
    case EXCEPTION_FLT_UNDERFLOW:
        return OnOther(info, kSigFpe, kFpeFltund, "floating-point underflow");
    case EXCEPTION_FLT_INEXACT_RESULT:
        return OnOther(info, kSigFpe, kFpeFltres, "floating-point inexact result");
    case EXCEPTION_FLT_INVALID_OPERATION:
    case EXCEPTION_FLT_STACK_CHECK:
        return OnOther(info, kSigFpe, kFpeFltinv, "floating-point invalid operation");
    case EXCEPTION_FLT_DENORMAL_OPERAND:
        return OnOther(info, kSigFpe, kFpeFltsub, "floating-point denormal operand");
    case STATUS_FLOAT_MULTIPLE_TRAPS:
    case STATUS_FLOAT_MULTIPLE_FAULTS:
        return OnOther(info, kSigFpe, kFpeFltinv, "SIMD floating-point exception");
    case EXCEPTION_IN_PAGE_ERROR:
        return OnOther(info, kSigBus, kBusAdrerr, "in-page error (the backing file could not be read)");
    case EXCEPTION_STACK_OVERFLOW:
        return OnOther(info, kSigSegv, kSegvMaperr, "stack overflow");
    case DBG_PRINTEXCEPTION_C:
    case DBG_PRINTEXCEPTION_WIDE_C:
        return EXCEPTION_CONTINUE_EXECUTION; // OutputDebugString without a debugger (as the bridge did)
    default:
        g_passed_other.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_SEARCH; // C++ exceptions, thread names, ...: their own handlers
    }
}

// ------------------------------------------------------------------------------------ fatal path

const char* CodeName(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
        return "access violation";
    case EXCEPTION_BREAKPOINT:
        return "breakpoint";
    case EXCEPTION_PRIV_INSTRUCTION:
        return "privileged instruction";
    case EXCEPTION_ILLEGAL_INSTRUCTION:
        return "illegal instruction";
    case EXCEPTION_SINGLE_STEP:
        return "single step";
    case EXCEPTION_STACK_OVERFLOW:
        return "stack overflow";
    case EXCEPTION_IN_PAGE_ERROR:
        return "in-page error";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
        return "integer division by zero";
    default:
        return "exception";
    }
}

/// Everything the reporter needs, copied by the faulting thread into static storage (no allocation).
struct FatalJob {
    EXCEPTION_RECORD record;
    CONTEXT context;
    bool nested;
    EXCEPTION_RECORD outer_record;
    CONTEXT outer_context;
    DWORD thread_id;
    char reason[160];
    char thread[160];
};
alignas(16) FatalJob g_job;
std::atomic<DWORD> g_fatal_thread{0}; // the thread that claimed the fatal path (0: none)
HANDLE g_fatal_start = nullptr;
HANDLE g_fatal_done = nullptr;
DWORD g_reporter_id = 0;
DWORD g_report_timeout_ms = 45000;

bool OnFatalReporter() {
    return g_reporter_id != 0 && GetCurrentThreadId() == g_reporter_id && g_fatal_thread.load() != 0;
}

void ModuleOf(u64 address, char* out, size_t size) {
    const char* name = nullptr;
    u64 offset = 0;
    if (FindGuestCode(address, &name, &offset)) {
        std::snprintf(out, size, "%s+%#llx", name, static_cast<unsigned long long>(offset));
        return;
    }
    std::snprintf(out, size, "not guest code");
}

/// Minimal report without locks or heap: the reporter did not finish, or the fatal path itself faulted.
[[noreturn]] void EmergencyExit(const char* why) {
    char line[512];
    char where[96];
    ModuleOf(g_job.context.Rip, where, sizeof(where));
    const int n = std::snprintf(
        line, sizeof(line),
        "[FATAL] (%s) code %#lx at rip %#llx (%s), rsp %#llx, thread %lu; %s\r\n", why,
        static_cast<unsigned long>(g_job.record.ExceptionCode), static_cast<unsigned long long>(g_job.context.Rip),
        where, static_cast<unsigned long long>(g_job.context.Rsp), static_cast<unsigned long>(g_job.thread_id),
        g_job.reason);
    if (n > 0) {
        Log::EmergencyWrite(line, static_cast<size_t>(std::min<int>(n, sizeof(line) - 1)));
    }
    TerminateProcess(GetCurrentProcess(), g_job.record.ExceptionCode != 0 ? g_job.record.ExceptionCode : 1);
    for (;;) {
        Sleep(INFINITE);
    }
}

std::string Registers(const CONTEXT& c) {
    return fmt::format("rax {:#x} rbx {:#x} rcx {:#x} rdx {:#x} rsi {:#x} rdi {:#x} rbp {:#x} rsp {:#x}\n"
                       "  r8 {:#x} r9 {:#x} r10 {:#x} r11 {:#x} r12 {:#x} r13 {:#x} r14 {:#x} r15 {:#x}\n"
                       "  rip {:#x} rflags {:#x} mxcsr {:#x}",
                       c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10, c.R11, c.R12,
                       c.R13, c.R14, c.R15, c.Rip, c.EFlags, c.MxCsr);
}

std::string BuildReport() {
    const auto& rec = g_job.record;
    const auto& ctx = g_job.context;
    char where[96];
    ModuleOf(ctx.Rip, where, sizeof(where));
    std::string r = fmt::format("FATAL guest exception: {}\n  code {:#010x} ({}) at rip {:#x} ({}; {})\n", g_job.reason,
                                rec.ExceptionCode, CodeName(rec.ExceptionCode), ctx.Rip, where,
                                Base::DescribeAddress(ctx.Rip));
    if (rec.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec.NumberParameters >= 2) {
        const u64 access = rec.ExceptionInformation[0];
        const u64 address = rec.ExceptionInformation[1];
        r += fmt::format("  {} of {:#x} ({})\n", access == 0 ? "read" : access == 1 ? "write" : access == 8 ? "execute" : "access",
                         address, Base::DescribeAddress(address));
    }
    r += fmt::format("  thread: host {} \"{}\"; {}\n", g_job.thread_id, Base::ThreadDescription(g_job.thread_id),
                     g_job.thread);
    r += "  " + Registers(ctx) + "\n";
    if (g_job.nested) {
        char outer_where[96];
        ModuleOf(g_job.outer_context.Rip, outer_where, sizeof(outer_where));
        r += fmt::format("  raised while handling: code {:#010x} at rip {:#x} ({}), rsp {:#x}\n",
                         g_job.outer_record.ExceptionCode, g_job.outer_context.Rip, outer_where,
                         g_job.outer_context.Rsp);
    }
    r += Base::StackDump(ctx.Rsp);
    const auto s = GetStats();
    r += fmt::format("  dispatcher: access violations {} (CPU patcher {}, bridge {}, guest signal {}, fatal {}, passed on "
                     "{}), traps {}, illegal {}, nested {}; red zone: guest {} / AVX over it {} / exposed {}\n",
                     s.access_violations, s.av_by[0], s.av_by[1], s.av_by[2], s.av_by[3], s.av_by[4], s.traps,
                     s.illegal, s.nested, s.guest_resolved, s.xstate_over_red_zone, s.exposed);
    r += fmt::format("  backends: {}\n  build: SecondSonPC {} ({} {})\n", g_options.backends, SSPC_GIT_REV, __DATE__,
                     __TIME__);
    return r;
}

void Report() {
    const std::string report = BuildReport();
    // Its own file first (no shared lock), then the log.
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string dir = std::string(g_options.logs_dir) + "\\crash";
    CreateDirectoryA(dir.c_str(), nullptr);
    const std::string path = fmt::format("{}\\fatal_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}_{}.txt", dir, t.wYear, t.wMonth,
                                         t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    const HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(file, report.data(), static_cast<DWORD>(report.size()), &written, nullptr);
        CloseHandle(file);
    }
    size_t begin = 0;
    while (begin < report.size()) {
        const size_t end = std::min(report.find('\n', begin), report.size());
        SSPC_LOG(Cpu, Critical, "{}", std::string_view(report).substr(begin, end - begin));
        begin = end + 1;
    }
    SSPC_LOG(Cpu, Critical, "  report: {}", path);
    if (g_options.write_minidump) {
        EXCEPTION_POINTERS pointers{&g_job.record, &g_job.context};
        Base::WriteMinidumpFor(&pointers, g_job.thread_id);
    }
    Log::Flush();
    if (const auto hook = g_fatal_hook.load()) {
        hook();
    }
}

DWORD WINAPI ReporterMain(void*) {
    WaitForSingleObject(g_fatal_start, INFINITE);
    try {
        Report();
    } catch (...) {
        // Out of memory or a formatting error: what was written stays; the process ends as planned.
    }
    SetEvent(g_fatal_done);
    return 0;
}

} // namespace

// ----------------------------------------------------------------------------------- interface

bool Install(const Options& options) {
    if (g_installed.load()) {
        return true;
    }
    g_options = options;
    g_fatal_start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_fatal_done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE reporter =
        g_fatal_start && g_fatal_done ? CreateThread(nullptr, 256 * 1024, &ReporterMain, nullptr, 0, &g_reporter_id)
                                      : nullptr;
    if (reporter == nullptr) {
        SSPC_LOG(Cpu, Error, "exception dispatcher: the fatal reporter thread could not be created ({})", GetLastError());
        return false;
    }
    SetThreadDescription(reporter, L"SecondSonPC:FatalReporter");
    CloseHandle(reporter);
    g_install_tsc = __rdtsc();
    QueryPerformanceCounter(&g_install_qpc);
    g_handle = AddVectoredExceptionHandler(1, &Dispatch);
    if (g_handle == nullptr) {
        SSPC_LOG(Cpu, Error, "exception dispatcher: AddVectoredExceptionHandler failed");
        return false;
    }
    g_installed.store(true);
    SSPC_LOG(Cpu, Info,
             "exception dispatcher: SecondSonPC's (first vectored handler); owners: CPU patcher {}, bridge GPU "
             "tracking/SRT {}, bridge CPU patcher {}; fatal reports -> {}\\crash",
             options.cpu_patcher ? "SecondSonPC" : "-", options.bridge.access_violation ? "yes" : "no",
             options.bridge.patch_trap ? "yes" : "no", options.logs_dir);
    return true;
}

bool Installed() {
    return g_installed.load(std::memory_order_relaxed);
}

void PassEverything() {
    g_pass_everything.store(true);
}

void RegisterGuestCode(u64 begin, u64 end, std::string_view name) {
    if (end <= begin) {
        return;
    }
    AcquireSRWLockExclusive(&g_range_lock);
    const u32 index = g_range_count.load(std::memory_order_relaxed);
    if (index == kMaxRanges) {
        ReleaseSRWLockExclusive(&g_range_lock);
        SSPC_LOG(Cpu, Error, "exception dispatcher: guest code table full, {} not registered", name);
        return;
    }
    Range& r = g_ranges[index];
    r.begin = begin;
    r.end = end;
    const u64 words = ((end - begin + 15) / 16 + 63) / 64;
    r.red_zone = static_cast<u64*>(VirtualAlloc(nullptr, words * 8, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    const size_t n = std::min(name.size(), sizeof(r.name) - 1);
    std::memcpy(r.name, name.data(), n);
    r.name[n] = '\0';
    g_range_count.store(index + 1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_range_lock);
}

bool IsGuestCode(u64 address) {
    return FindRange(address) != nullptr;
}

bool FindGuestCode(u64 address, const char** name, u64* offset) {
    const Range* r = FindRange(address);
    if (r == nullptr) {
        return false;
    }
    *name = r->name;
    *offset = address - r->begin;
    return true;
}

void MarkRedZoneFunction(u64 begin, u64 end) {
    Range* r = const_cast<Range*>(FindRange(begin));
    if (r == nullptr || r->red_zone == nullptr || end <= begin) {
        return;
    }
    const u64 last = (std::min(end, r->end) - 1 - r->begin) >> 4;
    for (u64 g = (begin - r->begin) >> 4; g <= last; ++g) {
        InterlockedOr64(reinterpret_cast<volatile LONG64*>(&r->red_zone[g >> 6]), static_cast<LONG64>(1ull << (g & 63)));
    }
}

bool InRedZoneFunction(u64 address) {
    const Range* r = FindRange(address);
    if (r == nullptr || r->red_zone == nullptr) {
        return false;
    }
    const u64 g = (address - r->begin) >> 4;
    return (r->red_zone[g >> 6] >> (g & 63)) & 1;
}

void SetGuestSignalDelivery(GuestSignalDelivery delivery) {
    g_signal_delivery.store(delivery, std::memory_order_release);
}

void SetThreadDescriber(ThreadDescriber describer) {
    g_describer.store(describer, std::memory_order_release);
}

void SetFatalHook(void (*hook)()) {
    g_fatal_hook.store(hook);
}

void Fatal(EXCEPTION_POINTERS* info, const char* reason) {
    const DWORD self = GetCurrentThreadId();
    DWORD none = 0;
    if (!g_fatal_thread.compare_exchange_strong(none, self)) {
        if (none == self || self == g_reporter_id) {
            EmergencyExit("fault inside the fatal path"); // g_job already holds the first exception
        }
        for (;;) {
            Sleep(INFINITE); // another thread is reporting; the process ends with its report
        }
    }
    g_job.record = *info->ExceptionRecord;
    g_job.record.ExceptionRecord = nullptr;
    std::memcpy(&g_job.context, info->ContextRecord, sizeof(CONTEXT));
    g_job.context.ContextFlags &= ~(CONTEXT_XSTATE & ~CONTEXT_AMD64); // the copy has no extended state
    g_job.nested = t_outer != nullptr;
    if (g_job.nested) {
        g_job.outer_record = *t_outer->ExceptionRecord;
        std::memcpy(&g_job.outer_context, t_outer->ContextRecord, sizeof(CONTEXT));
    }
    g_job.thread_id = self;
    // Byte copies only: on a stack overflow little of this thread's stack is left.
    const size_t reason_length = std::min(std::strlen(reason), sizeof(g_job.reason) - 1);
    std::memcpy(g_job.reason, reason, reason_length);
    g_job.reason[reason_length] = '\0';
    const auto describe = g_describer.load(std::memory_order_acquire);
    if (describe != nullptr && g_job.record.ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        describe(g_job.thread, sizeof(g_job.thread));
    } else {
        constexpr char kNone[] = "not described (stack overflow or no describer)";
        std::memcpy(g_job.thread, kNone, sizeof(kNone));
    }
    if (g_fatal_start == nullptr || !SetEvent(g_fatal_start) ||
        WaitForSingleObject(g_fatal_done, g_report_timeout_ms) != WAIT_OBJECT_0) {
        EmergencyExit("the fatal reporter did not finish");
    }
    TerminateProcess(GetCurrentProcess(), g_job.record.ExceptionCode);
    for (;;) {
        Sleep(INFINITE);
    }
}

Stats GetStats() {
    Stats s{};
    s.access_violations = g_av.load(std::memory_order_relaxed);
    s.traps = g_traps.load(std::memory_order_relaxed);
    s.illegal = g_illegal.load(std::memory_order_relaxed);
    for (u32 i = 0; i < kOwners; ++i) {
        s.av_by[i] = g_av_by[i].load(std::memory_order_relaxed);
        s.traps_by[i] = g_traps_by[i].load(std::memory_order_relaxed);
        s.illegal_by[i] = g_illegal_by[i].load(std::memory_order_relaxed);
    }
    s.other_guest = g_other_guest.load(std::memory_order_relaxed);
    s.passed_other = g_passed_other.load(std::memory_order_relaxed);
    s.nested = g_nested.load(std::memory_order_relaxed);
    s.execute_faults = g_execute.load(std::memory_order_relaxed);
    s.bridge_cycles = g_bridge_cycles.load(std::memory_order_relaxed);
    s.bridge_max = g_bridge_max.load(std::memory_order_relaxed);
    std::array<u64, kBuckets> hist{};
    u64 total = 0;
    for (u32 i = 0; i < kBuckets; ++i) {
        hist[i] = g_hist[i].load(std::memory_order_relaxed);
        total += hist[i];
    }
    const auto percentile = [&](double p) -> u64 {
        if (total == 0) {
            return 0;
        }
        const u64 want = static_cast<u64>(p * static_cast<double>(total - 1)) + 1;
        u64 seen = 0;
        for (u32 i = 0; i < kBuckets; ++i) {
            seen += hist[i];
            if (seen >= want) {
                return BucketValue(i);
            }
        }
        return BucketValue(kBuckets - 1);
    };
    s.bridge_p50 = percentile(0.50);
    s.bridge_p95 = percentile(0.95);
    s.bridge_p99 = percentile(0.99);
    LARGE_INTEGER now{}, freq{};
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double seconds = static_cast<double>(now.QuadPart - g_install_qpc.QuadPart) / static_cast<double>(freq.QuadPart);
    s.tsc_mhz = seconds > 0.01 ? static_cast<double>(__rdtsc() - g_install_tsc) / seconds / 1e6 : 0.0;
    s.guest_resolved = g_guest_resolved.load(std::memory_order_relaxed);
    s.xstate_over_red_zone = g_xstate_over.load(std::memory_order_relaxed);
    s.in_red_zone_function = g_in_rz_fn.load(std::memory_order_relaxed);
    s.exposed = g_exposed.load(std::memory_order_relaxed);
    for (size_t i = 0; i < g_first_exposed.size(); ++i) {
        s.first_exposed[i] = g_first_exposed[i].load(std::memory_order_relaxed);
    }
    return s;
}

void LogSummary(const char* reason) {
    if (!Installed()) {
        return;
    }
    const auto s = GetStats();
    const double us_per_cycle = s.tsc_mhz > 0 ? 1.0 / s.tsc_mhz : 0.0;
    const u64 bridge = s.av_by[static_cast<u32>(Owner::Bridge)];
    std::string exposed;
    for (const u64 rip : s.first_exposed) {
        if (rip == 0) {
            break;
        }
        const char* name = nullptr;
        u64 offset = 0;
        exposed += FindGuestCode(rip, &name, &offset) ? fmt::format(" {}+{:#x}", name, offset) : fmt::format(" {:#x}", rip);
    }
    SSPC_LOG(Cpu, Info,
             "exceptions {}: access violations {} (CPU patcher {}, bridge {}, guest signal {}, fatal {}, passed on {}); "
             "traps {} (CPU patcher {}, bridge {}, passed on {}); illegal {} (CPU emulation {}, bridge {}, passed on {}); "
             "guest FPU/other {}; other passed on {}; nested {}; execute faults {}; bridge handler time P50/P95/P99/max "
             "{:.2f}/{:.2f}/{:.2f}/{:.2f} us (mean {:.2f}); red zone: resolved in guest code {}, AVX state over "
             "[rsp-128, rsp) {}, in red-zone functions {}, both {}{}",
             reason, s.access_violations, s.av_by[0], bridge, s.av_by[2], s.av_by[3], s.av_by[4], s.traps, s.traps_by[0],
             s.traps_by[1], s.traps_by[4], s.illegal, s.illegal_by[0], s.illegal_by[1], s.illegal_by[4], s.other_guest,
             s.passed_other, s.nested, s.execute_faults, s.bridge_p50 * us_per_cycle, s.bridge_p95 * us_per_cycle,
             s.bridge_p99 * us_per_cycle, s.bridge_max * us_per_cycle,
             bridge != 0 ? static_cast<double>(s.bridge_cycles) / static_cast<double>(bridge) * us_per_cycle : 0.0,
             s.guest_resolved, s.xstate_over_red_zone, s.in_red_zone_function, s.exposed,
             exposed.empty() ? std::string{} : " (first at" + exposed + ")");
}

namespace Test {

void ResetStats() {
    for (auto* a : {&g_av, &g_traps, &g_illegal, &g_other_guest, &g_passed_other, &g_nested, &g_execute,
                    &g_guest_resolved, &g_xstate_over, &g_in_rz_fn, &g_exposed, &g_bridge_cycles, &g_bridge_max}) {
        a->store(0);
    }
    for (auto& a : g_av_by) {
        a.store(0);
    }
    for (auto& a : g_traps_by) {
        a.store(0);
    }
    for (auto& a : g_illegal_by) {
        a.store(0);
    }
    for (auto& a : g_hist) {
        a.store(0);
    }
    for (auto& a : g_first_exposed) {
        a.store(0);
    }
}

BridgeHandlers SetBridgeHandlers(const BridgeHandlers& handlers) {
    const BridgeHandlers previous = g_options.bridge;
    g_options.bridge = handlers;
    return previous;
}

void SetReportTimeout(u32 ms) {
    g_report_timeout_ms = ms;
}

} // namespace Test

} // namespace SecondSon::Cpu::Exceptions
