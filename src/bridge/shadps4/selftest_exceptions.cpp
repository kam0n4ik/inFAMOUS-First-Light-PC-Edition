// SecondSonPC — self-test of SecondSonPC's exception dispatcher (Phase 3, P3.4;
// docs/PHASE3_P34_EXCEPTIONS_CPU.md). Groups:
//   NativeExceptions        in this process, with the dispatcher installed (--selftest --native exceptions):
//                           a GPU-tracking test double as the bridge's handler; delegation and counters,
//                           CPU context (general registers, xmm, flags) across a resolved exception, host
//                           exceptions passed on (C++, SEH access violation, int3, RaiseException,
//                           OutputDebugString), concurrent faults from 8 threads, the hot path's time;
//   NativeExc(red zone)     what the kernel's exception frame does to the 128 bytes below rsp, measured in
//                           memory with the AVX state clean and in use, against the dispatcher's counters;
//   NativeExc(fatal)        child processes (--exceptions-child <case> <dir>), each with its own dispatcher:
//                           a guest access violation, ud2, int3, a fault inside an owner's handler, two
//                           threads failing at once, a reporter that never finishes, host
//                           exceptions that must survive; exit code = the exception code, one report with
//                           module + offset, no hang.
// "Guest code" here is a block of stubs in this executable registered as a guest range.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include "app/config.h"
#include "base/log.h"
#include "bridge/shadps4/loader_host.h"
#include "bridge/shadps4/selftest_vmm.h"
#include "runtime/cpu/exceptions.h"

// ------------------------------------------------------------------------------------- the stubs
// Microsoft x64 calling convention (called from this file): rcx, rdx, r8; rax returned. They only
// pretend to be guest code: their range — sections .text$sspcxt_a..c, which the linker places in name
// order — is registered with the dispatcher.
#define SSPC_XT_STUB(sec) extern "C" __attribute__((naked, noinline, section(sec)))

SSPC_XT_STUB(".text$sspcxt_a") void sspc_xt_begin() {
    asm volatile("int3\n");
}

// a write that faults on a protected page
SSPC_XT_STUB(".text$sspcxt_b") SecondSon::u64 sspc_xt_store(SecondSon::u8*) {
    asm volatile("movb $1, (%rcx)\n"
                 "ret\n");
}

// a read (of unmapped memory in the fatal cases)
SSPC_XT_STUB(".text$sspcxt_b") SecondSon::u64 sspc_xt_load(SecondSon::u64*) {
    asm volatile("movq (%rcx), %rax\n"
                 "ret\n");
}

SSPC_XT_STUB(".text$sspcxt_b") void sspc_xt_ud2() {
    asm volatile("ud2\n"
                 "ret\n");
}

SSPC_XT_STUB(".text$sspcxt_b") void sspc_xt_int3() {
    asm volatile("int3\n"
                 "ret\n");
}

// (target, seed, dirty_avx): fills [rsp-1024, rsp) with seed ^ index, leaves the AVX state in use (ymm0/ymm1
// upper halves) or clean (vzeroupper), writes to target (resolved by the tracking double), returns the intact
// bytes counted from rsp-8 down to the first changed qword.
SSPC_XT_STUB(".text$sspcxt_b") SecondSon::u64 sspc_xt_red_zone(SecondSon::u8*, SecondSon::u64, SecondSon::u64) {
    asm volatile("xorl %eax, %eax\n"
                 "1: movq %rdx, %r9\n"
                 "xorq %rax, %r9\n"
                 "movq %r9, -1024(%rsp,%rax,8)\n"
                 "incq %rax\n"
                 "cmpq $128, %rax\n"
                 "jb 1b\n"
                 "testq %r8, %r8\n"
                 "jz 2f\n"
                 "vpcmpeqd %ymm0, %ymm0, %ymm0\n"
                 "vpcmpeqd %ymm1, %ymm1, %ymm1\n"
                 "jmp 3f\n"
                 "2: vzeroupper\n"
                 "3: movb $1, (%rcx)\n"
                 "movl $127, %eax\n"
                 "xorl %r10d, %r10d\n"
                 "4: movq %rdx, %r9\n"
                 "xorq %rax, %r9\n"
                 "cmpq -1024(%rsp,%rax,8), %r9\n"
                 "jne 5f\n"
                 "addq $8, %r10\n"
                 "decq %rax\n"
                 "jns 4b\n"
                 "5: vzeroupper\n"
                 "movq %r10, %rax\n"
                 "ret\n");
}

