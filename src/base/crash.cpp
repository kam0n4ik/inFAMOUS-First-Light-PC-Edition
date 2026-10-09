// SecondSonPC — crash diagnostics in SecondSonPC.log.
//
// 1. First-chance fault recorder (SSPC-002, docs/KNOWN_ISSUES.md). The bridge's vectored handler
//    (shadPS4 signals.cpp) gives every access violation to cpu_patches.cpp TryPatchJit, which
//    decodes the instruction at RIP. GetModule() there takes the nearest module *below* RIP without
//    checking its end, so after a jump to an unmapped address above a guest module the decoder
//    reads unmapped memory (ZydisInputPeek): a second access violation inside the handler. The
//    original exception is lost, and the frame walk through guest code (no unwind data) never
//    reaches the unhandled exception filter, so the process dies without a report. This handler is
//    registered first, records the original fault and always passes it on unchanged.
// 2. Unhandled exception filter: code, address, registers, return addresses on the stack.
//
// Symbolize the exe+offset values with:
//   E:\SecondSonPC\toolchain\LLVM\bin\llvm-symbolizer.exe --obj=build\bin\SecondSonPC.exe --relative-address <offset>
#include "base/crash.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <intrin.h>
#include <windows.h>
#include <dbghelp.h>

#include "base/log.h"
#include "base/vram_monitor.h"

namespace SecondSon::Base {

namespace {

u64 g_image_base = 0;
u64 g_image_size = 0;

bool InImage(u64 address) {
    return address >= g_image_base && address < g_image_base + g_image_size;
}

// ---------------------------------------------------------------------------- memory queries
// Neither ever faults: the recorder runs inside exception dispatch.

struct Region {
    bool ok = false;
    u64 alloc_base = 0;
    u64 end = 0; // end of the region with the same state/protection
    DWORD state = 0;
    DWORD protect = 0;
    DWORD type = 0;
};

Region Query(u64 address) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof(mbi)) == 0) {
        return {};
    }
    return {true, reinterpret_cast<u64>(mbi.AllocationBase),
            reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize, mbi.State, mbi.Protect, mbi.Type};
}

bool IsExecutable(const Region& r) {
    constexpr DWORD kExec = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return r.ok && r.state == MEM_COMMIT && (r.protect & kExec) != 0 && (r.protect & PAGE_GUARD) == 0;
}

bool ReadMemory(u64 address, void* out, size_t size) {
    SIZE_T done = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), out, size, &done) &&
           done == size;
}

std::string Describe(u64 address) {
    if (InImage(address)) {
        return fmt::format("exe+{:#x}", address - g_image_base);
    }
    const auto r = Query(address);
    if (!r.ok) {
        return "outside the user address space";
    }
    if (r.state == MEM_FREE) {
        return "free (unmapped)";
    }
    const char* state = r.state == MEM_COMMIT ? "committed" : "reserved";
    const char* type = r.type == MEM_IMAGE ? "image" : r.type == MEM_MAPPED ? "mapped" : "private";
    return fmt::format("{} {} prot {:#x}, allocation {:#x}+{:#x}", state, type, r.protect, r.alloc_base,
                       address - r.alloc_base);
}

std::string HexBytes(u64 address, size_t size) {
    u8 bytes[32]{};
    if (size > sizeof(bytes) || !ReadMemory(address, bytes, size)) {
        return "unreadable";
    }
    std::string s;
    for (size_t i = 0; i < size; ++i) {
        s += fmt::format("{}{:02x}", i ? " " : "", bytes[i]);
    }
    return s;
}

std::string Narrow(const std::wstring& ascii) {
    std::string s;
    s.reserve(ascii.size());
    for (const wchar_t ch : ascii) {
        s.push_back(ch < 128 ? static_cast<char>(ch) : '?');
    }
    return s;
}

// ---------------------------------------------------------------------------- first-chance recorder

using MiniDumpWriteDumpFn = decltype(&MiniDumpWriteDump);
using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);

std::wstring g_dump_dir;
bool g_write_dump = false;
MiniDumpWriteDumpFn g_mini_dump = nullptr; // resolved at install: no loader lock inside a fault
GetThreadDescriptionFn g_thread_description = nullptr;

constexpr u32 kFullReports = 3;   // full reports per process; then one line each
constexpr u32 kShortReports = 16; // then nothing (the fault record is still kept)
constexpr DWORD kDumpTimeoutMs = 20000;

std::atomic<u32> g_serial{0};
std::atomic<bool> g_dump_taken{false};
std::atomic_flag g_stack_buffer_busy = ATOMIC_FLAG_INIT;
u64 g_stack_buffer[512]; // static: a fault near the end of a guest stack leaves little room there
SRWLOCK g_record_lock = SRWLOCK_INIT;
FaultRecord g_record{};
thread_local bool t_recording = false;

