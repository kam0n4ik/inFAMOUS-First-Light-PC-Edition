// SecondSonPC — libSceUserService (NATIVE). Behaviour of the five functions the game imports
// follows the Phase 0 oracle (third_party/shadPS4/src/core/libraries/system/userservice.cpp
// @ 0fe263a); unlike the oracle the event queue is locked (logins arrive on the input thread).
#include "runtime/user/user_service.h"

#include <array>
#include <mutex>
#include <queue>

#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::UserService {

namespace {

constexpr s32 ORBIS_OK = 0;
constexpr s32 ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT = static_cast<s32>(0x80960005);
constexpr s32 ORBIS_USER_SERVICE_ERROR_NO_EVENT = static_cast<s32>(0x80960007);

/// OrbisUserServiceEvent
struct Event {
    EventType type;
    s32 user_id;
};
static_assert(sizeof(Event) == 8);

/// OrbisUserServiceLoginUserIdList
struct LoginUserIdList {
    s32 user_id[kMaxLoginUsers];
};

std::mutex g_mutex;
Host* g_host = nullptr;
std::queue<Event> g_events;
std::array<s32, kMaxLoginUsers> g_slots{kInvalidUserId, kInvalidUserId, kInvalidUserId, kInvalidUserId};

bool ValidSlot(s32 player_index) {
    return player_index >= 1 && player_index <= kMaxLoginUsers;
}

SSPC_SYSV_ABI s32 Initialize(const void* /*params*/) {
    SSPC_LOG(User, Info, "sceUserServiceInitialize");
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 Terminate() {
    SSPC_LOG(User, Info, "sceUserServiceTerminate");
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetEvent(Event* event) {
    if (event == nullptr) {
        return ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT;
    }
    Event taken;
    Host* host;
    {
        std::scoped_lock lock{g_mutex};
        if (g_events.empty()) {
            return ORBIS_USER_SERVICE_ERROR_NO_EVENT;
        }
        taken = g_events.front();
        g_events.pop();
        host = g_host;
    }
    *event = taken;
    if (host != nullptr) {
        host->EventDelivered(taken.type, taken.user_id);
    }
    SSPC_LOG(User, Info, "event taken by the game: {} user {}",
             taken.type == EventType::Login ? "Login" : "Logout", taken.user_id);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetInitialUser(s32* user_id) {
    if (user_id == nullptr) {
        return ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT;
    }
    *user_id = UserIdOfSlot(1);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetLoginUserIdList(LoginUserIdList* list) {
    if (list == nullptr) {
        return ORBIS_USER_SERVICE_ERROR_INVALID_ARGUMENT;
    }
    std::scoped_lock lock{g_mutex};
    for (s32 i = 0; i < kMaxLoginUsers; ++i) {
        list->user_id[i] = g_slots[i];
    }
    return ORBIS_OK;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"j3YMu1MVNNo", "libSceUserService", "libSceUserService", "sceUserServiceInitialize", Fn(&Initialize), nullptr, "user"},
    {"bwFjS+bX9mA", "libSceUserService", "libSceUserService", "sceUserServiceTerminate", Fn(&Terminate), nullptr, "user"},
    {"yH17Q6NWtVg", "libSceUserService", "libSceUserService", "sceUserServiceGetEvent", Fn(&GetEvent), nullptr, "user"},
    {"CdWp0oHWGr0", "libSceUserService", "libSceUserService", "sceUserServiceGetInitialUser", Fn(&GetInitialUser), nullptr, "user"},
    {"fPhymKNvK-A", "libSceUserService", "libSceUserService", "sceUserServiceGetLoginUserIdList", Fn(&GetLoginUserIdList), nullptr, "user"},
};

} // namespace

void SetHost(Host* host) {
    std::scoped_lock lock{g_mutex};
    g_host = host;
}

void OnLogin(s32 player_index) {
    if (!ValidSlot(player_index)) {
        return;
    }
    const s32 id = UserIdOfSlot(player_index);
    std::scoped_lock lock{g_mutex};
    // Same rule as the oracle: a second login of the same user into the same slot (the initial
    // user is logged in at boot, then a controller connects) is not an event.
    if (g_slots[player_index - 1] == id) {
        return;
    }
    g_slots[player_index - 1] = id;
    g_events.push({EventType::Login, id});
    SSPC_LOG(User, Info, "user {} logged in (slot {})", id, player_index);
}

void OnLogout(s32 player_index) {
    if (!ValidSlot(player_index)) {
        return;
    }
    const s32 id = UserIdOfSlot(player_index);
    std::scoped_lock lock{g_mutex};
    g_slots[player_index - 1] = kInvalidUserId;
    g_events.push({EventType::Logout, id});
    SSPC_LOG(User, Info, "user {} logged out (slot {})", id, player_index);
}

std::span<const Runtime::Export> UserServiceExports() {
    return kExports;
}

} // namespace SecondSon::UserService
