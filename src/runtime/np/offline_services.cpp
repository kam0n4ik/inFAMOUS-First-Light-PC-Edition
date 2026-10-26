// SecondSonPC — the network control, SSL, HTTP and trophy libraries offline (NATIVE; group "offline"; `--bridge
// offline` sends them back to the oracle, third_party/shadPS4/src/core/libraries/network/netctl.cpp, ssl2.cpp,
// as they behave with the network disabled - the oracle's default and SecondSonPC's).
// The PC Edition has no PlayStation Network: the connection is down, nothing is ever reported to the registered
// callbacks, SSL contexts are numbers only. The one server the game asks over HTTP, the Paper Trail one
// (api.infamouspapertrail.com/update, shut down by Sony), is answered locally: every web step and key of the six
// cases is done, the in-game steps are the game's own (as it reported them). Trophies are kept on this PC: earned
// once, they stay earned (the game reads them back at start), the platinum comes with the last one, and each shows
// its notice over the game (ui/menu_controller.cpp). (libSceCommonDialog stays the bridge's
// until its dialogs move: the bridge's message / save data / error dialogs check its own "initialised" flag.)
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <map>
#include <mutex>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::Np {
std::string_view OnlineId(); // runtime/np/np_manager.cpp
}

namespace SecondSon::Ui {
void NotifyTrophy(s32 trophy); // ui/menu_controller.cpp: its notice over the game (any thread)
}

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

// ---- the local Paper Trail server

constexpr std::string_view kPaperTrailHost = "api.infamouspapertrail.com";
// the response's keys in the game's order (its table at 0xcd52f0): per case (w1..w6) its steps, "es" in the game,
// "w" on the web site, then the keys k1..k8
constexpr std::array<std::string_view, 48> kPaperTrailKeys = {
    "w1m1es", "w1m2w", "w1m3es", "w1m4w", "w1m5es", "w1m6w",
    "w2m1es", "w2m2w", "w2m3es", "w2m4w", "w2m5es", "w2m6w", "w2m7es", "w2m8w",
    "w3m1es", "w3m2w", "w3m3es", "w3m4w", "w3m5es", "w3m6w",
    "w4m1es", "w4m2w", "w4m3es", "w4m4w", "w4m5es", "w4m6w",
    "w5m1es", "w5m2w", "w5m3es", "w5m4w", "w5m5es", "w5m6w",
    "w6m1es", "w6m2w", "w6m3es", "w6m4w", "w6m5es", "w6m6w", "w6m7es", "w6m8w",
    "k1", "k2", "k3", "k4", "k5", "k6", "k7", "k8",
};

struct PaperTrailRequest {
    std::string body;     // what the game sent: {"<step>": 1|0, ..., "onlineId": "<id>"}
    std::string response; // made when the game asks for the status
    std::string headers;
    size_t read = 0;
};
std::map<s32, PaperTrailRequest> g_papertrail; // request id -> (requests to the Paper Trail server only)

/// The game's JSON body: each "name": 0|1 pair.
std::map<std::string, int, std::less<>> PaperTrailSteps(std::string_view body) {
    std::map<std::string, int, std::less<>> steps;
    size_t i = 0;
    while ((i = body.find('"', i)) != std::string_view::npos) {
        const size_t end = body.find('"', i + 1);
        if (end == std::string_view::npos) {
            break;
        }
        std::string name{body.substr(i + 1, end - i - 1)};
        size_t v = end + 1;
        while (v < body.size() && (body[v] == ' ' || body[v] == '\t' || body[v] == ':')) {
            ++v;
        }
        i = v;
        if (v < body.size() && std::isdigit(static_cast<unsigned char>(body[v]))) {
            steps[std::move(name)] = body[v] - '0';
        }
    }
    return steps;
}

