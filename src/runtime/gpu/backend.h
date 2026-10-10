// SecondSonPC — the guest GPU's owners (Phase 4, P4.1.3; docs/PHASE4_P41_GPU_FOUNDATION.md §5).
//
// One graphics backend runs the game: the bridge's (shadPS4's Vulkan renderer) or, from P4.2 on, SecondSonPC's. It is
// chosen once at start-up, before the first GnmDriver/VideoOut call, like the guest memory manager (P2.3.4). Each guest
// GPU resource below has exactly one owner for as long as it is claimed; a second claim is refused and reported, never
// silently replaced (the bridge's own GpuMemory::SetListener replaces the previous listener without a word — two page
// trackers would undo each other's protection). Nothing falls back from one backend to the other at run time.
//
// The registry holds no pointers into a backend: it only records who owns what, so no object's lifetime depends on it.
// Thread-safe; claims are rare (start-up, a backend's start/stop).
#pragma once

#include <string>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Gpu {

enum class BackendKind : u8 {
    None,   ///< nobody (not chosen yet / released)
    Bridge, ///< shadPS4's Vulkan renderer (Liverpool, rasterizer, presenter): the only one that runs the game in P4.1
    Sspc,   ///< SecondSonPC's own backend (P4.1: the Vulkan foundation; no guest command executes on it yet)
};

/// The guest GPU's resources a backend owns.
enum class Resource : u8 {
    CommandStream,  ///< the game's command buffers (sceGnmSubmit*, DingDong) and the PM4 they carry
    MemoryListener, ///< the guest memory manager's GPU notifications (map, unmap, register, guest writes)
    WriteTracking,  ///< page protection of guest memory for CPU write tracking, and its access violations
    Presentation,   ///< VideoOut buffers, flips, vblank, the window's swapchain
    Count,
};

std::string_view BackendName(BackendKind kind);
std::string_view ResourceName(Resource resource);

/// Records `owner` as the owner of `resource`. true if it was free or already `owner`'s; false if another backend owns
/// it: `error` then names the holder (the caller reports it), or without `error` the refusal is logged. `owner` must
/// not be None.
bool Claim(Resource resource, BackendKind owner, std::string* error = nullptr);

/// Gives `resource` back. true if `owner` held it; false (nothing changes) if someone else does or nobody.
bool Release(Resource resource, BackendKind owner);

BackendKind Owner(Resource resource);

/// The backend chosen for the game, once per process: true for the first choice (or the same again); false with
/// `error` for a different second choice. The choice does not claim anything: the chosen backend claims its resources
/// when it starts.
bool SelectBackend(BackendKind kind, std::string* error = nullptr);
BackendKind SelectedBackend();

/// A backend's life: Created → Started (its resources claimed, device up) → Stopped (released). A failed start leaves
/// it Failed with nothing claimed and nothing alive. Stop is idempotent; the destructor stops.
enum class BackendState : u8 { Created, Started, Stopped, Failed };
std::string_view BackendStateName(BackendState state);

class Backend {
public:
    virtual ~Backend() = default;
    virtual BackendKind Kind() const = 0;
    virtual BackendState State() const = 0;
    /// Brings the backend up and claims its resources (all or none). false with `error`: nothing claimed, nothing alive.
    virtual bool Start(std::string& error) = 0;
    virtual void Stop() = 0;
};

namespace Test {
/// Forgets every claim and the selection (self-tests only; never while a backend runs).
void ResetRegistry();
} // namespace Test

} // namespace SecondSon::Gpu
