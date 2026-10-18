// SecondSonPC — the game's event queues as a graphics backend delivers into them (Phase 3, P3.6;
// docs/PHASE3_P36_BRIDGE_ISOLATION.md §3).
//
// The queues' owner is the bridge's libkernel (equeue.cpp: sceKernelCreateEqueue, WaitEqueue, the user
// events); SecondSonPC keeps no second table. What a graphics backend needs from the owner — today the
// bridge's GnmDriver (end-of-pipe and compute RELEASE_MEM interrupts) and VideoOut (flip, vblank), in Phase 4
// SecondSonPC's own — is this contract: find the queue a guest handle names, register an event in it, remove
// it, trigger it from any thread. A queue is named by QueueId (handle + serial): a queue the game deleted, or
// a newer one that reuses its handle, receives nothing meant for the old one, and no call touches freed memory.
// Implemented over the bridge by src/bridge/shadps4/event_queue_host.cpp (patch 0025).
#pragma once

#include <optional>
#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Kernel::EventQueues {

/// The PS4's event filters a backend registers and triggers.
enum class Filter : s16 {
    User = -11,         ///< the game's own events (sceKernelAddUserEvent / TriggerUserEvent)
    VideoOut = -13,     ///< flip, vblank
    GraphicsCore = -14, ///< GPU interrupts: end of pipe, compute RELEASE_MEM
};

struct QueueId {
    s64 handle = 0;
    u64 serial = 0; ///< the queue object's; never reused

    bool operator==(const QueueId&) const = default;
};

class Owner {
public:
    virtual ~Owner() = default;

    /// The queue the guest handle names now. nullopt: none (the guest API answers EBADF / invalid event queue).
    virtual std::optional<QueueId> Find(s64 handle) = 0;

    /// Registers event (ident, filter) with the user data a wait returns; registering it again only replaces
    /// the user data (whether it is triggered stays). Events other than User are cleared when a wait returns
    /// them; User events stay triggered until removed. false: the queue is gone.
    virtual bool AddEvent(QueueId queue, u64 ident, Filter filter, void* udata) = 0;

    /// false: the queue is gone or has no such event.
    virtual bool RemoveEvent(QueueId queue, u64 ident, Filter filter) = 0;

    /// Marks (ident, filter) triggered and wakes every waiter of the queue; from any thread (GPU interrupt,
    /// present thread). `data`: GraphicsCore — the event's data; VideoOut — the hint (event id | flip argument
    /// << 16): the owner keeps its upper 48 bits and puts its time stamp (12 bits of the TSC) and a 4-bit
    /// trigger counter below; User — the user data. false: the queue is gone or the event is not registered.
    virtual bool Trigger(QueueId queue, u64 ident, Filter filter, u64 data) = 0;
};

/// Set once at start-up, before a graphics backend starts (InstallKernelHost); nullptr until then.
void SetOwner(Owner* owner);
Owner* GetOwner();

/// SecondSonPC's own queue table (runtime/kernel/equeue.cpp, group "kernel-equeue") as the owner: with its
/// exports bound, instead of the bridge's (InstallKernelHost chooses).
void InstallNativeOwner();
std::span<const Runtime::Export> NativeExports();

} // namespace SecondSon::Kernel::EventQueues