// What the recorder costs (LogFirstChanceStats): the bridge's GPU write tracking raises an access
// violation for every guest write to a protected page.
bool g_first_chance_installed = false;
PVOID g_late_handler = nullptr;       // DataFaultFilter, after the bridge's handler
std::atomic<u64> g_av_count{0};       // access violations seen first (all of them)
std::atomic<u64> g_av_late{0};        // data accesses nobody before DataFaultFilter handled
std::atomic<u64> g_av_host{0};        // ... of them from SecondSonPC.exe (no system call)
std::atomic<u64> g_av_queried{0};     // ... that cost a VirtualQuery of the address
std::atomic<u64> g_query_ticks{0};    // QueryPerformanceCounter ticks spent in those queries
std::atomic<u64> g_query_max_ticks{0};
bool g_stamp_first_chance = false;    // --track-stats (P2.5): time stamp of each access violation
thread_local u64 t_first_chance_tsc = 0;

constexpr const char* kRegNames[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
                                       "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};

std::string ThreadName() {
    PWSTR wide = nullptr;
    if (g_thread_description == nullptr || FAILED(g_thread_description(GetCurrentThread(), &wide)) ||
        wide == nullptr) {
        return "";
    }
    std::string name = Narrow(std::wstring(wide).substr(0, 64));
    LocalFree(wide);
    return name;
}

/// Owns copies of the exception record and context: after a timeout the writer may still run
/// when the original EXCEPTION_POINTERS are gone.
struct DumpJob {
    EXCEPTION_RECORD record{};
    CONTEXT context{};
    EXCEPTION_POINTERS exception{&record, &context};
    DWORD thread_id = 0;
    std::wstring path;
    BOOL ok = FALSE;
    DWORD error = 0;
};

DWORD WINAPI DumpThread(void* param) {
    auto* job = static_cast<DumpJob*>(param);
    const HANDLE file = CreateFileW(job->path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        job->error = GetLastError();
        return 0;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{job->thread_id, &job->exception, FALSE};
    const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithIndirectlyReferencedMemory |
                                                 MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData);
    job->ok = g_mini_dump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &mei, nullptr, nullptr);
    if (!job->ok) {
        job->error = GetLastError();
    }
    CloseHandle(file);
    return 0;
}

/// Writes the minidump on a helper thread (MiniDumpWriteDump on the faulting thread can deadlock
/// on locks it holds); waits at most kDumpTimeoutMs. True when the dump was written.
bool WriteDump(EXCEPTION_POINTERS* info) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    CreateDirectoryW(g_dump_dir.c_str(), nullptr);
    wchar_t name[96];
    std::swprintf(name, 96, L"\\SecondSonPC_%04u-%02u-%02u_%02u-%02u-%02u_%lu.dmp", t.wYear, t.wMonth, t.wDay,
                  t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    auto* job = new DumpJob;
    job->record = *info->ExceptionRecord;
    job->record.ExceptionRecord = nullptr; // no chained record outlives this handler
    job->context = *info->ContextRecord;
    job->thread_id = GetCurrentThreadId();
    job->path = g_dump_dir + name;
    const std::string path = Narrow(job->path);
    const HANDLE thread = CreateThread(nullptr, 0, &DumpThread, job, 0, nullptr);
    if (thread == nullptr) {
        SSPC_LOG(App, Critical, "  minidump: cannot start the writer thread (error {})", GetLastError());
        delete job;
        return false;
    }
    if (WaitForSingleObject(thread, kDumpTimeoutMs) != WAIT_OBJECT_0) {
        // The job stays allocated: the writer still uses it.
        SSPC_LOG(App, Critical, "  minidump: still writing after {} s, continuing ({})", kDumpTimeoutMs / 1000, path);
        CloseHandle(thread);
        return false;
    }
    CloseHandle(thread);
    const bool ok = job->ok != FALSE;
    if (ok) {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        const u64 size = GetFileAttributesExW(job->path.c_str(), GetFileExInfoStandard, &a)
                             ? (static_cast<u64>(a.nFileSizeHigh) << 32) | a.nFileSizeLow
                             : 0;
        SSPC_LOG(App, Critical, "  minidump: {} ({} KiB)", path, size / 1024);
    } else {
        SSPC_LOG(App, Critical, "  minidump: failed, error {:#x} ({})", job->error, path);
    }
    delete job;
    return ok;
}

