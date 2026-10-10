// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY: the bridge's side of SecondSonPC's process end (Phase 3, P3.6; base/lifecycle.h,
// docs/PHASE3_P36_BRIDGE_ISOLATION.md §4). The bridge still calls Core::Emulator::Shutdown from its asserts
// (an int3 follows) and from its vectored handler for an exception it could not resolve (the search goes on to
// SecondSonPC's filters). Its own shutdown stopped the save data backup, closed the shader cache, joined its
// play-time thread, wrote a play-time file and reset the controllers through an uninitialised pointer — a
// second, competing shutdown, run from inside exception handling. With patch 0026 it calls this hook instead:
// the logs are flushed, nothing is stopped or closed, the process ends on SecondSonPC's paths.
#include <atomic>

#include "base/log.h"
#include "bridge/shadps4/loader_host.h"
#include "common/logging/log.h"
#include "emulator.h"

namespace SecondSon::Bridge {

namespace {

std::atomic<u32> g_requests{0};

void OnBridgeShutdown() {
    if (g_requests.fetch_add(1) == 0) {
        // No allocation, no lock of SecondSonPC's log: this may run inside exception handling.
        static constexpr char kLine[] = "[BRIDGE] the bridge asked for its shutdown (an assert or an exception its "
                                        "handler left unresolved): logs flushed, nothing stopped\n";
        Log::EmergencyWrite(kLine, sizeof(kLine) - 1);
    }
    Common::Log::Flush();
}

} // namespace

void InstallBridgeShutdownHook() {
    Core::UseExternalShutdown(&OnBridgeShutdown);
}

u32 BridgeShutdownRequests() {
    return g_requests.load();
}

} // namespace SecondSon::Bridge
