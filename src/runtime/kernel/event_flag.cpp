// SecondSonPC — libkernel event flags (NATIVE since Phase 2, P2.2.2; docs/KERNEL_SYNC.md).
// sceKernel{Create,Delete,Set,Wait}EventFlag, the four the game imports (KERNEL_AUDIT: 22 created,
// 32 deleted, short-lived around loads). Behaviour is that of the Phase 0 oracle
// (third_party/shadPS4/src/core/libraries/kernel/threads/event_flag.cpp @ 0fe263a), so the game
// sees the same results, with deliberate, safer differences:
//   - handles go through the shared HandleTable (sync.h): a flag deleted while threads wait on it
//     stays alive until they have left (shared_ptr), and a stale/garbage handle returns ESRCH
//     instead of the oracle's raw-pointer use-after-free; the handle handed to the game is 1-based,
//     so it is never 0 (the oracle returns a heap pointer, never null);
//   - the remaining time is written back to *pTimeout on a signalled timed wait (the oracle only
//     zeroes it on a timeout);
//   - a conflicting attr or an invalid waitMode returns EINVAL (the oracle reaches UNREACHABLE / UB);
//   - priority-ordered waiters wake lowest value (most urgent) first, like the native semaphores and
//     Kernel::Host (the oracle's event flag orders them the other way round — a shadPS4 slip).
// Group "kernel-eventflag": `--bridge kernel-eventflag` sends all four back to the oracle.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/sync.h"

namespace SecondSon::Kernel {

namespace {

constexpr s32 ORBIS_OK = 0;
constexpr s32 kErrEperm = static_cast<s32>(0x80020001);
constexpr s32 kErrEsrch = static_cast<s32>(0x80020003);
constexpr s32 kErrEacces = static_cast<s32>(0x8002000D);
constexpr s32 kErrEbusy = static_cast<s32>(0x80020010);
constexpr s32 kErrEinval = static_cast<s32>(0x80020016);
constexpr s32 kErrEnomem = static_cast<s32>(0x8002000C);
constexpr s32 kErrEtimedout = static_cast<s32>(0x8002003C);
constexpr s32 kErrEnametoolong = static_cast<s32>(0x8002003F);
constexpr s32 kErrEcanceled = static_cast<s32>(0x80020055);

constexpr u32 kAttrFifo = 0x01; // SCE_KERNEL_EVF_ATTR_TH_FIFO
constexpr u32 kAttrPrio = 0x02; // SCE_KERNEL_EVF_ATTR_TH_PRIO
constexpr u32 kAttrSingle = 0x10;
constexpr u32 kAttrMulti = 0x20;

constexpr u32 kWaitAnd = 0x01;
constexpr u32 kWaitOr = 0x02;
constexpr u32 kClearAll = 0x10;
constexpr u32 kClearPat = 0x20;

constexpr u32 kMaxEventFlags = 0xFFFF;

using Sync::WaitList;
using Sync::WakeReason;
using Sync::Waiter;

enum class ClearMode : u8 { None, All, Bits };

/// One thread blocked in sceKernelWaitEventFlag; lives on the Wait() stack, linked into waiters_.
struct EvfWaiter {
    Waiter w;                // wake-up, wake reason (under the flag's lock), priority
    u64 bits = 0;            // the pattern this thread waits for
    bool want_and = false;   // AND vs OR
    ClearMode clear = ClearMode::None;
    u64 result = 0;          // pattern reported back (set by Set / Cancel; 0 on Delete)
};

/// A 64-bit event flag: bit pattern plus its blocked threads. One lock guards both.
class EventFlag {
public:
    EventFlag(std::string name, bool single, bool fifo, u64 bits)
        : name_{std::move(name)}, bits_{bits}, single_{single}, fifo_{fifo} {}