void LogStack(u64 rsp) {
    const auto region = Query(rsp);
    if (!region.ok || region.state != MEM_COMMIT || region.end <= rsp) {
        SSPC_LOG(App, Critical, "  stack: rsp {:#x} is not readable ({})", rsp, Describe(rsp));
        return;
    }
    if (g_stack_buffer_busy.test_and_set(std::memory_order_acquire)) {
        SSPC_LOG(App, Critical, "  stack: another thread is recording a fault, not dumped");
        return;
    }
    u64* stack = g_stack_buffer;
    const size_t count = static_cast<size_t>(std::min<u64>((region.end - rsp) / 8, std::size(g_stack_buffer)));
    if (!ReadMemory(rsp, stack, count * 8)) {
        SSPC_LOG(App, Critical, "  stack: rsp {:#x} could not be read", rsp);
        g_stack_buffer_busy.clear(std::memory_order_release);
        return;
    }
    SSPC_LOG(App, Critical, "  stack: rsp {:#x} ({}), {} qwords read", rsp, Describe(rsp), count);
    for (size_t i = 0; i < std::min<size_t>(count, 16); i += 4) {
        std::string line;
        for (size_t j = i; j < std::min(count, i + 4); ++j) {
            line += fmt::format(" {:016x}", stack[j]);
        }
        SSPC_LOG(App, Critical, "  [rsp+{:#05x}]{}", i * 8, line);
    }
    // Values that point into executable memory: return addresses of guest and host frames.
    int shown = 0;
    for (size_t i = 0; i < count && shown < 24; ++i) {
        const u64 v = stack[i];
        if (v < 0x10000 || !IsExecutable(Query(v))) {
            continue;
        }
        SSPC_LOG(App, Critical, "  code ptr [rsp+{:#x}] {:#x} {}; 16 bytes before: {}", i * 8, v, Describe(v),
                 HexBytes(v - 16, 16));
        ++shown;
    }
    g_stack_buffer_busy.clear(std::memory_order_release);
}

void StoreRecord(const FaultRecord& r) {
    AcquireSRWLockExclusive(&g_record_lock);
    g_record = r;
    ReleaseSRWLockExclusive(&g_record_lock);
}

void Record(EXCEPTION_POINTERS* info, const FaultFacts& f, FaultKind kind) {
    const auto* ctx = info->ContextRecord;
    FaultRecord r;
    r.serial = g_serial.fetch_add(1) + 1;
    r.kind = kind;
    r.rip = f.rip;
    r.address = f.address;
    ReadMemory(ctx->Rsp, &r.stack_top, 8);
    ReadMemory(ctx->Rsp - 8, &r.popped, 8);
    const u64 regs[16] = {ctx->Rax, ctx->Rcx, ctx->Rdx, ctx->Rbx, ctx->Rsp, ctx->Rbp, ctx->Rsi, ctx->Rdi,
                          ctx->R8,  ctx->R9,  ctx->R10, ctx->R11, ctx->R12, ctx->R13, ctx->R14, ctx->R15};
    std::string holding;
    for (u32 i = 0; i < 16; ++i) {
        if (regs[i] == f.rip) {
            r.regs_equal_rip |= 1u << i;
            holding += fmt::format("{}{}", holding.empty() ? "" : " ", kRegNames[i]);
        }
    }
    if (r.serial > kFullReports + kShortReports) {
        StoreRecord(r);
        return;
    }
    // An uncommitted-memory access reaches the recorder only after the bridge and the game's signal
    // handler declined it (the unhandled exception filter may still follow); only an execute fault
    // gets a minidump.
    const char* what = kind == FaultKind::Execute
                           ? "execute fault"
                           : "access to uncommitted memory from code outside the images, not handled by "
                             "the bridge or the game";
    const char* access = f.access == 0 ? "read" : f.access == 1 ? "write" : f.access == 8 ? "execute" : "?";
    if (r.serial > kFullReports) {
        SSPC_LOG(App, Error, "first-chance fault #{}: {}, {} of {:#x}, rip {:#x}, [rsp] {:#x}, thread {}", r.serial,
                 what, access, f.address, f.rip, r.stack_top, GetCurrentThreadId());
        StoreRecord(r);
        return;
    }

    SSPC_LOG(App, Critical, "first-chance fault #{} (SSPC-002 recorder): {}, {} of {:#x} at rip {:#x}; thread {} \"{}\"",
             r.serial, what, access, f.address, f.rip, GetCurrentThreadId(), ThreadName());
    SSPC_LOG(App, Critical, "  rip: {}", Describe(f.rip));
    if (f.address != f.rip) {
        SSPC_LOG(App, Critical, "  address: {}", Describe(f.address));
    }
    SSPC_LOG(App, Critical, "  rax {:#x} rcx {:#x} rdx {:#x} rbx {:#x} rsp {:#x} rbp {:#x} rsi {:#x} rdi {:#x}",
             regs[0], regs[1], regs[2], regs[3], regs[4], regs[5], regs[6], regs[7]);
    SSPC_LOG(App, Critical, "  r8 {:#x} r9 {:#x} r10 {:#x} r11 {:#x} r12 {:#x} r13 {:#x} r14 {:#x} r15 {:#x} rflags {:#x}",
             regs[8], regs[9], regs[10], regs[11], regs[12], regs[13], regs[14], regs[15], ctx->EFlags);
    // How control got to rip: a call leaves its return address at [rsp]; a ret to a bad address
    // has just popped it from [rsp-8]; jmp/call through a register leaves it in that register.
    SSPC_LOG(App, Critical, "  jump: [rsp] {:#x} ({}){}; [rsp-8] {:#x}{}; registers holding rip: {}", r.stack_top,
             Describe(r.stack_top),
             IsExecutable(Query(r.stack_top)) ? fmt::format(", 16 bytes before: {}", HexBytes(r.stack_top - 16, 16))
                                              : std::string{},
             r.popped, r.popped == f.rip ? " = rip (a ret popped the bad address)" : "",
             holding.empty() ? "none" : holding);
    LogStack(ctx->Rsp);
    LogVramSample("crash", false);
    Log::Flush();
    if (kind == FaultKind::Execute && g_write_dump && g_mini_dump != nullptr && !g_dump_taken.exchange(true)) {
        r.dump_written = WriteDump(info);
        Log::Flush();
    }
    StoreRecord(r);
}

