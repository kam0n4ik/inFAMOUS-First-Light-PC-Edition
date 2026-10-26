// SecondSonPC — libSceNpManager emulated online (NATIVE; group "np"; `--bridge np` sends it back to the oracle,
// third_party/shadPS4/src/core/libraries/np/np_manager.cpp with shadNet disabled).
// The PC Edition has no PlayStation Network, so the user is signed in to an emulated one: the state is SIGNED_IN from
// the start, presence is online, the account has a fixed online id, id and country, and the availability check passes.
// That is what the game needs before it asks the Paper Trail server (offline_services.cpp answers it locally). The
// request IDs, their results and the callback registrations behave as the console's API does; the state never
// changes, so sceNpCheckCallback has nothing to dispatch.
#include <array>
#include <cstring>
#include <mutex>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::Np {

namespace {

constexpr s32 kOk = 0;
constexpr s32 kErrInvalidArgument = static_cast<s32>(0x80550003);
constexpr s32 kErrUserNotFound = static_cast<s32>(0x80550007);
constexpr s32 kErrCallbackAlreadyRegistered = static_cast<s32>(0x80550008);
constexpr s32 kErrInvalidSize = static_cast<s32>(0x80550011);
constexpr s32 kErrAborted = static_cast<s32>(0x80550012);
constexpr s32 kErrRequestMax = static_cast<s32>(0x80550013);
constexpr s32 kErrRequestNotFound = static_cast<s32>(0x80550014);
constexpr s32 kErrInvalidId = static_cast<s32>(0x80550015);
constexpr s32 kErrCallbackMax = static_cast<s32>(0x8055001D);

constexpr s32 kUserIdInvalid = -1;
constexpr s32 kRequestIdOffset = 0x20000000; // the first request is 0x20000001
constexpr s32 kRequestLimit = 0x20;
constexpr size_t kCallbackMax = 8;

constexpr std::string_view kOnlineId = "t.me/cloudeluxe"; // SceNpOnlineId: at most 16 characters
constexpr u64 kAccountId = 0x5353504300000001ull;
static_assert(kOnlineId.size() >= 3 && kOnlineId.size() <= 16);

struct OnlineIdData {
    char data[16];
    char term;
    char dummy[3];
};
static_assert(sizeof(OnlineIdData) == 20);

struct CountryCode {
    char data[2];
    char term;
    char padding[1];
};
static_assert(sizeof(CountryCode) == 4);

enum class NpState : s32 { Unknown = 0, SignedOut = 1, SignedIn = 2 };
enum class PresenceStatus : s32 { Offline = 0, Online = 1 };

struct ContentRestriction {
    u64 size;
    s8 default_age_restriction;
    u8 padding[3];
    s32 age_restriction_count;
    const void* age_restriction;
};
static_assert(sizeof(ContentRestriction) == 24);

struct CreateAsyncRequestParameter {
    u64 size;
    u64 cpu_affinity_mask;
    s32 thread_priority;
    u8 padding[4];
};
static_assert(sizeof(CreateAsyncRequestParameter) == 24);

// ---- requests

enum class RequestState : u8 { None, Ready, Complete, Aborted };
struct Request {
    RequestState state = RequestState::None;
    bool async = false;
    s32 result = 0;
};

std::mutex g_mutex;
std::vector<Request> g_requests;
s32 g_active = 0;

s32 CreateRequest(bool async) {
    std::scoped_lock lock{g_mutex};
    if (g_active == kRequestLimit) {
        return kErrRequestMax;
    }
    size_t i = 0;
    while (i < g_requests.size() && g_requests[i].state != RequestState::None) {
        ++i;
    }
    if (i == g_requests.size()) {
        g_requests.push_back({});
    }
    g_requests[i] = {RequestState::Ready, async, 0};
    ++g_active;
    return static_cast<s32>(i) + kRequestIdOffset + 1;
}

/// The request of `id` (mutex held), or nullptr with the error in `err`.
Request* Find(s32 id, s32& err) {
    if (id == 0) {
        err = kErrInvalidArgument;
        return nullptr;
    }
    const s64 i = s64{id} - kRequestIdOffset - 1;
    if (g_active == 0 || i < 0 || i >= static_cast<s64>(g_requests.size()) ||
        g_requests[static_cast<size_t>(i)].state == RequestState::None) {
        err = kErrRequestNotFound;
        return nullptr;
    }
    Request& r = g_requests[static_cast<size_t>(i)];
    if (r.state == RequestState::Complete) {
        r.result = kErrInvalidArgument;
        err = kErrInvalidArgument;
        return nullptr;
    }
    if (r.state == RequestState::Aborted) {
        r.result = kErrAborted;
        err = kErrAborted;
        return nullptr;
    }
    return &r;
}

/// An async request returns OK at once; its result is read by sceNpPollAsync.
s32 Complete(Request& r, s32 result) {
    r.state = RequestState::Complete;
    r.result = result;
    return result != kOk && r.async ? kOk : result;
}

// ---- callbacks (registered; offline nothing is ever reported through them)

struct Callback {
    bool in_use = false;
    const void* func = nullptr;
    void* userdata = nullptr;
    s32 handle = 0;
};
std::array<Callback, kCallbackMax> g_state_cbs{}, g_presence_cbs{};
s32 g_presence_next = 0;

// ---- the API

SSPC_SYSV_ABI s32 CheckCallback() {
    return kOk; // (the emulated sign-in never changes)
}

SSPC_SYSV_ABI s32 CheckNpAvailabilityA(s32 req_id, s32 user_id) {
    if (user_id == kUserIdInvalid) {
        return kErrInvalidArgument;
    }
    std::scoped_lock lock{g_mutex};
    s32 err = 0;
    Request* r = Find(req_id, err);
    return r == nullptr ? err : Complete(*r, kOk);
}

SSPC_SYSV_ABI s32 CreateAsyncRequest(const CreateAsyncRequestParameter* param) {
    if (param == nullptr) {
        return kErrInvalidArgument;
    }
    if (param->size != sizeof(CreateAsyncRequestParameter)) {
        return kErrInvalidSize;
    }
    return CreateRequest(true);
}

SSPC_SYSV_ABI s32 DeleteRequest(s32 req_id) {
    std::scoped_lock lock{g_mutex};
    const s64 i = s64{req_id} - kRequestIdOffset - 1;
    if (g_active == 0 || i < 0 || i >= static_cast<s64>(g_requests.size()) ||
        g_requests[static_cast<size_t>(i)].state == RequestState::None) {
        return kErrRequestNotFound;
    }
    --g_active;
    g_requests[static_cast<size_t>(i)].state = RequestState::None;
    return kOk;
}

SSPC_SYSV_ABI s32 PollAsync(s32 req_id, s32* result) {
    if (result == nullptr) {
        return kErrInvalidArgument;
    }
    std::scoped_lock lock{g_mutex};
    const s64 i = s64{req_id} - kRequestIdOffset - 1;
    if (g_active == 0 || i < 0 || i >= static_cast<s64>(g_requests.size()) ||
        g_requests[static_cast<size_t>(i)].state == RequestState::None) {
        return kErrRequestNotFound;
    }
    const Request& r = g_requests[static_cast<size_t>(i)];
    if (!r.async || r.state == RequestState::Ready) {
        return kErrInvalidId;
    }
    *result = r.result;
    return kOk;
}

SSPC_SYSV_ABI s32 GetAccountCountryA(s32 user_id, CountryCode* country_code) {
    if (country_code == nullptr || user_id == kUserIdInvalid) {
        return kErrInvalidArgument;
    }
    *country_code = {{'u', 's'}, 0, {0}};
    return kOk;
}

SSPC_SYSV_ABI s32 GetAccountIdA(s32 user_id, u64* account_id) {
    if (account_id == nullptr || user_id == kUserIdInvalid) {
        return kErrInvalidArgument;
    }
    *account_id = kAccountId;
    return kOk;
}

SSPC_SYSV_ABI s32 GetGamePresenceStatusA(s32 user_id, PresenceStatus* status) {
    if (status == nullptr || user_id == kUserIdInvalid) {
        return kErrInvalidArgument;
    }
    *status = PresenceStatus::Online;
    return kOk;
}

SSPC_SYSV_ABI s32 GetOnlineId(s32 user_id, OnlineIdData* online_id) {
    if (user_id == kUserIdInvalid) {
        return kErrUserNotFound; // (the game's SDK is older than firmware 9.00: USER_NOT_FOUND, as the oracle)
    }
    if (online_id == nullptr) {
        return kErrInvalidArgument;
    }
    *online_id = {};
    std::memcpy(online_id->data, kOnlineId.data(), kOnlineId.size());
    return kOk;
}

SSPC_SYSV_ABI s32 GetState(s32 /*user_id*/, NpState* state) {
    if (state == nullptr) {
        return kErrInvalidArgument;
    }
    *state = NpState::SignedIn; // (an invalid user too: SDK older than 9.00)
    return kOk;
}

SSPC_SYSV_ABI s32 RegisterGamePresenceCallbackA(const void* callback, void* userdata) {
    if (callback == nullptr) {
        return kErrInvalidArgument;
    }
    std::scoped_lock lock{g_mutex};
    for (Callback& c : g_presence_cbs) {
        if (!c.in_use) {
            c = {true, callback, userdata, ++g_presence_next};
            return c.handle;
        }
    }
    return kErrCallbackMax;
}

SSPC_SYSV_ABI s32 RegisterStateCallbackA(const void* callback, void* userdata) {
    if (callback == nullptr) {
        return kErrInvalidArgument;
    }
    std::scoped_lock lock{g_mutex};
    for (const Callback& c : g_state_cbs) {
        if (c.in_use && c.func == callback) {
            return kErrCallbackAlreadyRegistered;
        }
    }
    for (size_t i = 0; i < g_state_cbs.size(); ++i) {
        if (!g_state_cbs[i].in_use) {
            g_state_cbs[i] = {true, callback, userdata, static_cast<s32>(i) + 1};
            return g_state_cbs[i].handle;
        }
    }
    return kErrCallbackMax;
}

SSPC_SYSV_ABI s32 SetContentRestriction(const ContentRestriction* r) {
    if (r == nullptr) {
        return kErrInvalidArgument;
    }
    if (r->size != sizeof(ContentRestriction)) {
        return kErrInvalidSize;
    }
    if (r->default_age_restriction < 0 || r->age_restriction_count > 0x100 ||
        (r->age_restriction_count > 0 && r->age_restriction == nullptr)) {
        return kErrInvalidArgument;
    }
    return kOk;
}

SSPC_SYSV_ABI s32 SetNpTitleId(const void* title_id, const void* title_secret) {
    return title_id == nullptr || title_secret == nullptr ? kErrInvalidArgument : kOk;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

#define NP(nid, name, fn) {nid, "libSceNpManager", "libSceNpManager", name, Fn(&fn), nullptr, "np"}
const Export kExports[] = {
    NP("3Zl8BePTh9Y", "sceNpCheckCallback", CheckCallback),
    NP("8Z2Jc5GvGDI", "sceNpCheckNpAvailabilityA", CheckNpAvailabilityA),
    NP("eiqMCt9UshI", "sceNpCreateAsyncRequest", CreateAsyncRequest),
    NP("S7QTn72PrDw", "sceNpDeleteRequest", DeleteRequest),
    NP("JT+t00a3TxA", "sceNpGetAccountCountryA", GetAccountCountryA),
    NP("rbknaUjpqWo", "sceNpGetAccountIdA", GetAccountIdA),
    NP("oPO9U42YpgI", "sceNpGetGamePresenceStatusA", GetGamePresenceStatusA),
    NP("XDncXQIJUSk", "sceNpGetOnlineId", GetOnlineId),
    NP("eQH7nWPcAgc", "sceNpGetState", GetState),
    NP("uqcPJLWL08M", "sceNpPollAsync", PollAsync),
    NP("KswxLxk4c1Y", "sceNpRegisterGamePresenceCallbackA", RegisterGamePresenceCallbackA),
    NP("qQJfO8HAiaY", "sceNpRegisterStateCallbackA", RegisterStateCallbackA),
    NP("A2CQ3kgSopQ", "sceNpSetContentRestriction", SetContentRestriction),
    NP("Ec63y59l9tw", "sceNpSetNpTitleId", SetNpTitleId),
};
#undef NP

} // namespace

std::string_view OnlineId() {
    return kOnlineId;
}

std::span<const Export> NpManagerExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Np
