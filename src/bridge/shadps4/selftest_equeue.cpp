// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — self-test of the game's event queues (Phase 3, P3.6; docs/PHASE3_P36_BRIDGE_ISOLATION.md §3):
// the guest functions the game imports (create, wait, the user events, delete) and the contract a graphics
// backend delivers through (runtime/kernel/event_queues.h over the bridge's libkernel, patch 0025): errors,
// time-outs, wake-ups of every waiter, deletion while a thread waits or producers deliver, a queue's identity
// when its handle is reused, the GPU interrupt path of sceGnmAddEqEvent after its queue is gone, races.
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "bridge/shadps4/selftest_threads.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/kernel/equeue.h"
#include "core/libraries/kernel/orbis_error.h"
#include "core/platform.h"
#include "runtime/kernel/event_queues.h"

namespace Libraries::Kernel { // the guest functions (BRIDGE exports; declared as in generated/bridge_exports.cpp)
int PS4_SYSV_ABI sceKernelCreateEqueue(OrbisKernelEqueue* eq, const char* name);
int PS4_SYSV_ABI sceKernelDeleteEqueue(OrbisKernelEqueue eq);
int PS4_SYSV_ABI sceKernelWaitEqueue(OrbisKernelEqueue eq, OrbisKernelEvent* ev, int num, int* out,
                                     OrbisKernelUseconds* timo);
int PS4_SYSV_ABI sceKernelAddUserEvent(OrbisKernelEqueue eq, int id);
int PS4_SYSV_ABI sceKernelTriggerUserEvent(OrbisKernelEqueue eq, int id, void* udata);
int PS4_SYSV_ABI sceKernelDeleteUserEvent(OrbisKernelEqueue eq, int id);
} // namespace Libraries::Kernel