/// The server's answer: the online id, no error, and every step: the web ones and the keys done, the in-game ones
/// as the game reported them.
std::string PaperTrailResponse(std::string_view body) {
    const auto steps = PaperTrailSteps(body);
    std::string r = "{\"onlineId\":\"";
    r += Np::OnlineId();
    r += "\",\"error\":\"\"";
    for (const std::string_view key : kPaperTrailKeys) {
        int value = 1;
        if (key.ends_with("es")) {
            const auto it = steps.find(key);
            value = it != steps.end() && it->second != 0 ? 1 : 0;
        }
        r += ",\"";
        r += key;
        r += "\":";
        r += static_cast<char>('0' + value);
    }
    r += "}";
    return r;
}

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
    if (url == nullptr) {
        return kHttpErrInvalidUrl;
    }
    const s32 id = HttpCreate(conn, false);
    if (id > 0 && std::string_view{url}.find(kPaperTrailHost) != std::string_view::npos) {
        std::scoped_lock lock{g_http_mutex};
        g_papertrail[id] = {};
        SSPC_LOG(SystemService, Info, "Paper Trail: request {} to {} (answered locally)", id, url);
    }
    return id;
}
SSPC_SYSV_ABI s32 HttpDeleteTemplate(s32 id) {
    return HttpDelete(id);
}
SSPC_SYSV_ABI s32 HttpDeleteConnection(s32 id) {
    return HttpDelete(id);
}
SSPC_SYSV_ABI s32 HttpDeleteRequest(s32 id) {
    {
        std::scoped_lock lock{g_http_mutex};
        g_papertrail.erase(id);
    }
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
SSPC_SYSV_ABI s32 HttpSendRequest(s32 id, const void* data, u64 size) {
    const s32 r = HttpKnown(id);
    if (r != kOk) {
        return r;
    }
    std::scoped_lock lock{g_http_mutex};
    const auto it = g_papertrail.find(id);
    if (it == g_papertrail.end()) {
        return kHttpErrNetwork; // (offline: no connection is ever made)
    }
    if (data != nullptr && size != 0) { // (the game sends its body in parts of 8 KiB, then one of size 0)
        it->second.body.append(static_cast<const char*>(data), size);
    }
    return kOk;
}

/// The Paper Trail request of `id` (mutex held), its answer made on the first call; nullptr for any other request.
PaperTrailRequest* PaperTrailAnswered(s32 id) {
    const auto it = g_papertrail.find(id);
    if (it == g_papertrail.end()) {
        return nullptr;
    }
    PaperTrailRequest& p = it->second;
    if (p.response.empty()) {
        p.response = PaperTrailResponse(p.body);
        p.headers = "Content-Type: application/json\r\nContent-Length: " + std::to_string(p.response.size()) + "\r\n";
        SSPC_LOG(SystemService, Info, "Paper Trail: the game sent {}", p.body);
        SSPC_LOG(SystemService, Info, "Paper Trail: answered {}", p.response);
    }
    return &p;
}

// (any other request was never sent: BEFORE_SEND for its status, headers and data)
SSPC_SYSV_ABI s32 HttpGetStatusCode(s32 id, s32* status) {
    if (const s32 r = HttpKnown(id); r != kOk) {
        return r;
    }
    std::scoped_lock lock{g_http_mutex};
    if (PaperTrailAnswered(id) == nullptr) {
        return kHttpErrBeforeSend;
    }
    if (status == nullptr) {
        return kKernelErrEinval;
    }
    *status = 200;
    return kOk;
}

SSPC_SYSV_ABI s32 HttpGetAllResponseHeaders(s32 id, char** header, u64* size) {
    if (const s32 r = HttpKnown(id); r != kOk) {
        return r;
    }
    std::scoped_lock lock{g_http_mutex};
    PaperTrailRequest* p = PaperTrailAnswered(id);
    if (p == nullptr) {
        return kHttpErrBeforeSend;
    }
    if (header == nullptr || size == nullptr) {
        return kKernelErrEinval;
    }
    *header = p->headers.data();
    *size = p->headers.size();
    return kOk;
}

SSPC_SYSV_ABI s32 HttpReadData(s32 id, void* data, u64 size) {
    if (const s32 r = HttpKnown(id); r != kOk) {
        return r;
    }
    std::scoped_lock lock{g_http_mutex};
    PaperTrailRequest* p = PaperTrailAnswered(id);
    if (p == nullptr) {
        return kHttpErrBeforeSend;
    }
    if (data == nullptr) {
        return kKernelErrEinval;
    }
    const size_t n = std::min<size_t>(size, p->response.size() - p->read);
    std::memcpy(data, p->response.data() + p->read, n);
    p->read += n;
    return static_cast<s32>(n); // (0: the whole answer is read)
}

// ---- libSceNpTrophy (contexts and handles as the oracle's). The trophy set's configuration (TROPCONF.ESFM) needs the
// console's key, so what it would say is the game's: 48 trophies (TROP000..047.PNG in trophy00.trp), 0 the platinum
// (the eboot's trophy names start with "Platinum"). Earned ones: Configure's file, a line "<id> <unix time>" each.

constexpr s32 kTrophyErrInvalidArgument = static_cast<s32>(0x80551604);
constexpr s32 kTrophyErrInvalidHandle = static_cast<s32>(0x80551608);
constexpr s32 kTrophyErrInvalidContext = static_cast<s32>(0x80551609);
constexpr s32 kTrophyErrInvalidTrophyId = static_cast<s32>(0x8055160A);
constexpr s32 kTrophyErrAlreadyUnlocked = static_cast<s32>(0x8055160C);
constexpr s32 kTrophyErrPlatinumCannotUnlock = static_cast<s32>(0x8055160D);
constexpr s32 kTrophyErrNotRegistered = static_cast<s32>(0x8055160F);
constexpr s32 kTrophyErrAlreadyRegistered = static_cast<s32>(0x80551610);
constexpr s32 kTrophyErrContextExists = static_cast<s32>(0x80551613);
constexpr s32 kTrophyErrContextMax = static_cast<s32>(0x80551623);
constexpr s32 kTrophyErrHandleMax = static_cast<s32>(0x80551624);
constexpr s32 kTrophyNumMax = 128;
constexpr s32 kTrophyInvalidId = -1;
constexpr s32 kTrophyPlatinum = 0;

struct TrophyContext {
    bool used = false;
    bool registered = false;
    s32 user = 0;
    u32 label = 0;
};
std::mutex g_trophy_mutex;
std::array<TrophyContext, 8> g_trophy_ctx{};
std::array<bool, 4> g_trophy_handles{};
std::filesystem::path g_trophy_file; // the earned trophies (empty: not configured, nothing kept)
std::filesystem::path g_trophy_trp;  // the game's trophy00.trp (the icons)
s32 g_trophy_count = 0;              // trophies of the set (its icons), 0: unknown - no platinum given
std::map<s32, s64> g_trophy_earned;  // id -> unix time

/// A trophy file's entry `name` -> its bytes into `out` (big endian: magic 0xDCA24D00, entry count at 0x10, entry
/// size at 0x14, entries from 0x60: name[32], offset u64, size u64); `icons` counts the TROPnnn.PNG entries. false: no
/// such file or entry.
bool TrpRead(const std::filesystem::path& trp, std::string_view name, std::vector<u8>* out, s32* icons = nullptr) {
    std::ifstream f(trp, std::ios::binary);
    std::array<u8, 0x60> h{};
    if (!f.read(reinterpret_cast<char*>(h.data()), h.size())) {
        return false;
    }
    const auto be32 = [](const u8* p) { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; };
    const auto be64 = [&](const u8* p) { return u64{be32(p)} << 32 | be32(p + 4); };
    const u32 count = be32(h.data() + 0x10), size = be32(h.data() + 0x14);
    if (be32(h.data()) != 0xDCA24D00u || count > 1024 || size < 48 || size > 256) {
        return false;
    }
    std::vector<u8> e(size);
    bool found = false;
    for (u32 i = 0; i < count && f.read(reinterpret_cast<char*>(e.data()), size); ++i) {
        const auto* n0 = reinterpret_cast<const char*>(e.data());
        const std::string_view n{n0, strnlen(n0, 32)};
        if (icons != nullptr && n.size() == 11 && n.starts_with("TROP") && n.ends_with(".PNG")) {
            ++*icons;
        }
        if (!found && out != nullptr && n == name) {
            const u64 off = be64(e.data() + 32), len = be64(e.data() + 40);
            const auto pos = f.tellg();
            if (len < (64u << 20)) {
                out->resize(len);
                found = static_cast<bool>(f.seekg(static_cast<std::streamoff>(off))) &&
                        static_cast<bool>(f.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(len)));
            }
            f.clear();
            f.seekg(pos);
        }
    }
    return found;
}

