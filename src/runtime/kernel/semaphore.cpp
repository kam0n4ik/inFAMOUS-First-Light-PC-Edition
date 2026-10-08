// SecondSonPC — libkernel semaphores (NATIVE since Phase 2, P2.2; docs/KERNEL_SYNC.md).
// sceKernelCreateSema / WaitSema / SignalSema / DeleteSema, the four the game imports (KERNEL_AUDIT:
// 20 created at start-up, ~460 waits/s, never deleted). Behaviour is that of the Phase 0 oracle
// (third_party/shadPS4/src/core/libraries/kernel/threads/semaphore.cpp @ 0fe263a), so the game
// sees the same results, with two deliberate differences:
//   - a semaphore deleted while threads wait on it stays alive until they have left (the oracle
//     destroys it while they still re-take its lock);
//   - the remaining time written back after a signalled timed wait never goes below 0 (the oracle's
//     u32 subtraction wraps when the signal arrives after the timeout ran out).
// Not validated, as in the oracle: need/signal counts (libkernel rejects < 1). Group "kernel-sema":
// `--bridge kernel-sema` sends all four back to the oracle.
#include <mutex>
#include <string>

#include <windows.h>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/sync.h"

namespace SecondSon::Kernel {

namespace {

constexpr s32 ORBIS_OK = 0;
constexpr s32 kErrEsrch = static_cast<s32>(0x80020003);
constexpr s32 kErrEnomem = static_cast<s32>(0x8002000C);
constexpr s32 kErrEacces = static_cast<s32>(0x8002000D);
constexpr s32 kErrEbusy = static_cast<s32>(0x80020010);
constexpr s32 kErrEinval = static_cast<s32>(0x80020016);
constexpr s32 kErrEtimedout = static_cast<s32>(0x8002003C);
constexpr s32 kErrEcanceled = static_cast<s32>(0x80020055);

constexpr u32 kAttrFifo = 1; // SCE_KERNEL_SEMA_ATTR_TH_FIFO; 0 and 2 order waiters by priority
constexpr u32 kMaxSemaphores = 0xFFFF;

using Sync::WaitList;
using Sync::WakeReason;
using Sync::Waiter;

class Semaphore {
public:
    Semaphore(std::string name, s32 init, s32 max, bool fifo)
        : name_{std::move(name)}, tokens_{init}, init_{init}, max_{max}, fifo_{fifo} {}

    s32 Wait(u32 handle, bool can_block, s32 need, u32* timeout_us) {
        std::unique_lock lock{mutex_};
        if (tokens_ >= need) {
            tokens_ -= need;
            return ORBIS_OK;
        }
        if (!can_block) {
            return kErrEbusy;
        }
        if (timeout_us != nullptr && *timeout_us == 0) {
            return kErrEtimedout;
        }
        Waiter w;
        w.need = need;
        w.priority = fifo_ ? 0 : CurrentPriority();
        waiters_.Add(&w, fifo_);
        lock.unlock();

        const auto start = std::chrono::steady_clock::now();
        const std::optional<u64> limit =
            timeout_us != nullptr ? std::optional<u64>{*timeout_us} : std::nullopt;
        Sync::Block(w, limit, "semaphore", handle, name_);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();

        lock.lock();
        if (w.reason == WakeReason::None) {
            waiters_.Remove(&w); // timed out, nobody woke us
        }
        if (timeout_us != nullptr) {
            const u64 left = *timeout_us > static_cast<u64>(elapsed) ? *timeout_us - elapsed : 0;
            *timeout_us = w.reason == WakeReason::Signaled ? static_cast<u32>(left) : 0;
        }
        switch (w.reason) {
        case WakeReason::Signaled:
            return ORBIS_OK;
        case WakeReason::Deleted:
            return kErrEacces;
        case WakeReason::Canceled:
            return kErrEcanceled;
        default:
            return kErrEtimedout;
        }
    }

    bool Signal(s32 count) {
        std::scoped_lock lock{mutex_};
        if (static_cast<s64>(tokens_) + count > max_) {
            return false;
        }
        tokens_ += count;
        // In wake order, every waiter whose need fits the tokens left (a waiter needing more is
        // skipped, not waited for — as in the oracle).
        auto& items = waiters_.Items();
        for (auto it = items.begin(); it != items.end();) {
            Waiter* w = *it;
            if (w->need > tokens_) {
                ++it;
                continue;
            }
            it = items.erase(it);
            tokens_ -= static_cast<s32>(w->need);
            WaitList::Wake(w, WakeReason::Signaled);
        }
        return true;
    }

    /// sceKernelCancelSema semantics (not imported by the game; used by tests).
    void Cancel(s32 set_count, s32* waiter_count) {
        std::scoped_lock lock{mutex_};
        if (waiter_count != nullptr) {
            *waiter_count = static_cast<s32>(waiters_.Size());
        }
        for (Waiter* w : waiters_.Items()) {
            WaitList::Wake(w, WakeReason::Canceled);
        }
        waiters_.Items().clear();
        tokens_ = set_count < 0 ? init_ : set_count;
    }

