// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — self-test of the process's end (Phase 3, P3.6; base/lifecycle.h, docs/PHASE3_P36_BRIDGE_ISOLATION.md
// §4). Every case runs in a child process (SecondSonPC.exe --lifecycle-child <case> <dir>) that ends itself; the
// parent checks its exit code, how long it took and its log: the steps ran once and in stage order, concurrent and
// nested exits, a hanging step (the watchdog), a fatal path first, threads blocked in the bridge's event queue and in
// the kernel, the bridge's own shutdown request (patch 0026), the guest's _exit (--native exit).
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include "base/lifecycle.h"
#include "base/log.h"
#include "bridge/shadps4/loader_host.h"
#include "bridge/shadps4/selftest_threads.h"
#include "common/singleton.h"
#include "core/libraries/kernel/equeue.h"
#include "emulator.h"
#include "runtime/exports.h"

namespace Libraries::Kernel { // BRIDGE exports (declared as in generated/bridge_exports.cpp)
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name);
int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo);
} // namespace Libraries::Kernel

namespace SecondSon::Bridge {

namespace {

using Lifecycle::Stage;

// ---- the child side

void StepReports() {
    SSPC_LOG(App, Info, "test step: reports");
}
void StepServices() {
    SSPC_LOG(App, Info, "test step: services");
}
void StepServicesNested() {
    SSPC_LOG(App, Info, "test step: services (calls Exit)");
    Lifecycle::Exit(5, "nested exit from a step");
}
void StepServicesHangs() {
    SSPC_LOG(App, Info, "test step: services (hangs)");
    Sleep(INFINITE);
}
void StepStorage() {
    SSPC_LOG(App, Info, "test step: storage");
}
void StepDevices() {
    SSPC_LOG(App, Info, "test step: devices");
}
void StepLogs() {
    SSPC_LOG(App, Info, "test step: logs");
}

void AddSteps(void (*services)()) {
    // Added out of order: the stages decide the order.
    Lifecycle::AddExitStep(Stage::Logs, "test logs", &StepLogs);
    Lifecycle::AddExitStep(Stage::Devices, "test devices", &StepDevices);
    Lifecycle::AddExitStep(Stage::Storage, "test storage", &StepStorage);
    Lifecycle::AddExitStep(Stage::Services, "test services", services);
    Lifecycle::AddExitStep(Stage::Reports, "test reports", &StepReports);
}

[[noreturn]] void ParkForever() {
    for (;;) {
        Sleep(INFINITE);
    }
}

// ---- the parent side

struct ChildResult {
    DWORD exit_code = 0xFFFFFFFF;
    bool finished = false;
    double seconds = 0;
    std::string log;
};

std::string ReadAll(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

ChildResult RunChild(const std::string& name, const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --lifecycle-child " + std::wstring(name.begin(), name.end()) +
                       L" \"" + dir.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    ChildResult r;
    const auto t0 = std::chrono::steady_clock::now();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        return r;
    }
    r.finished = WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0;
    if (!r.finished) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    r.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    GetExitCodeProcess(pi.hProcess, &r.exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.log = ReadAll(dir / "SecondSonPC.log");
    return r;
}

size_t Count(const std::string& text, std::string_view needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
}

/// The five test steps ran exactly once each, in stage order.
bool StepsInOrder(const std::string& log) {
    size_t at = 0;
    for (const char* step : {"test step: reports", "test step: services", "test step: storage", "test step: devices",
                             "test step: logs"}) {
        if (Count(log, step) != 1) {
            return false;
        }
        const size_t pos = log.find(step);
        if (pos < at) {
            return false;
        }
        at = pos;
    }
    return true;
}

std::string Brief(const ChildResult& r) {
    return fmt::format("finished {}, exit {:#x}, {:.1f} s", r.finished, r.exit_code, r.seconds);
}

void TestCases(TestGroup& g, const std::filesystem::path& base) {
    {
        const auto r = RunChild("exit", base / "exit");
        g.Check("Exit(7): code 7, the steps once each in stage order (added out of order), then the end",
                r.finished && r.exit_code == 7 && StepsInOrder(r.log) &&
                    r.log.find("exit: test exit (code 7)") != std::string::npos &&
                    Count(r.log, "exit steps done") == 1,
                Brief(r));
    }
    {
        const auto r = RunChild("quick-exit", base / "quick-exit");
        g.Check("std::quick_exit(9) by anyone: code 9, the same steps once", r.finished && r.exit_code == 9 &&
                                                                            StepsInOrder(r.log) &&
                                                                            r.log.find("exit: std::quick_exit") !=
                                                                                std::string::npos,
                Brief(r));
    }
    {
        const auto r = RunChild("concurrent", base / "concurrent");
        g.Check("16 threads call Exit at once: one wins (code 1..16), the steps run once, the others wait",
                r.finished && r.exit_code >= 1 && r.exit_code <= 16 && StepsInOrder(r.log) &&
                    Count(r.log, "exit steps done") == 1 && Count(r.log, "steps, watchdog") == 1 &&
                    r.log.find(fmt::format("exit: concurrent (code {}): ", r.exit_code)) != std::string::npos,
                fmt::format("{}, waiting {}", Brief(r), Count(r.log, "waiting for it to end")));
    }
    {
        const auto r = RunChild("nested", base / "nested");
        g.Check("Exit from inside an exit step: ends at once with the first exit's code, later steps skipped",
                r.finished && r.exit_code == 7 && r.log.find("requested from inside an exit step") != std::string::npos &&
                    Count(r.log, "test step: storage") == 0,
                Brief(r));
    }
    {
        const auto r = RunChild("step-hangs", base / "step-hangs");
        g.Check("a step that never returns: the watchdog ends the process with the code (1.5 s limit)",
                r.finished && r.exit_code == 7 && r.seconds < 15 &&
                    r.log.find("[EXIT] the exit steps did not finish in 1500 ms") != std::string::npos,
                Brief(r));
    }
    {
        const auto r = RunChild("fatal-first", base / "fatal-first");
        g.Check("a fatal path first: a later Exit waits, no step runs, the fatal path's code ends the process",
                r.finished && r.exit_code == 0x42 && Count(r.log, "test step:") == 0 &&
                    r.log.find("already failing") != std::string::npos,
                Brief(r));
    }
    {
        const auto r = RunChild("waiters", base / "waiters");
        g.Check("threads blocked forever (the bridge's event queue, a kernel wait): Exit(0) still ends the process",
                r.finished && r.exit_code == 0 && StepsInOrder(r.log) && r.seconds < 15, Brief(r));
    }
    {
        const auto r = RunChild("bridge-shutdown", base / "bridge-shutdown");
        g.Check("the bridge's Emulator::Shutdown (its asserts, its handler) twice: the hook flushes the logs once, "
                "starts no shutdown; the orderly exit afterwards runs normally",
                r.finished && r.exit_code == 0 && Count(r.log, "[BRIDGE] the bridge asked for its shutdown") == 1 &&
                    r.log.find("bridge requests 2, state running") != std::string::npos && StepsInOrder(r.log),
                Brief(r));
    }
    {
        const auto r = RunChild("guest-exit", base / "guest-exit");
        g.Check("the guest's _exit(42) (--native exit): the orderly exit, code 42",
                r.finished && r.exit_code == 42 && r.log.find("the guest called _exit(42)") != std::string::npos &&
                    StepsInOrder(r.log),
                Brief(r));
    }
}

} // namespace

std::vector<TestGroup> TestLifecycle(const std::filesystem::path& logs_dir) {
    std::vector<TestGroup> out(1);
    out[0].name = "Lifecycle";
    TestCases(out[0], logs_dir / "selftest_lifecycle");
    return out;
}

int RunLifecycleChild(int argc, char** argv) {
    if (argc < 4) {
        return 2;
    }
    const std::string name = argv[2];
    const std::filesystem::path dir = argv[3];
    if (!Log::Init(dir / "SecondSonPC.log", Log::Level::Info)) {
        return 3;
    }
    if (name == "exit") {
        AddSteps(&StepServices);
        Lifecycle::Install(10000);
        Lifecycle::Exit(7, "test exit");
    }
    if (name == "quick-exit") {
        AddSteps(&StepServices);
        Lifecycle::Install(10000);
        std::quick_exit(9);
    }
    if (name == "concurrent") {
        AddSteps(&StepServices);
        Lifecycle::Install(10000);
        std::atomic<bool> go{false};
        std::vector<std::thread> threads;
        for (int i = 0; i < 16; ++i) {
            threads.emplace_back([&go, i] {
                while (!go.load()) {
                }
                Lifecycle::Exit(i + 1, "concurrent");
            });
        }
        go.store(true);
        ParkForever(); // the winner ends the process
    }
    if (name == "nested") {
        AddSteps(&StepServicesNested);
        Lifecycle::Install(10000);
        Lifecycle::Exit(7, "test exit");
    }
    if (name == "step-hangs") {
        AddSteps(&StepServicesHangs);
        Lifecycle::Install(1500);
        Lifecycle::Exit(7, "test exit");
    }
    if (name == "fatal-first") {
        AddSteps(&StepServices);
        Lifecycle::Install(10000);
        Lifecycle::EnterFatal("test fatal"); // as the fatal reporter does; it then ends the process itself
        std::thread([] { Lifecycle::Exit(3, "an exit during a fatal report"); }).detach();
        Sleep(500);
        Log::Flush();
        TerminateProcess(GetCurrentProcess(), 0x42);
    }
    if (name == "waiters") {
        AddSteps(&StepServices);
        Lifecycle::Install(10000);
        Libraries::Kernel::OrbisKernelEqueue eq = 0;
        Libraries::Kernel::sceKernelCreateEqueue(&eq, "sspc_exit_waiter");
        std::thread([eq] {
            Libraries::Kernel::OrbisKernelEvent ev{};
            int out = 0;
            Libraries::Kernel::sceKernelWaitEqueue(eq, &ev, 1, &out, nullptr); // forever
        }).detach();
        HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::thread([never] { WaitForSingleObject(never, INFINITE); }).detach();
        Sleep(200);
        Lifecycle::Exit(0, "exit with blocked threads");
    }
    if (name == "bridge-shutdown") {
        AddSteps(&StepServices);
        Lifecycle::Install(10000);
        InstallBridgeShutdownHook();
        Common::Singleton<Core::Emulator>::Instance()->Shutdown(); // as an assert of the bridge does
        Common::Singleton<Core::Emulator>::Instance()->Shutdown();
        SSPC_LOG(App, Info, "bridge requests {}, state {}", BridgeShutdownRequests(),
                 Lifecycle::Current() == Lifecycle::State::Running ? "running" : "not running");
        Lifecycle::Exit(0, "after the bridge's shutdown requests");
    }
    if (name == "guest-exit") {
        AddSteps(&StepServices);
        Lifecycle::Install(10000);
        for (const auto& e : Runtime::Exports()) {
            if (std::string_view(e.name) == "_exit") {
                reinterpret_cast<void(SSPC_SYSV_ABI*)(s32)>(e.function)(42);
            }
        }
        return 6; // no _exit export
    }
    return 4; // unknown case
}

} // namespace SecondSon::Bridge
