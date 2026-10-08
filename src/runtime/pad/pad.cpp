// SecondSonPC — libScePad (NATIVE). See pad.h. Error paths and the data of a controller nobody uses
// follow the Phase 0 oracle; unlike the oracle the handle table is locked (the game may read pads
// from more than one thread).
#include "runtime/pad/pad.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "base/log.h"
#include "input/keyboard_mouse.h"
#include "runtime/exports.h"
#include "runtime/user/user_service.h"

namespace SecondSon::Pad {

namespace {

constexpr s32 ORBIS_OK = 0;
constexpr s32 ORBIS_PAD_ERROR_INVALID_ARG = static_cast<s32>(0x80920001);
constexpr s32 ORBIS_PAD_ERROR_INVALID_HANDLE = static_cast<s32>(0x80920003);
constexpr s32 ORBIS_PAD_ERROR_ALREADY_OPENED = static_cast<s32>(0x80920004);
constexpr s32 ORBIS_PAD_ERROR_NOT_INITIALIZED = static_cast<s32>(0x80920005);
constexpr s32 ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING = static_cast<s32>(0x80920006);
constexpr s32 ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER = static_cast<s32>(0x809b0001);
constexpr s32 ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN = static_cast<s32>(0x809b0081);

constexpr s32 kPortStandard = 0;
constexpr s32 kPortRemoteControl = 16;
constexpr s32 kUserSystem = 255; // ORBIS_USER_SERVICE_USER_ID_SYSTEM
constexpr u32 kIntercepted = 0x80000000;

// ---- libScePad structures (layouts of the PS4 SDK, checked below)

struct PadTouch {
    u16 x;
    u16 y;
    u8 id;
    u8 reserve[3];
};

struct PadData {
    u32 buttons;
    u8 left_x, left_y, right_x, right_y;
    u8 l2, r2, analog_padding[2];
    float orientation[4]; // x, y, z, w
    float acceleration[3];
    float angular_velocity[3];
    u8 touch_num;
    u8 touch_reserve[3];
    u32 time_since_touch_held_down;
    PadTouch touch[2];
    bool connected;
    u64 timestamp;
    u32 extension_unit_id;
    u8 extension_reserve;
    u8 extension_length;
    u8 extension_data[10];
    u8 connected_count;
    u8 reserve[2];
    u8 device_unique_data_len;
    u8 device_unique_data[12];
};
static_assert(sizeof(PadData) == 120);
static_assert(offsetof(PadData, orientation) == 12);
static_assert(offsetof(PadData, touch_num) == 52);
static_assert(offsetof(PadData, connected) == 76);
static_assert(offsetof(PadData, timestamp) == 80);
static_assert(offsetof(PadData, connected_count) == 104);

struct ControllerInformation {
    float touch_pixel_density;
    u16 touch_resolution_x;
    u16 touch_resolution_y;
    u8 dead_zone_left;
    u8 dead_zone_right;
    u8 connection_type;
    u8 connected_count;
    bool connected;
    s32 device_class;
    u8 reserve[8];
};
static_assert(sizeof(ControllerInformation) == 28);
static_assert(offsetof(ControllerInformation, device_class) == 16);

struct LightBarParam {
    u8 r, g, b, reserve;
};

struct VibrationParam {
    u8 large_motor;
    u8 small_motor;
};

/// Light bar colours of player slots 1..4 (blue, red, green, pink), as the oracle sets them on open.
constexpr u8 kUserColours[kSlots][3] = {{0, 0, 255}, {255, 0, 0}, {0, 255, 0}, {255, 0, 255}};

constexpr s32 kNoDevice = -1; // a port no controller ever connects to (the oracle's controller 4)
constexpr s32 kNoHandle = -2;

struct Opened {
    s32 user;
    s32 type;
    s32 index;
    s32 slot; // 0..3, or kNoDevice
};

std::mutex g_mutex;
Host* g_host = nullptr;
bool g_initialized = false;
s32 g_next_handle = 1;
std::map<s32, Opened> g_handles;
u64 g_virtual_touch_down_us = 0;
bool g_virtual_touch = false;

s32 SlotOf(s32 handle) {
    const auto it = g_handles.find(handle);
    return it == g_handles.end() ? kNoHandle : it->second.slot;
}

u32 Deflection(u8 x, u8 y) {
    return static_cast<u32>(std::abs(x - 128) + std::abs(y - 128));
}

/// The controller of a slot now (g_mutex held). Player 1: keyboard/mouse merged with a real
/// controller; the other slots: a real controller or nothing.
void Fill(PadData& d, s32 slot) {
    std::memset(&d, 0, sizeof(d));
    d.orientation[3] = 1.0f;
    Host* host = g_host;
    if (slot == kNoDevice || host == nullptr) {
        return; // never connected: zeros, identity orientation, connected = false (as the oracle)
    }
    const u64 now = host->ProcessTimeUs();
    PhysicalState p;
    const bool physical = host->ReadPhysical(slot, p);
    const bool keyboard = slot == 0;
    if (!physical && !keyboard) {
        return;
    }
    d.connected = true;
    d.timestamp = now;
    d.connected_count = physical ? p.connected_count : 1;
    if (host->OverlayHasInput()) {
        d.buttons = kIntercepted;
        d.left_x = d.left_y = d.right_x = d.right_y = 128;
        return;
    }

    Input::PadState v;
    if (keyboard) {
        v = Input::Player1().Read(now);
    }
    u32 buttons = v.buttons;
    u8 lx = v.lx, ly = v.ly, rx = v.rx, ry = v.ry, l2 = v.l2, r2 = v.r2;
    if (physical) {
        buttons |= p.buttons;
        if (Deflection(p.lx, p.ly) > Deflection(lx, ly)) {
            lx = p.lx;
            ly = p.ly;
        }
        if (Deflection(p.rx, p.ry) > Deflection(rx, ry)) {
            rx = p.rx;
            ry = p.ry;
        }
        l2 = std::max(l2, p.l2);
        r2 = std::max(r2, p.r2);
    }
    d.buttons = buttons;
    d.left_x = lx;
    d.left_y = ly;
    d.right_x = rx;
    d.right_y = ry;
    d.l2 = l2;
    d.r2 = r2;

    // Motion: the virtual controller while it is being moved, else the real one, else at rest.
    const bool virtual_motion = keyboard && (v.motion.active || !physical);
    const Input::Quat& q = virtual_motion ? v.motion.orientation : p.orientation;
    const Input::Vec3& a = virtual_motion ? v.motion.acceleration : p.acceleration;
    const Input::Vec3& w = virtual_motion ? v.motion.angular_velocity : p.angular_velocity;
    d.orientation[0] = q.x;
    d.orientation[1] = q.y;
    d.orientation[2] = q.z;
    d.orientation[3] = q.w;
    d.acceleration[0] = a.x;
    d.acceleration[1] = a.y;
    d.acceleration[2] = a.z;
    d.angular_velocity[0] = w.x;
    d.angular_velocity[1] = w.y;
    d.angular_velocity[2] = w.z;

    // Touch pad: a virtual finger wins over a real one.
    const bool virtual_touch = keyboard && v.touch_active;
    if (virtual_touch && !g_virtual_touch) {
        g_virtual_touch_down_us = now;
    }
    g_virtual_touch = virtual_touch;
    u32 held_us = 0;
    if (virtual_touch) {
        for (const auto& t : v.touch) {
            if (t.down) {
                d.touch[d.touch_num].x = t.x;
                d.touch[d.touch_num].y = t.y;
                d.touch[d.touch_num].id = t.id;
                ++d.touch_num;
            }
        }
        held_us = static_cast<u32>(now - g_virtual_touch_down_us);
    } else if (physical) {
        d.touch_num = p.touch_num;
        for (size_t i = 0; i < p.touch.size(); ++i) {
            d.touch[i].x = p.touch[i].x;
            d.touch[i].y = p.touch[i].y;
            d.touch[i].id = p.touch[i].id;
        }
        held_us = p.time_since_touch;
    }
    if (host->ReportsTouchTime()) {
        d.time_since_touch_held_down = held_us;
    }
}

// ---- exports

SSPC_SYSV_ABI s32 Init() {
    std::scoped_lock lock{g_mutex};
    g_initialized = true;
    SSPC_LOG(Input, Info, "scePadInit");
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 Open(s32 user_id, s32 type, s32 index, const void* /*param*/) {
    s32 handle;
    s32 slot;
    {
        std::scoped_lock lock{g_mutex};
        if (!g_initialized) {
            return ORBIS_PAD_ERROR_NOT_INITIALIZED;
        }
        if (user_id < 0) {
            return ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER;
        }
        for (const auto& [h, o] : g_handles) {
            if (o.user == user_id && o.type == type && o.index == index) {
                return ORBIS_PAD_ERROR_ALREADY_OPENED;
            }
        }
        if (user_id == kUserSystem) {
            if (type != kPortRemoteControl) {
                return ORBIS_DEVICE_SERVICE_ERROR_INVALID_USER;
            }
            handle = g_next_handle++;
            g_handles[handle] = {user_id, type, index, kNoDevice};
            SSPC_LOG(Input, Info, "scePadOpen: TV remote, handle {}", handle);
            return handle;
        }
        if (type == kPortRemoteControl) {
            return ORBIS_PAD_ERROR_INVALID_ARG;
        }
        const s32 player = user_id - UserService::kFirstUserId + 1;
        if (player < 1 || player > kSlots) {
            return ORBIS_DEVICE_SERVICE_ERROR_USER_NOT_LOGIN;
        }
        handle = g_next_handle++;
        // Only the standard port has a controller; others (special devices) stay unconnected.
        slot = type == kPortStandard ? player - 1 : kNoDevice;
        g_handles[handle] = {user_id, type, index, slot};
    }
    SSPC_LOG(Input, Info, "scePadOpen: user {} type {} index {} -> handle {} (slot {}{})", user_id,
             type, index, handle, slot + 1,
             slot == 0 ? ", keyboard/mouse + real controller" : "");
    // As the oracle: the light bar takes the player's colour and the orientation is reset.
    if (slot >= 0) {
        if (slot == 0) {
            Input::Player1().ResetOrientation();
        }
        if (Host* host = g_host) {
            host->SetLightBar(slot, kUserColours[slot][0], kUserColours[slot][1], kUserColours[slot][2]);
            host->ResetOrientation(slot);
        }
    }
    return handle;
}

SSPC_SYSV_ABI s32 Close(s32 handle) {
    std::scoped_lock lock{g_mutex};
    if (g_handles.erase(handle) == 0) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    SSPC_LOG(Input, Info, "scePadClose: handle {}", handle);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 ReadState(s32 handle, PadData* data) {
    if (data == nullptr) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    std::scoped_lock lock{g_mutex};
    const s32 slot = SlotOf(handle);
    if (slot == kNoHandle) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    Fill(*data, slot);
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetControllerInformation(s32 handle, ControllerInformation* info) {
    std::scoped_lock lock{g_mutex};
    const s32 slot = SlotOf(handle);
    if (slot == kNoHandle) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    if (info == nullptr) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    // Connection only: reading the whole state here would use up the mouse movement of the
    // next scePadReadState.
    bool connected = false;
    u8 connected_count = 0;
    if (Host* host = g_host; host != nullptr && slot != kNoDevice) {
        PhysicalState p;
        const bool physical = host->ReadPhysical(slot, p);
        connected = physical || slot == 0;
        connected_count = physical ? p.connected_count : (connected ? 1 : 0);
    }
    std::memset(info, 0, sizeof(*info));
    info->touch_pixel_density = 1.0f;
    info->touch_resolution_x = 1920;
    info->touch_resolution_y = 950;
    info->dead_zone_left = 1;
    info->dead_zone_right = 1;
    info->connection_type = 0; // local
    info->connected_count = connected_count;
    info->connected = connected;
    info->device_class = 0; // standard controller
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 SetLightBar(s32 handle, const LightBarParam* param) {
    s32 slot;
    {
        std::scoped_lock lock{g_mutex};
        slot = SlotOf(handle);
    }
    if (slot == kNoHandle) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    if (param == nullptr) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    if (param->r < 0xD && param->g < 0xD && param->b < 0xD) {
        return ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING;
    }
    if (Host* host = g_host; host != nullptr && slot >= 0) {
        host->SetLightBar(slot, param->r, param->g, param->b);
    }
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 SetVibration(s32 handle, const VibrationParam* param) {
    s32 slot;
    {
        std::scoped_lock lock{g_mutex};
        slot = SlotOf(handle);
    }
    if (slot == kNoHandle) {
        return ORBIS_PAD_ERROR_INVALID_HANDLE;
    }
    if (param == nullptr) {
        return ORBIS_PAD_ERROR_INVALID_ARG;
    }
    if (Host* host = g_host; host != nullptr && slot >= 0) {
        host->SetVibration(slot, param->large_motor, param->small_motor);
    }
    return ORBIS_OK;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"hv1luiJrqQM", "libScePad", "libScePad", "scePadInit", Fn(&Init), nullptr, "pad"},
    {"xk0AcarP3V4", "libScePad", "libScePad", "scePadOpen", Fn(&Open), nullptr, "pad"},
    {"6ncge5+l5Qs", "libScePad", "libScePad", "scePadClose", Fn(&Close), nullptr, "pad"},
    {"YndgXqQVV7c", "libScePad", "libScePad", "scePadReadState", Fn(&ReadState), nullptr, "pad"},
    {"gjP9-KQzoUk", "libScePad", "libScePad", "scePadGetControllerInformation", Fn(&GetControllerInformation), nullptr, "pad"},
    {"RR4novUEENY", "libScePad", "libScePad", "scePadSetLightBar", Fn(&SetLightBar), nullptr, "pad"},
    {"yFVnOdGxvZY", "libScePad", "libScePad", "scePadSetVibration", Fn(&SetVibration), nullptr, "pad"},
};

} // namespace

void SetHost(Host* host) {
    std::scoped_lock lock{g_mutex};
    g_host = host;
}

std::span<const Runtime::Export> PadExports() {
    return kExports;
}

} // namespace SecondSon::Pad