namespace SecondSon::Bridge {

namespace {

namespace K = Libraries::Kernel;
namespace Q = Kernel::EventQueues;
using Clock = std::chrono::steady_clock;

constexpr int kOk = 0;
constexpr int kEbadf = ORBIS_KERNEL_ERROR_EBADF;
constexpr int kEinval = ORBIS_KERNEL_ERROR_EINVAL;
constexpr int kEfault = ORBIS_KERNEL_ERROR_EFAULT;
constexpr int kEtimedout = ORBIS_KERNEL_ERROR_ETIMEDOUT;
constexpr int kEnoent = ORBIS_KERNEL_ERROR_ENOENT;
constexpr int kEnametoolong = ORBIS_KERNEL_ERROR_ENAMETOOLONG;
constexpr K::OrbisKernelEqueue kNoQueue = 0x7FFF0001;

double Ms(Clock::time_point since) {
    return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

struct WaitResult {
    int rc = -1;
    int out = -1;
    K::OrbisKernelEvent ev{};
    double ms = 0;
};

/// sceKernelWaitEqueue with num = 1 and `timeout_us` (0: poll; ~0u: no time-out).
WaitResult Wait(K::OrbisKernelEqueue eq, u32 timeout_us) {
    WaitResult r;
    K::OrbisKernelUseconds timo = timeout_us;
    const auto t0 = Clock::now();
    r.rc = K::sceKernelWaitEqueue(eq, &r.ev, 1, &r.out, timeout_us == ~0u ? nullptr : &timo);
    r.ms = Ms(t0);
    return r;
}

std::string Describe(const WaitResult& r) {
    return fmt::format("rc {:#x}, out {}, ident {:#x}, filter {}, data {:#x}, udata {}, {:.1f} ms",
                       static_cast<u32>(r.rc), r.out, r.ev.ident, static_cast<s32>(r.ev.filter), r.ev.data,
                       r.ev.udata, r.ms);
}

K::OrbisKernelEqueue Create(TestGroup& g, const char* name) {
    K::OrbisKernelEqueue eq = 0;
    const int rc = K::sceKernelCreateEqueue(&eq, name);
    g.Check(fmt::format("create \"{}\"", name), rc == kOk && eq > 0,
            fmt::format("rc {:#x}, handle {}", static_cast<u32>(rc), eq));
    return eq;
}

void TestGuestApi(TestGroup& g) {
    K::OrbisKernelEqueue eq = 0;
    g.Check("create: no handle pointer -> EINVAL", K::sceKernelCreateEqueue(nullptr, "x") == kEinval, "");
    g.Check("create: no name -> EINVAL", K::sceKernelCreateEqueue(&eq, nullptr) == kEinval, "");
    const std::string long_name(33, 'n');
    g.Check("create: a 33-byte name -> ENAMETOOLONG",
            K::sceKernelCreateEqueue(&eq, long_name.c_str()) == kEnametoolong, "");
    const auto q1 = Create(g, "sspc_eq_api1");
    const auto q2 = Create(g, "sspc_eq_api2");
    g.Check("two queues have two handles", q1 != q2, fmt::format("{} {}", q1, q2));

    K::OrbisKernelEvent ev{};
    int out = -1;
    K::OrbisKernelUseconds timo = 0;
    g.Check("wait: unknown handle -> EBADF", K::sceKernelWaitEqueue(kNoQueue, &ev, 1, &out, &timo) == kEbadf, "");
    g.Check("wait: no event array -> EFAULT", K::sceKernelWaitEqueue(q1, nullptr, 1, &out, &timo) == kEfault, "");
    out = -1;
    g.Check("wait: num 0 -> EINVAL, out 0", K::sceKernelWaitEqueue(q1, &ev, 0, &out, &timo) == kEinval && out == 0,
            fmt::format("out {}", out));
    auto r = Wait(q1, 0);
    g.Check("wait: poll of an empty queue -> ETIMEDOUT, out 0", r.rc == kEtimedout && r.out == 0, Describe(r));
    r = Wait(q1, 30000);
    g.Check("wait: 30 ms on an empty queue -> ETIMEDOUT after the time-out",
            r.rc == kEtimedout && r.out == 0 && r.ms >= 24 && r.ms < 2000, Describe(r));

    // User events, as the game's trophy thread uses them: added, triggered with user data, returned by every
    // wait until deleted (level-triggered), then gone.
    g.Check("user event: add to an unknown queue -> EBADF", K::sceKernelAddUserEvent(kNoQueue, 7) == kEbadf, "");
    g.Check("user event: trigger before it is added -> ENOENT",
            K::sceKernelTriggerUserEvent(q1, 7, nullptr) == kEnoent, "");
    g.Check("user event: add", K::sceKernelAddUserEvent(q1, 7) == kOk, "");
    void* const udata = reinterpret_cast<void*>(0x5EC01D50ull);
    g.Check("user event: trigger", K::sceKernelTriggerUserEvent(q1, 7, udata) == kOk, "");
    r = Wait(q1, 0);
    g.Check("user event: a wait returns it with its user data",
            r.rc == kOk && r.out == 1 && r.ev.ident == 7 && r.ev.filter == K::OrbisKernelEvent::Filter::User &&
                r.ev.udata == udata,
            Describe(r));
    r = Wait(q1, 0);
    g.Check("user event: still triggered for the next wait (level-triggered until deleted)",
            r.rc == kOk && r.out == 1 && r.ev.ident == 7, Describe(r));
    g.Check("user event: delete", K::sceKernelDeleteUserEvent(q1, 7) == kOk, "");
    r = Wait(q1, 0);
    g.Check("user event: gone after delete", r.rc == kEtimedout && r.out == 0, Describe(r));
    g.Check("user event: second delete -> ENOENT", K::sceKernelDeleteUserEvent(q1, 7) == kEnoent, "");

    // A blocked waiter wakes when the event arrives (a generous time-out: a lost wake-up shows as its length).
    g.Check("blocked wait: add", K::sceKernelAddUserEvent(q2, 1) == kOk, "");
    WaitResult blocked;
    std::thread waiter([&] { blocked = Wait(q2, 5'000'000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    K::sceKernelTriggerUserEvent(q2, 1, nullptr);
    waiter.join();
    g.Check("blocked wait: woken by the trigger, not by its 5 s time-out",
            blocked.rc == kOk && blocked.out == 1 && blocked.ms < 2000, Describe(blocked));

    g.Check("delete", K::sceKernelDeleteEqueue(q1) == kOk && K::sceKernelDeleteEqueue(q2) == kOk, "");
    g.Check("delete again -> EBADF", K::sceKernelDeleteEqueue(q1) == kEbadf, "");
    g.Check("wait on a deleted queue -> EBADF", Wait(q1, 0).rc == kEbadf, "");
    g.Check("trigger on a deleted queue -> EBADF", K::sceKernelTriggerUserEvent(q1, 7, nullptr) == kEbadf, "");
}

void TestWakeAndDelete(TestGroup& g) {
    // Q4: two waiters, one level-triggered event: both must wake (notify_one woke one of them).
    const auto q = Create(g, "sspc_eq_two_waiters");
    K::sceKernelAddUserEvent(q, 3);
    WaitResult a, b;
    std::thread ta([&] { a = Wait(q, 5'000'000); });
    std::thread tb([&] { b = Wait(q, 5'000'000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    K::sceKernelTriggerUserEvent(q, 3, nullptr);
    ta.join();
    tb.join();
    g.Check("two waiters, one trigger: both wake at once (no lost wake-up)",
            a.rc == kOk && b.rc == kOk && a.ms < 2000 && b.ms < 2000,
            fmt::format("{} | {}", Describe(a), Describe(b)));
    K::sceKernelDeleteEqueue(q);

    // Q3: the queue deleted while a thread waits on it: the waiter keeps the object, ends at its time-out.
    const auto d = Create(g, "sspc_eq_delete_waiting");
    WaitResult w;
    std::thread tw([&] { w = Wait(d, 300'000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int del = K::sceKernelDeleteEqueue(d);
    tw.join();
    g.Check("deleted while a thread waits: delete OK, the waiter ends at its time-out (the object outlives it)",
            del == kOk && w.rc == kEtimedout && w.ms >= 240 && w.ms < 3000,
            fmt::format("delete {:#x}, {}", static_cast<u32>(del), Describe(w)));
    auto* owner = Q::GetOwner();
    g.Check("deleted: guest wait -> EBADF, the contract finds nothing",
            Wait(d, 0).rc == kEbadf && (owner == nullptr || !owner->Find(d)), "");
}

void TestContract(TestGroup& g) {
    auto* owner = Q::GetOwner();
    g.Check("the owner of the game's queues is installed (InstallKernelHost)", owner != nullptr, "");
    if (owner == nullptr) {
        return;
    }
    g.Check("find: an unknown handle -> none", !owner->Find(kNoQueue), "");
    const auto eq = Create(g, "sspc_eq_contract");
    const auto id = owner->Find(eq);
    g.Check("find: a queue -> its handle and a serial", id && id->handle == eq && id->serial != 0,
            id ? fmt::format("{} #{}", id->handle, id->serial) : std::string("none"));
    if (!id) {
        return;
    }
    void* const udata = reinterpret_cast<void*>(0xE09ull);
    g.Check("GPU event: add (end of pipe 0x40)", owner->AddEvent(*id, 0x40, Q::Filter::GraphicsCore, udata), "");
    g.Check("GPU event: trigger", owner->Trigger(*id, 0x40, Q::Filter::GraphicsCore, 0x40), "");
    auto r = Wait(eq, 0);
    g.Check("GPU event: the guest's wait returns ident, filter, data and user data",
            r.rc == kOk && r.out == 1 && r.ev.ident == 0x40 &&
                r.ev.filter == K::OrbisKernelEvent::Filter::GraphicsCore && r.ev.data == 0x40 && r.ev.udata == udata,
            Describe(r));
    r = Wait(eq, 0);
    g.Check("GPU event: cleared once returned", r.rc == kEtimedout, Describe(r));
    g.Check("trigger of an event that is not registered -> false",
            !owner->Trigger(*id, 0x41, Q::Filter::GraphicsCore, 0x41), "");

    // VideoOut: the hint's upper bits kept, a time stamp and a counter of triggers since the last wait below.
    constexpr u64 kFlip = 6;
    g.Check("flip event: add", owner->AddEvent(*id, kFlip, Q::Filter::VideoOut, nullptr), "");
    const u64 hint = kFlip | (0x1234ull << 16);
    owner->Trigger(*id, kFlip, Q::Filter::VideoOut, hint);
    owner->Trigger(*id, kFlip, Q::Filter::VideoOut, hint);
    r = Wait(eq, 0);
    g.Check("flip event: flip argument in bits 16.., two triggers counted in bits 12..15",
            r.rc == kOk && r.ev.ident == kFlip && (r.ev.data >> 16) == 0x1234 && ((r.ev.data >> 12) & 0xF) == 2,
            Describe(r));

    g.Check("user event through the contract: add and trigger with user data",
            owner->AddEvent(*id, 9, Q::Filter::User, nullptr) && owner->Trigger(*id, 9, Q::Filter::User, 0xAB), "");
    r = Wait(eq, 0);
    g.Check("user event through the contract: the guest gets the user data",
            r.rc == kOk && r.ev.ident == 9 && r.ev.udata == reinterpret_cast<void*>(0xABull), Describe(r));
    g.Check("remove: true, then false; a removed event is not triggered",
            owner->RemoveEvent(*id, 9, Q::Filter::User) && !owner->RemoveEvent(*id, 9, Q::Filter::User) &&
                !owner->Trigger(*id, 9, Q::Filter::User, 1),
            "");

    // Identity: the deleted queue's id names nothing, also when a new queue gets the same handle.
    K::sceKernelDeleteEqueue(eq);
    g.Check("deleted: find -> none, add/trigger by the old id -> false",
            !owner->Find(eq) && !owner->AddEvent(*id, 1, Q::Filter::User, nullptr) &&
                !owner->Trigger(*id, 0x40, Q::Filter::GraphicsCore, 0x40),
            "");
    const auto again = Create(g, "sspc_eq_contract_again");
    const auto id2 = owner->Find(again);
    if (id2) {
        owner->AddEvent(*id2, 0x40, Q::Filter::GraphicsCore, nullptr);
    }
    const bool stale = owner->Trigger(*id, 0x40, Q::Filter::GraphicsCore, 0x40);
    r = Wait(again, 0);
    g.Check(fmt::format("a new queue {} the old handle: another serial, the old id delivers nothing into it",
                        again == eq ? "reusing" : "not reusing"),
            id2 && id2->serial != id->serial && !stale && r.rc == kEtimedout,
            fmt::format("handles {} {}, serials {} {}, {}", eq, again, id->serial, id2 ? id2->serial : 0,
                        Describe(r)));
    K::sceKernelDeleteEqueue(again);
}

void TestRaces(TestGroup& g) {
    auto* owner = Q::GetOwner();
    if (owner == nullptr) {
        return;
    }
    // Producers on a live queue while other threads create and delete queues and look handles up.
    const auto eq = Create(g, "sspc_eq_race");
    const auto id = owner->Find(eq);
    if (!id) {
        return;
    }
    owner->AddEvent(*id, 1, Q::Filter::GraphicsCore, nullptr);
    std::atomic<u64> delivered{0}, refused{0}, churn_ok{0}, churn_bad{0}, polls{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    const auto t0 = Clock::now();
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            for (int n = 0; n < 20000; ++n) {
                (owner->Trigger(*id, 1, Q::Filter::GraphicsCore, 1) ? delivered : refused)++;
            }
        });
    }
    for (int i = 0; i < 2; ++i) {
        threads.emplace_back([&, i] {
            for (int n = 0; n < 1500; ++n) {
                K::OrbisKernelEqueue q = 0;
                const auto name = fmt::format("sspc_churn{}_{}", i, n % 10);
                const bool ok =
                    K::sceKernelCreateEqueue(&q, name.c_str()) == kOk && K::sceKernelDeleteEqueue(q) == kOk;
                (ok ? churn_ok : churn_bad)++;
            }
        });
    }
    threads.emplace_back([&] {
        while (!stop.load()) {
            for (K::OrbisKernelEqueue h = 1; h < 64; ++h) {
                (void)owner->Find(h);
            }
            (void)Wait(eq, 0);
            polls++;
        }
    });
    for (size_t i = 0; i + 1 < threads.size(); ++i) {
        threads[i].join();
    }
    stop.store(true);
    threads.back().join();
    g.Check("4 producers x 20000 triggers on a live queue while 2 threads create/delete 3000 queues and one "
            "looks handles up and polls: every trigger delivered, every create/delete OK",
            delivered.load() == 80000 && refused.load() == 0 && churn_ok.load() == 3000 && churn_bad.load() == 0,
            fmt::format("delivered {}, refused {}, churn {}/{}, polls {}, {:.0f} ms", delivered.load(),
                        refused.load(), churn_ok.load(), churn_bad.load(), polls.load(), Ms(t0)));

    // The queue deleted while producers deliver: later triggers are refused, nothing crashes.
    std::atomic<bool> go{true};
    delivered = 0;
    refused = 0;
    threads.clear();
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&] {
            while (go.load()) {
                (owner->Trigger(*id, 1, Q::Filter::GraphicsCore, 1) ? delivered : refused)++;
            }
        });
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const int del = K::sceKernelDeleteEqueue(eq);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    go.store(false);
    for (auto& t : threads) {
        t.join();
    }
    g.Check("deleted while 4 producers deliver: delete OK, triggers delivered before and refused after",
            del == kOk && delivered.load() > 0 && refused.load() > 0 && !owner->Find(eq),
            fmt::format("delete {:#x}, delivered {}, refused {}", static_cast<u32>(del), delivered.load(),
                        refused.load()));
}

void TestGnmInterrupt(TestGroup& g) {
    // sceGnmAddEqEvent registers a GPU interrupt handler that delivers into the queue (Q3: it held a pointer to
    // the queue object). The handler of a deleted queue must deliver nothing and touch nothing.
    constexpr u64 kEop = static_cast<u64>(Platform::InterruptId::GfxEop);
    auto* irq = Platform::IrqC::Instance();
    const auto q = Create(g, "sspc_eq_gnm");
    void* const udata = reinterpret_cast<void*>(0x6E0ull);
    g.Check("sceGnmAddEqEvent(end of pipe)", Libraries::GnmDriver::sceGnmAddEqEvent(q, kEop, udata) == kOk, "");
    irq->Signal(Platform::InterruptId::GfxEop);
    auto r = Wait(q, 0);
    g.Check("an end-of-pipe interrupt delivers into the queue",
            r.rc == kOk && r.ev.ident == kEop && r.ev.filter == K::OrbisKernelEvent::Filter::GraphicsCore &&
                r.ev.data == kEop && r.ev.udata == udata,
            Describe(r));
    g.Check("the queue deleted without sceGnmDeleteEqEvent", K::sceKernelDeleteEqueue(q) == kOk, "");
    irq->Signal(Platform::InterruptId::GfxEop); // was a use-after-free in the GPU thread
    g.Check("an interrupt after that: its stale handler delivers nothing and touches no freed queue", true, "");
    const auto q2 = Create(g, "sspc_eq_gnm2");
    g.Check(fmt::format("a new queue (handle reused: {}) registers the same interrupt (another handler key)",
                        q2 == q ? "yes" : "no"),
            Libraries::GnmDriver::sceGnmAddEqEvent(q2, kEop, nullptr) == kOk, "");
    irq->Signal(Platform::InterruptId::GfxEop);
    r = Wait(q2, 0);
    g.Check("the interrupt reaches the new queue", r.rc == kOk && r.ev.ident == kEop, Describe(r));
    g.Check("sceGnmDeleteEqEvent", Libraries::GnmDriver::sceGnmDeleteEqEvent(q2, kEop) == kOk, "");
    irq->Signal(Platform::InterruptId::GfxEop);
    r = Wait(q2, 0);
    g.Check("after sceGnmDeleteEqEvent the interrupt delivers nothing", r.rc == kEtimedout, Describe(r));
    g.Check("sceGnmAddEqEvent on an unknown queue -> EBADF",
            Libraries::GnmDriver::sceGnmAddEqEvent(kNoQueue, kEop, nullptr) == kEbadf, "");
    K::sceKernelDeleteEqueue(q2);
}

} // namespace

std::vector<TestGroup> TestEventQueues() {
    std::vector<TestGroup> out(3);
    out[0].name = "EventQueues";
    out[1].name = "EventQueues(mt)";
    out[2].name = "EventQueues(gnm)";
    TestGuestApi(out[0]);
    TestContract(out[0]);
    TestWakeAndDelete(out[1]);
    TestRaces(out[1]);
    TestGnmInterrupt(out[2]);
    return out;
}

} // namespace SecondSon::Bridge
