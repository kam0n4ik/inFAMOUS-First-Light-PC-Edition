// SecondSonPC — libkernel pthread mutexes (NATIVE since Phase 2.2.3, group "kernel-mutex" together
// with condvar.cpp; docs/KERNEL_SYNC.md). The game's 19 mutex imports: scePthreadMutex{Init,Destroy,
// Lock,Trylock,Unlock} and scePthreadMutexattr{Init,Destroy,Settype,Setprotocol} (SCE error codes),
// pthread_mutex_{init,destroy,lock,trylock,unlock} and pthread_mutexattr_{init,destroy,settype} of
// libScePosix, pthread_mutex_{lock,unlock} of libkernel (POSIX error numbers).
// Semantics of libthr (FreeBSD) and of the Phase 0 oracle
// (third_party/shadPS4/src/core/libraries/kernel/threads/mutex.cpp @ 0fe263a):
//   - pthread_mutex_t holds a pointer to a host object; NULL and 1 are static initializers
//     (error-check / adaptive), created on first use under one lock; 2 = destroyed (EINVAL);
//   - default type error-check: relock by the owner -> EDEADLK, trylock by the owner -> EBUSY;
//     recursive counts levels (EAGAIN on overflow); normal deadlocks on relock (libkernel blocks,
//     the oracle asserts: logged as Critical, then blocks); adaptive spins 2000 times first (only
//     with protocol NONE, as the oracle);
//   - unlock by a thread that does not own the mutex -> EPERM; destroy of an owned one -> EBUSY;
//   - the lock word is free / locked / contended; a thread that cannot take it sleeps on the word
//     (std::atomic::wait = WaitOnAddress) and is woken by the unlock that sees "contended".
// Deliberately different: a destroyed object is never freed (kept in a retired list, ~80 bytes,
// a few dozen per hour of play), so a thread still leaving Lock() on it reads valid memory (the
// oracle deletes it under such a thread). PTHREAD_PRIO_INHERIT/PROTECT are stored, priorities are
// not boosted (neither does the oracle).
#include <immintrin.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <vector>

#include <windows.h>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/pthread_sync.h"

