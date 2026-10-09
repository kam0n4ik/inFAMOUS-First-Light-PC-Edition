// SecondSonPC — libkernel reader/writer locks (NATIVE since Phase 2.2.3, group "kernel-rwlock";
// docs/KERNEL_SYNC.md). The game's 7 imports (all from libSceFios2.prx), sce* (SCE error codes):
// scePthreadRwlock{Init,Destroy,Rdlock,Wrlock,Unlock,Tryrdlock,Trywrlock}. No other import takes a
// rwlock, so the group moves on its own.
// Semantics of libkernel's libthr (FreeBSD thr_rwlock.c over a urwlock):
//   - pthread_rwlock_t holds a pointer to a host object; NULL = static initializer (created on first
//     use), 1 = destroyed (EINVAL); destroy does not check holders (libthr, oracle);
//   - writers are preferred: a new reader waits while a writer holds the lock or waits for it,
//     unless the thread already holds a read lock (of any rwlock: libthr's per-thread rdlock_count),
//     so a recursive read lock cannot deadlock behind a waiting writer;
//   - tryrdlock / trywrlock -> EBUSY instead of waiting; unlock by a thread that holds neither a
//     read lock nor the write lock -> EPERM; a writer locking its own lock again deadlocks (libthr;
//     logged as Critical first).
// Deliberately different from the Phase 0 oracle (threads/rwlock.cpp @ 0fe263a): the oracle is
// reader-first (Common::SharedFirstMutex — readers never wait for a waiting writer, writers can
// starve) and an unlock by a thread holding nothing calls unlock_shared() (undefined behaviour).
// The oracle checks the attribute type only for SDK >= 4.50; this game is SDK 4.00, so neither does
// SecondSonPC. A destroyed object is never freed (retired list).
#include <condition_variable>
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

struct Rwlock {
    std::mutex lock;
    std::condition_variable readers_cv;
    std::condition_variable writers_cv;
    u32 readers = 0;         // read locks held
    u32 waiting_writers = 0;
    u64 writer = 0;          // owner of the write lock (Self()), 0 = none
    char name[32]{};
};

Rwlock* const kRwlockInitializer = nullptr;
Rwlock* const kRwlockDestroyed = reinterpret_cast<Rwlock*>(1);

thread_local s32 t_rdlock_count = 0; // read locks this thread holds, on any rwlock (libthr)

std::mutex g_static_lock;
std::mutex g_retired_lock;
std::vector<Rwlock*> g_retired;

uintptr_t Bits(const Rwlock* r) {
    return reinterpret_cast<uintptr_t>(r);
}

s32 Init(Rwlock** rwlock, const char* name) {
    *rwlock = nullptr;
    auto* r = new (std::nothrow) Rwlock{};
    if (r == nullptr) {
        return kEnomem;
    }
    if (name != nullptr) {
        strncpy_s(r->name, name, _TRUNCATE);
    }
    *rwlock = r;
    return kOk;
}

Rwlock* Resolve(Rwlock** rwlock, s32* error) {
    Rwlock* r = *rwlock;
    if (Bits(r) <= Bits(kRwlockDestroyed)) [[unlikely]] {
        if (r == kRwlockDestroyed) {
            *error = kEinval;
            return nullptr;
        }
        std::scoped_lock lock{g_static_lock};
        if (*rwlock == kRwlockInitializer) {
            if (const s32 e = Init(rwlock, nullptr); e != kOk) {
                *error = e;
                return nullptr;
            }
        }
        r = *rwlock;
        if (r == kRwlockDestroyed) {
            *error = kEinval;
            return nullptr;
        }
    }
    return r;
}

bool ReaderMayEnter(const Rwlock* r) {
    return r->writer == 0 && (r->waiting_writers == 0 || t_rdlock_count > 0);
}

s32 Rdlock(Rwlock** rwlock, bool wait) {
    s32 error = kOk;
    Rwlock* r = Resolve(rwlock, &error);
    if (r == nullptr) {
        return error;
    }
    std::unique_lock lock{r->lock};
    if (!ReaderMayEnter(r)) {
        if (!wait) {
            return kEbusy;
        }
        if (r->writer == Self()) {
            SSPC_LOG(Kernel, Critical, "pthread rwlock \"{}\": read lock by the thread holding its write lock "
                     "(thread {}): deadlock, as on the PS4", r->name, GetCurrentThreadId());
        }
        r->readers_cv.wait(lock, [r] { return ReaderMayEnter(r); });
    }
    ++r->readers;
    ++t_rdlock_count;
    return kOk;
}