    s32 Wait(u32 handle, u64 bits, bool want_and, ClearMode clear, u64* result, u32* timeout_us,
             bool can_block) {
        std::unique_lock lock{mutex_};
        // A SINGLE flag rejects a second waiter even when the pattern is ready (as in the oracle).
        if (single_ && !waiters_.empty()) {
            return kErrEperm;
        }
        if (Met(bits, want_and)) {
            if (result != nullptr) {
                *result = bits_;
            }
            ApplyClear(bits, clear);
            return ORBIS_OK;
        }
        if (!can_block) {
            return kErrEbusy; // sceKernelPollEventFlag
        }
        if (timeout_us != nullptr && *timeout_us == 0) {
            if (result != nullptr) {
                *result = bits_;
            }
            return kErrEtimedout;
        }

        EvfWaiter e;
        e.bits = bits;
        e.want_and = want_and;
        e.clear = clear;
        e.w.priority = fifo_ ? 0 : CurrentPriority();
        AddWaiter(&e);
        lock.unlock();

        const auto start = std::chrono::steady_clock::now();
        const std::optional<u64> limit =
            timeout_us != nullptr ? std::optional<u64>{*timeout_us} : std::nullopt;
        Sync::Block(e.w, limit, "event flag", handle, name_);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();

        lock.lock();
        if (e.w.reason == WakeReason::None) {
            RemoveWaiter(&e); // timed out, nobody woke us
        }
        if (timeout_us != nullptr) {
            const u64 left = *timeout_us > static_cast<u64>(elapsed) ? *timeout_us - elapsed : 0;
            *timeout_us = e.w.reason == WakeReason::Signaled ? static_cast<u32>(left) : 0;
        }
        switch (e.w.reason) {
        case WakeReason::Signaled:
            if (result != nullptr) {
                *result = e.result;
            }
            return ORBIS_OK;
        case WakeReason::Deleted:
            if (result != nullptr) {
                *result = e.result; // 0: Delete does not publish a pattern
            }
            return kErrEacces;
        case WakeReason::Canceled:
            if (result != nullptr) {
                *result = e.result;
            }
            return kErrEcanceled;
        default:
            if (result != nullptr) {
                *result = bits_;
            }
            return kErrEtimedout;
        }
    }

    void Set(u64 bits) {
        std::scoped_lock lock{mutex_};
        bits_ |= bits;
        // In wake order, every waiter the pattern now satisfies (a CLEAR waiter earlier in the list
        // can consume the bits before a later one sees them, as in the oracle). Woken waiters leave
        // the list at once, so a second Set never re-reports to a thread already on its way out.
        for (auto it = waiters_.begin(); it != waiters_.end();) {
            EvfWaiter* e = *it;
            if (!Met(e->bits, e->want_and)) {
                ++it;
                continue;
            }
            e->result = bits_;
            ApplyClear(e->bits, e->clear);
            it = waiters_.erase(it);
            WaitList::Wake(&e->w, WakeReason::Signaled);
        }
    }

    /// sceKernelClearEventFlag: keep only the bits in `pattern` (libkernel's &= semantics).
    void Clear(u64 pattern) {
        std::scoped_lock lock{mutex_};
        bits_ &= pattern;
    }

    /// sceKernelCancelEventFlag (not imported by the game; used by tests).
    void Cancel(u64 pattern, s32* num_waiters) {
        std::scoped_lock lock{mutex_};
        if (num_waiters != nullptr) {
            *num_waiters = static_cast<s32>(waiters_.size());
        }
        bits_ = pattern;
        for (EvfWaiter* e : waiters_) {
            e->result = pattern;
            WaitList::Wake(&e->w, WakeReason::Canceled);
        }
        waiters_.clear();
    }

    void Delete() {
        std::scoped_lock lock{mutex_};
        for (EvfWaiter* e : waiters_) {
            WaitList::Wake(&e->w, WakeReason::Deleted);
        }
        waiters_.clear();
    }

    u64 Bits() {
        std::scoped_lock lock{mutex_};
        return bits_;
    }

    size_t WaiterCount() {
        std::scoped_lock lock{mutex_};
        return waiters_.size();
    }

private:
    bool Met(u64 bits, bool want_and) const {
        return want_and ? (bits_ & bits) == bits : (bits_ & bits) != 0;
    }

