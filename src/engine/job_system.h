// SecondSonPC — the engine's job queue, reconstructed (docs/native-reconstruction/JOB_SYSTEM_RECONSTRUCTION.md).
//
// inFAMOUS Second Son runs its general jobs (JobWorker1..5, eboot thread table 0xae0f70 entries 11..15) from one
// global FIFO of job batches owned by a JobManager object (eboot 0x1d1bfe0) and guarded by one pthread mutex and
// condition variable. Three of its functions are reconstructed here and can replace the guest ones
// (SSPC_NATIVE_JOBS=1; off by default):
//   eboot 0x243f30  NextJob     a worker takes the next entry, sleeping on the condition variable while empty
//   eboot 0x243d70  TryNextJob  the same without sleeping (nullptr when the queue is empty)
//   eboot 0x243c50  WaitBatch   waits for every entry of a batch, running queued entries (any batch's) meanwhile
// They work on the guest's own data (the same layout, the guest's own mutex and condition variable through its PLT
// stubs), so the guest functions that are not replaced (submit 0x243a40, the workers 0x243680, dependency kicks
// 0x2446b0/0x244770, 0x244100) keep working with them. The behaviour is equivalent to the guest code instruction by
// instruction (reconstructed from the disassembly), except where an Options field says otherwise; the replacement
// also counts what the queue does (Stats).
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::Jobs {

/// One job of a batch (24 bytes). state: 0 queued, 1 taken, 2 done.
struct GuestEntry {
    u32 state;
    u32 reserved;
    u64 fn; ///< SSPC_SYSV_ABI void (*)(u64 arg)
    u64 arg;
};
static_assert(sizeof(GuestEntry) == 24);

/// A batch: header + `count` entries. state: 0 empty, 2 submitted, 3 every entry taken, 4 waited for.
struct GuestBatch {
    u32 state;
    s32 count;
    GuestBatch* next;
    GuestBatch* prev;
    GuestEntry* Entries() {
        return reinterpret_cast<GuestEntry*>(this + 1);
    }
};
static_assert(sizeof(GuestBatch) == 24);

/// The engine's iterators over intrusive lists register here (eboot 0x1d1c290 / 0x1d1c298): `slot` points at the
/// link field the iterator reads next; unlinking a node moves the iterators that point into it.
struct GuestIterator {
    void* slot;
    GuestIterator* next;
};

using InitFn = s32(SSPC_SYSV_ABI*)(void* object, const void* attr, const char* name);
using LockFn = s32(SSPC_SYSV_ABI*)(void* object);
using CondWaitFn = s32(SSPC_SYSV_ABI*)(void* cond, void* mutex);
using DelayFn = void(SSPC_SYSV_ABI*)(u32 microseconds);
using JobFn = void(SSPC_SYSV_ABI*)(u64 arg);

/// Where the guest's queue and primitives are (the game: eboot addresses; the tests: host objects).
struct Environment {
    u8* manager = nullptr;                    ///< the JobManager (eboot 0x1d1bfe0)
    GuestIterator** next_iterators = nullptr; ///< eboot 0x1d1c290
    GuestIterator** prev_iterators = nullptr; ///< eboot 0x1d1c298
    InitFn mutex_init = nullptr;              ///< scePthreadMutexInit (PLT 0x9845b0)
    InitFn cond_init = nullptr;               ///< scePthreadCondInit (PLT 0x984590)
    LockFn mutex_lock = nullptr;              ///< scePthreadMutexLock (PLT 0x9845c0)
    LockFn mutex_unlock = nullptr;            ///< scePthreadMutexUnlock (PLT 0x9845d0)
    CondWaitFn cond_wait = nullptr;           ///< scePthreadCondWait (PLT 0x984d10)
    DelayFn delay_us = nullptr;               ///< the engine's delay(us): rdtsc spin up to 500 us (eboot 0x53b2b0)
};

/// JobManager fields (offsets from Environment::manager).
namespace Field {
constexpr size_t kQuit = 0x260;   ///< u8: the workers stop
constexpr size_t kMutex = 0x268;  ///< ScePthreadMutex
constexpr size_t kCond = 0x270;   ///< ScePthreadCond
constexpr size_t kInit = 0x278;   ///< u8: mutex and condition variable initialised (lazily, by whoever comes first)
constexpr size_t kHead = 0x280;   ///< GuestBatch*: the oldest submitted batch with entries not yet taken
constexpr size_t kTail = 0x288;   ///< GuestBatch*
constexpr size_t kCursor = 0x290; ///< s32: the next entry of the head batch
constexpr size_t kSize = 0x2c0;   ///< at least this much of the object is the queue's (tests allocate it)
} // namespace Field

/// What WaitBatch does while its batch is not done and the queue is empty.
enum class WaitPolicy : u8 {
    Guest, ///< the engine's delay(1): a 1-microsecond rdtsc spin without PAUSE (the original behaviour)
    Pause, ///< a 1-microsecond spin with PAUSE (gives the core's other hardware thread its resources)
    Yield, ///< SwitchToThread (the rest of the time slice to another ready thread on this core, if any)
};

struct Options {
    WaitPolicy wait = WaitPolicy::Guest;
};

/// Counters since Configure (monotonic; times in TSC ticks, TscPerSecond converts).
struct Stats {
    u64 worker_takes = 0;    ///< entries NextJob gave to workers
    u64 worker_sleeps = 0;   ///< condition-variable waits in NextJob (the queue was empty)
    u64 worker_idle_tsc = 0; ///< workers inside NextJob (lock + sleeping)
    u64 worker_busy_tsc = 0; ///< workers between NextJob calls (running their entries)
    u64 try_calls = 0;       ///< TryNextJob calls
    u64 try_hits = 0;        ///< ... that returned an entry
    u64 waits = 0;           ///< WaitBatch calls for a batch not yet waited for
    u64 wait_tsc = 0;        ///< time inside those calls
    u64 wait_max_tsc = 0;    ///< the longest one
    u64 helped = 0;          ///< entries WaitBatch ran itself
    u64 helped_tsc = 0;      ///< time running them
    u64 idle_spins = 0;      ///< WaitPolicy steps (the queue was empty, the batch not done)
    u32 worker_threads = 0;  ///< distinct threads that called NextJob
};

void Configure(const Environment& env, const Options& options);
Stats Snapshot();
u64 TscPerSecond();
const char* WaitPolicyName(WaitPolicy p);

// The replacements (System V ABI: the guest calls them).
SSPC_SYSV_ABI GuestEntry* NextJob();
SSPC_SYSV_ABI GuestEntry* TryNextJob();
SSPC_SYSV_ABI void WaitBatch(u8* manager, GuestBatch* batch);

/// The game: when SSPC_NATIVE_JOBS=1, verifies the eboot (guest_hooks.h) and installs the three replacements; also
/// reads SSPC_NATIVE_JOBS_WAIT=guest|pause|yield and SSPC_NATIVE_JOBS_STATS=<seconds> (a log line per interval,
/// 0 = none; default 10). Call between loading the eboot and starting it. Returns false (and logs why) when it did
/// not install anything; true also when the switch is off.
bool InstallFromEnvironment(u64 eboot_base);

} // namespace SecondSon::Engine::Jobs
