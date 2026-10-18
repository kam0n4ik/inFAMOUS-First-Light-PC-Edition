// SecondSonPC — libkernel event queues (NATIVE, group "kernel-equeue"; docs/AUDIT.md Phase 3): the queue table and
// the game's API to it (sceKernel{Create,Delete,Wait}Equeue, the user events it imports) plus the contract a graphics backend
// delivers through (event_queues.h: SecondSonPC's GnmDriver and VideoOut). Behaviour is the oracle's
// (third_party/shadPS4/src/core/libraries/kernel/equeue.cpp with patch 0025): events are (ident, filter); adding one
// again only replaces its user data; every filter but User is cleared when a wait returns it, a user event stays
// triggered until deleted; a trigger wakes every waiter. A queue is held by reference:
// deleted while a thread waits on it or a producer delivers into it, it lives until they are done with it; a later
// queue that reuses the handle has another serial. Handles are descriptor numbers (the file system's table), as on
// the console. The game uses no timer events and no kqueue/kevent (docs/generated/imports.csv); they stay unbound.
// `--bridge kernel-equeue` (or the bridge's GPU backend) gives the queues back to the oracle.
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <intrin.h>

#include "base/abi.h"
#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/filesystem/fs_host.h"
#include "runtime/kernel/event_queues.h"

namespace SecondSon::Kernel::EventQueues {

namespace {

constexpr s32 kOk = 0;
constexpr s32 kErrEnoent = static_cast<s32>(0x80020002);
constexpr s32 kErrEbadf = static_cast<s32>(0x80020009);
constexpr s32 kErrEnomem = static_cast<s32>(0x8002000C);
constexpr s32 kErrEfault = static_cast<s32>(0x8002000E);
constexpr s32 kErrEinval = static_cast<s32>(0x80020016);
constexpr s32 kErrEtimedout = static_cast<s32>(0x8002003C);
constexpr s32 kErrEnametoolong = static_cast<s32>(0x8002003F);

constexpr u16 kFlagAdd = 0x1;
constexpr u16 kFlagOneShot = 0x10;
constexpr u16 kFlagClear = 0x20;

/// The console's struct kevent (32 bytes).
struct KEvent {
    u64 ident;
    s16 filter;
    u16 flags;
    u32 fflags;
    u64 data;
    void* udata;
};
static_assert(sizeof(KEvent) == 32);

struct Event {
    KEvent ev{};
    bool triggered = false;
};

class Queue {
public:
    Queue(s64 handle, u64 serial, std::string name) : handle_(handle), serial_(serial), name_(std::move(name)) {}

    QueueId Id() const {
        return {handle_, serial_};
    }

    bool Add(u64 ident, s16 filter, u16 flags, void* udata) {
        std::scoped_lock lock{mutex_};
        for (Event& e : events_) {
            if (e.ev.ident == ident && e.ev.filter == filter) {
                e.ev.udata = udata; // (trigger state and data stay)
                return true;
            }
        }
        Event e;
        e.ev = {ident, filter, static_cast<u16>(flags & ~kFlagAdd), 0, 0, udata};
        if (filter != static_cast<s16>(Filter::User)) {
            e.ev.flags |= kFlagClear;
        }
        events_.push_back(e);
        return true;
    }

    bool Remove(u64 ident, s16 filter) {
        std::scoped_lock lock{mutex_};
        for (auto it = events_.begin(); it != events_.end(); ++it) {
            if (it->ev.ident == ident && it->ev.filter == filter) {
                events_.erase(it);
                return true;
            }
        }
        return false;
    }

