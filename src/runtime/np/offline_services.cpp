// SecondSonPC — the network control and SSL libraries offline (NATIVE; group "offline"; `--bridge
// offline` sends them back to the oracle, third_party/shadPS4/src/core/libraries/network/netctl.cpp, ssl2.cpp,
// as they behave with the network disabled - the oracle's default and SecondSonPC's).
// The PC Edition has no PlayStation Network: the connection is down, nothing is ever reported to the registered
// callbacks, SSL contexts are numbers only. (libSceCommonDialog stays the bridge's until its dialogs move: the
// bridge's message / save data / error dialogs check its own "initialised" flag.)
#include <array>
#include <atomic>
#include <mutex>

#include "base/abi.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::Offline {

namespace {

constexpr s32 kOk = 0;
constexpr s32 kNetCtlErrCallbackMax = static_cast<s32>(0x80412103);
constexpr s32 kNetCtlErrInvalidId = static_cast<s32>(0x80412105);
constexpr s32 kNetCtlErrInvalidAddr = static_cast<s32>(0x80412107);
constexpr s32 kNetCtlErrNotConnected = static_cast<s32>(0x80412108);
constexpr s32 kNetCtlStateDisconnected = 0;

// ---- libSceNetCtl

struct NetCtlCallback {
    const void* func = nullptr;
    void* arg = nullptr;
};
std::mutex g_netctl_mutex;
std::array<NetCtlCallback, 8> g_netctl_cbs{};

SSPC_SYSV_ABI s32 NetCtlInit() {
    return kOk;
}

SSPC_SYSV_ABI s32 NetCtlTerm() {
    return kOk;
}

SSPC_SYSV_ABI s32 NetCtlGetState(s32* state) {
    if (state == nullptr) {
        return kNetCtlErrInvalidAddr;
    }
    *state = kNetCtlStateDisconnected;
    return kOk;
}

SSPC_SYSV_ABI s32 NetCtlGetInfo(s32 /*code*/, void* /*info*/) {
    return kNetCtlErrNotConnected;
}

SSPC_SYSV_ABI s32 NetCtlRegisterCallback(const void* func, void* arg, s32* cid) {
    if (func == nullptr || cid == nullptr) {
        return kNetCtlErrInvalidAddr;
    }
    std::scoped_lock lock{g_netctl_mutex};
    for (size_t i = 0; i < g_netctl_cbs.size(); ++i) {
        if (g_netctl_cbs[i].func == nullptr) {
            g_netctl_cbs[i] = {func, arg};
            *cid = static_cast<s32>(i);
            return kOk;
        }
    }
    return kNetCtlErrCallbackMax;
}

SSPC_SYSV_ABI s32 NetCtlUnregisterCallback(s32 cid) {
    std::scoped_lock lock{g_netctl_mutex};
    if (cid < 0 || static_cast<size_t>(cid) >= g_netctl_cbs.size() || g_netctl_cbs[cid].func == nullptr) {
        return kNetCtlErrInvalidId;
    }
    g_netctl_cbs[cid] = {};
    return kOk;
}

// ---- libSceSsl (contexts are numbers: nothing is ever connected)

std::atomic<s32> g_ssl_next{0};

SSPC_SYSV_ABI s32 SslInit(u64 /*pool_size*/) {
    return ++g_ssl_next; // (a context id > 0, as the oracle)
}

SSPC_SYSV_ABI s32 SslTerm(s32 /*ctx*/) {
    return kOk;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    {"gky0+oaNM4k", "libSceNetCtl", "libSceNetCtl", "sceNetCtlInit", Fn(&NetCtlInit), nullptr, "offline"},
    {"Z4wwCFiBELQ", "libSceNetCtl", "libSceNetCtl", "sceNetCtlTerm", Fn(&NetCtlTerm), nullptr, "offline"},
    {"uBPlr0lbuiI", "libSceNetCtl", "libSceNetCtl", "sceNetCtlGetState", Fn(&NetCtlGetState), nullptr, "offline"},
    {"obuxdTiwkF8", "libSceNetCtl", "libSceNetCtl", "sceNetCtlGetInfo", Fn(&NetCtlGetInfo), nullptr, "offline"},
    {"UJ+Z7Q+4ck0", "libSceNetCtl", "libSceNetCtl", "sceNetCtlRegisterCallback", Fn(&NetCtlRegisterCallback), nullptr,
     "offline"},
    {"Rqm2OnZMCz0", "libSceNetCtl", "libSceNetCtl", "sceNetCtlUnregisterCallback", Fn(&NetCtlUnregisterCallback),
     nullptr, "offline"},
    {"hdpVEUDFW3s", "libSceSsl", "libSceSsl", "sceSslInit", Fn(&SslInit), nullptr, "offline"},
    {"0K1yQ6Lv-Yc", "libSceSsl", "libSceSsl", "sceSslTerm", Fn(&SslTerm), nullptr, "offline"},
};

} // namespace

std::span<const Export> OfflineExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Offline