namespace SecondSon::Kernel::PthreadSync {

namespace {

/// What a guest pthread_mutexattr_t points to (libthr's and the oracle's layout).
struct MutexAttr {
    MutexType type;
    MutexProtocol protocol;
    s32 ceiling;
};

constexpr MutexAttr kDefaultAttr{MutexType::ErrorCheck, MutexProtocol::None, 0};
constexpr MutexAttr kAdaptiveAttr{MutexType::AdaptiveNp, MutexProtocol::None, 0};
constexpr s32 kPrioRrHighest = 767; // the ceiling pthread_mutexattr_setprotocol stores (as the oracle)

std::mutex g_static_lock;
std::mutex g_retired_lock;
std::vector<Mutex*> g_retired;
std::atomic<u32> g_unnamed{0};

uintptr_t Bits(const Mutex* m) {
    return reinterpret_cast<uintptr_t>(m);
}

s32 Init(Mutex** mutex, const MutexAttr* attr, const char* name) {
    if (attr == nullptr) {
        attr = &kDefaultAttr;
    } else if (attr->type < MutexType::ErrorCheck || attr->type >= MutexType::Max ||
               attr->protocol > MutexProtocol::Protect) {
        return kEinval;
    }
    auto* m = new (std::nothrow) Mutex{};
    if (m == nullptr) {
        return kEnomem;
    }
    if (name != nullptr) {
        strncpy_s(m->name, name, _TRUNCATE);
    } else {
        std::snprintf(m->name, sizeof(m->name), "Mutex%u", g_unnamed.fetch_add(1));
    }
    m->flags = static_cast<u32>(attr->type);
    m->protocol = attr->protocol;
    m->spinloops = attr->type == MutexType::AdaptiveNp ? kAdaptiveSpins : 0;
    *mutex = m;
    return kOk;
}

/// The object behind *mutex; a statically initialised mutex is created here. nullptr: *error set.
Mutex* Resolve(Mutex** mutex, s32* error) {
    Mutex* m = *mutex;
    if (Bits(m) <= Bits(kMutexDestroyed)) [[unlikely]] {
        if (m == kMutexDestroyed) {
            *error = kEinval;
            return nullptr;
        }
        std::scoped_lock lock{g_static_lock};
        s32 e = kOk;
        if (*mutex == kMutexInitializer) {
            e = Init(mutex, &kDefaultAttr, nullptr);
        } else if (*mutex == kAdaptiveInitializer) {
            e = Init(mutex, &kAdaptiveAttr, nullptr);
        }
        m = *mutex;
        if (e != kOk || m == kMutexDestroyed) {
            *error = e != kOk ? e : kEinval;
            return nullptr;
        }
    }
    return m;
}

[[noreturn]] void Deadlock(const Mutex* m) {
    SSPC_LOG(Kernel, Critical,
             "pthread mutex \"{}\" (normal type) locked again by its owner, thread {}: deadlock, as on "
             "the PS4",
             m->name, GetCurrentThreadId());
    Log::Flush();
    for (;;) {
        Sleep(INFINITE);
    }
}

s32 SelfLock(Mutex* m) {
    switch (m->Type()) {
    case MutexType::ErrorCheck:
    case MutexType::AdaptiveNp:
        return kEdeadlk;
    case MutexType::Normal:
        Deadlock(m);
    case MutexType::Recursive:
        if (m->count + 1 > 0) {
            ++m->count;
            return kOk;
        }
        return kEagain;
    default:
        return kEinval;
    }
}

s32 SelfTryLock(Mutex* m) {
    switch (m->Type()) {
    case MutexType::ErrorCheck:
    case MutexType::Normal:
    case MutexType::AdaptiveNp:
        return kEbusy;
    case MutexType::Recursive:
        if (m->count + 1 > 0) {
            ++m->count;
            return kOk;
        }
        return kEagain;
    default:
        return kEinval;
    }
}

bool TryAcquire(Mutex* m) {
    u32 expected = 0;
    return m->state.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
}

/// Slow path: spin (adaptive), then sleep on the lock word marked "contended".
void Acquire(Mutex* m) {
    if (m->protocol == MutexProtocol::None) {
        for (s32 n = m->spinloops; n > 0; --n) {
            if (m->state.load(std::memory_order_relaxed) == 0 && TryAcquire(m)) {
                return;
            }
            _mm_pause();
        }
    }
    u32 c = m->state.exchange(2, std::memory_order_acquire);
    while (c != 0) {
        m->state.wait(2, std::memory_order_relaxed);
        c = m->state.exchange(2, std::memory_order_acquire);
    }
}

s32 LockMutex(Mutex* m) {
    const u64 self = Self();
    if (m->owner.load(std::memory_order_relaxed) == self) {
        return SelfLock(m);
    }
    if (!TryAcquire(m)) {
        Acquire(m);
    }
    m->owner.store(self, std::memory_order_relaxed);
    return kOk;
}

s32 TryLockMutex(Mutex* m) {
    const u64 self = Self();
    if (m->owner.load(std::memory_order_relaxed) == self) {
        return SelfTryLock(m);
    }
    if (!TryAcquire(m)) {
        return kEbusy;
    }
    m->owner.store(self, std::memory_order_relaxed);
    return kOk;
}

s32 UnlockMutex(Mutex* m) {
    if (m->owner.load(std::memory_order_relaxed) != Self()) [[unlikely]] {
        return kEperm;
    }
    if (m->Type() == MutexType::Recursive && m->count > 0) {
        --m->count;
        return kOk;
    }
    m->owner.store(0, std::memory_order_relaxed);
    if (m->state.exchange(0, std::memory_order_release) == 2) {
        m->state.notify_one();
    }
    return kOk;
}

void Retire(Mutex* m) {
    std::scoped_lock lock{g_retired_lock};
    g_retired.push_back(m);
}

s32 Destroy(Mutex** mutex) {
    Mutex* m = *mutex;
    if (Bits(m) < Bits(kMutexDestroyed)) {
        return kOk; // never used: nothing to destroy (libthr, oracle)
    }
    if (m == kMutexDestroyed) {
        return kEinval;
    }
    if (m->owner.load(std::memory_order_acquire) != 0) {
        return kEbusy;
    }
    *mutex = kMutexDestroyed;
    Retire(m);
    return kOk;
}

s32 Lock(Mutex** mutex) {
    s32 error = kOk;
    Mutex* m = Resolve(mutex, &error);
    return m != nullptr ? LockMutex(m) : error;
}

s32 TryLock(Mutex** mutex) {
    s32 error = kOk;
    Mutex* m = Resolve(mutex, &error);
    return m != nullptr ? TryLockMutex(m) : error;
}

s32 Unlock(Mutex** mutex) {
    Mutex* m = *mutex;
    if (Bits(m) <= Bits(kMutexDestroyed)) [[unlikely]] {
        return m == kMutexDestroyed ? kEinval : kEperm;
    }
    return UnlockMutex(m);
}

s32 AttrInit(MutexAttr** attr) {
    auto* a = new (std::nothrow) MutexAttr{kDefaultAttr};
    if (a == nullptr) {
        return kEnomem;
    }
    *attr = a;
    return kOk;
}

s32 AttrDestroy(MutexAttr** attr) {
    if (attr == nullptr || *attr == nullptr) {
        return kEinval;
    }
    delete *attr;
    *attr = nullptr;
    return kOk;
}

s32 AttrSettype(MutexAttr** attr, u32 type) {
    if (attr == nullptr || *attr == nullptr || type < static_cast<u32>(MutexType::ErrorCheck) ||
        type >= static_cast<u32>(MutexType::Max)) {
        return kEinval;
    }
    (*attr)->type = static_cast<MutexType>(type);
    return kOk;
}

s32 AttrSetprotocol(MutexAttr** attr, u32 protocol) {
    if (attr == nullptr || *attr == nullptr || protocol > static_cast<u32>(MutexProtocol::Protect)) {
        return kEinval;
    }
    (*attr)->protocol = static_cast<MutexProtocol>(protocol);
    (*attr)->ceiling = kPrioRrHighest;
    return kOk;
}

// ---- entry points (System V ABI) ---------------------------------------------------------------

SSPC_SYSV_ABI s32 PosixMutexInit(Mutex** mutex, MutexAttr* const* attr) {
    return Init(mutex, attr != nullptr ? *attr : nullptr, nullptr);
}
SSPC_SYSV_ABI s32 PosixMutexDestroy(Mutex** mutex) {
    return Destroy(mutex);
}
SSPC_SYSV_ABI s32 PosixMutexLock(Mutex** mutex) {
    return Lock(mutex);
}
SSPC_SYSV_ABI s32 PosixMutexTrylock(Mutex** mutex) {
    return TryLock(mutex);
}
SSPC_SYSV_ABI s32 PosixMutexUnlock(Mutex** mutex) {
    return Unlock(mutex);
}
SSPC_SYSV_ABI s32 PosixAttrInit(MutexAttr** attr) {
    return AttrInit(attr);
}
SSPC_SYSV_ABI s32 PosixAttrDestroy(MutexAttr** attr) {
    return AttrDestroy(attr);
}
SSPC_SYSV_ABI s32 PosixAttrSettype(MutexAttr** attr, u32 type) {
    return AttrSettype(attr, type);
}

SSPC_SYSV_ABI s32 SceMutexInit(Mutex** mutex, MutexAttr* const* attr, const char* name) {
    return Sce(Init(mutex, attr != nullptr ? *attr : nullptr, name));
}
SSPC_SYSV_ABI s32 SceMutexDestroy(Mutex** mutex) {
    return Sce(Destroy(mutex));
}
SSPC_SYSV_ABI s32 SceMutexLock(Mutex** mutex) {
    return Sce(Lock(mutex));
}
SSPC_SYSV_ABI s32 SceMutexTrylock(Mutex** mutex) {
    return Sce(TryLock(mutex));
}
SSPC_SYSV_ABI s32 SceMutexUnlock(Mutex** mutex) {
    return Sce(Unlock(mutex));
}
SSPC_SYSV_ABI s32 SceAttrInit(MutexAttr** attr) {
    return Sce(AttrInit(attr));
}
SSPC_SYSV_ABI s32 SceAttrDestroy(MutexAttr** attr) {
    return Sce(AttrDestroy(attr));
}
SSPC_SYSV_ABI s32 SceAttrSettype(MutexAttr** attr, u32 type) {
    return Sce(AttrSettype(attr, type));
}
SSPC_SYSV_ABI s32 SceAttrSetprotocol(MutexAttr** attr, u32 protocol) {
    return Sce(AttrSetprotocol(attr, protocol));
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

constexpr const char* kGroup = "kernel-mutex";

const Runtime::Export kExports[] = {
    {"cmo1RIYva9o", "libkernel", "libkernel", "scePthreadMutexInit", Fn(&SceMutexInit), nullptr, kGroup},
    {"2Of0f+3mhhE", "libkernel", "libkernel", "scePthreadMutexDestroy", Fn(&SceMutexDestroy), nullptr, kGroup},
    {"9UK1vLZQft4", "libkernel", "libkernel", "scePthreadMutexLock", Fn(&SceMutexLock), nullptr, kGroup},
    {"upoVrzMHFeE", "libkernel", "libkernel", "scePthreadMutexTrylock", Fn(&SceMutexTrylock), nullptr, kGroup},
    {"tn3VlD0hG60", "libkernel", "libkernel", "scePthreadMutexUnlock", Fn(&SceMutexUnlock), nullptr, kGroup},
    {"F8bUHwAG284", "libkernel", "libkernel", "scePthreadMutexattrInit", Fn(&SceAttrInit), nullptr, kGroup},
    {"smWEktiyyG0", "libkernel", "libkernel", "scePthreadMutexattrDestroy", Fn(&SceAttrDestroy), nullptr, kGroup},
    {"iMp8QpE+XO4", "libkernel", "libkernel", "scePthreadMutexattrSettype", Fn(&SceAttrSettype), nullptr, kGroup},
    {"1FGvU0i9saQ", "libkernel", "libkernel", "scePthreadMutexattrSetprotocol", Fn(&SceAttrSetprotocol), nullptr,
     kGroup},
    {"ttHNfU+qDBU", "libScePosix", "libkernel", "pthread_mutex_init", Fn(&PosixMutexInit), nullptr, kGroup},
    {"ltCfaGr2JGE", "libScePosix", "libkernel", "pthread_mutex_destroy", Fn(&PosixMutexDestroy), nullptr, kGroup},
    {"7H0iTOciTLo", "libScePosix", "libkernel", "pthread_mutex_lock", Fn(&PosixMutexLock), nullptr, kGroup},
    {"K-jXhbt2gn4", "libScePosix", "libkernel", "pthread_mutex_trylock", Fn(&PosixMutexTrylock), nullptr, kGroup},
    {"2Z+PpY6CaJg", "libScePosix", "libkernel", "pthread_mutex_unlock", Fn(&PosixMutexUnlock), nullptr, kGroup},
    {"dQHWEsJtoE4", "libScePosix", "libkernel", "pthread_mutexattr_init", Fn(&PosixAttrInit), nullptr, kGroup},
    {"HF7lK46xzjY", "libScePosix", "libkernel", "pthread_mutexattr_destroy", Fn(&PosixAttrDestroy), nullptr, kGroup},
    {"mDmgMOGVUqg", "libScePosix", "libkernel", "pthread_mutexattr_settype", Fn(&PosixAttrSettype), nullptr, kGroup},
    {"7H0iTOciTLo", "libkernel", "libkernel", "pthread_mutex_lock", Fn(&PosixMutexLock), nullptr, kGroup},
    {"2Z+PpY6CaJg", "libkernel", "libkernel", "pthread_mutex_unlock", Fn(&PosixMutexUnlock), nullptr, kGroup},
};

} // namespace

u64 Self() {
    return GetCurrentThreadId();
}

s32 CvUnlock(Mutex* m, s32* recurse) {
    *recurse = m->count;
    m->count = 0;
    return UnlockMutex(m);
}

s32 CvLock(Mutex* m, s32 recurse) {
    const s32 error = LockMutex(m);
    if (error == kOk) {
        m->count = recurse;
    }
    return error;
}

s32 IsOwned(Mutex* const* mutex) {
    const Mutex* m = *mutex;
    if (Bits(m) <= Bits(kMutexDestroyed)) {
        return m == kMutexDestroyed ? kEinval : kEperm;
    }
    return m->owner.load(std::memory_order_relaxed) == Self() ? kOk : kEperm;
}

namespace Test {
u64 RetiredMutexes() {
    std::scoped_lock lock{g_retired_lock};
    return g_retired.size();
}
} // namespace Test

} // namespace SecondSon::Kernel::PthreadSync

namespace SecondSon::Kernel {
std::span<const Runtime::Export> MutexExports() {
    return PthreadSync::kExports;
}
} // namespace SecondSon::Kernel