    bool Trigger(u64 ident, s16 filter, u64 data) {
        bool found = false;
        {
            std::scoped_lock lock{mutex_};
            for (Event& e : events_) {
                if (e.ev.ident != ident || e.ev.filter != filter) {
                    continue;
                }
                e.triggered = true;
                if (filter == static_cast<s16>(Filter::User)) {
                    e.ev.udata = reinterpret_cast<void*>(data);
                } else if (filter == static_cast<s16>(Filter::VideoOut)) {
                    // data = the hint: event id (8 bits) | video id (8) | flip argument (48)
                    if (data != 0 && (data & 0xFF) == e.ev.ident && e.ev.ident != 0xFE) {
                        u64 counter = (e.ev.data >> 12) & 0xF;
                        if (counter != 0xF) {
                            ++counter;
                        }
                        e.ev.data = (__rdtsc() & 0xFFF) | (counter << 12) | (data & 0xFFFFFFFFFFFF0000ull);
                    }
                } else {
                    e.ev.data = data;
                }
                found = true;
            }
        }
        cond_.notify_all(); // (every waiter re-checks, as the oracle with patch 0025)
        return found;
    }

    /// Up to `num` triggered events into `out`; `timeout_us` nullptr: until one is there, 0: a poll.
    s32 Wait(KEvent* out, s32 num, const u32* timeout_us) {
        std::unique_lock lock{mutex_};
        s32 count = 0;
        const auto take = [&] {
            count = Take(out, num);
            return count > 0;
        };
        if (timeout_us != nullptr && *timeout_us == 0) {
            take();
        } else if (timeout_us == nullptr) {
            cond_.wait(lock, take);
        } else {
            cond_.wait_for(lock, std::chrono::microseconds(*timeout_us), take);
        }
        return count;
    }

private:
    s32 Take(KEvent* out, s32 num) {
        s32 count = 0;
        for (auto it = events_.begin(); it != events_.end() && count < num;) {
            if (!it->triggered) {
                ++it;
                continue;
            }
            out[count++] = it->ev;
            if ((it->ev.flags & kFlagClear) != 0) {
                it->triggered = false;
                it->ev.fflags = 0;
                it->ev.data = 0;
            }
            if ((it->ev.flags & kFlagOneShot) != 0) {
                it = events_.erase(it);
            } else {
                ++it;
            }
        }
        return count;
    }

