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

/// P2.3.3c: the bridge's interfaces over the manager (selftest_native.cpp): the same calls on the bridge and
/// on the manager compared step by step, the adapters under threads. *diff_transcript: the scripted
/// scenario's steps on both sides (logs/selftest_native_diff.txt).
std::vector<TestGroup> TestNativeMemoryAdapters(u32 sdk_version, std::string* diff_transcript);

/// A free stretch of the host's address space at or above 2 TiB (0: none); the self-test's managers live there.
u64 FindFreeNativeWindow(u64 size);

} // namespace SecondSon::Bridge
