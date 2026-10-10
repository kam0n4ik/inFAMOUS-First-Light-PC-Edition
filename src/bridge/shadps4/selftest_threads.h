// SecondSonPC — self-test of SecondSonPC's guest thread manager (Phase 2, P2.4;
// docs/PHASE2_P24_NATIVE_THREADS.md). The manager runs on a test host: guest stacks in host memory
// (with a no-access guard) or in the selected guest memory manager (`guest_stacks`: the bridge's in
// --selftest, SecondSonPC's in --selftest --native memory), TCBs of its own. "Guest" code is
// System V functions of the test that call the manager's exports as the game does.
#pragma once

#include <vector>

#include "bridge/shadps4/selftest_vmm.h"

namespace SecondSon::App {
struct Config;
}

namespace SecondSon::Bridge {

std::vector<TestGroup> TestNativeThreads(bool guest_stacks);

/// P3.3 (src/bridge/shadps4/selftest_fs.cpp): SecondSonPC's TCB slot and FS patcher.
std::vector<TestGroup> TestNativeFs(const App::Config& cfg);

} // namespace SecondSon::Bridge
