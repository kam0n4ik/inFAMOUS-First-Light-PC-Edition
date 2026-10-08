// SecondSonPC — libSceUserService (NATIVE). One local user per player slot; the user of slot 1 is
// the initial user. Logins come from the input layer (controllers, BRIDGE until Phase 5) through
// the host; the game sees them as Login/Logout events.
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::UserService {

constexpr s32 kMaxLoginUsers = 4;
constexpr s32 kInvalidUserId = -1;
/// User ids of slots 1..4: 1000..1003 (the ids of the Phase 0 oracle; save data lives under
/// userdata\home\<id>, and the BRIDGE Pad looks controllers up by the same ids).
constexpr s32 kFirstUserId = 1000;

enum class EventType : s32 {
    Login = 0,
    Logout = 1,
};

/// What the native UserService needs from subsystems that are still BRIDGE.
class Host {
public:
    virtual ~Host() = default;
    /// The game took an event (NpManager, BRIDGE, follows the sign-in state of the user).
    virtual void EventDelivered(EventType type, s32 user_id) = 0;
};
void SetHost(Host* host);

/// From the input layer: player slot `player_index` (1..4) logged in / out.
void OnLogin(s32 player_index);
void OnLogout(s32 player_index);

constexpr s32 UserIdOfSlot(s32 player_index) {
    return kFirstUserId + player_index - 1;
}

std::span<const Runtime::Export> UserServiceExports();

} // namespace SecondSon::UserService
