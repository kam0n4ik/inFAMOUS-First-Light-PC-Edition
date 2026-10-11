// SecondSonPC — Gpu::GuestMemory (src/runtime/gpu/gpu_memory.h) over the bridge's Core::GpuMemory (P4.1.3): the selected
// guest memory manager (the bridge's or SecondSonPC's with --native memory) seen through SecondSonPC's own contract.
//
// Notifications: while a SecondSonPC listener holds the claim, one forwarder is registered with the manager. The
// manager's single listener slot is never shared — a listener registered directly (the bridge's rasterizer) makes
// SetListener refuse. The bridge's manager calls its listener on every map/unmap without a null check
// (core/memory.cpp:641), so the slot is never left empty by this adapter: unregistering puts a listener that ignores
// everything in its place. The forwarder's target is atomic: a notification already running on another thread sees the
// old or the new target, never a torn one; it may still reach the old listener after SetListener returns (the
// manager's notifications are not synchronized with SetListener), so a listener must stay alive while guest memory
// can still change (P4.1: the owner registers at start-up and keeps it).
#include <atomic>
#include <limits>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "bridge/shadps4/gpu_memory_host.h"
#include "core/address_space.h"
#include "common/signal_context.h"
#include "core/gpu_memory.h"
#include "core/signals.h"
#include "runtime/gpu/gpu_memory.h"

namespace SecondSon::Bridge {

namespace {

class Forwarder final : public Core::GpuMemoryListener {
public:
    std::atomic<Gpu::MemoryListener*> target{nullptr};
    void OnGpuUnmap(VAddr addr, u64 size) override {
        if (auto* t = target.load(std::memory_order_acquire)) {
            t->OnUnmap(addr, size);
        }
    }
    void OnGpuRegister(VAddr addr, u64 size) override {
        if (auto* t = target.load(std::memory_order_acquire)) {
            t->OnRegister(addr, size);
        }
    }
    void OnGpuMap(VAddr addr, u64 size) override {
        if (auto* t = target.load(std::memory_order_acquire)) {
            t->OnMap(addr, size);
        }
    }
    void OnGuestWrite(VAddr addr, u64 size) override {
        if (auto* t = target.load(std::memory_order_acquire)) {
            t->OnGuestWrite(addr, size);
        }
    }
};

/// What the slot holds when SecondSonPC's listener leaves it (never empty, see above).
class Nobody final : public Core::GpuMemoryListener {
public:
    void OnGpuUnmap(VAddr, u64) override {}
    void OnGpuRegister(VAddr, u64) override {}
    void OnGpuMap(VAddr, u64) override {}
    void OnGuestWrite(VAddr, u64) override {}
};

class BridgeGuestMemory final : public Gpu::GuestMemory {
public:
    bool SetListener(Gpu::MemoryListener* listener, Gpu::BackendKind owner, std::string* error) override {
        std::scoped_lock lock{mutex_};
        const auto fail = [&](std::string text) {
            if (error != nullptr) {
                *error = std::move(text);
            }
            return false;
        };
        if (owner == Gpu::BackendKind::None || Gpu::Owner(Gpu::Resource::MemoryListener) != owner) {
            return fail(fmt::format("the {} backend does not own the guest memory notifications (owner: {})",
                                    Gpu::BackendName(owner),
                                    Gpu::BackendName(Gpu::Owner(Gpu::Resource::MemoryListener))));
        }
        auto& gm = Core::GetGpuMemory();
        if (listener == nullptr) {
            if (!registered_) {
                return true;
            }
            Core::GpuMemoryListener* const was = gm.SetListener(&nobody_);
            if (was != &forwarder_) { // someone replaced the forwarder: give the slot back as it was
                gm.SetListener(was);
                return fail("the guest memory manager's listener is not SecondSonPC's forwarder; left as it is");
            }
            forwarder_.target.store(nullptr, std::memory_order_release);
            registered_ = false;
            return true;
        }
        if (registered_) {
            forwarder_.target.store(listener, std::memory_order_release); // the same owner changes its listener
            return true;
        }
        forwarder_.target.store(listener, std::memory_order_release);
        Core::GpuMemoryListener* const was = gm.SetListener(&forwarder_);
        if (was != nullptr && was != &nobody_) { // another listener (the bridge's rasterizer): never replace it
            gm.SetListener(was);
            forwarder_.target.store(nullptr, std::memory_order_release);
            return fail("the guest memory manager already has a listener (the bridge's renderer); a second one is "
                        "refused");
        }
        registered_ = true;
        return true;
    }
    void CopySparse(u64 addr, u8* dest, u64 size) override {
        Core::GetGpuMemory().CopySparseMemory(addr, dest, size);
    }
    bool TryWriteBacking(void* address, const void* data, u64 size) override {
        return Core::GetGpuMemory().TryWriteBacking(address, data, size);
    }
    void ProtectTracking(u64 addr, u64 size, Gpu::TrackingProtection protection) override {
        Core::MemoryPermission perms = Core::MemoryPermission::None;
        if (protection == Gpu::TrackingProtection::Read) {
            perms = Core::MemoryPermission::Read;
        } else if (protection == Gpu::TrackingProtection::ReadWrite) {
            perms = Core::MemoryPermission::ReadWrite;
        }
        Core::GetGpuMemory().ProtectTracking(addr, size, perms);
    }
    u64 SystemReservedEnd() override {
        return Core::GetGpuMemory().SystemReservedEnd();
    }
    bool BackingRun(u64 addr, u8** host, u64* run_begin, u64* run_size) override {
        VAddr begin = 0;
        const bool ok = Core::GetGpuMemory().BackingRun(addr, host, &begin, run_size);
        *run_begin = begin;
        return ok;
    }

protected:
    u64 ClampLargeRange(u64 addr, u64 size) override {
        return Core::GetGpuMemory().ClampRangeSize(addr, size);
    }

private:
    std::mutex mutex_;
    Forwarder forwarder_;
    Nobody nobody_;
    bool registered_ = false;
};

BridgeGuestMemory g_memory;

} // namespace

void InstallGpuGuestMemory() {
    Gpu::InstallGuestMemory(&g_memory);
}

} // namespace SecondSon::Bridge

namespace SecondSon::Gpu {

namespace {
std::atomic<TrackingFaultHandler> g_tracking_handler{nullptr};

bool TrackingFault(void* context, void* fault_address) {
    const TrackingFaultHandler h = g_tracking_handler.load(std::memory_order_acquire);
    return h != nullptr && h(reinterpret_cast<u64>(fault_address), Common::IsWriteError(context));
}
} // namespace

// P4.4: SecondSonPC's backend's write tracking, first in the bridge's access violation dispatch (where SecondSonPC's
// own exception dispatcher sends access violations too); the bridge's page manager is not running in that mode.
void InstallTrackingFaultHandler(TrackingFaultHandler handler) {
    if (g_tracking_handler.exchange(handler, std::memory_order_acq_rel) == nullptr) {
        Core::Signals::Instance()->RegisterAccessViolationHandler(&TrackingFault, std::numeric_limits<u32>::min());
    }
}

} // namespace SecondSon::Gpu
