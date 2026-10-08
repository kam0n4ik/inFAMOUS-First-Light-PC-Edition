// SecondSonPC — TEMPORARY bridge for the native UserService: logins/logouts that shadPS4's input
// layer (controllers, BRIDGE) reports through UserManagement reach SecondSonPC's UserService via
// the event sink of patch 0003; events the game takes are passed on to NpManager (BRIDGE).
#include <functional>
#include <memory>
#include <string> // np_manager.h does not include what it uses

#include "bridge/shadps4/loader_host.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/system/userservice.h"
#include "core/user_settings.h"
#include "runtime/user/user_service.h"
#include "base/log.h"

namespace SecondSon::Bridge {

namespace {

namespace US = Libraries::UserService;

class UserHostImpl final : public UserService::Host {
public:
    void EventDelivered(UserService::EventType type, s32 user_id) override {
        Libraries::Np::NpManager::NotifyNpStateFromUserServiceEvent(
            type == UserService::EventType::Login ? US::OrbisUserServiceEventType::Login
                                                  : US::OrbisUserServiceEventType::Logout,
            user_id);
    }
};

void Sink(const US::OrbisUserServiceEvent& e) {
    const auto* u = UserManagement.GetUserByID(e.userId);
    if (u == nullptr) {
        SSPC_LOG(User, Error, "input layer reported unknown user {}", e.userId);
        return;
    }
    if (u->user_id != UserService::UserIdOfSlot(u->player_index)) {
        // The BRIDGE Pad finds controllers by the shadPS4 user id; the game knows SecondSonPC's.
        SSPC_LOG(User, Error,
                 "shadPS4 user {} sits in slot {}, SecondSonPC expects user {} there "
                 "(runtime\\sspc\\user\\users.json was changed)",
                 u->user_id, u->player_index, UserService::UserIdOfSlot(u->player_index));
    }
    if (e.event == US::OrbisUserServiceEventType::Login) {
        UserService::OnLogin(u->player_index);
    } else {
        UserService::OnLogout(u->player_index);
    }
}

} // namespace

void InstallUserHost() {
    static UserHostImpl host;
    UserService::SetHost(&host);
    US::SetUserServiceEventSink(&Sink);
    SSPC_LOG(User, Info, "UserService NATIVE: logins from the input layer go to SecondSonPC");
}

} // namespace SecondSon::Bridge
