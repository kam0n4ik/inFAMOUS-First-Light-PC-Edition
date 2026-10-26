// SecondSonPC — libSceAppContentUtil (NATIVE). The game's add-ons are unlock keys only: their content (Cole's
// Legacy - the jacket and its missions -, the vests of the pre-order and promotion offers) is on the disc, and the
// game asks for each key by its entitlement label (eboot 0x571930, the table at 0xd76ce0: UNLOCKAPPLICATIO, then
// UNLOCKVEST{JOS000,PENNYA,EXIST0,SKELET,HANDPR}, UNLOCKCOLEJACKET, UNLOCKCOLEMISSIO) and accepts status 0 or 4.
// Every UNLOCK* key is owned (status 0: a key without extra data); any other label finds nothing, as the Phase 0
// oracle does with no add-on folder and no licenses
// (third_party/shadPS4/src/core/libraries/app_content/app_content.cpp @ 0fe263a).
#include <atomic>
#include <cstring>
#include <span>
#include <string_view>

#include "base/log.h"
#include "loader/linker.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::AppContent {

namespace {

constexpr s32 ORBIS_OK = 0;
constexpr s32 ORBIS_APP_CONTENT_ERROR_PARAMETER = static_cast<s32>(0x80D90002);
constexpr s32 ORBIS_APP_CONTENT_ERROR_BUSY = static_cast<s32>(0x80D90003);
constexpr s32 ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT = static_cast<s32>(0x80D90007);
constexpr u64 FW_150 = 0x1500000;
constexpr u32 kAddcontNoExtraData = 0; // ORBIS_APP_CONTENT_ADDCONT_DOWNLOAD_STATUS_NO_EXTRA_DATA

struct AddcontInfo { // OrbisAppContentAddcontInfo
    char entitlement_label[17];
    char padding[3];
    u32 status;
};

std::atomic<bool> g_initialized{false};

SSPC_SYSV_ABI s32 Initialize(const void* /*init_param*/, void* /*boot_param*/) {
    const u64 sdk = Loader::Linker::Active() != nullptr ? Loader::Linker::Active()->SdkVersion() : 0;
    if (sdk >= FW_150 && g_initialized.load()) {
        SSPC_LOG(SystemService, Error, "sceAppContentInitialize: already initialized");
        return ORBIS_APP_CONTENT_ERROR_BUSY;
    }
    g_initialized = true;
    SSPC_LOG(SystemService, Info, "sceAppContentInitialize: no additional content installed");
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetAddcontInfo(u32 service_label, const char* entitlement_label, void* info) {
    if (entitlement_label == nullptr || info == nullptr) {
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }
    const std::string_view label{entitlement_label, strnlen(entitlement_label, 16)};
    if (!label.starts_with("UNLOCK")) {
        SSPC_LOG(SystemService, Debug, "sceAppContentGetAddcontInfo(label {}, {}) -> no entitlement", service_label,
                 label);
        return ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
    }
    auto* out = static_cast<AddcontInfo*>(info);
    std::memset(out, 0, sizeof(AddcontInfo));
    std::memcpy(out->entitlement_label, label.data(), label.size());
    out->status = kAddcontNoExtraData;
    SSPC_LOG(SystemService, Info, "sceAppContentGetAddcontInfo(label {}, {}) -> owned", service_label, label);
    return ORBIS_OK;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    {"R9lA82OraNs", "libSceAppContent", "libSceAppContentUtil", "sceAppContentInitialize", Fn(&Initialize), nullptr, "appcontent"},
    {"m47juOmH0VE", "libSceAppContent", "libSceAppContentUtil", "sceAppContentGetAddcontInfo", Fn(&GetAddcontInfo), nullptr, "appcontent"},
};

} // namespace

std::span<const Export> AppContentExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::AppContent