    void ApplyClear(u64 wait_bits, ClearMode clear) {
        switch (clear) {
        case ClearMode::None:
            break;
        case ClearMode::All:
            bits_ = 0;
            break;
        case ClearMode::Bits:
            bits_ &= ~wait_bits;
            break;
        }
    }

    void AddWaiter(EvfWaiter* e) {
        if (fifo_) {
            waiters_.push_back(e);
            return;
        }
        // Before the first waiter of lower priority (greater value): FIFO among equals.
        auto it = waiters_.begin();
        while (it != waiters_.end() && (*it)->w.priority <= e->w.priority) {
            ++it;
        }
        waiters_.insert(it, e);
    }

    void RemoveWaiter(EvfWaiter* e) {
        std::erase(waiters_, e);
    }

    static s32 CurrentPriority() {
        Host* host = GetHost();
        return host != nullptr ? host->CurrentThreadPriority() : 0;
    }

    std::mutex mutex_;
    std::vector<EvfWaiter*> waiters_;
    std::string name_;
    u64 bits_;
    bool single_;
    bool fifo_;
};

Sync::HandleTable<EventFlag>& Table() {
    static Sync::HandleTable<EventFlag> table{kMaxEventFlags};
    return table;
}

/// Guest handle (1-based) -> object. Handle 0 and anything past the table are invalid.
std::shared_ptr<EventFlag> Lookup(u64 ef) {
    if (ef == 0 || ef > kMaxEventFlags) {
        return nullptr;
    }
    return Table().Get(static_cast<u32>(ef - 1));
}

std::shared_ptr<EventFlag> EraseHandle(u64 ef) {
    if (ef == 0 || ef > kMaxEventFlags) {
        return nullptr;
    }
    return Table().Erase(static_cast<u32>(ef - 1));
}

/// attr -> (single, fifo); false if the attribute bits conflict (the oracle hits UNREACHABLE).
bool ParseAttr(u32 attr, bool& single, bool& fifo) {
    if (attr > (kAttrMulti | kAttrPrio)) {
        return false;
    }
    const u32 queue = attr & 0x0f;
    const u32 thread = attr & 0xf0;
    if (queue != 0 && queue != kAttrFifo && queue != kAttrPrio) { // 0x03 = FIFO|PRIO and up
        return false;
    }
    if (thread != 0 && thread != kAttrSingle && thread != kAttrMulti) { // 0x30 = SINGLE|MULTI
        return false;
    }
    fifo = queue != kAttrPrio;     // default (0) and FIFO order by arrival
    single = thread != kAttrMulti; // default (0) and SINGLE allow one waiter
    return true;
}

/// waitMode -> (want_and, clear); false if the mode bits are invalid (the oracle hits UNREACHABLE).
bool ParseWaitMode(u32 mode, bool& want_and, ClearMode& clear) {
    switch (mode & 0x0f) {
    case kWaitAnd:
        want_and = true;
        break;
    case kWaitOr:
        want_and = false;
        break;
    default:
        return false;
    }
    switch (mode & 0xf0) {
    case 0x00:
        clear = ClearMode::None;
        break;
    case kClearAll:
        clear = ClearMode::All;
        break;
    case kClearPat:
        clear = ClearMode::Bits;
        break;
    default:
        return false;
    }
    return true;
}

SSPC_SYSV_ABI s32 CreateEventFlag(u64* ef, const char* name, u32 attr, u64 init_pattern,
                                  const void* opt) {
    if (ef == nullptr || name == nullptr) {
        return kErrEinval;
    }
    bool single = true, fifo = true;
    if (opt != nullptr || !ParseAttr(attr, single, fifo)) {
        SSPC_LOG(Kernel, Error, "sceKernelCreateEventFlag: invalid parameters (attr {:#x})", attr);
        return kErrEinval;
    }
    if (std::strlen(name) >= 32) {
        return kErrEnametoolong;
    }
    const auto handle = Table().Insert(std::make_shared<EventFlag>(name, single, fifo, init_pattern));
    if (!handle) {
        SSPC_LOG(Kernel, Error, "sceKernelCreateEventFlag: all {} handles are in use", kMaxEventFlags);
        return kErrEnomem;
    }
    *ef = static_cast<u64>(*handle) + 1;
    SSPC_LOG(Kernel, Debug, "sceKernelCreateEventFlag \"{}\" -> {} (attr {:#x}, init {:#x})", name, *ef,
             attr, init_pattern);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 DeleteEventFlag(u64 ef) {
    const auto e = EraseHandle(ef);
    if (e == nullptr) {
        return kErrEsrch;
    }
    e->Delete();
    SSPC_LOG(Kernel, Debug, "sceKernelDeleteEventFlag {}", ef);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 SetEventFlag(u64 ef, u64 bit_pattern) {
    const auto e = Lookup(ef);
    if (e == nullptr) {
        return kErrEsrch;
    }
    e->Set(bit_pattern);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 WaitEventFlag(u64 ef, u64 bit_pattern, u32 wait_mode, u64* result, u32* timeout_us) {
    const auto e = Lookup(ef);
    if (e == nullptr) {
        return kErrEsrch;
    }
    if (bit_pattern == 0) {
        return kErrEinval;
    }
    bool want_and = true;
    ClearMode clear = ClearMode::None;
    if (!ParseWaitMode(wait_mode, want_and, clear)) {
        return kErrEinval;
    }
    return e->Wait(static_cast<u32>(ef), bit_pattern, want_and, clear, result, timeout_us, true);
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"BpFoboUJoZU", "libkernel", "libkernel", "sceKernelCreateEventFlag", Fn(&CreateEventFlag), nullptr,
     "kernel-eventflag"},
    {"8mql9OcQnd4", "libkernel", "libkernel", "sceKernelDeleteEventFlag", Fn(&DeleteEventFlag), nullptr,
     "kernel-eventflag"},
    {"IOnSvHzqu6A", "libkernel", "libkernel", "sceKernelSetEventFlag", Fn(&SetEventFlag), nullptr,
     "kernel-eventflag"},
    {"JTvBflhYazQ", "libkernel", "libkernel", "sceKernelWaitEventFlag", Fn(&WaitEventFlag), nullptr,
     "kernel-eventflag"},
};

} // namespace

std::span<const Runtime::Export> EventFlagExports() {
    return kExports;
}

namespace EventFlagTest {

s32 Poll(u64 ef, u64 bits, u32 wait_mode, u64* result) {
    const auto e = Lookup(ef);
    if (e == nullptr) {
        return kErrEsrch;
    }
    if (bits == 0) {
        return kErrEinval;
    }
    bool want_and = true;
    ClearMode clear = ClearMode::None;
    if (!ParseWaitMode(wait_mode, want_and, clear)) {
        return kErrEinval;
    }
    return e->Wait(static_cast<u32>(ef), bits, want_and, clear, result, nullptr, false);
}

s32 Clear(u64 ef, u64 pattern) {
    const auto e = Lookup(ef);
    if (e == nullptr) {
        return kErrEsrch;
    }
    e->Clear(pattern);
    return ORBIS_OK;
}

s32 Cancel(u64 ef, u64 pattern, s32* num_waiters) {
    const auto e = Lookup(ef);
    if (e == nullptr) {
        return kErrEsrch;
    }
    e->Cancel(pattern, num_waiters);
    return ORBIS_OK;
}

s64 Bits(u64 ef) {
    const auto e = Lookup(ef);
    return e != nullptr ? static_cast<s64>(e->Bits()) : -1;
}

s32 Waiters(u64 ef) {
    const auto e = Lookup(ef);
    return e != nullptr ? static_cast<s32>(e->WaiterCount()) : -1;
}

} // namespace EventFlagTest

} // namespace SecondSon::Kernel