    const s64 handle_;
    const u64 serial_;
    const std::string name_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::vector<Event> events_;
};

std::shared_mutex g_table_mutex;
std::unordered_map<s64, std::shared_ptr<Queue>> g_table;
u64 g_serial = 1;

std::shared_ptr<Queue> Lookup(s64 handle) {
    std::shared_lock lock{g_table_mutex};
    const auto it = g_table.find(handle);
    return it != g_table.end() ? it->second : nullptr;
}

std::shared_ptr<Queue> Lookup(QueueId id) {
    auto q = Lookup(id.handle);
    return q != nullptr && q->Id() == id ? q : nullptr;
}

class NativeOwner final : public Owner {
public:
    std::optional<QueueId> Find(s64 handle) override {
        const auto q = Lookup(handle);
        return q != nullptr ? std::optional{q->Id()} : std::nullopt;
    }
    bool AddEvent(QueueId queue, u64 ident, Filter filter, void* udata) override {
        const auto q = Lookup(queue);
        return q != nullptr && q->Add(ident, static_cast<s16>(filter), kFlagAdd, udata);
    }
    bool RemoveEvent(QueueId queue, u64 ident, Filter filter) override {
        const auto q = Lookup(queue);
        return q != nullptr && q->Remove(ident, static_cast<s16>(filter));
    }
    bool Trigger(QueueId queue, u64 ident, Filter filter, u64 data) override {
        const auto q = Lookup(queue);
        return q != nullptr && q->Trigger(ident, static_cast<s16>(filter), data);
    }
};

// ---- the game's API

SSPC_SYSV_ABI s32 CreateEqueue(s64* eq, const char* name) {
    if (eq == nullptr || name == nullptr) {
        return kErrEinval;
    }
    if (std::strlen(name) > 32) {
        return kErrEnametoolong;
    }
    const s32 fd = Filesystem::ReserveDescriptor();
    if (fd < 0) {
        return kErrEnomem;
    }
    {
        std::unique_lock lock{g_table_mutex};
        g_table[fd] = std::make_shared<Queue>(fd, g_serial++, name);
    }
    *eq = fd;
    return kOk;
}

SSPC_SYSV_ABI s32 DeleteEqueue(s64 eq) {
    std::shared_ptr<Queue> q; // (released after the lock: a waiter or producer may still hold it)
    {
        std::unique_lock lock{g_table_mutex};
        const auto it = g_table.find(eq);
        if (it == g_table.end()) {
            return kErrEbadf;
        }
        q = std::move(it->second);
        g_table.erase(it);
    }
    Filesystem::ReleaseDescriptor(static_cast<s32>(eq)); // (out of the table first: no new queue takes it before)
    return kOk;
}

SSPC_SYSV_ABI s32 WaitEqueue(s64 eq, KEvent* ev, s32 num, s32* out, u32* timeout_us) {
    const auto q = Lookup(eq);
    if (q == nullptr) {
        return kErrEbadf;
    }
    if (ev == nullptr) {
        return kErrEfault;
    }
    if (num < 1) {
        *out = 0;
        return kErrEinval;
    }
    *out = q->Wait(ev, num, timeout_us);
    return *out == 0 ? kErrEtimedout : kOk;
}

s32 AddUser(s64 eq, s32 id, u16 flags) {
    const auto q = Lookup(eq);
    if (q == nullptr) {
        return kErrEbadf;
    }
    return q->Add(static_cast<u64>(static_cast<s64>(id)), static_cast<s16>(Filter::User), flags, nullptr)
               ? kOk
               : kErrEnomem;
}

SSPC_SYSV_ABI s32 AddUserEvent(s64 eq, s32 id) {
    return AddUser(eq, id, kFlagAdd);
}

SSPC_SYSV_ABI s32 TriggerUserEvent(s64 eq, s32 id, void* udata) {
    const auto q = Lookup(eq);
    if (q == nullptr) {
        return kErrEbadf;
    }
    return q->Trigger(static_cast<u64>(static_cast<s64>(id)), static_cast<s16>(Filter::User),
                      reinterpret_cast<u64>(udata))
               ? kOk
               : kErrEnoent;
}

SSPC_SYSV_ABI s32 DeleteUserEvent(s64 eq, s32 id) {
    const auto q = Lookup(eq);
    if (q == nullptr) {
        return kErrEbadf;
    }
    return q->Remove(static_cast<u64>(static_cast<s64>(id)), static_cast<s16>(Filter::User)) ? kOk : kErrEnoent;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

#define EQ(nid, name, fn) Runtime::Export{nid, "libkernel", "libkernel", name, Fn(&fn), nullptr, "kernel-equeue"}
const Runtime::Export kExports[] = {
    EQ("D0OdFMjp46I", "sceKernelCreateEqueue", CreateEqueue),
    EQ("jpFjmgAC5AE", "sceKernelDeleteEqueue", DeleteEqueue),
    EQ("fzyMKs9kim0", "sceKernelWaitEqueue", WaitEqueue),
    EQ("4R6-OvI2cEA", "sceKernelAddUserEvent", AddUserEvent),
    EQ("F6e0kwo4cnk", "sceKernelTriggerUserEvent", TriggerUserEvent),
    EQ("LJDwdSNTnDg", "sceKernelDeleteUserEvent", DeleteUserEvent),
};
#undef EQ

} // namespace

void InstallNativeOwner() {
    static NativeOwner owner;
    SetOwner(&owner);
    SSPC_LOG(Kernel, Info, "event queues: SecondSonPC's (group kernel-equeue)");
}

std::span<const Runtime::Export> NativeExports() {
    return kExports;
}

} // namespace SecondSon::Kernel::EventQueues
