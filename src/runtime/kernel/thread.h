// SecondSonPC — native guest threads (Phase 2, P2.4; docs/PHASE2_P24_NATIVE_THREADS.md). With
// --native threads every thread function the game imports (group "threads": scePthreadCreate,
// Join, Detach, Exit, Self, Once, the attributes, priorities, affinity, TLS keys and their POSIX
// names) is SecondSonPC's, and the game's main thread is started here too. Without it the bridge's
// thread manager runs as before. The two are never mixed: a native thread has no bridge Pthread
// (g_curthread stays null on it), so BRIDGE functions that need one are refused with
// --native threads (src/app/main.cpp).
#pragma once

#include <functional>
#include <optional>
#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Kernel::Threads {

/// What the thread manager takes from outside (the bridge glue in the game, a test double in the
/// self-test). Called from the creating or the exiting thread, never under a manager lock.
class ThreadHost {
public:
    virtual ~ThreadHost() = default;
    /// A guest stack: `size` bytes of read/write memory with `guard` bytes of no-access memory
    /// right below it, as one area. Returns the base of the usable part, or nullptr. Stacks are
    /// never unmapped: the manager keeps a gone thread's stack for the next thread of the same size
    /// (as the bridge does; its memory manager cannot unmap a stack whose guard split the area, D17).
    virtual void* MapStack(u64 size, u64 guard) = 0;
    /// The thread control block (FS base) with the static TLS of a new thread; `thread` is what
    /// scePthreadSelf returns on it (stored in the TCB, like libkernel). `initial`: the main thread.
    virtual void* CreateTcb(void* thread, bool initial) = 0;
    virtual void DestroyTcb(void* tcb) = 0;
    /// The calling host thread's guest FS base (what guest `fs:` accesses read).
    virtual void SetTcbBase(void* tcb) = 0;
    /// [p, p + size) is readable guest memory (thread names come from the guest).
    virtual bool IsReadable(const void* p, u64 size) = 0;
    /// The calling thread starts / stops running guest code (the bridge's debug thread list).
    virtual void ThreadStarted() = 0;
    virtual void ThreadExiting() = 0;
};

void SetThreadHost(ThreadHost* host);

/// --native threads: chosen once at start-up, before any guest thread exists.
void Enable();
bool Enabled();

/// The game's main thread (the loader's MainThread body): a native guest thread with the default
/// attributes and the primary TLS block. Returns false if it could not be created.
bool StartMainThread(std::function<void()> body);

/// Scheduling priority of the calling native guest thread; nullopt on any other thread.
std::optional<s32> CurrentPriority();

std::span<const Runtime::Export> ThreadExports(); // group "threads" (experimental)

/// Self-test access.
namespace Test {
struct Counts {
    u64 slots_in_use;  // threads created and not yet reaped (running, exited, unjoined)
    u64 created;       // since start
    u64 reaped;        // resources freed (stack, TCB, host handle)
    u64 pending_reap;  // detached threads that exited, resources not freed yet
    u64 stacks_mapped; // guest stacks mapped by the manager (in use or kept for reuse)
    u64 stacks_cached; // ... of them kept for reuse
};
Counts GetCounts();
/// Frees what detached threads that are gone left behind (normally done by the next create).
void ReapDetached();
/// Waits up to `ms` for every detached thread to be reaped; false if some are still running.
bool WaitDetached(u32 ms);
} // namespace Test

} // namespace SecondSon::Kernel::Threads
