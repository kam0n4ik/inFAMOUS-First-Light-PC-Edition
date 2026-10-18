// SecondSonPC — the network control, SSL and HTTP libraries offline (NATIVE; group "offline"; `--bridge
// offline` sends them back to the oracle, third_party/shadPS4/src/core/libraries/network/netctl.cpp, ssl2.cpp,
// as they behave with the network disabled - the oracle's default and SecondSonPC's).
// The PC Edition has no PlayStation Network: the connection is down, nothing is ever reported to the registered
// callbacks, SSL contexts are numbers only. (libSceCommonDialog stays the bridge's until its dialogs move: the
// bridge's message / save data / error dialogs check its own "initialised" flag.)
#include <array>
#include <atomic>
#include <mutex>
#include <set>

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

// ---- libSceHttp (contexts, templates, connections and requests are ids; nothing is ever sent: NETWORK)

constexpr s32 kHttpErrBeforeInit = static_cast<s32>(0x80431001);
constexpr s32 kHttpErrInvalidId = static_cast<s32>(0x80431100);
constexpr s32 kHttpErrInvalidUrl = static_cast<s32>(0x80433060);
constexpr s32 kHttpErrNetwork = static_cast<s32>(0x80431063);
constexpr s32 kHttpErrBeforeSend = static_cast<s32>(0x80431065);
constexpr s32 kKernelErrEinval = static_cast<s32>(0x80020016);

std::mutex g_http_mutex;
std::set<s32> g_http_ctx, g_http_ids; // (templates, connections and requests share one id space, as the oracle)
s32 g_http_next_ctx = 0, g_http_next_id = 0;

SSPC_SYSV_ABI s32 HttpInit(s32 /*net_mem*/, s32 /*ssl_ctx*/, u64 pool_size) {
    if (pool_size == 0) {
        return kKernelErrEinval;
    }
    std::scoped_lock lock{g_http_mutex};
    g_http_ctx.insert(++g_http_next_ctx);
    return g_http_next_ctx;
}

SSPC_SYSV_ABI s32 HttpTerm(s32 ctx) {
    std::scoped_lock lock{g_http_mutex};
    if (g_http_ctx.empty()) {
        return kHttpErrBeforeInit;
    }
    return g_http_ctx.erase(ctx) != 0 ? kOk : kHttpErrInvalidId;
}

/// A new template / connection / request id when `parent` is known (a context for a template).
s32 HttpCreate(s32 parent, bool parent_is_ctx) {
    std::scoped_lock lock{g_http_mutex};
    if (g_http_ctx.empty()) {
        return kHttpErrBeforeInit;
    }
    if (!(parent_is_ctx ? g_http_ctx : g_http_ids).contains(parent)) {
        return kHttpErrInvalidId;
    }
    g_http_ids.insert(++g_http_next_id);
    return g_http_next_id;
}

s32 HttpKnown(s32 id) {
    std::scoped_lock lock{g_http_mutex};
    if (g_http_ctx.empty()) {
        return kHttpErrBeforeInit;
    }
    return g_http_ids.contains(id) ? kOk : kHttpErrInvalidId;
}

s32 HttpDelete(s32 id) {
    std::scoped_lock lock{g_http_mutex};
    if (g_http_ctx.empty()) {
        return kHttpErrBeforeInit;
    }
    return g_http_ids.erase(id) != 0 ? kOk : kHttpErrInvalidId;
}

