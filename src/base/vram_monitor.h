// SecondSonPC — video memory budget/usage of this process, as the OS reports it (DXGI).
// Diagnostics for SSPC-001: the failing allocations use VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
// so the per-process budget is the number that matters.
#pragma once

#include "base/abi.h"

namespace SecondSon::Base {

/// Logs one sample every `interval_seconds` (0 = off) on a background thread.
void StartVramLog(u32 interval_seconds);

/// Logs one sample now. `allow_init = false` (crash handler): only if the DXGI adapter is
/// already open (--vram-log), never creates DXGI objects inside an exception filter.
void LogVramSample(const char* reason, bool allow_init = true);

} // namespace SecondSon::Base
