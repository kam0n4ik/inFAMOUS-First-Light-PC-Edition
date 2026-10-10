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
/// self-test). Called from the creating, joining or exiting thread, never under a manager lock.
class ThreadHost {
public:
    virtual ~ThreadHost() = default;
    /// A guest stack: `size` bytes of read/write memory with `guard` bytes of no-access address
    /// space right below it. Returns the base of the usable part, or nullptr. Each of the two parts
    /// must be a whole mapping of its own, so that UnmapStack never asks the memory manager to split
    /// one (the bridge's asserts on that, D17).
    virtual void* MapStack(u64 size, u64 guard) = 0;
    /// Gives back what MapStack returned (its thread is gone, P3.2). false: the memory manager
    /// refused; the manager then forgets the stack (counted, never handed out again).
    virtual bool UnmapStack(void* stack, u64 size, u64 guard) = 0;
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

/// The calling native guest thread's id and name (fatal reports, P3.4): no lock, no allocation. false on
/// any other thread.
bool CurrentThreadInfo(u32* tid, const char** name);

std::span<const Runtime::Export> ThreadExports(); // group "threads" (experimental)

/// The stacks of gone threads kept for the next thread of the same size (P3.2): at most `count`
/// stacks and `bytes` bytes (usable part + guard); a stack that does not fit is unmapped, oldest
/// first. The defaults come from the game's own use (docs/PHASE3_INDEPENDENT_RUNTIME.md §10).
struct StackCacheLimits {
    u32 count;
    u64 bytes;
};
constexpr u32 kStackCacheCapacity = 64; ///< the most `count` may be
StackCacheLimits DefaultStackCacheLimits();

/// What the manager holds and has done (one consistent snapshot).
struct Stats {
    u64 slots_in_use;       ///< threads created and not yet reaped (running, exited, unjoined)
    u64 created;            ///< since start
    u64 reaped;             ///< resources freed (stack, TCB, host handle)
    u64 pending_reap;       ///< detached threads that exited, resources not freed yet
    u64 peak_threads;       ///< the most slots in use at once
    u64 stacks_mapped;      ///< guest stacks the manager holds now (in use or cached)
    u64 stack_bytes;        ///< ... their usable bytes (committed by the memory manager)
    u64 guard_bytes;        ///< ... their guards (address space only)
    u64 peak_stacks_mapped; ///< the most stacks held at once
    u64 peak_stack_bytes;   ///< the most usable + guard bytes held at once
    u64 stacks_cached;      ///< of stacks_mapped: kept for reuse
    u64 cache_bytes;        ///< ... usable + guard bytes
    u64 peak_cache_bytes;
    u64 map_calls;          ///< ThreadHost::MapStack that succeeded
    u64 unmap_calls;        ///< ThreadHost::UnmapStack that succeeded
    u64 unmap_failures;     ///< refused by the memory manager: the stack is forgotten (leaked)
    u64 reuses;             ///< stacks handed out again from the cache
    u64 evictions;          ///< stacks unmapped because the cache was full
    u64 rejected;           ///< stacks not handed out: overlap with a held one, or not what was held
};
Stats GetStats();
/// One line with GetStats() (the session's end).
void LogSummary();

/// Self-test access.
namespace Test {
/// Frees what detached threads that are gone left behind (normally done by the next create).
void ReapDetached();
/// Waits up to `ms` until every detached thread has ended and been reaped; false otherwise.
bool WaitDetached(u32 ms);
/// Sets the cache limits (count <= kStackCacheCapacity); returns the previous ones. Stacks over the
/// new limits are unmapped now.
StackCacheLimits SetStackCacheLimits(StackCacheLimits limits);
/// Unmaps every cached stack; returns how many.
u64 DrainStackCache();
/// The line per created / released thread on or off; returns the previous setting.
bool SetEventLog(bool on);
} // namespace Test

} // namespace SecondSon::Kernel::Threads
