// SecondSonPC — the owner of the game's event queues behind the contract of event_queues.h (P3.6).
#include "runtime/kernel/event_queues.h"

#include <atomic>

namespace SecondSon::Kernel::EventQueues {

namespace {
std::atomic<Owner*> g_owner{nullptr};
}

void SetOwner(Owner* owner) {
    g_owner.store(owner, std::memory_order_release);
}

Owner* GetOwner() {
    return g_owner.load(std::memory_order_acquire);
}

} // namespace SecondSon::Kernel::EventQueues
