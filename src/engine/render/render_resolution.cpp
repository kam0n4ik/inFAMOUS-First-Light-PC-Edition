// SecondSonPC — the game's internal render resolution (see render_resolution.h).
#include "engine/render/render_resolution.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"

namespace SecondSon::Engine::Render {

namespace {

// 0x1eff47: mov dword [rbx+0xC], 0x780 ; mov dword [rbx+0x10], 0x438 (render width / height of the PS4 build).
constexpr u64 kRenderSizeSite = 0x1eff47;
constexpr u8 kRenderSizeBytes[14] = {0xC7, 0x43, 0x0C, 0x80, 0x07, 0x00, 0x00,
                                     0xC7, 0x43, 0x10, 0x38, 0x04, 0x00, 0x00};

std::atomic<u32> g_width{1920};
std::atomic<u32> g_height{1080};

std::string Env(const char* name) {
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

} // namespace

RenderSize RenderSizeForScale(float scale) {
    scale = std::clamp(scale, 1.0f, 3.0f);
    auto round = [](float v, u32 step) { return std::max<u32>(step, static_cast<u32>(std::lround(v / step)) * step); };
    RenderSize s{round(kDisplaySize.width / scale, 16), round(kDisplaySize.height / scale, 8)};
    s.width = std::clamp<u32>(s.width, 640, kDisplaySize.width);
    s.height = std::clamp<u32>(s.height, 360, kDisplaySize.height);
    return s;
}

bool InstallRenderResolution(u64 eboot_base, RenderSize size) {
    if (const std::string env = Env("SSPC_RENDER_RES"); !env.empty()) {
        unsigned w = 0, h = 0;
        if (std::sscanf(env.c_str(), "%ux%u", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 1920 && h <= 1080) {
            size = {w, h};
        } else {
            SSPC_LOG(Engine, Warning, "SSPC_RENDER_RES='{}' is not WxH within 320x180..1920x1080 - ignored", env);
        }
    }
    size.width &= ~1u;
    size.height &= ~1u;
    if (size.width == 1920 && size.height == 1080) {
        return true;
    }
    auto* at = reinterpret_cast<u8*>(eboot_base + kRenderSizeSite);
    if (std::memcmp(at, kRenderSizeBytes, sizeof(kRenderSizeBytes)) != 0) {
        SSPC_LOG(Engine, Error, "Render resolution {}x{} not applied: eboot+{:#x} is not the analysed code (another "
                                "game version?) - the game renders at 1920x1080",
                 size.width, size.height, kRenderSizeSite);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(kRenderSizeBytes), PAGE_EXECUTE_READWRITE, &old)) {
        SSPC_LOG(Engine, Error, "Render resolution not applied: VirtualProtect failed ({})", GetLastError());
        return false;
    }
    std::memcpy(at + 3, &size.width, 4);
    std::memcpy(at + 10, &size.height, 4);
    DWORD unused = 0;
    VirtualProtect(at, sizeof(kRenderSizeBytes), old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kRenderSizeBytes));
    g_width.store(size.width, std::memory_order_release);
    g_height.store(size.height, std::memory_order_release);
    SSPC_LOG(Engine, Warning, "Render resolution: the game's scene renders at {}x{} (display {}x{}; eboot+{:#x})",
             size.width, size.height, kDisplaySize.width, kDisplaySize.height, kRenderSizeSite);
    return true;
}

namespace {

constexpr ProbeSite kCameraJitterSite{{0x472810, 0x1ed, 0xaf06825055bbe18dull}, 17};
constexpr GuestFunction kResetWindow{0x581a10, 0x34, 0x26b44a8bd1f71035ull}; // Camera::ResetFrustumWindow
constexpr u64 kTemporalAaFlag = 0xda07f9; // byte: SMAA T2x on (camera jitter + two-frame resolve)
constexpr u64 kMainCamera = 0x7f2d2b8;    // Camera*
constexpr u64 kCameraWindow = 0x5ac;      // float left, bottom, right, top of the frustum window

using CameraJitterFn = SSPC_SYSV_ABI void (*)(u64 ctx);
using ResetWindowFn = SSPC_SYSV_ABI void (*)(u64 camera);
CameraJitterFn g_camera_jitter = nullptr;
ResetWindowFn g_reset_window = nullptr;
u64 g_base = 0;
std::atomic<bool> g_taa_off{false};
std::atomic<u64> g_taa_cleared{0};

SSPC_SYSV_ABI void CameraJitterProbe(u64 ctx) {
    g_camera_jitter(ctx);
    if (!g_taa_off.load(std::memory_order_relaxed)) {
        return;
    }
    auto* flag = reinterpret_cast<volatile u8*>(g_base + kTemporalAaFlag);
    if (*flag != 0) {
        *flag = 0;
        g_taa_cleared.fetch_add(1, std::memory_order_relaxed);
    }
    const u64 camera = *reinterpret_cast<const u64*>(g_base + kMainCamera);
    if (camera != 0) {
        const auto* w = reinterpret_cast<const float*>(camera + kCameraWindow);
        if (w[0] != -1.0f || w[1] != -1.0f || w[2] != 1.0f || w[3] != 1.0f) {
            g_reset_window(camera); // (the jitter T2x left on it)
        }
    }
}

} // namespace

bool InstallTemporalAaControl(u64 eboot_base) {
    std::string error;
    if (!VerifyGuestFunction(eboot_base, kResetWindow, error)) {
        SSPC_LOG(Engine, Error, "Temporal AA control not installed (the game's SMAA T2x stays): {}", error);
        return false;
    }
    void* original =
        InstallEntryProbe(eboot_base, kCameraJitterSite, reinterpret_cast<const void*>(&CameraJitterProbe), error);
    if (original == nullptr) {
        SSPC_LOG(Engine, Error, "Temporal AA control not installed (the game's SMAA T2x stays): {}", error);
        return false;
    }
    g_base = eboot_base;
    g_reset_window = reinterpret_cast<ResetWindowFn>(eboot_base + kResetWindow.rva);
    g_camera_jitter = reinterpret_cast<CameraJitterFn>(original);
    SSPC_LOG(Engine, Info, "Temporal AA control: entry probe on CameraJitter 0x472810 (SMAA T2x {})",
             g_taa_off.load() ? "off: the upscaler jitters the scene" : "the game's");
    return true;
}

void SetTemporalAaOff(bool off) {
    g_taa_off.store(off, std::memory_order_relaxed);
}

RenderSize CurrentRenderSize() {
    return {g_width.load(std::memory_order_acquire), g_height.load(std::memory_order_acquire)};
}

} // namespace SecondSon::Engine::Render
