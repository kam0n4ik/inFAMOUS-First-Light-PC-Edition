// SecondSonPC — what engine functions cost on the CPU in the running game (Native Engine Reconstruction, the choice
// of candidates; docs/NATIVE_ENGINE_RECONSTRUCTION.md §3). SSPC_NATIVE_ENGINE_MEASURE=1, off by default.
//
// Each listed function gets a signature-independent timing stub: the stub saves the argument registers, records the
// caller's return address and the time on a per-thread shadow stack, restores the registers and CALLS the function's
// clone with the caller's own stack arguments in place (the caller's return-address slot is popped and the stub's
// call pushes its own into the same slot), then pops the shadow stack, re-pushes the caller's return address and
// returns. Inclusive time per call and exclusive time (minus the measured functions it called) per function and
// frame are reported every 10 s; the stub's own cost is calibrated at install and reported next to it.
// Functions already hooked by another switch (CITY-OPT probes, a reconstruction) do not hash and are skipped.
#pragma once

#include <filesystem>

#include "base/abi.h"

namespace SecondSon::NativeEngine::Measure {

/// The loader's window: installs the stubs when SSPC_NATIVE_ENGINE_MEASURE=1 (or a comma list of eboot addresses).
bool InstallFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir);

/// For tests: wraps the function at `address` (`size` bytes, hash-checked by the caller) in a timing stub and
/// installs the jump. Returns the slot index, or -1.
int InstallSite(u64 address, u32 size, const char* name, std::string& error);

struct SiteTotals {
    const char* name;
    u64 address;
    u64 calls;
    u64 inclusive_ticks;
    u64 exclusive_ticks;
};
u32 SiteCount();
SiteTotals Totals(u32 site);

/// The stub's own cost per call (ticks), measured at the first install.
double StubOverheadTicks();

} // namespace SecondSon::NativeEngine::Measure