s32 Wrlock(Rwlock** rwlock, bool wait) {
    s32 error = kOk;
    Rwlock* r = Resolve(rwlock, &error);
    if (r == nullptr) {
        return error;
    }
    std::unique_lock lock{r->lock};
    if (r->writer != 0 || r->readers != 0) {
        if (!wait) {
            return kEbusy;
        }
        if (r->writer == Self()) {
            SSPC_LOG(Kernel, Critical, "pthread rwlock \"{}\": write lock again by its writer (thread {}): "
                     "deadlock, as on the PS4", r->name, GetCurrentThreadId());
        }
        ++r->waiting_writers;
        r->writers_cv.wait(lock, [r] { return r->writer == 0 && r->readers == 0; });
        --r->waiting_writers;
    }
    r->writer = Self();
    return kOk;
}

s32 Unlock(Rwlock** rwlock) {
    Rwlock* r = *rwlock;
    if (Bits(r) <= Bits(kRwlockDestroyed)) [[unlikely]] {
        return kEinval;
    }
    std::scoped_lock lock{r->lock};
    if (r->writer != 0) {
        if (r->writer != Self()) {
            return kEperm;
        }
        r->writer = 0;
        if (r->waiting_writers > 0) {
            r->writers_cv.notify_one();
        }
        r->readers_cv.notify_all(); // readers re-check: only recursive ones pass a waiting writer
        return kOk;
    }
    if (r->readers == 0) {
        return kEperm;
    }
    --r->readers;
    --t_rdlock_count;
    if (r->readers == 0 && r->waiting_writers > 0) {
        r->writers_cv.notify_one();
    }
    return kOk;
}

s32 Destroy(Rwlock** rwlock) {
    Rwlock* r = *rwlock;
    if (r == kRwlockInitializer) {
        return kOk;
    }
    if (r == kRwlockDestroyed) {
        return kEinval;
    }
    *rwlock = kRwlockDestroyed;
    std::scoped_lock lock{g_retired_lock};
    g_retired.push_back(r);
    return kOk;
}

SSPC_SYSV_ABI s32 SceRwlockInit(Rwlock** rwlock, const void* /*attr*/, const char* name) {
    return Sce(Init(rwlock, name));
}
SSPC_SYSV_ABI s32 SceRwlockDestroy(Rwlock** rwlock) {
    return Sce(Destroy(rwlock));
}
SSPC_SYSV_ABI s32 SceRwlockRdlock(Rwlock** rwlock) {
    return Sce(Rdlock(rwlock, true));
}
SSPC_SYSV_ABI s32 SceRwlockWrlock(Rwlock** rwlock) {
    return Sce(Wrlock(rwlock, true));
}
SSPC_SYSV_ABI s32 SceRwlockTryrdlock(Rwlock** rwlock) {
    return Sce(Rdlock(rwlock, false));
}
SSPC_SYSV_ABI s32 SceRwlockTrywrlock(Rwlock** rwlock) {
    return Sce(Wrlock(rwlock, false));
}
SSPC_SYSV_ABI s32 SceRwlockUnlock(Rwlock** rwlock) {
    return Sce(Unlock(rwlock));
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

constexpr const char* kGroup = "kernel-rwlock";

const Runtime::Export kExports[] = {
    {"6ULAa0fq4jA", "libkernel", "libkernel", "scePthreadRwlockInit", Fn(&SceRwlockInit), nullptr, kGroup},
    {"BB+kb08Tl9A", "libkernel", "libkernel", "scePthreadRwlockDestroy", Fn(&SceRwlockDestroy), nullptr, kGroup},
    {"Ox9i0c7L5w0", "libkernel", "libkernel", "scePthreadRwlockRdlock", Fn(&SceRwlockRdlock), nullptr, kGroup},
    {"mqdNorrB+gI", "libkernel", "libkernel", "scePthreadRwlockWrlock", Fn(&SceRwlockWrlock), nullptr, kGroup},
    {"XD3mDeybCnk", "libkernel", "libkernel", "scePthreadRwlockTryrdlock", Fn(&SceRwlockTryrdlock), nullptr, kGroup},
    {"bIHoZCTomsI", "libkernel", "libkernel", "scePthreadRwlockTrywrlock", Fn(&SceRwlockTrywrlock), nullptr, kGroup},
    {"+L98PIbGttk", "libkernel", "libkernel", "scePthreadRwlockUnlock", Fn(&SceRwlockUnlock), nullptr, kGroup},
};

} // namespace

} // namespace SecondSon::Kernel::PthreadSync

namespace SecondSon::Kernel {
std::span<const Runtime::Export> RwlockExports() {
    return PthreadSync::kExports;
}
} // namespace SecondSon::Kernel
