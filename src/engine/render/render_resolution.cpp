// SecondSonPC — the game's internal render resolution (see render_resolution.h).
#include "engine/render/render_resolution.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

namespace {

// The render size changed while the game runs - the PS4 Pro's own path. FrameBegin 0x1f0be0 (every frame, the game's
// render thread) switches the Pro's modes: it writes the device's render size (+0xC/+0x10), the per-size-class table
// (device +0xdf0f14: w, h pairs; the scene targets' current sizes) from it, re-describes the display targets
// (RenderTarget::Configure 0x300b00) and every scene target (ConfigureSceneTargets 0x18e210 on 0xe682b0). Their memory
// was allocated at start from the table of maxima (+0xdf0f7c, AllocateTarget 0x300a90 -> 0x2ffeb0): a size up to the
// one the game started with fits without new memory.
constexpr ProbeSite kFrameBeginSite{{0x1f0be0, 0x5aa, 0x6273d28a515609b7ull}, 20};
constexpr GuestFunction kConfigureTarget{0x300b00, 0x2bb, 0x4489645ef331292aull};
constexpr GuestFunction kConfigureSceneTargets{0x18e210, 0x2f9, 0x804f07e3bc802864ull};
constexpr u64 kSceneTargets = 0xe682b0; // ConfigureSceneTargets' object
constexpr u64 kQuarterHeightFlag = 0x1d0cbd5;
constexpr u64 kSizeTable = 0xdf0f14; // device offset: the size classes' current w, h (u32 pairs)

using FrameBeginFn = SSPC_SYSV_ABI void (*)(u64 device, u32 arg);
using ConfigureFn = SSPC_SYSV_ABI void (*)(u64 object);
FrameBeginFn g_frame_begin = nullptr;
ConfigureFn g_configure_target = nullptr;
ConfigureFn g_configure_scene = nullptr;
u64 g_switch_base = 0;
std::atomic<u64> g_requested{0}; // w << 32 | h (0: none)
std::atomic<u64> g_frames{0};
RenderSize g_started{1920, 1080}; // the size the targets' memory was allocated for
struct DiagSwitch {
    u64 frame;
    RenderSize size;
};
std::vector<DiagSwitch> g_diag;

void WriteSizes(u64 device, u32 w, u32 h) {
    auto* d = reinterpret_cast<u8*>(device);
    const auto put = [&](u64 off, u32 a, u32 b) {
        std::memcpy(d + off, &a, 4);
        std::memcpy(d + off + 4, &b, 4);
    };
    const bool quarter_h = *reinterpret_cast<const u8*>(g_switch_base + kQuarterHeightFlag) != 0;
    // (as VideoOut init 0x1eff00 derives them from the render size, PS4 - not checkerboard)
    std::memcpy(d + 0xC, &w, 4);
    std::memcpy(d + 0x10, &h, 4);
    put(0xdf0f1c, w, h);
    put(0xdf0f64, w, h);
    put(0xdf0f6c, w, h);
    put(0xdf0f24, w, h);
    const u32 hw = w >> 1, hh = h >> 1;
    put(0xdf0f74, hw, hh);
    const u32 qh = quarter_h ? h >> 2 : hh;
    put(0xdf0f2c, hw, qh);
    put(0xdf0f5c, (hw + 0x13) / 0x14, (qh + 0x13) / 0x14);
    const u32 sw = std::max<u32>(0x1e0, w >> 2), sh = std::max<u32>(0x10e, qh >> 1);
    put(0xdf0f34, sw, sh);
    put(0xdf0f3c, sw >> 1, sh >> 1);
    put(0xdf0f44, sw >> 2, sh >> 2);
    put(0xdf0f4c, sw >> 3, sh >> 3);
    put(0xdf0f54, sw >> 4, sh >> 4);
}

SSPC_SYSV_ABI void FrameBeginProbe(u64 device, u32 arg) {
    const u64 frame = g_frames.fetch_add(1, std::memory_order_relaxed);
    for (const DiagSwitch& s : g_diag) {
        if (s.frame == frame) {
            g_requested.store(u64{s.size.width} << 32 | s.size.height, std::memory_order_release);
        }
    }
    if (const u64 req = g_requested.exchange(0, std::memory_order_acq_rel); req != 0) {
        const u32 w = static_cast<u32>(req >> 32), h = static_cast<u32>(req);
        const RenderSize cur = CurrentRenderSize();
        if (w != cur.width || h != cur.height) {
            WriteSizes(device, w, h);
            g_configure_target(device + 0x8c4);
            g_configure_target(device + 0x20a * 8);
            g_configure_target(device + 0x2fc * 8);
            g_configure_scene(g_switch_base + kSceneTargets);
            g_width.store(w, std::memory_order_release);
            g_height.store(h, std::memory_order_release);
            SSPC_LOG(Engine, Warning, "Render resolution: the scene now renders at {}x{} (was {}x{}; frame {})", w, h,
                     cur.width, cur.height, frame);
        }
    }
    g_frame_begin(device, arg);
}

} // namespace

bool InstallRenderSwitch(u64 eboot_base) {
    std::string error;
    if (!VerifyGuestFunction(eboot_base, kConfigureTarget, error) ||
        !VerifyGuestFunction(eboot_base, kConfigureSceneTargets, error)) {
        SSPC_LOG(Engine, Error, "Render resolution switch not installed (the size stays as started): {}", error);
        return false;
    }
    void* original =
        InstallEntryProbe(eboot_base, kFrameBeginSite, reinterpret_cast<const void*>(&FrameBeginProbe), error);
    if (original == nullptr) {
        SSPC_LOG(Engine, Error, "Render resolution switch not installed (the size stays as started): {}", error);
        return false;
    }
    g_switch_base = eboot_base;
    g_configure_target = reinterpret_cast<ConfigureFn>(eboot_base + kConfigureTarget.rva);
    g_configure_scene = reinterpret_cast<ConfigureFn>(eboot_base + kConfigureSceneTargets.rva);
    g_frame_begin = reinterpret_cast<FrameBeginFn>(original);
    g_started = CurrentRenderSize();
    // SSPC_DIAG_RES_SWITCH=frame:WxH[,frame:WxH...]: switches at those game frames (diagnostics)
    if (const std::string env = Env("SSPC_DIAG_RES_SWITCH"); !env.empty()) {
        size_t at = 0;
        while (at < env.size()) {
            const size_t end = std::min(env.find(',', at), env.size());
            unsigned long long f = 0;
            unsigned w = 0, h = 0;
            if (std::sscanf(env.substr(at, end - at).c_str(), "%llu:%ux%u", &f, &w, &h) == 3) {
                g_diag.push_back({f, {w, h}});
            }
            at = end + 1;
        }
        SSPC_LOG(Engine, Warning, "DIAGNOSTIC: SSPC_DIAG_RES_SWITCH='{}' ({} switches)", env, g_diag.size());
    }
    SSPC_LOG(Engine, Info, "Render resolution switch: entry probe on FrameBegin 0x1f0be0 (up to {}x{})",
             g_started.width, g_started.height);
    return true;
}

bool RequestRenderSize(RenderSize size) {
    size.width = std::clamp<u32>(size.width & ~1u, 320, g_started.width);
    size.height = std::clamp<u32>(size.height & ~1u, 180, g_started.height);
    if (g_frame_begin == nullptr) {
        return false;
    }
    g_requested.store(u64{size.width} << 32 | size.height, std::memory_order_release);
    return true;
}

} // namespace SecondSon::Engine::Render
