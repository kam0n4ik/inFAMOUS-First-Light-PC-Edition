// SecondSonPC — libkernel condition variables (NATIVE since Phase 2.2.3, group "kernel-mutex": a
// condvar releases and re-takes the caller's mutex, so it moves with the mutexes; docs/KERNEL_SYNC.md).
// The game's 6 imports, all sce* (SCE error codes): scePthreadCond{Init,Destroy,Wait,Timedwait,
// Signal,Broadcast}; scePthreadCondTimedwait is libthr's pthread_cond_reltimedwait_np (relative
// microseconds). Semantics of libthr and of the oracle (threads/condvar.cpp @ 0fe263a):
//   - pthread_cond_t holds a pointer to a host object; NULL = static initializer, created on first
//     use; 1 = destroyed (EINVAL);
//   - wait: the caller must own the mutex (EPERM; EINVAL for a destroyed mutex, as the oracle's
//     IsOwned); the thread is queued *before* the mutex is released, so a signal sent after the
//     release always finds it (no lost wake-up); every recursion level of the mutex is released and
//     restored; a timed wait returns ETIMEDOUT with the mutex held again;
//   - signal wakes the longest waiting thread (FIFO, libkernel's TAILQ), broadcast all of them;
//     neither needs the mutex; with no waiter they do nothing;
//   - no spurious wake-ups: a waiter returns only when signalled, broadcast or timed out; a signal
//     that races the timeout wins (as in the oracle).
// Deliberately different: destroy with waiters -> EBUSY (libthr; the oracle deletes the object under
// them); a destroyed object is never freed (retired list). Not supported because the game imports
// neither: cancelling a waiting thread (pthread_cancel), condattr clocks for absolute waits (the
// clock is stored; the game's only timed wait is relative).
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>
#include <optional>
#include <vector>

#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/pthread_sync.h"
#include "runtime/kernel/sync.h"

