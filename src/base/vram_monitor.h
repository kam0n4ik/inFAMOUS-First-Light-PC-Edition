// SecondSonPC — video memory budget/usage of this process, as the OS reports it (DXGI).
// Diagnostics for SSPC-001 (docs\KNOWN_ISSUES.md): the OS view of the whole process; the per-heap
// view of the GPU bridge (VMA, driver budget, allocation ledger) is logged by the sample hook.
#pragma once

#include "base/abi.h"

namespace SecondSon::Base {

/// Logs one sample every `interval_seconds` (0 = off) on a background thread.
void StartVramLog(u32 interval_seconds);

/// Logs one sample now. `allow_init = false` (crash handler): the DXGI line only if the adapter
/// is already open (--vram-log), never creates DXGI objects inside an exception filter. The sample
/// hook, if set, runs in both cases.
void LogVramSample(const char* reason, bool allow_init = true);

/// The process's video memory as the OS counts it (DXGI; every allocation of the process, the driver's included).
struct VideoMemoryInfo {
    u64 local_usage = 0, local_budget = 0, nonlocal_usage = 0, nonlocal_budget = 0; ///< bytes
};
/// Queries it without logging (the Performance Inspector, once a second). false: no adapter.
bool QueryVideoMemory(VideoMemoryInfo& out);

/// Called after every sample with the same reason ("sample", "crash", ...); set by the GPU bridge.
using VramSampleHook = void (*)(const char* reason);
void SetVramSampleHook(VramSampleHook hook);

} // namespace SecondSon::Base
