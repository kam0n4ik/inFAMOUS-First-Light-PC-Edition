// SecondSonPC — self-test of SecondSonPC's own virtual memory manager (Phase 2, P2.3.3b/c;
// docs/PHASE2_P233_NATIVE_VMM.md). Runs in --selftest next to the other groups (src/bridge/shadps4/selftest.cpp)
// on address windows above 1 TiB that nothing else uses; the game's memory stays the bridge's.
#pragma once

#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Bridge {

/// One group of checks, reported like the other self-test groups.
struct TestGroup {
    std::string name;
    int checks = 0;
    int failures = 0;
    std::vector<std::string> lines; ///< DIFF and INFO lines

    void Check(const std::string& what, bool ok, const std::string& detail);
    void Info(const std::string& text);
};

/// P2.3.3b: the manager on its own (host address space, areas, physical memory, defects of the bridge
/// fixed, a page model, threads, the commit experiment).
std::vector<TestGroup> TestNativeVirtualMemory(u32 sdk_version);

} // namespace SecondSon::Bridge
