// SecondSonPC — libSceSystemService (NATIVE part).
#pragma once

#include <functional>
#include <string>

#include "base/abi.h"

namespace SecondSon::SystemService {

// SCE_SYSTEM_SERVICE_PARAM_ID_*
enum class ParamId : s32 {
    Lang = 1,
    DateFormat = 2,
    TimeFormat = 3,
    TimeZone = 4,
    Summertime = 5,
    SystemName = 6,
    GameParentalLevel = 7,
    EnterButtonAssign = 1000,
};

constexpr s32 kOk = 0;
constexpr s32 kErrorParameter = static_cast<s32>(0x80A10003); // SCE_SYSTEM_SERVICE_ERROR_PARAMETER
constexpr u64 kSystemNameMax = 65;                            // 64 bytes + NUL (game passes 0x41)

struct Settings {
    int language_id = 1;
    std::string system_name; // empty by default, see docs/KNOWN_ISSUES.md SSPC-001
    bool enter_button_cross = true;
};

void Configure(const Settings& settings);

/// DIAGNOSTIC: runs once, on the guest thread, at the first ParamGetInt call (a point both loader
/// paths reach with every module loaded and relocated; used for relocation snapshots).
void SetFirstParamGetIntHook(std::function<void()> hook);

// NID fZo48un7LK4. s32 sceSystemServiceParamGetInt(s32 paramId, s32* value)
SSPC_SYSV_ABI s32 ParamGetInt(s32 param_id, s32* value);

// NID SsC-m-S9JTA. s32 sceSystemServiceParamGetString(s32 paramId, char* buf, size_t bufSize)
// Second Son calls (6 /*SystemName*/, stack buffer, 0x41) at eboot 0x15d6ee and strlen()s the
// result without initializing the buffer first, so the buffer must always be written.
SSPC_SYSV_ABI s32 ParamGetString(s32 param_id, char* buf, u64 buf_size);

} // namespace SecondSon::SystemService