/// Writes the earned trophies (mutex held): a new file, then over the old one.
void TrophySave() {
    if (g_trophy_file.empty()) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(g_trophy_file.parent_path(), ec);
    auto tmp = g_trophy_file;
    tmp += ".new";
    {
        std::ofstream f(tmp, std::ios::trunc);
        for (const auto& [id, time] : g_trophy_earned) {
            f << id << ' ' << time << '\n';
        }
        if (!f) {
            SSPC_LOG(SystemService, Error, "trophies: cannot write {}", tmp.string());
            return;
        }
    }
    std::filesystem::rename(tmp, g_trophy_file, ec);
    if (ec) {
        SSPC_LOG(SystemService, Error, "trophies: cannot replace {}: {}", g_trophy_file.string(), ec.message());
    }
}

/// `id` earned now (mutex held).
void TrophyEarn(s32 id) {
    g_trophy_earned[id] =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    Ui::NotifyTrophy(id);
}

/// The context of id `c` and the handle `h` are valid (mutex held); 0 or the error.
s32 TrophyCheck(s32 c, s32 h, bool need_registered) {
    if (c <= 0 || c > static_cast<s32>(g_trophy_ctx.size()) || !g_trophy_ctx[c - 1].used) {
        return kTrophyErrInvalidContext;
    }
    if (h <= 0 || h > static_cast<s32>(g_trophy_handles.size()) || !g_trophy_handles[h - 1]) {
        return kTrophyErrInvalidHandle;
    }
    return need_registered && !g_trophy_ctx[c - 1].registered ? kTrophyErrNotRegistered : kOk;
}

