// SecondSonPC — native pthread mutexes and condition variables (Phase 2.2.3, group "kernel-mutex";
// docs/KERNEL_SYNC.md). Shared by mutex.cpp and condvar.cpp: a condition variable releases and
// re-takes the caller's mutex, so the two move between NATIVE and BRIDGE together (the oracle's
// condvar reads the oracle's mutex object; the oracle's mutex unlock wakes condvar waiters it deferred).
#pragma once

#include <atomic>
#include <cstddef>

#include "base/abi.h"

namespace SecondSon::Kernel::PthreadSync {

enum class MutexType : u32 { ErrorCheck = 1, Recursive = 2, Normal = 3, AdaptiveNp = 4, Max = 5 };
enum class MutexProtocol : u32 { None = 0, Inherit = 1, Protect = 2 };

constexpr u32 kTypeMask = 0xff;
constexpr s32 kAdaptiveSpins = 2000; // libthr MUTEX_ADAPTIVE_SPINS (as the oracle)

/// The object a guest pthread_mutex_t points to. Up to `flags` the layout is libthr's and the
/// oracle's: right after scePthreadMutexInit the game's libc.prx ORs PMUTEX_FLAG_PRIVATE (0x100)
/// into the 32-bit word at +0x20 with a plain `or dword [rax+0x20], 0x100` (libc.prx 0x5b96a,
/// 0x5bbf1, 0x5d81c). So the type stays in the low byte of that word, it is read from there on every
/// lock, and SecondSonPC never writes the word after initialisation (a concurrent read-modify-write
/// would race with libc's).
struct Mutex {
    std::atomic<u64> owner;   // 0x00 host thread id of the owner, 0 = not owned
    s32 count;                // 0x08 recursive locks beyond the first
    s32 spinloops;            // 0x0C adaptive spinning before sleeping
    s32 yieldloops;           // 0x10 (libthr field, unused)
    MutexProtocol protocol;   // 0x14 PTHREAD_PRIO_NONE/INHERIT/PROTECT (stored; no priority change)
    u64 reserved;             // 0x18
    u32 flags;                // 0x20 type | 0x100 private (libc) | 0x200 deferred (libthr)
    std::atomic<u32> state;   // 0x24 0 free, 1 locked, 2 locked and a thread may sleep on it
    char name[32];

    MutexType Type() const {
        return static_cast<MutexType>(flags & kTypeMask);
    }
};
static_assert(offsetof(Mutex, owner) == 0x00 && offsetof(Mutex, count) == 0x08 &&
              offsetof(Mutex, flags) == 0x20 && offsetof(Mutex, state) == 0x24,
              "libc.prx writes the word at +0x20 of the mutex object");

// pthread_mutex_t values that are not objects (libthr / the oracle).
inline Mutex* const kMutexInitializer = nullptr;                          // PTHREAD_MUTEX_INITIALIZER
inline Mutex* const kAdaptiveInitializer = reinterpret_cast<Mutex*>(1);  // PTHREAD_ADAPTIVE_MUTEX_INITIALIZER_NP
inline Mutex* const kMutexDestroyed = reinterpret_cast<Mutex*>(2);

/// The calling thread's identity as a mutex owner (guest threads are host threads 1:1).
u64 Self();

/// Condition variables: releases every recursion level of a mutex the caller owns, and takes it
/// back. POSIX error numbers (0 = success).
s32 CvUnlock(Mutex* m, s32* recurse);
s32 CvLock(Mutex* m, s32 recurse);
/// 0 when the calling thread owns *mutex; EINVAL for a destroyed mutex, EPERM otherwise.
s32 IsOwned(Mutex* const* mutex);

constexpr s32 kOk = 0;
constexpr s32 kEperm = 1;
constexpr s32 kEagain = 35;
constexpr s32 kEnomem = 12;
constexpr s32 kEbusy = 16;
constexpr s32 kEinval = 22;
constexpr s32 kEdeadlk = 11;
constexpr s32 kEtimedout = 60;

/// sce* entry points report POSIX errors as SCE_KERNEL_ERROR_* (0x80020000 + errno), like the
/// oracle's ORBIS() wrapper.
inline s32 Sce(s32 posix) {
    return posix == 0 ? 0 : static_cast<s32>(0x80020000u + static_cast<u32>(posix));
}

/// Self-test access.
namespace Test {
u64 RetiredMutexes();
u64 RetiredConds();
} // namespace Test

} // namespace SecondSon::Kernel::PthreadSync
