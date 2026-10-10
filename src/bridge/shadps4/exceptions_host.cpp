// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY bridge glue of SecondSonPC's exception dispatcher (Phase 3, P3.4;
// docs/PHASE3_P34_EXCEPTIONS_CPU.md). With --native exceptions the bridge registers no vectored handler of
// its own (patch 0022); its handlers become the dispatcher's BridgeHandlers: the GPU write tracking and the
// SRT walker (Core::SignalDispatch::DispatchAccessViolation, timed for --track-stats as the bridge's own
// handler did) and, only while the bridge's CPU patcher owns the guest's code, its int3 protocol
// (Core::HandlePatchBreakpoint, patch 0016) and its illegal-instruction emulation. Also: guest thread
// descriptions of either thread manager for fatal reports, the bridge's log flush before the process ends.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include <windows.h>

#include "app/config.h"
#include "base/crash.h"
#include "base/log.h"
#include "bridge/shadps4/loader_host.h"
#include "bridge/shadps4/track_diag.h"
#include "common/logging/log.h"
#include "core/cpu_patches.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/signals.h"
#include "runtime/cpu/exceptions.h"
#include "runtime/kernel/signal.h"
#include "runtime/kernel/thread.h"

namespace SecondSon::Bridge {

namespace {

Core::SignalDispatch* g_signals = nullptr;
bool g_native_exceptions = false;
std::string g_backends;
std::string g_logs_dir;

bool BridgeAccessViolation(EXCEPTION_POINTERS* info, u64 address) {
    const u64 start = TrackDiag::Enabled() ? TrackDiag::Ticks() : 0;
    const bool handled = g_signals->DispatchAccessViolation(info, reinterpret_cast<void*>(address));
    if (start != 0) {
        TrackDiag::OnDispatch(start, handled);
    }
    return handled;
}

bool BridgePatchTrap(EXCEPTION_POINTERS* info, bool misreported) {
    return Core::HandlePatchBreakpoint(info->ContextRecord, info->ExceptionRecord->ExceptionAddress, misreported);
}

bool BridgeIllegalInstruction(EXCEPTION_POINTERS* info) {
    return g_signals->DispatchIllegalInstruction(info);
}

void DescribeThread(char* out, u64 size) {
    u32 tid = 0;
    const char* name = nullptr;
    if (Kernel::Threads::CurrentThreadInfo(&tid, &name)) {
        std::snprintf(out, size, "SecondSonPC guest thread %u \"%s\"", tid, name);
    } else if (const auto* t = Libraries::Kernel::g_curthread) {
        std::snprintf(out, size, "bridge guest thread %d \"%s\"", t->tid.load(std::memory_order_relaxed),
                      t->name.c_str());
    } else {
        std::snprintf(out, size, "no guest thread (host thread)");
    }
}

void OnBridgeRemoveHandlers() {
    // An assert of the bridge: its int3 must reach the unhandled exception filter (and the bridge's
    // termination), not loop through the dispatcher.
    Cpu::Exceptions::PassEverything();
}

void FlushBridgeLog() {
    Common::Log::Flush();
}

void LogExceptionStats(const char* reason) {
    Cpu::Exceptions::LogSummary(reason);
}

bool Listed(const std::vector<std::string>& list, std::string_view value) {
    return std::find(list.begin(), list.end(), value) != list.end();
}

} // namespace

bool InstallExceptionRuntime(const App::Config& cfg, bool native_cpu) {
    // Before the bridge's dispatcher exists: it must not register its own handler.
    Core::UseExternalExceptionDispatch(&OnBridgeRemoveHandlers);
    g_signals = Core::Signals::Instance();
    const auto& n = cfg.native_groups;
    g_backends = fmt::format("exceptions SecondSonPC, CPU patcher {}, memory {}{}, threads {}, loader {}",
                             native_cpu ? "SecondSonPC" : "bridge", Listed(n, "memory") ? "SecondSonPC" : "bridge",
                             cfg.memory_commit_on_demand ? " (commit on demand)" : "",
                             Listed(n, "threads") ? "SecondSonPC" : "bridge",
                             cfg.loader == App::Config::LoaderBackend::Sspc ? "SecondSonPC" : "shadps4");
    g_logs_dir = cfg.logs_dir.string();
    Cpu::Exceptions::Options options;
    options.bridge.access_violation = &BridgeAccessViolation;
    if (!native_cpu) {
        options.bridge.patch_trap = &BridgePatchTrap;
        options.bridge.illegal_instruction = &BridgeIllegalInstruction;
    }
    options.cpu_patcher = native_cpu;
    options.write_minidump = cfg.crash_dump;
    options.backends = g_backends.c_str();
    options.logs_dir = g_logs_dir.c_str();
    Cpu::Exceptions::SetThreadDescriber(&DescribeThread);
    Cpu::Exceptions::SetFatalHook(&FlushBridgeLog);
    if (!Cpu::Exceptions::Install(options)) {
        return false;
    }
    Kernel::Signals::Install(); // guest signals (group "exceptions") are delivered by the dispatcher
    Base::SetFirstChanceStatsHook(&LogExceptionStats);
    g_native_exceptions = true;
    SSPC_LOG(Cpu, Warning, "EXPERIMENTAL: SecondSonPC's exception dispatcher ({})", g_backends);
    return true;
}

bool NativeExceptions() {
    return g_native_exceptions;
}

} // namespace SecondSon::Bridge