SSPC_SYSV_ABI s32 TrophyCreateContext(s32* context, s32 user, u32 label, u64 options) {
    if (context == nullptr || options != 0) {
        return kTrophyErrInvalidArgument;
    }
    std::scoped_lock lock{g_trophy_mutex};
    for (const TrophyContext& t : g_trophy_ctx) {
        if (t.used && t.user == user && t.label == label) {
            return kTrophyErrContextExists;
        }
    }
    for (size_t i = 0; i < g_trophy_ctx.size(); ++i) {
        if (!g_trophy_ctx[i].used) {
            g_trophy_ctx[i] = {true, false, user, label};
            *context = static_cast<s32>(i) + 1;
            return kOk;
        }
    }
    return kTrophyErrContextMax;
}

SSPC_SYSV_ABI s32 TrophyDestroyContext(s32 context) {
    std::scoped_lock lock{g_trophy_mutex};
    if (context <= 0 || context > static_cast<s32>(g_trophy_ctx.size()) || !g_trophy_ctx[context - 1].used) {
        return kTrophyErrInvalidContext;
    }
    g_trophy_ctx[context - 1] = {};
    return kOk;
}

SSPC_SYSV_ABI s32 TrophyCreateHandle(s32* handle) {
    if (handle == nullptr) {
        return kTrophyErrInvalidArgument;
    }
    std::scoped_lock lock{g_trophy_mutex};
    for (size_t i = 0; i < g_trophy_handles.size(); ++i) {
        if (!g_trophy_handles[i]) {
            g_trophy_handles[i] = true;
            *handle = static_cast<s32>(i) + 1;
            return kOk;
        }
    }
    return kTrophyErrHandleMax;
}

SSPC_SYSV_ABI s32 TrophyDestroyHandle(s32 handle) {
    std::scoped_lock lock{g_trophy_mutex};
    if (handle <= 0 || handle > static_cast<s32>(g_trophy_handles.size()) || !g_trophy_handles[handle - 1]) {
        return kTrophyErrInvalidHandle;
    }
    g_trophy_handles[handle - 1] = false;
    return kOk;
}

SSPC_SYSV_ABI s32 TrophyRegisterContext(s32 context, s32 handle, u64 options) {
    if (options != 0) {
        return kTrophyErrInvalidArgument;
    }
    std::scoped_lock lock{g_trophy_mutex};
    if (const s32 r = TrophyCheck(context, handle, false); r != kOk) {
        return r;
    }
    if (g_trophy_ctx[context - 1].registered) {
        return kTrophyErrAlreadyRegistered;
    }
    g_trophy_ctx[context - 1].registered = true;
    return kOk;
}

SSPC_SYSV_ABI s32 TrophyGetTrophyUnlockState(s32 context, s32 handle, u32* flags, u32* count) {
    if (flags == nullptr || count == nullptr) {
        return kTrophyErrInvalidArgument;
    }
    std::scoped_lock lock{g_trophy_mutex};
    if (const s32 r = TrophyCheck(context, handle, true); r != kOk) {
        return r;
    }
    std::memset(flags, 0, kTrophyNumMax / 8); // (SceNpTrophyFlagArray: 128 bits, bit n of word n / 32)
    for (const auto& [id, time] : g_trophy_earned) {
        flags[id >> 5] |= 1u << (id & 31);
    }
    *count = static_cast<u32>(g_trophy_earned.size());
    return kOk;
}