namespace SecondSon::Kernel::PthreadSync {

namespace {

using Sync::Waiter;
using Sync::WaitList;
using Sync::WakeReason;

struct Cond {
    std::mutex lock;
    WaitList waiters;
    u32 clock_id = 0;
    char name[32]{};
};

/// What a guest pthread_condattr_t points to (libthr's layout).
struct CondAttr {
    s32 pshared;
    u32 clock_id;
};

Cond* const kCondInitializer = nullptr;
Cond* const kCondDestroyed = reinterpret_cast<Cond*>(1);

std::mutex g_static_lock;
std::mutex g_retired_lock;
std::vector<Cond*> g_retired;
std::atomic<u32> g_unnamed{0};

uintptr_t Bits(const Cond* c) {
    return reinterpret_cast<uintptr_t>(c);
}

s32 Init(Cond** cond, CondAttr* const* attr, const char* name) {
    *cond = nullptr; // as the oracle: a failed init leaves a static initializer
    auto* c = new (std::nothrow) Cond{};
    if (c == nullptr) {
        return kEnomem;
    }
    if (name != nullptr) {
        strncpy_s(c->name, name, _TRUNCATE);
    } else {
        std::snprintf(c->name, sizeof(c->name), "Cond%u", g_unnamed.fetch_add(1));
    }
    c->clock_id = attr != nullptr && *attr != nullptr ? (*attr)->clock_id : 0;
    *cond = c;
    return kOk;
}

Cond* Resolve(Cond** cond, s32* error) {
    Cond* c = *cond;
    if (Bits(c) <= Bits(kCondDestroyed)) [[unlikely]] {
        if (c == kCondDestroyed) {
            *error = kEinval;
            return nullptr;
        }
        std::scoped_lock lock{g_static_lock};
        if (*cond == kCondInitializer) {
            if (const s32 e = Init(cond, nullptr, nullptr); e != kOk) {
                *error = e;
                return nullptr;
            }
        }
        c = *cond;
        if (c == kCondDestroyed) {
            *error = kEinval;
            return nullptr;
        }
    }
    return c;
}

s32 Wait(Cond** cond, Mutex** mutex, std::optional<u64> timeout_us) {
    s32 error = kOk;
    Cond* c = Resolve(cond, &error);
    if (c == nullptr) {
        return error;
    }
    if (const s32 e = IsOwned(mutex); e != kOk) {
        return e;
    }
    Mutex* m = *mutex;
    Waiter w;
    {
        std::scoped_lock lock{c->lock};
        c->waiters.Add(&w, true); // queued before the mutex is released
    }
    s32 recurse = 0;
    CvUnlock(m, &recurse);
    bool woken = true;
    if (timeout_us) {
        woken = w.wake.try_acquire_for(std::chrono::microseconds(*timeout_us));
    } else {
        w.wake.acquire();
    }
    if (!woken) {
        std::scoped_lock lock{c->lock};
        if (w.reason == WakeReason::None) {
            c->waiters.Remove(&w);
        } else {
            woken = true; // signalled while timing out: the signal wins
        }
    }
    const s32 relock = CvLock(m, recurse);
    return woken ? relock : kEtimedout;
}

s32 Signal(Cond** cond) {
    s32 error = kOk;
    Cond* c = Resolve(cond, &error);
    if (c == nullptr) {
        return error;
    }
    std::scoped_lock lock{c->lock};
    auto& items = c->waiters.Items();
    if (!items.empty()) {
        Waiter* w = items.front();
        items.erase(items.begin());
        WaitList::Wake(w, WakeReason::Signaled);
    }
    return kOk;
}

s32 Broadcast(Cond** cond) {
    s32 error = kOk;
    Cond* c = Resolve(cond, &error);
    if (c == nullptr) {
        return error;
    }
    std::scoped_lock lock{c->lock};
    for (Waiter* w : c->waiters.Items()) {
        WaitList::Wake(w, WakeReason::Signaled);
    }
    c->waiters.Items().clear();
    return kOk;
}

s32 Destroy(Cond** cond) {
    Cond* c = *cond;
    if (c == kCondInitializer) {
        return kOk;
    }
    if (c == kCondDestroyed) {
        return kEinval;
    }
    {
        std::scoped_lock lock{c->lock};
        if (c->waiters.Size() != 0) {
            return kEbusy;
        }
    }
    *cond = kCondDestroyed;
    std::scoped_lock lock{g_retired_lock};
    g_retired.push_back(c);
    return kOk;
}

SSPC_SYSV_ABI s32 SceCondInit(Cond** cond, CondAttr* const* attr, const char* name) {
    return Sce(Init(cond, attr, name));
}
SSPC_SYSV_ABI s32 SceCondDestroy(Cond** cond) {
    return Sce(Destroy(cond));
}
SSPC_SYSV_ABI s32 SceCondWait(Cond** cond, Mutex** mutex) {
    return Sce(Wait(cond, mutex, std::nullopt));
}
SSPC_SYSV_ABI s32 SceCondTimedwait(Cond** cond, Mutex** mutex, u32 usec) {
    return Sce(Wait(cond, mutex, u64{usec}));
}
SSPC_SYSV_ABI s32 SceCondSignal(Cond** cond) {
    return Sce(Signal(cond));
}
SSPC_SYSV_ABI s32 SceCondBroadcast(Cond** cond) {
    return Sce(Broadcast(cond));
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

constexpr const char* kGroup = "kernel-mutex";

const Runtime::Export kExports[] = {
    {"2Tb92quprl0", "libkernel", "libkernel", "scePthreadCondInit", Fn(&SceCondInit), nullptr, kGroup},
    {"g+PZd2hiacg", "libkernel", "libkernel", "scePthreadCondDestroy", Fn(&SceCondDestroy), nullptr, kGroup},
    {"WKAXJ4XBPQ4", "libkernel", "libkernel", "scePthreadCondWait", Fn(&SceCondWait), nullptr, kGroup},
    {"BmMjYxmew1w", "libkernel", "libkernel", "scePthreadCondTimedwait", Fn(&SceCondTimedwait), nullptr, kGroup},
    {"kDh-NfxgMtE", "libkernel", "libkernel", "scePthreadCondSignal", Fn(&SceCondSignal), nullptr, kGroup},
    {"JGgj7Uvrl+A", "libkernel", "libkernel", "scePthreadCondBroadcast", Fn(&SceCondBroadcast), nullptr, kGroup},
};

} // namespace

namespace Test {
u64 RetiredConds() {
    std::scoped_lock lock{g_retired_lock};
    return g_retired.size();
}
} // namespace Test

} // namespace SecondSon::Kernel::PthreadSync

namespace SecondSon::Kernel {
std::span<const Runtime::Export> CondvarExports() {
    return PthreadSync::kExports;
}
} // namespace SecondSon::Kernel