SSPC_SYSV_ABI s32 HttpCreateTemplate(s32 ctx, const char* /*agent*/, s32 /*ver*/, s32 /*proxy*/) {
    return HttpCreate(ctx, true);
}
SSPC_SYSV_ABI s32 HttpCreateConnectionWithURL(s32 tmpl, const char* url, bool /*keepalive*/) {
    return url == nullptr ? kHttpErrInvalidUrl : HttpCreate(tmpl, false);
}
SSPC_SYSV_ABI s32 HttpCreateRequestWithURL(s32 conn, s32 /*method*/, const char* url, u64 /*length*/) {
    return url == nullptr ? kHttpErrInvalidUrl : HttpCreate(conn, false);
}
SSPC_SYSV_ABI s32 HttpDeleteTemplate(s32 id) {
    return HttpDelete(id);
}
SSPC_SYSV_ABI s32 HttpDeleteConnection(s32 id) {
    return HttpDelete(id);
}
SSPC_SYSV_ABI s32 HttpDeleteRequest(s32 id) {
    return HttpDelete(id);
}
SSPC_SYSV_ABI s32 HttpSetTimeOut(s32 id, u32 /*usec*/) {
    return HttpKnown(id);
}
SSPC_SYSV_ABI s32 HttpSetChunkedTransferEnabled(s32 id, s32 /*enable*/) {
    return HttpKnown(id);
}
SSPC_SYSV_ABI s32 HttpAddRequestHeader(s32 id, const char* /*name*/, const char* /*value*/, u32 /*mode*/) {
    return HttpKnown(id);
}
SSPC_SYSV_ABI s32 HttpsLoadCert(s32 ctx, s32 /*count*/, const void** /*list*/, const void* /*cert*/,
                                const void* /*key*/) {
    std::scoped_lock lock{g_http_mutex};
    if (g_http_ctx.empty()) {
        return kHttpErrBeforeInit;
    }
    return g_http_ctx.contains(ctx) ? kOk : kHttpErrInvalidId;
}
SSPC_SYSV_ABI s32 HttpSendRequest(s32 id, const void* /*data*/, u64 /*size*/) {
    const s32 r = HttpKnown(id);
    return r != kOk ? r : kHttpErrNetwork; // (offline: no connection is ever made)
}
SSPC_SYSV_ABI s32 HttpNotSent(s32 id) {
    const s32 r = HttpKnown(id);
    return r != kOk ? r : kHttpErrBeforeSend; // (status, headers and data of a request never sent)
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
#define HTTP(nid, name, fn) {nid, "libSceHttp", "libSceHttp", name, Fn(&fn), nullptr, "offline"}
    HTTP("A9cVMUtEp4Y", "sceHttpInit", HttpInit),
    HTTP("Ik-KpLTlf7Q", "sceHttpTerm", HttpTerm),
    HTTP("0gYjPTR-6cY", "sceHttpCreateTemplate", HttpCreateTemplate),
    HTTP("qgxDBjorUxs", "sceHttpCreateConnectionWithURL", HttpCreateConnectionWithURL),
    HTTP("Aeu5wVKkF9w", "sceHttpCreateRequestWithURL", HttpCreateRequestWithURL),
    HTTP("4I8vEpuEhZ8", "sceHttpDeleteTemplate", HttpDeleteTemplate),
    HTTP("P6A3ytpsiYc", "sceHttpDeleteConnection", HttpDeleteConnection),
    HTTP("qe7oZ+v4PWA", "sceHttpDeleteRequest", HttpDeleteRequest),
    HTTP("0S9tTH0uqTU", "sceHttpSetConnectTimeOut", HttpSetTimeOut),
    HTTP("yigr4V0-HTM", "sceHttpSetRecvTimeOut", HttpSetTimeOut),
    HTTP("xegFfZKBVlw", "sceHttpSetSendTimeOut", HttpSetTimeOut),
    HTTP("PDxS48xGQLs", "sceHttpSetChunkedTransferEnabled", HttpSetChunkedTransferEnabled),
    HTTP("EY28T2bkN7k", "sceHttpAddRequestHeader", HttpAddRequestHeader),
    HTTP("DK+GoXCNT04", "sceHttpsLoadCert", HttpsLoadCert),
    HTTP("1e2BNwI-XzE", "sceHttpSendRequest", HttpSendRequest),
    HTTP("0a2TBNfE3BU", "sceHttpGetStatusCode", HttpNotSent),
    HTTP("aCYPMSUIaP8", "sceHttpGetAllResponseHeaders", HttpNotSent),
    HTTP("P5pdoykPYTk", "sceHttpReadData", HttpNotSent),
#undef HTTP
};

} // namespace

std::span<const Export> OfflineExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Offline
