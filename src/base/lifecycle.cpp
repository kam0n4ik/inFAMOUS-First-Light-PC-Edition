// SecondSonPC — the one owner of the process's end (P3.6; see lifecycle.h).
#include "base/lifecycle.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <windows.h>

#include "base/log.h"

namespace SecondSon::Lifecycle {

namespace {

struct Step {
    Stage stage;
    const char* name;
    void (*fn)();
};

constexpr u32 kMaxSteps = 32;
constexpr const char* kStageNames[] = {"reports", "services", "storage", "devices", "logs"};
static_assert(std::size(kStageNames) == static_cast<size_t>(Stage::Count));

Step g_steps[kMaxSteps];
std::atomic<u32> g_step_count{0};
std::atomic<State> g_state{State::Running};
std::atomic<DWORD> g_exiting_thread{0};
std::atomic<int> g_code{1}; // std::quick_exit does not tell its code: the watchdog then ends with 1
std::atomic<bool> g_steps_started{false};
std::atomic<bool> g_installed{false};
std::atomic<u32> g_watchdog_ms{30000};

[[noreturn]] void Park() {
    for (;;) {
        Sleep(INFINITE); // the thread ending the process (or the watchdog) takes this one with it
    }
}

DWORD WINAPI Watchdog(void*) {
    const u32 ms = g_watchdog_ms.load();
    Sleep(ms);
    // The steps hang (a step waits for a thread that never answers): end anyway. No lock: the hung step may
    // hold the log's.
    char line[200];
    const int code = g_code.load();
    const int n = std::snprintf(line, sizeof(line),
                                "[EXIT] the exit steps did not finish in %u ms: ending the process with code %d\n",
                                ms, code);
    if (n > 0) {
        Log::EmergencyWrite(line, static_cast<size_t>(n));
    }
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return 0;
}

void RunSteps(const char* reason) {
    if (g_steps_started.exchange(true)) {
        return; // Exit ran them already; this is its std::quick_exit
    }
    if (HANDLE watchdog = CreateThread(nullptr, 64 * 1024, &Watchdog, nullptr, 0, nullptr)) {
        CloseHandle(watchdog);
    }
    const u64 t0 = GetTickCount64();
    const u32 count = std::min(g_step_count.load(std::memory_order_acquire), kMaxSteps);
    SSPC_LOG(App, Info, "exit: {} (code {}): {} steps, watchdog {} ms", reason, g_code.load(), count,
             g_watchdog_ms.load());
    for (u32 stage = 0; stage < static_cast<u32>(Stage::Count); ++stage) {
        for (u32 i = 0; i < count; ++i) {
            const Step& step = g_steps[i];
            if (static_cast<u32>(step.stage) != stage) {
                continue;
            }
            if (g_state.load() == State::Fatal) {
                // A fatal exception on another thread: its path ends the process; no more steps.
                SSPC_LOG(App, Warning, "exit: a fatal exception ends the process; remaining exit steps skipped");
                Park();
            }
            SSPC_LOG(App, Info, "exit step ({}): {}", kStageNames[stage], step.name);
            step.fn();
        }
    }
    SSPC_LOG(App, Info, "exit steps done in {} ms", GetTickCount64() - t0);
}

void OnQuickExit() {
    State expected = State::Running;
    if (g_state.compare_exchange_strong(expected, State::Exiting)) {
        g_exiting_thread.store(GetCurrentThreadId());
        RunSteps("std::quick_exit");
        return;
    }
    if (expected == State::Fatal) {
        Park(); // ExitProcess now would end the fatal reporter in the middle of its report
    }
    if (g_exiting_thread.load() != GetCurrentThreadId()) {
        Park(); // another thread is running the exit
    }
    RunSteps("std::quick_exit"); // Exit's own std::quick_exit: the steps ran already
}

} // namespace

void AddExitStep(Stage stage, const char* name, void (*step)()) {
    const u32 index = g_step_count.load(std::memory_order_relaxed);
    if (index >= kMaxSteps || stage >= Stage::Count || step == nullptr) {
        SSPC_LOG(App, Error, "exit step {} not added (full or invalid)", name);
        return;
    }
    g_steps[index] = Step{stage, name, step};
    g_step_count.store(index + 1, std::memory_order_release);
}

void Install(u32 watchdog_ms) {
    g_watchdog_ms.store(watchdog_ms);
    if (!g_installed.exchange(true)) {
        std::at_quick_exit(&OnQuickExit);
    }
}

void Exit(int code, const char* reason) {
    State expected = State::Running;
    if (!g_state.compare_exchange_strong(expected, State::Exiting)) {
        if (expected == State::Exiting && g_exiting_thread.load() == GetCurrentThreadId()) {
            // From inside an exit step: the steps cannot run twice and this call cannot return.
            SSPC_LOG(App, Warning, "exit: {} (code {}) requested from inside an exit step; ending now with code {}",
                     reason, code, g_code.load());
            TerminateProcess(GetCurrentProcess(), static_cast<UINT>(g_code.load()));
        }
        SSPC_LOG(App, Info, "exit: {} (code {}) while the process is already {}; waiting for it to end", reason,
                 code, expected == State::Exiting ? "exiting" : "failing");
        Park();
    }
    g_exiting_thread.store(GetCurrentThreadId());
    g_code.store(code);
    RunSteps(reason);
    std::quick_exit(code); // the other at_quick_exit handlers (the bridge's log); ours sees the steps done
}

bool EnterFatal(const char* /*reason*/) {
    return g_state.exchange(State::Fatal) != State::Fatal;
}

State Current() {
    return g_state.load();
}

namespace Test {
u32 StepCount() {
    return g_step_count.load();
}
} // namespace Test

} // namespace SecondSon::Lifecycle
