// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — libSceSysmodule (NATIVE). Load/unload/preload logic ported from
// third_party/shadPS4/src/core/libraries/sysmodule/{sysmodule,sysmodule_internal}.cpp @ 0fe263a.
// Differences: game modules go through SecondSonPC's Loader::Linker; firmware modules are never
// loaded from disk (SecondSonPC has no sys_modules folder): every "LLE allowed" module takes the
// branch the oracle takes when the .sprx is absent; retail console, not PS4 Pro.
#include "runtime/sysmodule/sysmodule.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include "base/log.h"
#include "loader/linker.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::Sysmodule {

namespace {

#include "runtime/sysmodule/sysmodule_table.inc"

constexpr s32 ORBIS_OK = 0;
constexpr s32 ORBIS_SYSMODULE_INVALID_ID = static_cast<s32>(0x805A1000);
constexpr s32 ORBIS_SYSMODULE_NOT_LOADED = static_cast<s32>(0x805A1001);
constexpr s32 ORBIS_SYSMODULE_LOCK_FAILED = static_cast<s32>(0x805A10FF);
constexpr s32 ORBIS_KERNEL_ERROR_EINVAL = static_cast<s32>(0x80020016);
constexpr s32 ORBIS_KERNEL_ERROR_ESTART = static_cast<s32>(0x80020064);

constexpr u64 FW_200 = 0x2000000;
constexpr u64 FW_300 = 0x3000000;
constexpr u64 FW_450 = 0x4500000;
constexpr u64 FW_700 = 0x7000000;
constexpr u64 FW_750 = 0x7500000;
constexpr u64 FW_1150 = 0x11500000;

constexpr bool kDevKit = false; // retail console
constexpr bool kNeo = false;    // not a PS4 Pro

std::mutex g_mutex;
s32 g_stub_handle = 100;

u64 SdkVersion() {
    return Loader::Linker::Active()->SdkVersion();
}

bool IsDebugModule(s32 id) {
    for (const auto& mod : g_modules_array) {
        if (static_cast<s32>(mod.id) == id && (mod.flags & IsDebug) != 0) {
            return true;
        }
    }
    return false;
}

s32 ValidateModuleId(s32 id) {
    if ((id & 0x7fffffff) == 0) {
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    const u64 sdk = SdkVersion();
    if (id == 0xb8 && sdk >= FW_750) { // libSceGameCustomDialog
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    if (id == 0xb0 && sdk >= FW_700) { // libSceNpSnsFacebookDialog
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    if (id == 0x80 && sdk >= FW_300) { // libSceJson
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    if (IsDebugModule(id) && !kDevKit) {
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    return ORBIS_OK;
}

/// Firmware modules the oracle would load as LLE if present, and whether it then had an HLE
/// replacement to register (`true`). SecondSonPC serves those functions from its binding table.
struct AllowedModule {
    std::string_view name;
    bool has_hle;
};
constexpr std::array kAllowedLle = std::to_array<AllowedModule>({
    {"libSceNgs2.sprx", true},          {"libSceUlt.sprx", false},
    {"libSceAvPlayer.sprx", true},      {"libSceAvPlayerStreaming.sprx", false},
    {"libSceRtc.sprx", true},           {"libSceJpegDec.sprx", false},
    {"libSceJpegEnc.sprx", true},       {"libScePngDec.sprx", true},
    {"libScePngEnc.sprx", true},        {"libSceJson.sprx", false},
    {"libSceJson2.sprx", false},        {"libSceCesCs.sprx", false},
    {"libSceAt9Enc.sprx", false},       {"libSceAudiodec.sprx", false},
    {"libSceAudiodecCpu.sprx", false},  {"libSceAudiodecCpuDdp.sprx", false},
    {"libSceAudiodecCpuM4aac.sprx", false}, {"libSceAudiodecCpuDtsHdLbr.sprx", false},
    {"libSceAudiodecCpuHevag.sprx", false}, {"libSceFont.sprx", true},
    {"libSceFontFt.sprx", true},        {"libSceFreeTypeOt.sprx", false},
    {"libSceFreeTypeOl.sprx", false},   {"libSceFreeTypeOptOl.sprx", false},
    {"libSceBeisobmf.sprx", false},     {"libSceBemp2sys.sprx", false},
    {"libSceRudp.sprx", true},          {"libSceWkFontConfig.sprx", false},
    {"libScePsmKitSystem.sprx", false}, {"libSceDepth.sprx", false},
    {"libScePadTracker.sprx", false},   {"libSceMoveTracker.sprx", false},
    {"libSceSystemGesture.sprx", true}, {"libSceXml.sprx", false},
});

s32 LoadModuleInternal(s32 index, s32 argc, const void* argv, s32* res_out) {
    auto& mod = g_modules_array[index];
    if (mod.is_loaded > 0) {
        mod.is_loaded++;
        return ORBIS_OK;
    }

    s32 start_result = 0;
    if ((mod.flags & IsGame) != 0) {
        const std::string guest_path = std::string("/app0/sce_module/").append(mod.name).append(".prx");
        const s32 handle = Loader::Linker::Active()->LoadAndStartModule(guest_path, argc, argv,
                                                                        &start_result);
        if (handle < 0) {
            SSPC_LOG(Loader, Error, "sysmodule: cannot load game library {}", guest_path);
            return handle;
        }
        mod.handle = handle;
        mod.is_loaded++;
    } else {
        std::string mod_name = mod.name;
        if (index == 0xd && kDevKit) { // libSceGnmDriver
            mod_name.append("_padebug");
        }
        if (index == 0xb3 && kDevKit) { // libSceVrTracker
            mod_name.append("_debug");
        }
        if ((mod.flags & IsNeo) == 0 && (mod.flags & IsNeoMode) != 0 && kNeo) {
            mod_name.append("ForNeoMode");
        } else if ((mod.flags & IsNeo) != 0 && kNeo) {
            mod_name.append("ForNeo");
        }
        mod_name.append(".sprx");

        const auto it = std::find_if(kAllowedLle.begin(), kAllowedLle.end(),
                                     [&](const AllowedModule& m) { return m.name == mod_name; });
        if (it == kAllowedLle.end()) {
            // Not an LLE candidate: bookkeeping success, as on the oracle.
            mod.is_loaded++;
            mod.handle = g_stub_handle++;
            if (res_out != nullptr) {
                *res_out = ORBIS_OK;
            }
            return ORBIS_OK;
        }
        if (it->has_hle) {
            SSPC_LOG(Loader, Info, "sysmodule: {} not loaded, its functions come from the binding table",
                     mod_name);
            Loader::Linker::Active()->RelocateAll(); // the oracle relocates after registering an HLE
        } else {
            SSPC_LOG(Loader, Info, "sysmodule: {} not loaded (no replacement)", mod_name);
        }
        mod.handle = g_stub_handle++;
        mod.is_loaded++;
    }
    if (res_out != nullptr) {
        *res_out = start_result;
    }
    return ORBIS_OK;
}

s32 LoadModule(s32 id, s32 argc, const void* argv, s32* res_out) {
    const OrbisSysmoduleModuleInternal* requested = nullptr;
    for (const auto& mod : g_modules_array) {
        if (static_cast<s32>(mod.id) == id) {
            requested = &mod;
            break;
        }
    }
    if (requested == nullptr || requested->id == 0) {
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    if (requested->to_load == nullptr) {
        return ORBIS_SYSMODULE_LOCK_FAILED;
    }
    SSPC_LOG(Loader, Info, "sysmodule: loading {}", requested->name);
    for (s64 i = requested->num_to_load - 1; i >= 0; i--) {
        const u32 mod_index = requested->to_load[i];
        if (!kDevKit && (g_modules_array[mod_index].flags & IsDebug) != 0) {
            continue;
        }
        const s32 result = i != 0 ? LoadModuleInternal(mod_index, 0, nullptr, nullptr)
                                  : LoadModuleInternal(mod_index, argc, argv, res_out);
        if (result != ORBIS_OK) {
            return result;
        }
    }
    return ORBIS_OK;
}

s32 UnloadModule(s32 id, s32* res_out) {
    OrbisSysmoduleModuleInternal mod{};
    for (size_t i = 0; i < g_modules_array.size(); ++i) {
        mod = g_modules_array[i];
        if (static_cast<s32>(mod.id) != id) {
            continue;
        }
        if (i == 0x22) { // libSceDiscMap is not checked
            continue;
        }
        for (const u32 index : g_preload_list_2) {
            if (index == i && mod.is_loaded == 1) {
                return ORBIS_OK;
            }
        }
        break;
    }
    if (static_cast<s32>(mod.id) != id || mod.id == 0) {
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    if (mod.num_to_load == 0 || mod.to_load == nullptr) {
        return ORBIS_SYSMODULE_LOCK_FAILED;
    }
    for (s64 i = 0; i < mod.num_to_load; i++) {
        // The oracle works on a copy here, so the load counters are not decremented either.
        OrbisSysmoduleModuleInternal dep = g_modules_array[mod.to_load[i]];
        if ((dep.flags & IsDebug) != 0 && !kDevKit) {
            continue;
        }
        if (dep.is_loaded == 0) {
            return ORBIS_SYSMODULE_NOT_LOADED;
        }
        dep.is_loaded--;
        if (i == 0 && res_out != nullptr) {
            *res_out = ORBIS_OK;
        }
    }
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 SceSysmoduleLoadModule(u16 id) {
    SSPC_LOG(Loader, Info, "sceSysmoduleLoadModule({:#x})", id);
    s32 result = ValidateModuleId(id);
    if (result < ORBIS_OK) {
        return result;
    }
    {
        std::scoped_lock lock{g_mutex};
        result = LoadModule(id, 0, nullptr, nullptr);
    }
    if (result == ORBIS_KERNEL_ERROR_ESTART) {
        return SdkVersion() < FW_1150 ? ORBIS_KERNEL_ERROR_EINVAL : ORBIS_KERNEL_ERROR_ESTART;
    }
    return result;
}

SSPC_SYSV_ABI s32 SceSysmoduleUnloadModule(u16 id) {
    SSPC_LOG(Loader, Info, "sceSysmoduleUnloadModule({:#x})", id);
    if (id == 0) {
        return ORBIS_SYSMODULE_INVALID_ID;
    }
    std::scoped_lock lock{g_mutex};
    return UnloadModule(id, nullptr);
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    {"g8cM39EUZ6o", "libSceSysmodule", "libSceSysmodule", "sceSysmoduleLoadModule",
     Fn(&SceSysmoduleLoadModule), nullptr, "sysmodule"},
    {"eR2bZFAAU0Q", "libSceSysmodule", "libSceSysmodule", "sceSysmoduleUnloadModule",
     Fn(&SceSysmoduleUnloadModule), nullptr, "sysmodule"},
};

} // namespace

s32 PreloadForLibkernel() {
    SSPC_LOG(Loader, Info, "sceSysmodulePreloadModuleForLibkernel");
    const u64 sdk = SdkVersion();
    for (const u32 module_index : g_preload_list_3) {
        // libSceDbg, libScePerf, libSceMat, libSceRazorCpu_debug: devkit only
        if ((module_index == 0x12 || module_index == 0x1e || module_index == 0x24 ||
             module_index == 0x26) && !kDevKit) {
            continue;
        }
        if (module_index == 0x22 && sdk >= FW_200) { // libSceDiscMap
            continue;
        }
        if (module_index == 0x23 && !kDevKit) { // libSceDbgAssist
            continue;
        }
        if (module_index == 0x25 && sdk < FW_450 && !kDevKit) { // libSceRazorCpu
            continue;
        }
        if (module_index == 0x28 && sdk < FW_700) { // libSceHttp2
            continue;
        }
        if ((module_index == 0x29 || module_index == 0x2a) && sdk < FW_750) {
            continue;
        }
        if (module_index == 1 || module_index == 2) { // libkernel, libSceLibcInternal
            g_modules_array[module_index].is_loaded++;
            continue;
        }
        if (LoadModuleInternal(static_cast<s32>(module_index), 0, nullptr, nullptr) != ORBIS_OK) {
            SSPC_LOG(Loader, Critical, "sysmodule: cannot preload {}", g_modules_array[module_index].name);
        }
    }
    return ORBIS_OK;
}

std::span<const Export> SysmoduleExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Sysmodule
