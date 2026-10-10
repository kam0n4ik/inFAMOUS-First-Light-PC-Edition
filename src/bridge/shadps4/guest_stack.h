// SecondSonPC — guest stacks of the native thread manager in the selected guest memory manager (P3.2;
// docs/PHASE3_INDEPENDENT_RUNTIME.md §10). The game's ThreadHost (kernel_host.cpp) and the self-test's
// (selftest_threads.cpp) map and unmap stacks only through these two functions.
//
// Layout: [base, base + guard) a Reserved range (no host pages: any access faults), then
// [base + guard, base + guard + size) a read/write Stack mapping over it (MAP_FIXED). Two mappings, so that
// each VMA is exactly one host allocation: unmapping the whole stack never asks the memory manager to split
// one. The bridge's manager asserts when it has to split anonymous memory (D17 = SSPC-006: a guard made with
// mprotect inside one mapping cuts its VMA in two over one host allocation); SecondSonPC's refuses it. With
// this layout both unmap a stack within their contract.
#pragma once

#include "common/types.h"

namespace SecondSon::Bridge {

/// The guard and the stack, the search for free space starting at `hint` (going up). Returns the base of
/// the usable part (base + guard), or nullptr (nothing left mapped).
void* MapGuestStack(VAddr hint, u64 size, u64 guard);

/// Unmaps what MapGuestStack returned, guard included, in one call. false: the memory manager refused.
bool UnmapGuestStack(void* stack, u64 size, u64 guard);

} // namespace SecondSon::Bridge