SSPC_SYSV_ABI s32 TrophyUnlockTrophy(s32 context, s32 handle, s32 trophy, s32* platinum) {
    std::scoped_lock lock{g_trophy_mutex};
    if (context <= 0 || context > static_cast<s32>(g_trophy_ctx.size())) {
        return kTrophyErrInvalidContext;
    }
    if (handle <= 0) {
        return kTrophyErrInvalidHandle;
    }
    if (trophy >= kTrophyNumMax) {
        return kTrophyErrInvalidTrophyId;
    }
    if (platinum == nullptr) {
        return kTrophyErrInvalidArgument;
    }
    if (const s32 r = TrophyCheck(context, handle, true); r != kOk) {
        return r;
    }
    *platinum = kTrophyInvalidId;
    if (trophy < 0) {
        return kTrophyErrInvalidTrophyId;
    }
    if (g_trophy_earned.contains(trophy)) {
        return kTrophyErrAlreadyUnlocked;
    }
    if (trophy == kTrophyPlatinum) {
        return kTrophyErrPlatinumCannotUnlock;
    }
    TrophyEarn(trophy);
    bool all = g_trophy_count > 1 && !g_trophy_earned.contains(kTrophyPlatinum);
    for (s32 i = 1; all && i < g_trophy_count; ++i) {
        all = g_trophy_earned.contains(i);
    }
    if (all) {
        TrophyEarn(kTrophyPlatinum);
        *platinum = kTrophyPlatinum;
    }
    TrophySave();
    SSPC_LOG(SystemService, Info, "trophy {} earned{} ({} of {})", trophy, all ? ", and the platinum" : "",
             g_trophy_earned.size(), g_trophy_count);
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
    HTTP("0a2TBNfE3BU", "sceHttpGetStatusCode", HttpGetStatusCode),
    HTTP("aCYPMSUIaP8", "sceHttpGetAllResponseHeaders", HttpGetAllResponseHeaders),
    HTTP("P5pdoykPYTk", "sceHttpReadData", HttpReadData),
#undef HTTP
#define TROPHY(nid, name, fn) {nid, "libSceNpTrophy", "libSceNpTrophy", name, Fn(&fn), nullptr, "offline"}
    TROPHY("XbkjbobZlCY", "sceNpTrophyCreateContext", TrophyCreateContext),
    TROPHY("q7U6tEAQf7c", "sceNpTrophyCreateHandle", TrophyCreateHandle),
    TROPHY("E1Wrwd07Lr8", "sceNpTrophyDestroyContext", TrophyDestroyContext),
    TROPHY("GNcF4oidY0Y", "sceNpTrophyDestroyHandle", TrophyDestroyHandle),
    TROPHY("LHuSmO3SLd8", "sceNpTrophyGetTrophyUnlockState", TrophyGetTrophyUnlockState),
    TROPHY("TJCAxto9SEU", "sceNpTrophyRegisterContext", TrophyRegisterContext),
    TROPHY("28xmRUFao68", "sceNpTrophyUnlockTrophy", TrophyUnlockTrophy),
#undef TROPHY
};

} // namespace

std::span<const Export> OfflineExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Offline

namespace SecondSon::Runtime::Trophies {

/// Before the game runs (the bootstrap): where the earned trophies are kept and the game's trophy file.
void Configure(const std::filesystem::path& earned_file, const std::filesystem::path& trp) {
    using namespace Offline;
    std::scoped_lock lock{g_trophy_mutex};
    g_trophy_file = earned_file;
    g_trophy_trp = trp;
    g_trophy_count = 0;
    TrpRead(trp, "", nullptr, &g_trophy_count);
    g_trophy_earned.clear();
    std::ifstream f(earned_file);
    s32 id = 0;
    s64 time = 0;
    std::vector<s32> unseen; // time 0: earned before trophies were kept (restored by hand), its notice not shown yet
    while (f >> id >> time) {
        if (id >= 0 && id < kTrophyNumMax) {
            g_trophy_earned[id] = time;
            if (time == 0) {
                unseen.push_back(id);
            }
        }
    }
    f.close();
    for (const s32 u : unseen) {
        TrophyEarn(u); // its notice once the game window is there, and the time it is shown at
    }
    if (!unseen.empty()) {
        TrophySave();
    }
    SSPC_LOG(SystemService, Info, "trophies: {} of {} earned, {} restored ({})", g_trophy_earned.size(),
             g_trophy_count, unseen.size(), earned_file.string());
}

/// Trophy `id`'s icon (PNG bytes) from the game's trophy file; false without it.
bool Icon(s32 id, std::vector<u8>& png) {
    std::filesystem::path trp;
    {
        std::scoped_lock lock{Offline::g_trophy_mutex};
        trp = Offline::g_trophy_trp;
    }
    char name[16];
    std::snprintf(name, sizeof(name), "TROP%03d.PNG", id);
    return id >= 0 && !trp.empty() && Offline::TrpRead(trp, name, &png);
}

} // namespace SecondSon::Runtime::Trophies
