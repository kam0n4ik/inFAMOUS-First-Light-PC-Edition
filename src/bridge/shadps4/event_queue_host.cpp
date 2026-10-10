// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY: the game's event queues (runtime/kernel/event_queues.h) over their owner, the
// bridge's libkernel (Phase 3, P3.6; docs/PHASE3_P36_BRIDGE_ISOLATION.md §3). Each call is the bridge's
// delivery-by-identity API of patch 0025 (the one its GnmDriver and VideoOut use): a lookup in the queue table
// under its lock, the queue held for the call. No state of its own.
#include "bridge/shadps4/loader_host.h"
#include "core/libraries/kernel/equeue.h"
#include "runtime/kernel/event_queues.h"

namespace SecondSon::Bridge {

namespace {

namespace Q = Kernel::EventQueues;
using Libraries::Kernel::EqueueId;
using Libraries::Kernel::OrbisKernelEvent;

static_assert(static_cast<s16>(Q::Filter::User) == OrbisKernelEvent::Filter::User);
static_assert(static_cast<s16>(Q::Filter::VideoOut) == OrbisKernelEvent::Filter::VideoOut);
static_assert(static_cast<s16>(Q::Filter::GraphicsCore) == OrbisKernelEvent::Filter::GraphicsCore);

EqueueId ToBridge(Q::QueueId queue) {
    return {queue.handle, queue.serial};
}

class BridgeEventQueues final : public Q::Owner {
public:
    std::optional<Q::QueueId> Find(s64 handle) override {
        const auto id = Libraries::Kernel::FindEqueueId(handle);
        return id ? std::optional{Q::QueueId{id->handle, id->serial}} : std::nullopt;
    }

    bool AddEvent(Q::QueueId queue, u64 ident, Q::Filter filter, void* udata) override {
        Libraries::Kernel::EqueueEvent event{};
        event.event.ident = ident;
        event.event.filter = static_cast<OrbisKernelEvent::Filter>(filter);
        event.event.flags = OrbisKernelEvent::Flags::Add;
        event.event.udata = udata;
        return Libraries::Kernel::EqueueAddEvent(ToBridge(queue), event);
    }

    bool RemoveEvent(Q::QueueId queue, u64 ident, Q::Filter filter) override {
        return Libraries::Kernel::EqueueRemoveEvent(ToBridge(queue), ident, static_cast<s16>(filter));
    }

    bool Trigger(Q::QueueId queue, u64 ident, Q::Filter filter, u64 data) override {
        return Libraries::Kernel::EqueueTriggerEvent(ToBridge(queue), ident, static_cast<s16>(filter),
                                                      reinterpret_cast<void*>(data));
    }
};

} // namespace

void InstallEventQueueHost() {
    static BridgeEventQueues owner;
    Q::SetOwner(&owner);
}

} // namespace SecondSon::Bridge