FaultFacts Facts(const EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    return {rec->ExceptionCode, rec->ExceptionInformation[0], rec->ExceptionInformation[1],
            info->ContextRecord->Rip, false, false};
}

bool IsAccessViolation(const EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    return rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 && !t_recording;
}

/// Registered first (before the bridge's handler): records execute faults, which the bridge's
/// handler turns into a second fault of its own (SSPC-002). Every other access violation leaves at
/// once, without a system call: the bridge's GPU write tracking raises one for each guest write to
/// a protected page — hundreds to thousands per second in gameplay. A VirtualQuery here for each of
/// them (up to 1 ms in this process's ~1 TB address space) cut gameplay from ~25 to ~5 FPS.
LONG WINAPI FirstChanceFilter(EXCEPTION_POINTERS* info) {
    if (!IsAccessViolation(info)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_stamp_first_chance) {
        t_first_chance_tsc = __rdtsc();
    }
    g_av_count.fetch_add(1, std::memory_order_relaxed);
    const FaultFacts f = Facts(info);
    if (ClassifyFault(f) == FaultKind::Execute) {
        t_recording = true;
        Record(info, f, FaultKind::Execute);
        t_recording = false;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// Registered after the bridge's handler (InstallLateFaultRecorder): sees only the data accesses
/// that neither the bridge nor the game's signal handler resolved, and records guest code touching
/// memory that is not committed. Host code is left to the unhandled exception filter.
LONG WINAPI DataFaultFilter(EXCEPTION_POINTERS* info) {
    if (!IsAccessViolation(info)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    FaultFacts f = Facts(info);
    if (ClassifyFault(f) == FaultKind::Execute) {
        return EXCEPTION_CONTINUE_SEARCH; // recorded by FirstChanceFilter
    }
    g_av_late.fetch_add(1, std::memory_order_relaxed);
    if (InImage(f.rip)) {
        g_av_host.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    const auto target = Query(f.address);
    QueryPerformanceCounter(&q1);
    const u64 ticks = static_cast<u64>(q1.QuadPart - q0.QuadPart);
    g_av_queried.fetch_add(1, std::memory_order_relaxed);
    g_query_ticks.fetch_add(ticks, std::memory_order_relaxed);
    for (u64 max = g_query_max_ticks.load(std::memory_order_relaxed);
         ticks > max && !g_query_max_ticks.compare_exchange_weak(max, ticks, std::memory_order_relaxed);) {
    }
    f.address_committed = target.ok && target.state == MEM_COMMIT;
    if (!f.address_committed) {
        const auto code = Query(f.rip);
        f.rip_in_image = code.ok && code.type == MEM_IMAGE;
    }
    const auto kind = ClassifyFault(f);
    if (kind != FaultKind::Ignore) {
        t_recording = true;
        Record(info, f, kind);
        t_recording = false;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------------------- unhandled filter

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    const auto* ctx = info->ContextRecord;
    const u64 address = reinterpret_cast<u64>(rec->ExceptionAddress);
    SSPC_LOG(App, Critical, "unhandled exception {:#010x} at {:#x}{}", rec->ExceptionCode, address,
             InImage(address) ? fmt::format(" (exe+{:#x})", address - g_image_base) : "");
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        static constexpr const char* kKinds[] = {"read", "write", "?", "?", "?", "?", "?", "?", "execute"};
        const auto kind = rec->ExceptionInformation[0];
        SSPC_LOG(App, Critical, "  access violation: {} of {:#x}", kind < 9 ? kKinds[kind] : "?",
                 rec->ExceptionInformation[1]);
    }
    SSPC_LOG(App, Critical, "  rip {:#x} rsp {:#x} rbp {:#x} rax {:#x} rcx {:#x} rdx {:#x}", ctx->Rip,
             ctx->Rsp, ctx->Rbp, ctx->Rax, ctx->Rcx, ctx->Rdx);
    SSPC_LOG(App, Critical, "  rdi {:#x} rsi {:#x} r8 {:#x} r9 {:#x} r10 {:#x} r11 {:#x}", ctx->Rdi,
             ctx->Rsi, ctx->R8, ctx->R9, ctx->R10, ctx->R11);
    // Return-address candidates: stack qwords that point into the executable image.
    const auto* stack = reinterpret_cast<const u64*>(ctx->Rsp);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(stack, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT) {
        const u64 end = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
        int shown = 0;
        for (u64 i = 0; i < 2048 && reinterpret_cast<u64>(stack + i + 1) <= end && shown < 24; ++i) {
            if (InImage(stack[i])) {
                SSPC_LOG(App, Critical, "  stack[{:#x}] exe+{:#x}", i * 8, stack[i] - g_image_base);
                ++shown;
            }
        }
    }
    LogVramSample("crash", false);
    Log::Flush();
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

FaultKind ClassifyFault(const FaultFacts& f) {
    if (f.code != EXCEPTION_ACCESS_VIOLATION) {
        return FaultKind::Ignore;
    }
    if (f.access == 8 || f.address == f.rip) {
        return FaultKind::Execute;
    }
    if (!f.rip_in_image && !f.address_committed) {
        return FaultKind::GuestUnmappedAccess;
    }
    return FaultKind::Ignore;
}

FaultRecord LastFaultRecord() {
    AcquireSRWLockShared(&g_record_lock);
    const FaultRecord r = g_record;
    ReleaseSRWLockShared(&g_record_lock);
    return r;
}

void LogFirstChanceStats(const char* reason) {
    if (!g_first_chance_installed) {
        SSPC_LOG(App, Info, "first-chance stats {}: recorder off (--no-fault-recorder)", reason);
        return;
    }
    static LARGE_INTEGER freq{};
    static SRWLOCK lock = SRWLOCK_INIT;
    static u64 last_count = 0, last_late = 0, last_queried = 0, last_ticks = 0;
    static LARGE_INTEGER last_time{};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const u64 count = g_av_count.load(std::memory_order_relaxed);
    const u64 late = g_av_late.load(std::memory_order_relaxed);
    const u64 host = g_av_host.load(std::memory_order_relaxed);
    const u64 queried = g_av_queried.load(std::memory_order_relaxed);
    const u64 ticks = g_query_ticks.load(std::memory_order_relaxed);
    const double max_us = g_query_max_ticks.load(std::memory_order_relaxed) * 1e6 / freq.QuadPart;
    AcquireSRWLockExclusive(&lock);
    const double dt = last_time.QuadPart != 0 ? double(now.QuadPart - last_time.QuadPart) / freq.QuadPart : 0.0;
    const u64 d_count = count - last_count;
    const u64 d_late = late - last_late;
    const u64 d_queried = queried - last_queried;
    const double d_query_ms = double(ticks - last_ticks) * 1e3 / freq.QuadPart;
    last_count = count;
    last_late = late;
    last_queried = queried;
    last_ticks = ticks;
    last_time = now;
    ReleaseSRWLockExclusive(&lock);
    SSPC_LOG(App, Info,
             "first-chance stats {}: access violations {} (+{} in {:.1f} s = {:.0f}/s); not handled by the "
             "bridge/game {} (+{}{}), from exe {}, VirtualQuery {} (+{}), query time {:.1f} ms (+{:.1f} ms), "
             "max {:.0f} us; recorded {}",
             reason, count, d_count, dt, dt > 0 ? d_count / dt : 0.0, late, d_late,
             g_late_handler != nullptr ? "" : ", data-fault recorder not installed yet", host, queried, d_queried,
             double(ticks) * 1e3 / freq.QuadPart, d_query_ms, max_us, g_serial.load());
}

void EnableFirstChanceStamp() {
    g_stamp_first_chance = true;
}

u64 FirstChanceStamp() {
    return t_first_chance_tsc;
}

void InstallLateFaultRecorder() {
    if (!g_first_chance_installed) {
        return;
    }
    // Re-added at the end of the handler list: behind every handler registered so far.
    if (g_late_handler != nullptr) {
        RemoveVectoredExceptionHandler(g_late_handler);
    }
    g_late_handler = AddVectoredExceptionHandler(0, &DataFaultFilter);
    if (g_late_handler == nullptr) {
        SSPC_LOG(App, Error, "data-fault recorder: AddVectoredExceptionHandler failed");
        return;
    }
    SSPC_LOG(App, Info, "crash diagnostics: data-fault recorder after the bridge's exception handler");
}

void InstallCrashLogger(const std::filesystem::path& dump_dir, bool write_dump, bool first_chance) {
    const auto* base = reinterpret_cast<const u8*>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    g_image_base = reinterpret_cast<u64>(base);
    g_image_size = nt->OptionalHeader.SizeOfImage;

    g_dump_dir = dump_dir.wstring();
    g_write_dump = write_dump;
    if (const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) {
        g_mini_dump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    }
    if (const HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
        g_thread_description =
            reinterpret_cast<GetThreadDescriptionFn>(GetProcAddress(kernel, "GetThreadDescription"));
    }
    SetUnhandledExceptionFilter(&CrashFilter);
    if (!first_chance) {
        SSPC_LOG(App, Warning, "DIAGNOSTIC: --no-fault-recorder: first-chance fault recorder off "
                               "(unhandled exception filter only)");
        return;
    }
    // First = 1: runs before the bridge's handler, which is added later with First = 0. The
    // data-fault half follows the bridge's handler: InstallLateFaultRecorder().
    if (AddVectoredExceptionHandler(1, &FirstChanceFilter) == nullptr) {
        SSPC_LOG(App, Error, "first-chance fault recorder: AddVectoredExceptionHandler failed");
    } else {
        g_first_chance_installed = true;
    }
    SSPC_LOG(App, Info, "crash diagnostics: first-chance fault recorder on; minidump of an execute fault {}",
             write_dump && g_mini_dump != nullptr ? "-> " + dump_dir.string() : std::string("off"));
}

// ---------------------------------------------------------------------------- --fault-test

namespace {

using TestFn = void (*)();

/// Calls fn under SEH; false when an exception was raised (and caught here, after the recorder).
__declspec(noinline) bool CallGuarded(TestFn fn) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

volatile u64 g_host_target = 0;

__declspec(noinline) void HostRead() {
    (void)*reinterpret_cast<volatile u64*>(g_host_target);
}

/// Stands in for the bridge's GPU write tracking: a write to the tracked page makes it writable and
/// the write is retried.
u8* volatile g_tracked_page = nullptr;

LONG WINAPI TrackingHandler(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    u8* page = g_tracked_page;
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2 &&
        rec->ExceptionInformation[0] == 1 && page != nullptr &&
        rec->ExceptionInformation[1] - reinterpret_cast<u64>(page) < 0x1000) {
        DWORD old = 0;
        VirtualProtect(page, 0x1000, PAGE_READWRITE, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/// Emits `mov rax, imm64` at p; returns the next byte.
u8* MovRax(u8* p, u64 value) {
    p[0] = 0x48;
    p[1] = 0xB8;
    std::memcpy(p + 2, &value, 8);
    return p + 10;
}

struct FaultChecks {
    void Check(const std::string& what, bool ok, const std::string& detail = {}) {
        ++checks;
        if (!ok) {
            ++failures;
        }
        lines.push_back(fmt::format("{} {}{}", ok ? "PASS" : "FAIL", what, ok || detail.empty() ? "" : ": " + detail));
    }
    int checks = 0;
    int failures = 0;
    std::vector<std::string> lines;
};

void RunLiveFaults(FaultChecks& c, const std::filesystem::path& logs_dir, u8* reserved, u8* code, u8* data_ro,
                   u8* data_rw) {
    const u64 bad = reinterpret_cast<u64>(reserved) + 0x120;
    // ret_stub: mov rax, bad; push rax; xor eax, eax; ret  -> rip = bad, [rsp-8] = bad, rax = 0
    u8* ret_stub = code;
    u8* p = MovRax(ret_stub, bad);
    const u8 ret_tail[] = {0x50, 0x31, 0xC0, 0xC3};
    std::memcpy(p, ret_tail, sizeof(ret_tail));
    // jmp_stub: mov rax, bad; jmp rax
    u8* jmp_stub = code + 0x40;
    p = MovRax(jmp_stub, bad);
    const u8 jmp_tail[] = {0xFF, 0xE0};
    std::memcpy(p, jmp_tail, sizeof(jmp_tail));
    // read_stub: mov rax, bad; mov rax, [rax]; ret  (code outside any image)
    u8* read_stub = code + 0x80;
    p = MovRax(read_stub, bad);
    const u8 read_tail[] = {0x48, 0x8B, 0x00, 0xC3};
    std::memcpy(p, read_tail, sizeof(read_tail));
    // write_ro_stub: mov rax, data_ro; mov [rax], rax; ret  (like a page the bridge protects)
    u8* write_ro_stub = code + 0xC0;
    p = MovRax(write_ro_stub, reinterpret_cast<u64>(data_ro));
    const u8 write_tail[] = {0x48, 0x89, 0x00, 0xC3};
    std::memcpy(p, write_tail, sizeof(write_tail));
    // int3_stub: int3; ret  (not an access violation)
    u8* int3_stub = code + 0x100;
    int3_stub[0] = 0xCC;
    int3_stub[1] = 0xC3;
    FlushInstructionCache(GetCurrentProcess(), code, 0x1000);

    // The first execute fault writes this process's one minidump; the test removes it afterwards.
    const auto test_dumps = logs_dir / "crash" / "faulttest";
    std::error_code ec;
    std::filesystem::create_directories(test_dumps, ec);
    g_dump_dir = test_dumps.wstring();
    g_write_dump = true;
    c.Check("dbghelp MiniDumpWriteDump available", g_mini_dump != nullptr);

    // 1. call to an unmapped address
    u32 before = LastFaultRecord().serial;
    bool returned = CallGuarded(reinterpret_cast<TestFn>(bad));
    FaultRecord r = LastFaultRecord();
    c.Check("call to reserved memory: recorded as execute fault",
            !returned && r.serial == before + 1 && r.kind == FaultKind::Execute && r.rip == bad,
            fmt::format("serial {}->{}, rip {:#x}", before, r.serial, r.rip));
    c.Check("call: [rsp] is the return address in SecondSonPC.exe", InImage(r.stack_top),
            fmt::format("{:#x}", r.stack_top));
    c.Check("call: minidump written", r.dump_written);
    int dumps = 0;
    for (const auto& e : std::filesystem::directory_iterator(test_dumps, ec)) {
        c.Check("call: minidump file is not empty", e.file_size() > 0);
        ++dumps;
        std::filesystem::remove(e.path(), ec);
    }
    std::filesystem::remove(test_dumps, ec);
    c.Check("call: exactly one minidump", dumps == 1, fmt::format("{}", dumps));

    // 2. ret to an unmapped address
    before = r.serial;
    returned = CallGuarded(reinterpret_cast<TestFn>(ret_stub));
    r = LastFaultRecord();
    // (The compiler may keep `bad` in a callee-saved register of this function: only rax, which
    // the stub cleared, is checked.)
    c.Check("ret to reserved memory: recorded, [rsp-8] = rip, rax does not hold it",
            !returned && r.serial == before + 1 && r.kind == FaultKind::Execute && r.rip == bad && r.popped == bad &&
                (r.regs_equal_rip & 1) == 0,
            fmt::format("popped {:#x}, regs {:#x}", r.popped, r.regs_equal_rip));
    c.Check("ret: second execute fault writes no second minidump", !r.dump_written);

    // 3. jmp rax to an unmapped address
    before = r.serial;
    returned = CallGuarded(reinterpret_cast<TestFn>(jmp_stub));
    r = LastFaultRecord();
    c.Check("jmp rax to reserved memory: recorded, rax holds rip",
            !returned && r.serial == before + 1 && r.rip == bad && (r.regs_equal_rip & 1) != 0,
            fmt::format("regs {:#x}", r.regs_equal_rip));

    // 4. call into committed non-executable memory (DEP)
    before = r.serial;
    returned = CallGuarded(reinterpret_cast<TestFn>(data_rw));
    r = LastFaultRecord();
    c.Check("call into a read/write page (DEP): execute fault",
            !returned && r.serial == before + 1 && r.kind == FaultKind::Execute &&
                r.rip == reinterpret_cast<u64>(data_rw));

    // 5. code outside any image reads reserved memory
    before = r.serial;
    returned = CallGuarded(reinterpret_cast<TestFn>(read_stub));
    r = LastFaultRecord();
    c.Check("read of reserved memory from code outside the images: recorded",
            !returned && r.serial == before + 1 && r.kind == FaultKind::GuestUnmappedAccess && r.address == bad &&
                r.rip == reinterpret_cast<u64>(read_stub) + 10,
            fmt::format("kind {}, address {:#x}, rip {:#x}", static_cast<int>(r.kind), r.address, r.rip));

    // 6-8. faults the recorder must leave alone
    before = r.serial;
    g_host_target = bad;
    returned = CallGuarded(&HostRead);
    c.Check("host code read of reserved memory: not recorded", !returned && LastFaultRecord().serial == before);
    returned = CallGuarded(reinterpret_cast<TestFn>(write_ro_stub));
    c.Check("write to a read-only committed page: not recorded", !returned && LastFaultRecord().serial == before);
    returned = CallGuarded(reinterpret_cast<TestFn>(int3_stub));
    c.Check("int3: not recorded", !returned && LastFaultRecord().serial == before);

    // 9. P0 FPS regression: a fault the bridge's handler resolves (GPU write tracking, ~1000/s in
    // gameplay) must cost the recorder no system call. As in the game, the data-fault half sits
    // behind that handler and never sees it; the first half only counts it.
    const PVOID tracking = AddVectoredExceptionHandler(0, &TrackingHandler);
    InstallLateFaultRecorder();
    g_tracked_page = data_ro;
    const u64 seen = g_av_count.load(), late = g_av_late.load(), queried = g_av_queried.load();
    returned = CallGuarded(reinterpret_cast<TestFn>(write_ro_stub));
    c.Check("write tracked by the bridge's handler: resumed, seen first, no VirtualQuery, not recorded",
            returned && g_av_count.load() == seen + 1 && g_av_late.load() == late &&
                g_av_queried.load() == queried && LastFaultRecord().serial == before,
            fmt::format("returned {}, seen +{}, late +{}, queried +{}", returned, g_av_count.load() - seen,
                        g_av_late.load() - late, g_av_queried.load() - queried));
    g_tracked_page = nullptr;
    // A fault the tracking handler does not take still reaches the data-fault recorder behind it.
    before = LastFaultRecord().serial;
    returned = CallGuarded(reinterpret_cast<TestFn>(read_stub));
    r = LastFaultRecord();
    c.Check("behind the bridge's handler: read of reserved memory still recorded",
            !returned && r.serial == before + 1 && r.kind == FaultKind::GuestUnmappedAccess && r.address == bad);
    RemoveVectoredExceptionHandler(tracking);
}

} // namespace

int RunFaultSelfTest(const std::filesystem::path& logs_dir) {
    FaultChecks c;
    InstallLateFaultRecorder(); // no bridge here: directly behind FirstChanceFilter
    constexpr u32 kAv = EXCEPTION_ACCESS_VIOLATION;
    c.Check("classify: execute fault (access 8)",
            ClassifyFault({kAv, 8, 0x1000, 0x1000, false, false}) == FaultKind::Execute);
    c.Check("classify: fault address = rip", ClassifyFault({kAv, 0, 0x1000, 0x1000, false, false}) == FaultKind::Execute);
    c.Check("classify: read of uncommitted memory from code outside the images",
            ClassifyFault({kAv, 0, 0x2000, 0x1000, false, false}) == FaultKind::GuestUnmappedAccess);
    c.Check("classify: write to a protected (committed) page is the bridge's",
            ClassifyFault({kAv, 1, 0x2000, 0x1000, false, true}) == FaultKind::Ignore);
    c.Check("classify: host code fault is left to the unhandled filter",
            ClassifyFault({kAv, 0, 0x2000, 0x1000, true, false}) == FaultKind::Ignore);
    c.Check("classify: not an access violation",
            ClassifyFault({EXCEPTION_ILLEGAL_INSTRUCTION, 0, 0, 0x1000, false, false}) == FaultKind::Ignore);

    auto* reserved = static_cast<u8*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE, PAGE_NOACCESS));
    auto* code = static_cast<u8*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    auto* data_ro = static_cast<u8*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READONLY));
    auto* data_rw = static_cast<u8*>(VirtualAlloc(nullptr, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (reserved && code && data_ro && data_rw) {
        RunLiveFaults(c, logs_dir, reserved, code, data_ro, data_rw);
    } else {
        c.Check("allocate test memory", false, fmt::format("error {}", GetLastError()));
    }
    for (u8* m : {reserved, code, data_ro, data_rw}) {
        if (m != nullptr) {
            VirtualFree(m, 0, MEM_RELEASE);
        }
    }

    const auto summary = fmt::format("fault test: {} checks, {} failed - {}", c.checks, c.failures,
                                     c.failures == 0 ? "PASS" : "FAIL");
    std::ofstream out(logs_dir / "faulttest.txt");
    for (const auto& line : c.lines) {
        out << line << '\n';
        std::printf("%s\n", line.c_str());
    }
    out << summary << '\n';
    std::printf("%s\n", summary.c_str());
    SSPC_LOG(App, Info, "{}", summary);
    Log::Flush();
    return c.failures == 0 ? 0 : 1;
}

} // namespace SecondSon::Base
