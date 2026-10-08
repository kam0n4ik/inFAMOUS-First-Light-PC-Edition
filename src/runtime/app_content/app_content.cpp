// SecondSonPC — libSceAppContentUtil (NATIVE). SecondSonPC installs no additional content, so the
// title has no DLC entitlements: Initialize succeeds (once), GetAddcontInfo finds nothing. This is
// what the Phase 0 oracle does with no add-on folder and no licenses
// (third_party/shadPS4/src/core/libraries/app_content/app_content.cpp @ 0fe263a).
#include <atomic>
#include <span>

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
    SSPC_LOG(SystemService, Debug, "sceAppContentGetAddcontInfo(label {}, {:.16}) -> no entitlement",
             service_label, entitlement_label);
    return ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
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
