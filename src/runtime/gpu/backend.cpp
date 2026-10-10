// SecondSonPC — the guest GPU's owners (P4.1.3). See backend.h.
#include "runtime/gpu/backend.h"

#include <array>
#include <atomic>
#include <mutex>

#include "runtime/gpu/gpu_memory.h"

#include <fmt/format.h>

#include "base/log.h"

namespace SecondSon::Gpu {

namespace {

std::mutex g_mutex;
std::array<BackendKind, static_cast<size_t>(Resource::Count)> g_owner{};
BackendKind g_selected = BackendKind::None;

} // namespace

std::string_view BackendName(BackendKind kind) {
    switch (kind) {
    case BackendKind::None:
        return "none";
    case BackendKind::Bridge:
        return "bridge (shadPS4 Vulkan renderer)";
    case BackendKind::Sspc:
        return "SecondSonPC";
    }
    return "?";
}

std::string_view ResourceName(Resource resource) {
    switch (resource) {
    case Resource::CommandStream:
        return "command stream";
    case Resource::MemoryListener:
        return "guest memory notifications";
    case Resource::WriteTracking:
        return "CPU write tracking";
    case Resource::Presentation:
        return "presentation";
    case Resource::Count:
        break;
    }
    return "?";
}

std::string_view BackendStateName(BackendState state) {
    switch (state) {
    case BackendState::Created:
        return "created";
    case BackendState::Started:
        return "started";
    case BackendState::Stopped:
        return "stopped";
    case BackendState::Failed:
        return "failed";
    }
    return "?";
}

bool Claim(Resource resource, BackendKind owner, std::string* error) {
    if (resource >= Resource::Count || owner == BackendKind::None) {
        if (error != nullptr) {
            *error = "invalid claim (no such resource, or no owner)";
        }
        return false;
    }
    std::scoped_lock lock{g_mutex};
    auto& slot = g_owner[static_cast<size_t>(resource)];
    if (slot != BackendKind::None && slot != owner) {
        const auto text = fmt::format("the guest GPU's {} belongs to the {} backend; the {} backend cannot take it",
                                      ResourceName(resource), BackendName(slot), BackendName(owner));
        if (error != nullptr) {
            *error = text; // the caller reports it
        } else {
            SSPC_LOG(GPU, Error, "{}", text);
        }
        return false;
    }
    if (slot == BackendKind::None) {
        SSPC_LOG(GPU, Info, "the guest GPU's {}: owner {}", ResourceName(resource), BackendName(owner));
    }
    slot = owner;
    return true;
}

bool Release(Resource resource, BackendKind owner) {
    if (resource >= Resource::Count || owner == BackendKind::None) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    auto& slot = g_owner[static_cast<size_t>(resource)];
    if (slot != owner) {
        return false;
    }
    slot = BackendKind::None;
    SSPC_LOG(GPU, Info, "the guest GPU's {}: released by {}", ResourceName(resource), BackendName(owner));
    return true;
}

BackendKind Owner(Resource resource) {
    if (resource >= Resource::Count) {
        return BackendKind::None;
    }
    std::scoped_lock lock{g_mutex};
    return g_owner[static_cast<size_t>(resource)];
}

bool SelectBackend(BackendKind kind, std::string* error) {
    if (kind == BackendKind::None) {
        if (error != nullptr) {
            *error = "no backend";
        }
        return false;
    }
    std::scoped_lock lock{g_mutex};
    if (g_selected != BackendKind::None && g_selected != kind) {
        if (error != nullptr) {
            *error = fmt::format("the {} backend was chosen already; the {} backend cannot replace it in this process",
                                 BackendName(g_selected), BackendName(kind));
        }
        return false;
    }
    g_selected = kind;
    return true;
}

BackendKind SelectedBackend() {
    std::scoped_lock lock{g_mutex};
    return g_selected;
}

namespace {
std::atomic<GuestMemory*> g_guest_memory{nullptr};
} // namespace

GuestMemory* GetGuestMemory() {
    return g_guest_memory.load(std::memory_order_acquire);
}

void InstallGuestMemory(GuestMemory* memory) {
    g_guest_memory.store(memory, std::memory_order_release);
}

namespace Test {

void ResetRegistry() {
    std::scoped_lock lock{g_mutex};
    g_owner.fill(BackendKind::None);
    g_selected = BackendKind::None;
}

} // namespace Test

} // namespace SecondSon::Gpu
