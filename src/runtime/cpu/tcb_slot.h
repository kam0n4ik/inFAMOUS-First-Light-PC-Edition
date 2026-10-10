// SecondSonPC — the guest thread control block of the calling host thread (Phase 3, P3.3;
// docs/PHASE3_INDEPENDENT_RUNTIME.md). On the PS4 the FS base of a thread is its TCB; on Windows the
// FS base of 64-bit code is 0, so the guest's FS-relative instructions are rewritten (fs_patcher.h)
// to read the TCB from one Windows TLS slot, owned here. Every guest thread stores its TCB in it
// before running guest code — SecondSonPC's threads (--native threads) and, through the adapter of
// patch 0021, the bridge's threads too: one slot for every thread, whichever manager made it.
#pragma once

#include "base/abi.h"

namespace SecondSon::Cpu {

/// Allocates the slot (once; later calls return the same answer). Must run before any guest thread
/// exists and before anything generates code that reads the slot. false: no TLS slot left.
bool InitTcbSlot();

/// The slot index (valid after InitTcbSlot returned true). Below 64 it is one of the TEB's
/// TlsSlots (gs:[0x1480 + 8 * slot]); from 64 on an expansion slot (gs:[0x1780] -> [slot - 64]).
u32 TcbSlot();

/// The calling thread's guest TCB (nullptr on a thread that never ran guest code).
void SetCurrentTcb(void* tcb);
void* CurrentTcb();

} // namespace SecondSon::Cpu
