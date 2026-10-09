// SecondSonPC — libkernel (Phase 2, P2.2). NATIVE groups: time — clocks, sleeps, TSC frequency —
// and sceKernelIsNeoMode ("kernel"); semaphores ("kernel-sema", docs/KERNEL_SYNC.md). The other
// synchronisation objects, threads and memory stay BRIDGE (docs/PHASE2_PLAN.md,
// docs/generated/KERNEL_AUDIT.md). P2.3.1: direct memory ("kernel-dmem", experimental, off unless
// --native kernel-dmem; docs/PHASE2_P23_MEMORY.md).
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Kernel {

/// What the native libkernel functions still need from the bridge.
class Host {
public:
    virtual ~Host() = default;
    /// The guest errno of the calling thread (one variable shared with every BRIDGE function).
    virtual s32* Errno() = 0;
    /// The bridge's process clock (CLOCK_PROCTIME): VideoOut/AudioOut timestamps the game compares
    /// with it are BRIDGE until Phase 4.
    virtual u64 ProcessTimeUs() = 0;
    /// Scheduling priority of the calling guest thread (lower value = more urgent): the wake order
    /// of priority-ordered waits. Threads are BRIDGE until P2.4.
    virtual s32 CurrentThreadPriority() = 0;
};
void SetHost(Host* host);
Host* GetHost();

/// neo: [Console] neoMode; title_supports_neo: param.sfo ATTRIBUTE bit 23.
void Configure(bool neo, bool title_supports_neo);
/// What sceKernelIsNeoMode tells the game (also decides the direct memory budget).
bool NeoMode();

/// Start-up: the TSC frequency is measured in the background (100 ms) from here on.
void Start();

std::span<const Runtime::Export> KernelExports();
std::span<const Runtime::Export> SemaphoreExports();
std::span<const Runtime::Export> EventFlagExports();
std::span<const Runtime::Export> KernelUtilExports(); // kernel_util.cpp, group "kernel-util"
std::span<const Runtime::Export> MutexExports();      // mutex.cpp, group "kernel-mutex"
std::span<const Runtime::Export> CondvarExports();    // condvar.cpp, group "kernel-mutex" (waits on mutexes)
std::span<const Runtime::Export> RwlockExports();     // rwlock.cpp, group "kernel-rwlock"
std::span<const Runtime::Export> DirectMemoryExports(); // memory.cpp, group "kernel-dmem" (experimental)

/// Self-test access to the native semaphores (functions the game does not import, and state).
namespace SemaphoreTest {
s32 Poll(u32 sem, s32 need);
s32 Cancel(u32 sem, s32 set_count, s32* waiter_count);
s32 Tokens(u32 sem);  // -1: no such semaphore
s32 Waiters(u32 sem); // -1: no such semaphore
} // namespace SemaphoreTest

/// Self-test access to the native event flags (functions the game does not import, and state).
namespace EventFlagTest {
s32 Poll(u64 ef, u64 bits, u32 wait_mode, u64* result);
s32 Clear(u64 ef, u64 pattern);
s32 Cancel(u64 ef, u64 pattern, s32* num_waiters);
s64 Bits(u64 ef);    // -1: no such event flag
s32 Waiters(u64 ef); // -1: no such event flag
} // namespace EventFlagTest

} // namespace SecondSon::Kernel
