// SecondSonPC — libSceSystemService (NATIVE part): sceSystemServiceParamGetInt / ParamGetString.
#include "runtime/system/system_service.h"

#include <cstring>
#include <mutex>
#include <windows.h>

#include "base/log.h"
#include "runtime/language.h"

namespace SecondSon::SystemService {

namespace {

Settings g_settings;
std::function<void()> g_first_get_int_hook;
std::once_flag g_first_get_int_once;

// SCE_SYSTEM_PARAM_DATE_FORMAT_* / TIME_FORMAT_* / ENTER_BUTTON_ASSIGN_*
constexpr s32 kDateYYYYMMDD = 0;
constexpr s32 kDateDDMMYYYY = 1;
constexpr s32 kDateMMDDYYYY = 2;
constexpr s32 kTime12Hour = 0;
constexpr s32 kTime24Hour = 1;
constexpr s32 kEnterCircle = 0;
constexpr s32 kEnterCross = 1;

s32 DateFormatFor(int lang) {
    switch (lang) {
    case 1: // en-US
        return kDateMMDDYYYY;
    case 0:  // ja-JP
    case 9:  // ko-KR
    case 10: // zh-TW
    case 11: // zh-CN
        return kDateYYYYMMDD;
    default:
        return kDateDDMMYYYY;
    }
}

const char* ParamName(s32 id) {
    switch (static_cast<ParamId>(id)) {
    case ParamId::Lang:
        return "Lang";
    case ParamId::DateFormat:
        return "DateFormat";
    case ParamId::TimeFormat:
        return "TimeFormat";
    case ParamId::TimeZone:
        return "TimeZone";
    case ParamId::Summertime:
        return "Summertime";
    case ParamId::SystemName:
        return "SystemName";
    case ParamId::GameParentalLevel:
        return "GameParentalLevel";
    case ParamId::EnterButtonAssign:
        return "EnterButtonAssign";
    }
    return "?";
}

} // namespace

void Configure(const Settings& settings) {
    g_settings = settings;
}

void SetFirstParamGetIntHook(std::function<void()> hook) {
    g_first_get_int_hook = std::move(hook);
}

SSPC_SYSV_ABI s32 ParamGetInt(s32 param_id, s32* value) {
    if (g_first_get_int_hook) {
        std::call_once(g_first_get_int_once, [] { g_first_get_int_hook(); });
    }
    if (value == nullptr) {
        SSPC_LOG(SystemService, Error, "ParamGetInt({} {}): value == nullptr -> {:#x}", param_id,
                 ParamName(param_id), static_cast<u32>(kErrorParameter));
        return kErrorParameter;
    }

    TIME_ZONE_INFORMATION tz{};
    s32 result = 0;
    switch (static_cast<ParamId>(param_id)) {
    case ParamId::Lang: {
        result = g_settings.language_id;
        const auto* lang = Runtime::FindLanguageById(result);
        *value = result;
        SSPC_LOG(Language, Info, "ParamGetInt(Lang) -> {} ({}, game files lang_{}_*) ret=0", result,
                 lang ? lang->locale : "?", lang ? lang->game_tag : "?");
        return kOk;
    }
    case ParamId::DateFormat:
        result = DateFormatFor(g_settings.language_id);
        break;
    case ParamId::TimeFormat:
        result = g_settings.language_id == 1 ? kTime12Hour : kTime24Hour;
        break;
    case ParamId::TimeZone:
        // Minutes east of UTC, without DST (DST is reported separately as Summertime).
        GetTimeZoneInformation(&tz);
        result = -static_cast<s32>(tz.Bias);
        break;
    case ParamId::Summertime:
        result = GetTimeZoneInformation(&tz) == TIME_ZONE_ID_DAYLIGHT ? 1 : 0;
        break;
    case ParamId::GameParentalLevel:
        result = 0; // SCE_SYSTEM_PARAM_GAME_PARENTAL_OFF
        break;
    case ParamId::EnterButtonAssign:
        result = g_settings.enter_button_cross ? kEnterCross : kEnterCircle;
        break;
    case ParamId::SystemName:
    default:
        // SystemName is a string parameter; unknown ids are rejected (no fake success).
        SSPC_LOG(SystemService, Warning, "ParamGetInt({} {}): unsupported id -> {:#x}", param_id,
                 ParamName(param_id), static_cast<u32>(kErrorParameter));
        return kErrorParameter;
    }
    *value = result;
    SSPC_LOG(SystemService, Debug, "ParamGetInt({} {}) -> {} ret=0", param_id, ParamName(param_id),
             result);
    return kOk;
}

SSPC_SYSV_ABI s32 ParamGetString(s32 param_id, char* buf, u64 buf_size) {
    if (buf == nullptr || buf_size == 0) {
        SSPC_LOG(SystemService, Error, "ParamGetString({} {}, buf={}, size={}): bad buffer -> {:#x}",
                 param_id, ParamName(param_id), static_cast<void*>(buf), buf_size,
                 static_cast<u32>(kErrorParameter));
        return kErrorParameter;
    }
    if (static_cast<ParamId>(param_id) != ParamId::SystemName) {
        SSPC_LOG(SystemService, Warning,
                 "ParamGetString({} {}, size={}): unsupported id -> {:#x}, buffer untouched",
                 param_id, ParamName(param_id), buf_size, static_cast<u32>(kErrorParameter));
        return kErrorParameter;
    }
    const auto& name = g_settings.system_name;
    const u64 limit = buf_size < kSystemNameMax ? buf_size : kSystemNameMax;
    const u64 copied = name.size() < limit ? name.size() : limit - 1;
    std::memcpy(buf, name.data(), copied);
    buf[copied] = '\0';
    SSPC_LOG(SystemService, Info, "ParamGetString(SystemName, buf={}, size={}) -> \"{}\" ({} bytes{}) ret=0",
             static_cast<void*>(buf), buf_size, std::string_view(buf, copied), copied,
             copied < name.size() ? ", truncated" : "");
    return kOk;
}

} // namespace SecondSon::SystemService