// (target, out): known values in every general register (but rsp, rcx, r11), xmm0-xmm5 and CF, a write to
// target (resolved), then everything stored to out[0..19].
SSPC_XT_STUB(".text$sspcxt_b") void sspc_xt_context(SecondSon::u8*, SecondSon::u64*) {
    asm volatile("pushq %rbx\n"
                 "pushq %rbp\n"
                 "pushq %rsi\n"
                 "pushq %rdi\n"
                 "pushq %r12\n"
                 "pushq %r13\n"
                 "pushq %r14\n"
                 "pushq %r15\n"
                 "movq %rdx, %r11\n"
                 "movabsq $0x1111111111111111, %rax\n"
                 "movq %rax, %xmm0\n"
                 "movabsq $0x2222222222222222, %rax\n"
                 "movq %rax, %xmm1\n"
                 "movabsq $0x3333333333333333, %rax\n"
                 "movq %rax, %xmm2\n"
                 "movabsq $0x4444444444444444, %rax\n"
                 "movq %rax, %xmm3\n"
                 "movabsq $0x5555555555555555, %rax\n"
                 "movq %rax, %xmm4\n"
                 "movabsq $0x6666666666666666, %rax\n"
                 "movq %rax, %xmm5\n"
                 "movabsq $0xA0A0A0A0A0A0A0A0, %rax\n"
                 "movabsq $0xA1A1A1A1A1A1A1A1, %rbx\n"
                 "movabsq $0xA2A2A2A2A2A2A2A2, %rdx\n"
                 "movabsq $0xA3A3A3A3A3A3A3A3, %rbp\n"
                 "movabsq $0xA4A4A4A4A4A4A4A4, %rsi\n"
                 "movabsq $0xA5A5A5A5A5A5A5A5, %rdi\n"
                 "movabsq $0xA6A6A6A6A6A6A6A6, %r8\n"
                 "movabsq $0xA7A7A7A7A7A7A7A7, %r9\n"
                 "movabsq $0xA8A8A8A8A8A8A8A8, %r10\n"
                 "movabsq $0xA9A9A9A9A9A9A9A9, %r12\n"
                 "movabsq $0xAAAAAAAAAAAAAAAA, %r13\n"
                 "movabsq $0xABABABABABABABAB, %r14\n"
                 "movabsq $0xACACACACACACACAC, %r15\n"
                 "stc\n"
                 "movb $1, (%rcx)\n"
                 "pushfq\n"
                 "popq 0x68(%r11)\n"
                 "movq %rax, 0x00(%r11)\n"
                 "movq %rbx, 0x08(%r11)\n"
                 "movq %rdx, 0x10(%r11)\n"
                 "movq %rbp, 0x18(%r11)\n"
                 "movq %rsi, 0x20(%r11)\n"
                 "movq %rdi, 0x28(%r11)\n"
                 "movq %r8,  0x30(%r11)\n"
                 "movq %r9,  0x38(%r11)\n"
                 "movq %r10, 0x40(%r11)\n"
                 "movq %r12, 0x48(%r11)\n"
                 "movq %r13, 0x50(%r11)\n"
                 "movq %r14, 0x58(%r11)\n"
                 "movq %r15, 0x60(%r11)\n"
                 "movq %xmm0, 0x70(%r11)\n"
                 "movq %xmm1, 0x78(%r11)\n"
                 "movq %xmm2, 0x80(%r11)\n"
                 "movq %xmm3, 0x88(%r11)\n"
                 "movq %xmm4, 0x90(%r11)\n"
                 "movq %xmm5, 0x98(%r11)\n"
                 "popq %r15\n"
                 "popq %r14\n"
                 "popq %r13\n"
                 "popq %r12\n"
                 "popq %rdi\n"
                 "popq %rsi\n"
                 "popq %rbp\n"
                 "popq %rbx\n"
                 "ret\n");
}