    void Delete() {
        std::scoped_lock lock{mutex_};
        for (Waiter* w : waiters_.Items()) {
            WaitList::Wake(w, WakeReason::Deleted);
        }
        waiters_.Items().clear();
    }

    s32 Tokens() {
        std::scoped_lock lock{mutex_};
        return tokens_;
    }

    size_t WaiterCount() {
        std::scoped_lock lock{mutex_};
        return waiters_.Size();
    }

private:
    static s32 CurrentPriority() {
        Host* host = GetHost();
        return host != nullptr ? host->CurrentThreadPriority() : 0;
    }

    std::mutex mutex_;
    WaitList waiters_;
    std::string name_;
    s32 tokens_;
    s32 init_;
    s32 max_;
    bool fifo_;
};

Sync::HandleTable<Semaphore>& Table() {
    static Sync::HandleTable<Semaphore> table{kMaxSemaphores};
    return table;
}

SSPC_SYSV_ABI s32 CreateSema(u32* sem, const char* name, u32 attr, s32 init, s32 max, const void* /*opt*/) {
    if (name == nullptr || attr > 2 || init < 0 || max <= 0 || init > max) {
        SSPC_LOG(Kernel, Error, "sceKernelCreateSema: invalid parameters (attr {}, init {}, max {})", attr, init, max);
        return kErrEinval;
    }
    const auto handle = Table().Insert(std::make_shared<Semaphore>(name, init, max, attr == kAttrFifo));
    if (!handle) {
        SSPC_LOG(Kernel, Error, "sceKernelCreateSema: all {} semaphore handles are in use", kMaxSemaphores);
        return kErrEnomem;
    }
    *sem = *handle;
    SSPC_LOG(Kernel, Debug, "sceKernelCreateSema \"{}\" -> {} (attr {}, init {}, max {})", name, *handle, attr,
             init, max);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 WaitSema(u32 sem, s32 need, u32* timeout_us) {
    const auto s = Table().Get(sem);
    return s != nullptr ? s->Wait(sem, true, need, timeout_us) : kErrEsrch;
}

SSPC_SYSV_ABI s32 SignalSema(u32 sem, s32 count) {
    const auto s = Table().Get(sem);
    if (s == nullptr) {
        return kErrEsrch;
    }
    return s->Signal(count) ? ORBIS_OK : kErrEinval;
}

SSPC_SYSV_ABI s32 DeleteSema(u32 sem) {
    const auto s = Table().Erase(sem);
    if (s == nullptr) {
        return kErrEsrch;
    }
    s->Delete();
    return ORBIS_OK;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"188x57JYp0g", "libkernel", "libkernel", "sceKernelCreateSema", Fn(&CreateSema), nullptr, "kernel-sema"},
    {"Zxa0VhQVTsk", "libkernel", "libkernel", "sceKernelWaitSema", Fn(&WaitSema), nullptr, "kernel-sema"},
    {"4czppHBiriw", "libkernel", "libkernel", "sceKernelSignalSema", Fn(&SignalSema), nullptr, "kernel-sema"},
    {"R1Jvn8bSCW8", "libkernel", "libkernel", "sceKernelDeleteSema", Fn(&DeleteSema), nullptr, "kernel-sema"},
};

} // namespace

namespace Sync {

bool Block(Waiter& w, std::optional<u64> timeout_us, std::string_view what, u32 handle, std::string_view name) {
    if (timeout_us) {
        return w.wake.try_acquire_for(std::chrono::microseconds(*timeout_us));
    }
    if (w.wake.try_acquire_for(kLongWait)) {
        return true;
    }
    SSPC_LOG(Kernel, Warning, "thread {} has been waiting on {} {} \"{}\" for {} s (reported once per wait)",
             GetCurrentThreadId(), what, handle, name, kLongWait.count());
    w.wake.acquire();
    return true;
}

} // namespace Sync

std::span<const Runtime::Export> SemaphoreExports() {
    return kExports;
}

namespace SemaphoreTest {

s32 Poll(u32 sem, s32 need) {
    const auto s = Table().Get(sem);
    return s != nullptr ? s->Wait(sem, false, need, nullptr) : kErrEsrch;
}

s32 Cancel(u32 sem, s32 set_count, s32* waiter_count) {
    const auto s = Table().Get(sem);
    if (s == nullptr) {
        return kErrEsrch;
    }
    s->Cancel(set_count, waiter_count);
    return ORBIS_OK;
}

s32 Tokens(u32 sem) {
    const auto s = Table().Get(sem);
    return s != nullptr ? s->Tokens() : -1;
}

s32 Waiters(u32 sem) {
    const auto s = Table().Get(sem);
    return s != nullptr ? static_cast<s32>(s->WaiterCount()) : -1;
}

} // namespace SemaphoreTest

} // namespace SecondSon::Kernel
