// SecondSonPC — Native Kernel Synchronization Layer (Phase 2, P2.2; docs/KERNEL_SYNC.md).
// What every libkernel synchronisation object needs, independent of shadPS4:
//   - HandleTable: guest handles -> objects. Objects are shared_ptr: a thread blocked on an object
//     keeps it alive while another thread deletes it (shadPS4's semaphore table destroys it under
//     the waiters' feet).
//   - Waiter / WaitList: blocked threads in wake order (priority, FIFO among equals; or FIFO), each
//     with its own wake-up and a wake reason decided under the object's lock — a timeout racing a
//     signal is resolved by the reason, never lost.
//   - Block(): the actual sleep, with timeouts; an untimed wait that lasts kLongWait is reported
//     once (hang diagnostics).
// Semaphores use it first; event flags, mutexes and condition variables follow one group at a time.
#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <semaphore>
#include <shared_mutex>
#include <string_view>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Kernel::Sync {

enum class WakeReason : u8 { None, Signaled, Deleted, Canceled };

/// One blocked thread; lives on its stack, linked into an object's WaitList under the object's lock.
struct Waiter {
    std::binary_semaphore wake{0};
    WakeReason reason = WakeReason::None; // written by the waker under the object's lock
    s32 priority = 0;                     // guest thread priority: lower value wakes first
    s64 need = 0;                         // object specific (semaphore: tokens)
};

/// Wake order of an object's blocked threads.
class WaitList {
public:
    void Add(Waiter* w, bool fifo) {
        auto it = items_.end();
        if (!fifo) {
            // Before the first waiter of lower priority (greater value): FIFO among equals.
            it = items_.begin();
            while (it != items_.end() && (*it)->priority <= w->priority) {
                ++it;
            }
        }
        items_.insert(it, w);
    }
    void Remove(Waiter* w) {
        std::erase(items_, w);
    }
    /// Sets the outcome and wakes `w`; the caller holds the object's lock and removes `w` itself.
    static void Wake(Waiter* w, WakeReason reason) {
        w->reason = reason;
        w->wake.release();
    }
    std::vector<Waiter*>& Items() {
        return items_;
    }
    size_t Size() const {
        return items_.size();
    }

private:
    std::vector<Waiter*> items_;
};

/// Hang diagnostics: an untimed wait reports itself once after this long.
inline constexpr std::chrono::seconds kLongWait{60};

/// Sleeps until `w` is woken or `timeout_us` passes (nullopt: no timeout). The caller holds no lock
/// and decides the outcome from w.reason after re-taking the object's lock. `what`, `handle` and
/// `name` only describe the object in the long-wait report.
bool Block(Waiter& w, std::optional<u64> timeout_us, std::string_view what, u32 handle, std::string_view name);

/// Guest handles of one object kind. Allocation order is that of shadPS4's SlotVector (lowest
/// free index first, freed indices reused last-in first-out), so handles match the oracle's.
template <typename T>
class HandleTable {
public:
    explicit HandleTable(u32 capacity) : capacity_{capacity} {}

    /// nullopt when all `capacity` handles are in use.
    std::optional<u32> Insert(std::shared_ptr<T> object) {
        std::unique_lock lock{mutex_};
        if (free_.empty()) {
            const u32 old_size = static_cast<u32>(slots_.size());
            if (old_size >= capacity_) {
                return std::nullopt;
            }
            const u32 new_size = std::min<u32>(capacity_, old_size + kGrow);
            slots_.resize(new_size);
            for (u32 i = new_size; i-- > old_size;) {
                free_.push_back(i);
            }
        }
        const u32 handle = free_.back();
        free_.pop_back();
        slots_[handle] = std::move(object);
        return handle;
    }

    std::shared_ptr<T> Get(u32 handle) const {
        std::shared_lock lock{mutex_};
        return handle < slots_.size() ? slots_[handle] : nullptr;
    }

    /// Removes the handle; returns the object (still alive for its waiters) or nullptr.
    std::shared_ptr<T> Erase(u32 handle) {
        std::unique_lock lock{mutex_};
        if (handle >= slots_.size() || slots_[handle] == nullptr) {
            return nullptr;
        }
        auto object = std::move(slots_[handle]);
        slots_[handle] = nullptr;
        free_.push_back(handle);
        return object;
    }

private:
    static constexpr u32 kGrow = 64;
    mutable std::shared_mutex mutex_;
    std::vector<std::shared_ptr<T>> slots_;
    std::vector<u32> free_;
    u32 capacity_;
};

} // namespace SecondSon::Kernel::Sync
