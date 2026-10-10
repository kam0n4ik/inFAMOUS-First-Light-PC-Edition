// SecondSonPC — the guest errno (Phase 3, P3.5.1, EXPERIMENTAL: --native errno; docs/PHASE3_P35_KERNEL_HLE.md §2).
//
// libkernel's `int* __error(void)` gives the calling thread's errno cell; libc.prx, libSceFios2.prx and the game call
// it each time they read or set errno (~18 000 calls in a 98 s run) and never keep the pointer across threads. On the
// console the cell lives in the thread's own structure (zeroed when the thread is created, valid until it ends).
//
// SecondSonPC's cell is a static thread-local of the executable: one per host thread. Every guest thread of either
// manager (SecondSonPC's: Kernel::Threads, CreateThread per thread; the bridge's: Core::NativeThread, CreateThread per
// thread; the main thread: a host thread of its own) is exactly one host thread, created for it and ended with it, so
// the cell is per guest thread, 0 when the thread starts, at one address from its first guest instruction until after
// its TLS destructors and ExitThread — longer than any guest code of that thread can run. No allocation, no lock.
// Host code keeps its own errno (the CRT's); only the guest's functions write this one.
//
// One owner at a time: by default the bridge's (its thread-local, reached through Kernel::Host::Errno); with
// --native errno SecondSonPC's — the game's __error is this module's export and the bridge's __Error() returns the
// same cell (patch 0023, Libraries::Kernel::UseExternalErrno), so BRIDGE and NATIVE functions write one variable.
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Kernel::GuestErrno {

/// SecondSonPC's cell of the calling thread (whether or not it is the guest errno in this run).
s32* OwnLocation();

/// --native errno: SecondSonPC's cell is now the guest errno (the caller has pointed the bridge at OwnLocation).
/// Once, at start-up, before guest code.
void Enable();
bool Enabled();

/// Group "errno" (experimental): __error.
std::span<const Runtime::Export> Exports();

} // namespace SecondSon::Kernel::GuestErrno