SSPC_XT_STUB(".text$sspcxt_c") void sspc_xt_end() {
    asm volatile("int3\n");
}

namespace SecondSon::Bridge {

namespace {

namespace X = Cpu::Exceptions;

void RegisterStubs() {
    static bool done = false;
    if (!done) {
        X::RegisterGuestCode(reinterpret_cast<u64>(&sspc_xt_begin), reinterpret_cast<u64>(&sspc_xt_end) + 16,
                             "selftest guest");
        done = true;
    }
}

// ---- the GPU write tracking test double: pages it watches become writable on a write fault

constexpr u32 kPages = 16;
u8* g_pages = nullptr; // kPages pages, read-only while "tracked"
std::atomic<u64> g_tracked{0};

bool TrackingDouble(_EXCEPTION_POINTERS* info, u64 address) {
    const auto* rec = info->ExceptionRecord;
    const u64 base = reinterpret_cast<u64>(g_pages);
    if (g_pages == nullptr || rec->ExceptionInformation[0] != 1 || address - base >= kPages * 0x1000ull) {
        return false;
    }
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(address & ~0xFFFull), 0x1000, PAGE_READWRITE, &old);
    g_tracked.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void Track(u32 page) {
    DWORD old = 0;
    VirtualProtect(g_pages + page * 0x1000ull, 0x1000, PAGE_READONLY, &old);
}

using HostFn = void (*)();

__declspec(noinline) bool SehCatches(HostFn fn) {
    __try {
        fn();
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

volatile u64 g_null_target = 0x10;

__declspec(noinline) void HostAccessViolation() {
    *reinterpret_cast<volatile u64*>(g_null_target) = 1;
}
__declspec(noinline) void HostBreakpoint() {
    __debugbreak();
}
__declspec(noinline) void HostRaise() {
    RaiseException(0xE0535350, 0, 0, nullptr);
}

void TestInProcess(TestGroup& g) {
    using X::Owner;
    const auto previous = X::Test::SetBridgeHandlers({&TrackingDouble, nullptr, nullptr});
    X::Test::ResetStats();

    // 1. A tracked write from guest code: the bridge's handler resolves it, the dispatcher counts it.
    Track(0);
    sspc_xt_store(g_pages);
    auto s = X::GetStats();
    g.Check("a tracked write from guest code is resolved by the bridge's handler",
            g_pages[0] == 1 && s.av_by[static_cast<u32>(Owner::Bridge)] == 1 && s.guest_resolved == 1,
            fmt::format("bridge {}, guest {}", s.av_by[static_cast<u32>(Owner::Bridge)], s.guest_resolved));

    // 2. Every register, xmm0-5 and the carry flag survive a resolved exception.
    Track(1);
    u64 out[20]{};
    sspc_xt_context(g_pages + 0x1000, out);
    const u64 want[13] = {0xA0A0A0A0A0A0A0A0, 0xA1A1A1A1A1A1A1A1, 0xA2A2A2A2A2A2A2A2, 0xA3A3A3A3A3A3A3A3,
                          0xA4A4A4A4A4A4A4A4, 0xA5A5A5A5A5A5A5A5, 0xA6A6A6A6A6A6A6A6, 0xA7A7A7A7A7A7A7A7,
                          0xA8A8A8A8A8A8A8A8, 0xA9A9A9A9A9A9A9A9, 0xAAAAAAAAAAAAAAAA, 0xABABABABABABABAB,
                          0xACACACACACACACAC};
    bool regs = std::equal(std::begin(want), std::end(want), out);
    for (u32 i = 0; i < 6; ++i) {
        regs = regs && out[14 + i] == 0x1111111111111111ull * (i + 1);
    }
    g.Check("general registers, xmm0-xmm5 and CF are unchanged across the resolved exception",
            regs && (out[13] & 1) == 1, fmt::format("rflags {:#x}, rax {:#x}, xmm5 {:#x}", out[13], out[0], out[19]));

    // 3. Host exceptions pass through to their own handlers.
    X::Test::ResetStats();
    bool cpp = false;
    try {
        throw std::runtime_error("selftest");
    } catch (const std::runtime_error&) {
        cpp = true;
    }
    g.Check("a C++ exception in host code reaches its catch", cpp, "");
    g.Check("an access violation in host code reaches its SEH handler", SehCatches(&HostAccessViolation), "");
    g.Check("a breakpoint in host code reaches its SEH handler", SehCatches(&HostBreakpoint), "");
    g.Check("RaiseException in host code reaches its SEH handler", SehCatches(&HostRaise), "");
    OutputDebugStringA("SecondSonPC self-test: OutputDebugString under the dispatcher\n");
    g.Check("OutputDebugString returns", true, "");
    s = X::GetStats();
    g.Check("host exceptions are passed on, none fatal",
            s.av_by[static_cast<u32>(Owner::PassedOn)] == 1 && s.traps_by[static_cast<u32>(Owner::PassedOn)] == 1 &&
                s.av_by[static_cast<u32>(Owner::Fatal)] == 0 && s.passed_other >= 2,
            fmt::format("av passed {}, traps passed {}, other passed {}", s.av_by[static_cast<u32>(Owner::PassedOn)],
                        s.traps_by[static_cast<u32>(Owner::PassedOn)], s.passed_other));

    // 4. 8 threads x 2000 rounds at the same time, pages of their own; each round faults twice: a write
    //    from host code (the lambda) and one from the guest stub.
    X::Test::ResetStats();
    const u64 before = g_tracked.load();
    std::atomic<u32> bad{0};
    std::vector<std::thread> threads;
    constexpr u32 kThreads = 8, kRounds = 2000;
    for (u32 t = 0; t < kThreads; ++t) {
        threads.emplace_back([t, &bad] {
            for (u32 i = 0; i < kRounds; ++i) {
                Track(2 + t);
                g_pages[(2 + t) * 0x1000ull] = 0;
                Track(2 + t);
                sspc_xt_store(g_pages + (2 + t) * 0x1000ull);
                if (g_pages[(2 + t) * 0x1000ull] != 1) {
                    bad.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    s = X::GetStats();
    g.Check("8 threads x 2000 rounds of tracked writes at once: all resolved, every value written",
            bad.load() == 0 && g_tracked.load() - before == 2ull * kThreads * kRounds &&
                s.av_by[static_cast<u32>(Owner::Bridge)] == 2ull * kThreads * kRounds,
            fmt::format("bad {}, resolved {}", bad.load(), s.av_by[static_cast<u32>(Owner::Bridge)]));

    // 5. The hot path's time (dispatcher + the double, whose VirtualProtect is most of it).
    const double us = s.tsc_mhz > 0 ? 1.0 / s.tsc_mhz : 0.0;
    g.Info(fmt::format("INFO  hot path (dispatcher + test double incl. its VirtualProtect): P50 {:.2f} us, P95 {:.2f} "
                       "us, P99 {:.2f} us, max {:.1f} us over {} exceptions (TSC {:.0f} MHz)",
                       s.bridge_p50 * us, s.bridge_p95 * us, s.bridge_p99 * us, s.bridge_max * us,
                       s.av_by[static_cast<u32>(Owner::Bridge)], s.tsc_mhz));
    X::Test::SetBridgeHandlers(previous);
    X::Test::ResetStats();
}

void TestRedZone(TestGroup& g) {
    const auto previous = X::Test::SetBridgeHandlers({&TrackingDouble, nullptr, nullptr});
    struct Case {
        bool dirty;
        bool marked;
    };
    u64 intact_clean = 0, intact_dirty = 0;
    const u64 fn = reinterpret_cast<u64>(&sspc_xt_red_zone);
    for (const Case c : {Case{false, false}, Case{true, false}, Case{true, true}}) {
        X::Test::ResetStats();
        if (c.marked) {
            X::MarkRedZoneFunction(fn, fn + 128);
        }
        Track(12);
        const u64 intact = sspc_xt_red_zone(g_pages + 12 * 0x1000ull, 0x5A5A0000DEAD0000ull, c.dirty ? 1 : 0);
        const auto s = X::GetStats();
        (c.dirty ? intact_dirty : intact_clean) = intact;
        if (!c.dirty) {
            g.Check("AVX state clean: the 128 bytes below rsp survive the exception, not counted as overwritten",
                    intact >= 128 && s.xstate_over_red_zone == 0,
                    fmt::format("{} bytes intact, counted {}", intact, s.xstate_over_red_zone));
        } else if (!c.marked) {
            // Measured, not assumed: the counter must agree with what happened to memory.
            g.Check("AVX state in use: the counter says what the memory shows",
                    (intact < 128) == (s.xstate_over_red_zone == 1) && s.exposed == 0,
                    fmt::format("{} bytes intact, counted {}, exposed {}", intact, s.xstate_over_red_zone, s.exposed));
        } else {
            g.Check("... in a function marked as a red-zone user: counted as exposed, with its rip",
                    s.in_red_zone_function == 1 && s.exposed == s.xstate_over_red_zone &&
                        (s.exposed == 0 || s.first_exposed[0] - fn < 128),
                    fmt::format("in function {}, exposed {}, first {:#x}", s.in_red_zone_function, s.exposed,
                                s.first_exposed[0]));
        }
    }
    g.Info(fmt::format("INFO  red zone: {} bytes below rsp intact with the AVX state clean, {} with it in use "
                       "(the System V red zone is 128): {}",
                       intact_clean, intact_dirty,
                       intact_dirty < 128 ? "the kernel's AVX state area overwrites it on this system"
                                          : "not overwritten on this system"));
    X::Test::SetBridgeHandlers(previous);
    X::Test::ResetStats();
}

// ---- the fatal path in child processes

struct ChildResult {
    DWORD exit_code = 0;
    bool finished = false;
    std::string report; // logs\crash\fatal_*.txt (all of them)
    u32 reports = 0;
    std::string log;    // its SecondSonPC.log
    double seconds = 0;
};

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

ChildResult RunChild(const std::string& name, const std::filesystem::path& dir, DWORD timeout_ms = 60000) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --exceptions-child " + std::wstring(name.begin(), name.end()) +
                       L" \"" + dir.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    ChildResult r;
    const auto t0 = std::chrono::steady_clock::now();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return r;
    }
    r.finished = WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0;
    if (!r.finished) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    GetExitCodeProcess(pi.hProcess, &r.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    for (const auto& e : std::filesystem::directory_iterator(dir / "crash", ec)) {
        if (e.path().filename().string().starts_with("fatal_")) {
            r.report += ReadAll(e.path());
            ++r.reports;
        }
    }
    r.log = ReadAll(dir / "SecondSonPC.log");
    return r;
}

void TestFatal(TestGroup& g, const std::filesystem::path& logs_dir) {
    const auto base = logs_dir / "selftest_exceptions";
    struct Case {
        const char* name;
        DWORD code;
        const char* expect; // in the report
        const char* what;
    };
    const Case cases[] = {
        {"guest-av", EXCEPTION_ACCESS_VIOLATION, "read of 0x10", "a guest read of unmapped memory"},
        {"guest-ud2", EXCEPTION_ILLEGAL_INSTRUCTION, "illegal instruction", "a guest ud2"},
        {"guest-int3", EXCEPTION_BREAKPOINT, "breakpoint (int3) not placed by a patcher", "a guest int3"},
        {"nested", EXCEPTION_ACCESS_VIOLATION, "raised while handling", "a fault inside the bridge's handler"},
        {"two-threads", EXCEPTION_ACCESS_VIOLATION, "FATAL guest exception", "two threads failing at once"},
    };
    for (const auto& c : cases) {
        const auto r = RunChild(c.name, base / c.name);
        const bool module = r.report.find("selftest guest+0x") != std::string::npos;
        g.Check(fmt::format("fatal: {}: exit code {:#x}, one report with the reason and module + offset", c.what,
                            static_cast<u32>(c.code)),
                r.finished && r.exit_code == c.code && r.reports == 1 && r.report.find(c.expect) != std::string::npos &&
                    module && r.report.find("backends:") != std::string::npos &&
                    r.log.find("FATAL guest exception") != std::string::npos,
                fmt::format("finished {}, exit {:#x}, reports {}, module {}, {:.1f} s", r.finished, r.exit_code,
                            r.reports, module, r.seconds));
    }
    {
        const auto r = RunChild("reporter-hangs", base / "reporter-hangs");
        g.Check("fatal: the reporter never finishes: the faulting thread writes its own line and ends the process",
                r.finished && r.exit_code == EXCEPTION_ACCESS_VIOLATION &&
                    r.log.find("[FATAL] (the fatal reporter did not finish)") != std::string::npos && r.seconds < 30,
                fmt::format("finished {}, exit {:#x}, {:.1f} s", r.finished, r.exit_code, r.seconds));
    }
    {
        const auto r = RunChild("host-survives", base / "host-survives");
        g.Check("host exceptions under a child's dispatcher: passed on, the process exits normally",
                r.finished && r.exit_code == 0 && r.reports == 0,
                fmt::format("finished {}, exit {:#x}, reports {}", r.finished, r.exit_code, r.reports));
    }
}

// ---- the child side

bool NestedDouble(_EXCEPTION_POINTERS*, u64) {
    return sspc_xt_load(reinterpret_cast<u64*>(g_null_target)) != 0; // faults inside the handler
}

void BlockForever() {
    Sleep(INFINITE);
}

} // namespace

std::vector<TestGroup> TestNativeExceptions(const App::Config& cfg) {
    std::vector<TestGroup> out(3);
    out[0].name = "NativeExceptions";
    out[1].name = "NativeExc(red zone)";
    out[2].name = "NativeExc(fatal)";
    if (X::Installed()) {
        RegisterStubs();
        g_pages = static_cast<u8*>(VirtualAlloc(nullptr, kPages * 0x1000ull, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        TestInProcess(out[0]);
        TestRedZone(out[1]);
    } else {
        out[0].Info("INFO  the dispatcher is not installed in this run (--selftest --native exceptions runs these)");
        out[1].Info("INFO  the dispatcher is not installed in this run (--selftest --native exceptions runs these)");
    }
    TestFatal(out[2], cfg.logs_dir);
    return out;
}

int RunExceptionsChild(int argc, char** argv) {
    if (argc < 4) {
        return 2;
    }
    const std::string name = argv[2];
    const std::filesystem::path dir = argv[3];
    if (!Log::Init(dir / "SecondSonPC.log", Log::Level::Info)) {
        return 3;
    }
    static const std::string logs = dir.string();
    X::Options options;
    options.bridge.access_violation = name == "nested" ? &NestedDouble : &TrackingDouble;
    options.backends = "self-test child";
    options.logs_dir = logs.c_str();
    if (name == "reporter-hangs") {
        X::SetFatalHook(&BlockForever);
        X::Test::SetReportTimeout(3000);
    }
    if (!X::Install(options)) {
        return 4;
    }
    RegisterStubs();
    g_pages = static_cast<u8*>(VirtualAlloc(nullptr, kPages * 0x1000ull, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (name == "guest-av" || name == "reporter-hangs") {
        sspc_xt_load(reinterpret_cast<u64*>(g_null_target));
    } else if (name == "guest-ud2") {
        sspc_xt_ud2();
    } else if (name == "guest-int3") {
        sspc_xt_int3();
    } else if (name == "nested") {
        Track(0);
        sspc_xt_store(g_pages);
    } else if (name == "two-threads") {
        std::atomic<bool> go{false};
        const auto fail = [&go] {
            while (!go.load()) {
            }
            sspc_xt_load(reinterpret_cast<u64*>(g_null_target));
        };
        std::thread a(fail);
        std::thread b(fail);
        go.store(true);
        a.join();
        b.join();
    } else if (name == "host-survives") {
        bool ok = true;
        try {
            throw std::runtime_error("child");
        } catch (const std::runtime_error&) {
        }
        ok = ok && SehCatches(&HostAccessViolation) && SehCatches(&HostBreakpoint) && SehCatches(&HostRaise);
        OutputDebugStringA("child\n");
        Track(0);
        sspc_xt_store(g_pages);
        ok = ok && g_pages[0] == 1;
        X::LogSummary("child");
        Log::Flush();
        return ok ? 0 : 5;
    }
    return 6; // the case should not have returned
}

} // namespace SecondSon::Bridge
