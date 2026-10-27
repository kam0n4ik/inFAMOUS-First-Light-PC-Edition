// SecondSonPC — the game's internal render resolution (see render_resolution.h).
#include "engine/render/render_resolution.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"

namespace SecondSon::Engine::Render {

namespace {

#if defined(SSPC_PRODUCT_FIRST_LIGHT)
// First Light's VideoOut init: the same code as Second Son's, 0x46a90 further (tools/sigmap.py; each pattern below is
// in First Light's text once)
constexpr u64 kRenderSizeSite = 0x2369d7;
constexpr u64 kDisplaySite = 0x2369e5;
constexpr u64 kScreenSite = 0x236e88;
#else
constexpr u64 kRenderSizeSite = 0x1eff47;
constexpr u64 kDisplaySite = 0x1eff55;
constexpr u64 kScreenSite = 0x1f03f8;
#endif
// 0x1eff47: mov dword [rbx+0xC], 0x780 ; mov dword [rbx+0x10], 0x438 (render width / height of the PS4 build).
constexpr u8 kRenderSizeBytes[14] = {0xC7, 0x43, 0x0C, 0x80, 0x07, 0x00, 0x00,
                                     0xC7, 0x43, 0x10, 0x38, 0x04, 0x00, 0x00};
// 0x1eff55: mov dword [rbx], 0x780 ; mov dword [rbx+4], 0x438 (display width / height)
constexpr u8 kDisplayBytes[13] = {0xC7, 0x03, 0x80, 0x07, 0x00, 0x00, 0xC7, 0x43, 0x04, 0x38, 0x04, 0x00, 0x00};
// 0x1f03f8: mov dword [r15], 0x438 ; mov dword [r12], 0x780 (the screen size 0x1d0ccc4 / 0x1d0ccc0)
constexpr u8 kScreenBytes[15] = {0x41, 0xC7, 0x07, 0x38, 0x04, 0x00, 0x00, 0x41,
                                 0xC7, 0x04, 0x24, 0x80, 0x07, 0x00, 0x00};

std::atomic<u32> g_width{1920};
std::atomic<u32> g_height{1080};
std::atomic<u64> g_display{u64{1920} << 32 | 1080};
std::atomic<u32> g_render_scale{kRenderScaleAuto};
DisplayPlan g_plan{.output = {1920, 1080}, .display = {1920, 1080}, .screen = {1920, 1080}, .scene = {1920, 1080}};

std::string Env(const char* name) {
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

/// Writes the two 32-bit immediates of `bytes` (at `first` and `second`) at eboot+`site` when the code there is
/// `bytes`.
bool PatchImmediates(u64 eboot_base, u64 site, std::span<const u8> bytes, u32 first_at, u32 first, u32 second_at,
                     u32 second, std::string_view what) {
    auto* at = reinterpret_cast<u8*>(eboot_base + site);
    if (std::memcmp(at, bytes.data(), bytes.size()) != 0) {
        SSPC_LOG(Engine, Error, "{} not applied: eboot+{:#x} is not the analysed code (another game version?)", what,
                 site);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(at, bytes.size(), PAGE_EXECUTE_READWRITE, &old)) {
        SSPC_LOG(Engine, Error, "{} not applied: VirtualProtect failed ({})", what, GetLastError());
        return false;
    }
    std::memcpy(at + first_at, &first, 4);
    std::memcpy(at + second_at, &second, 4);
    DWORD unused = 0;
    VirtualProtect(at, bytes.size(), old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, bytes.size());
    return true;
}

/// A diagnostic WxH override of `size` from `name`; false when absent or malformed (logged).
bool EnvSize(const char* name, Size& size) {
    const std::string env = Env(name);
    if (env.empty()) {
        return false;
    }
    Size s;
    if (!ParseSize(env, s)) {
        SSPC_LOG(Engine, Warning, "{}='{}' is not WxH within 320x240..7680x4320 - ignored", name, env);
        return false;
    }
    size = {s.width & ~1u, s.height & ~1u};
    SSPC_LOG(Engine, Warning, "DIAGNOSTIC: {}: {}x{}", name, size.width, size.height);
    return true;
}

} // namespace

void SetDisplayPlan(const DisplayPlan& plan) {
    g_plan = plan;
    g_display.store(u64{plan.display.width} << 32 | plan.display.height, std::memory_order_release);
}

const DisplayPlan& GetDisplayPlan() {
    return g_plan;
}

void SetRenderScale(u32 render_scale) {
    g_render_scale.store(render_scale, std::memory_order_relaxed);
}

RenderSize SceneSizeForRenderScale() {
    const RenderSize d = DisplaySize();
    const Size s = SceneSizeFor({d.width, d.height}, g_render_scale.load(std::memory_order_relaxed));
    return {s.width, s.height};
}

RenderSize RenderSizeForScale(float scale) {
    scale = std::clamp(scale, 1.0f, 3.0f);
    const RenderSize d = DisplaySize();
    auto round = [](float v, u32 step) { return std::max<u32>(step, static_cast<u32>(std::lround(v / step)) * step); };
    RenderSize s{round(d.width / scale, 16), round(d.height / scale, 8)};
    s.width = std::clamp<u32>(s.width, std::min<u32>(640, d.width), d.width);
    s.height = std::clamp<u32>(s.height, std::min<u32>(360, d.height), d.height);
    return s;
}

bool InstallRenderResolution(u64 eboot_base) {
    Size display = g_plan.display, screen = g_plan.screen;
    // the scene starts at the largest size the render scale and the upscalers may ask for (its targets' memory is
    // allocated for it at start); the size asked for is switched to at the game's first frame (InstallRenderSwitch)
    Size scene = SceneSizeFor(display, kRenderScaleMax);
    EnvSize("SSPC_DISPLAY_RES", display);
    EnvSize("SSPC_SCREEN_RES", screen);
    if (EnvSize("SSPC_RENDER_RES", scene)) {
        scene = {std::min(scene.width, display.width), std::min(scene.height, display.height)};
    }
    // all three sites or none (a wide display with the 16:9 screen would be a stretched picture)
    const auto pristine = [eboot_base](u64 site, std::span<const u8> bytes) {
        return std::memcmp(reinterpret_cast<const void*>(eboot_base + site), bytes.data(), bytes.size()) == 0;
    };
    const bool sizes_kept = display == Size{1920, 1080} && screen == Size{1920, 1080} && scene == Size{1920, 1080};
    if (!sizes_kept && !(pristine(kDisplaySite, kDisplayBytes) && pristine(kScreenSite, kScreenBytes) &&
                         pristine(kRenderSizeSite, kRenderSizeBytes))) {
        SSPC_LOG(Engine, Error, "Resolution {}x{} (screen {}x{}, scene {}x{}) not applied: eboot+{:#x} / +{:#x} / "
                 "+{:#x} are not the analysed code (another game version?) - the game stays 1920x1080",
                 display.width, display.height, screen.width, screen.height, scene.width, scene.height, kDisplaySite,
                 kScreenSite, kRenderSizeSite);
        g_display.store(u64{1920} << 32 | 1080, std::memory_order_release);
        return false;
    }
    bool ok = true;
    if (display != Size{1920, 1080}) {
        ok = PatchImmediates(eboot_base, kDisplaySite, kDisplayBytes, 2, display.width, 9, display.height,
                             "Display size");
        g_display.store(u64{display.width} << 32 | display.height, std::memory_order_release);
    }
    if (screen != Size{1920, 1080}) {
        // (h first: mov [r15], h ; mov [r12], w)
        ok = PatchImmediates(eboot_base, kScreenSite, kScreenBytes, 3, screen.height, 11, screen.width,
                             "Screen size") && ok;
    }
    if (scene != Size{1920, 1080} &&
        PatchImmediates(eboot_base, kRenderSizeSite, kRenderSizeBytes, 3, scene.width, 10, scene.height,
                        "Render resolution")) {
        g_width.store(scene.width, std::memory_order_release);
        g_height.store(scene.height, std::memory_order_release);
    }
    const RenderSize d = DisplaySize(), r = CurrentRenderSize();
    if (d.width != 1920 || d.height != 1080 || r.width != 1920 || r.height != 1080 || screen != Size{1920, 1080}) {
        SSPC_LOG(Engine, Warning, "Resolution: display {}x{}, screen {}x{}, scene up to {}x{} (eboot+{:#x}, +{:#x}, "
                 "+{:#x})", d.width, d.height, screen.width, screen.height, r.width, r.height, kDisplaySite,
                 kScreenSite, kRenderSizeSite);
    }
    return ok;
}

namespace {

#if defined(SSPC_PRODUCT_FIRST_LIGHT)
// First Light (research/fl_sites.md §4): the same functions; the three globals read as base + displacement have new
// displacements as well as new bases
constexpr ProbeSite kCameraJitterSite{{0x51c840, 0x205, 0x1403a59b70323dd4ull}, 17};
constexpr GuestFunction kResetWindow{0x65c210, 0x34, 0xba919ade99796653ull};
constexpr ProbeSite kPass32Site{{0x1c0b40, 0x54c, 0xbec00e949ba22471ull}, 20};
constexpr u64 kTemporalAaFlag = 0xe9c829;
constexpr u64 kReevaluate = 0x806be14;
constexpr u64 kDisableSmaa = 0xfae035;
constexpr u64 kNoAa = 0xfa3a5d;
constexpr u64 kMainCamera = 0x81a41f8;
#else
constexpr ProbeSite kCameraJitterSite{{0x472810, 0x1ed, 0xaf06825055bbe18dull}, 17};
constexpr GuestFunction kResetWindow{0x581a10, 0x34, 0x26b44a8bd1f71035ull}; // Camera::ResetFrustumWindow
constexpr ProbeSite kPass32Site{{0x1929c0, 0x42e, 0xcd353017f8e6ce48ull}, 20};
constexpr u64 kTemporalAaFlag = 0xda07f9; // byte: SMAA T2x on (camera jitter + two-frame resolve)
constexpr u64 kReevaluate = 0x7e00964;    // int: frames since T2x last switched (the timer is read from 1 on)
constexpr u64 kDisableSmaa = 0xe87c85;    // byte: the scripts' DISABLE_SMAA override
constexpr u64 kNoAa = 0xe86e3d;           // byte: pass 32 tone-maps into its output, no anti-aliasing
constexpr u64 kMainCamera = 0x7f2d2b8;    // Camera*
#endif
constexpr u64 kCameraWindow = 0x5ac;      // float left, bottom, right, top of the frustum window

enum class AaMode : u32 { Game, T2x, Fxaa, Off };

using CameraJitterFn = SSPC_SYSV_ABI void (*)(u64 ctx);
using Pass32Fn = SSPC_SYSV_ABI void (*)(u64 ctx);
using ResetWindowFn = SSPC_SYSV_ABI void (*)(u64 camera);
CameraJitterFn g_camera_jitter = nullptr;
Pass32Fn g_pass32 = nullptr;
ResetWindowFn g_reset_window = nullptr;
u64 g_base = 0;
std::atomic<bool> g_taa_off{false};              // an upscaler jitters the scene
std::atomic<u32> g_aa_mode{u32(AaMode::Game)}; // [Graphics] antiAliasing
std::atomic<u64> g_taa_cleared{0};
u8 g_last_flag = 0xff; // (the game's thread only: its own T2x / FXAA switches, logged)
u32 g_switches_logged = 0;

AaMode EffectiveAa() {
    const auto mode = static_cast<AaMode>(g_aa_mode.load(std::memory_order_relaxed));
    if (g_taa_off.load(std::memory_order_relaxed)) {
        return mode == AaMode::Off ? AaMode::Off : AaMode::Fxaa;
    }
    return mode;
}

SSPC_SYSV_ABI void Pass32Probe(u64 ctx) {
    if (EffectiveAa() == AaMode::Off) {
        *reinterpret_cast<volatile u8*>(g_base + kNoAa) = 1; // (0x1901a0 writes it again at the next frame)
    }
    g_pass32(ctx);
}

SSPC_SYSV_ABI void CameraJitterProbe(u64 ctx) {
    const AaMode mode = EffectiveAa();
    auto* flag = reinterpret_cast<volatile u8*>(g_base + kTemporalAaFlag);
    if (mode == AaMode::T2x) {
        *reinterpret_cast<volatile s32*>(g_base + kReevaluate) = 0; // (no timer this frame: the flag is kept)
        *flag = 1;
    }
    g_camera_jitter(ctx);
    if (mode == AaMode::Game && *flag != g_last_flag && g_switches_logged < 32) {
        ++g_switches_logged;
        SSPC_LOG(Engine, Info, "Anti-aliasing: the game switched to {}{}", *flag != 0 ? "SMAA T2x" : "FXAA",
                 *reinterpret_cast<const volatile u8*>(g_base + kDisableSmaa) != 0 ? " (DISABLE_SMAA)" : "");
    }
    g_last_flag = *flag;
    if (mode != AaMode::Fxaa && mode != AaMode::Off) {
        return;
    }
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
    void* pass32 = InstallEntryProbe(eboot_base, kPass32Site, reinterpret_cast<const void*>(&Pass32Probe), error,
                                     reinterpret_cast<void**>(&g_pass32));
    if (pass32 == nullptr) {
        SSPC_LOG(Engine, Error, "Anti-aliasing: pass 32's probe not installed (\"off\" acts as FXAA): {}", error);
    } else {
        g_pass32 = reinterpret_cast<Pass32Fn>(pass32);
    }
    static constexpr const char* kNames[] = {"the game's", "SMAA T2x", "FXAA", "off"};
    SSPC_LOG(Engine, Info, "Anti-aliasing control: entry probes on CameraJitter {:#x} and pass 32 {:#x} ({}{})",
             kCameraJitterSite.function.rva, kPass32Site.function.rva, kNames[static_cast<u32>(EffectiveAa())],
             g_taa_off.load() ? ": the upscaler jitters the scene" : "");
    return true;
}

void SetTemporalAaOff(bool off) {
    g_taa_off.store(off, std::memory_order_relaxed);
}

void SetAntiAliasing(const std::string& mode) {
    const AaMode m = mode == "t2x" ? AaMode::T2x : mode == "fxaa" ? AaMode::Fxaa : mode == "off" ? AaMode::Off
                                                                                            : AaMode::Game;
    g_aa_mode.store(static_cast<u32>(m), std::memory_order_relaxed);
}

RenderSize CurrentRenderSize() {
    return {g_width.load(std::memory_order_acquire), g_height.load(std::memory_order_acquire)};
}

RenderSize DisplaySize() {
    const u64 d = g_display.load(std::memory_order_acquire);
    return {static_cast<u32>(d >> 32), static_cast<u32>(d)};
}

namespace {

// The render size changed while the game runs - the PS4 Pro's own path. FrameBegin 0x1f0be0 (every frame, the game's
// render thread) switches the Pro's modes: it writes the device's render size (+0xC/+0x10), the per-size-class table
// (device +0xdf0f14: w, h pairs; the scene targets' current sizes) from it, re-describes the display targets
// (RenderTarget::Configure 0x300b00) and every scene target (ConfigureSceneTargets 0x18e210 on 0xe682b0). Their memory
// was allocated at start from the table of maxima (+0xdf0f7c, AllocateTarget 0x300a90 -> 0x2ffeb0): a size up to the
// one the game started with fits without new memory.
#if defined(SSPC_PRODUCT_FIRST_LIGHT)
// First Light (research/fl_sites.md §2): FrameBegin is Second Son's instruction for instruction; the device's offsets
// (+0xdf0f14 ... +0xdf0f7c) are the same
constexpr ProbeSite kFrameBeginSite{{0x237670, 0x5aa, 0x13ecaea8b414b6f9ull}, 20};
constexpr GuestFunction kConfigureTarget{0x382420, 0x2c3, 0xcb8210727671d826ull};
constexpr GuestFunction kConfigureSceneTargets{0x1bc1d0, 0x2ba, 0x4d6fb6a99a3129faull};
constexpr u64 kSceneTargets = 0xf84740;
constexpr u64 kQuarterHeightFlag = 0x1f271d5;
#else
constexpr ProbeSite kFrameBeginSite{{0x1f0be0, 0x5aa, 0x6273d28a515609b7ull}, 20};
constexpr GuestFunction kConfigureTarget{0x300b00, 0x2bb, 0x4489645ef331292aull};
constexpr GuestFunction kConfigureSceneTargets{0x18e210, 0x2f9, 0x804f07e3bc802864ull};
constexpr u64 kSceneTargets = 0xe682b0; // ConfigureSceneTargets' object
constexpr u64 kQuarterHeightFlag = 0x1d0cbd5;
#endif
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

// The HUD's margins in screen units (0x1d0ccc8 x, 0x1d0cccc y), as the game computes them from the Display Area ratio
// at start (0x1f0407) and on a system change (0x1f2a60, 0x1f2b60): (size - size * ratio) / 2, at least 16.
#if defined(SSPC_PRODUCT_FIRST_LIGHT)
constexpr u64 kScreenWidth = 0x1f272c0; // First Light: the device global + 0xdf0ff0, as Second Son's
constexpr u64 kScreenHeight = 0x1f272c4;
constexpr u64 kMarginX = 0x1f272c8;
constexpr u64 kMarginY = 0x1f272cc;
#else
constexpr u64 kScreenWidth = 0x1d0ccc0;
constexpr u64 kScreenHeight = 0x1d0ccc4;
constexpr u64 kMarginX = 0x1d0ccc8;
constexpr u64 kMarginY = 0x1d0cccc;
#endif
std::atomic<u32> g_safe_area{100};  // %
std::atomic<u32> g_hud_aspect{0};   // the HUD area's width / height * 1000, 0: the whole screen

s32 SafeMargin(s32 size, float ratio) {
    const s32 m = (size - static_cast<s32>(static_cast<float>(size) * ratio)) / 2;
    return m <= 15 ? 16 : m;
}

void WriteHudMargins() {
    const s32 w = *reinterpret_cast<volatile s32*>(g_switch_base + kScreenWidth);
    const s32 h = *reinterpret_cast<volatile s32*>(g_switch_base + kScreenHeight);
    if (w <= 0 || h <= 0) {
        return; // (before the game's VideoOut init)
    }
    const float ratio = static_cast<float>(g_safe_area.load(std::memory_order_relaxed)) / 100.0f;
    s32 area = w;
    if (const u32 a = g_hud_aspect.load(std::memory_order_relaxed); a != 0) {
        area = std::min<s32>(w, static_cast<s32>(std::lround(h * (a / 1000.0))) & ~1);
    }
    const s32 mx = (w - area) / 2 + SafeMargin(area, ratio), my = SafeMargin(h, ratio);
    auto* x = reinterpret_cast<volatile s32*>(g_switch_base + kMarginX);
    auto* y = reinterpret_cast<volatile s32*>(g_switch_base + kMarginY);
    if (*x != mx || *y != my) {
        *x = mx;
        *y = my;
    }
}

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
    WriteHudMargins();
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
    SSPC_LOG(Engine, Info, "Render resolution switch: entry probe on FrameBegin {:#x} (up to {}x{})",
             kFrameBeginSite.function.rva, g_started.width, g_started.height);
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

bool RenderSwitchInstalled() {
    return g_frame_begin != nullptr;
}

void SetHudLayout(u32 safe_area_percent, std::string_view area) {
    g_safe_area.store(std::clamp<u32>(safe_area_percent, 85, 100), std::memory_order_relaxed);
    g_hud_aspect.store(area == "16:9" ? 1778 : area == "21:9" ? 2370 : 0, std::memory_order_relaxed);
}

float HudSafeAreaRatio() {
    return static_cast<float>(g_safe_area.load(std::memory_order_relaxed)) / 100.0f;
}

namespace {

// Camera::SetFov(camera, vertical FOV in radians) 0x580580: stores the angle (+0x590), tan(angle / 2) (+0x5a4) and
// aspect * tan (+0x5a0) and rebuilds the projection. The player camera's update 0x14ddd0 calls it every frame with its
// controller's angle (+0x468 + +0x358: the base FOV and its aim / sprint offset) - the call returning to 0x14f517.
#if defined(SSPC_PRODUCT_FIRST_LIGHT)
// First Light (research/fl_sites.md §3): the same 15 callers; its SetFov no longer stores +0x5d4 / +0x5dc (unused here)
constexpr ProbeSite kSetFovSite{{0x65af10, 0xaf, 0x30d2052b9c584467ull}, 17};
constexpr u64 kPlayerSetFovReturn = 0x172fae;
#else
constexpr ProbeSite kSetFovSite{{0x580580, 0xc1, 0xef658398f5aad2a4ull}, 17};
constexpr u64 kPlayerSetFovReturn = 0x14f517;
#endif
constexpr float kMaxFov = 2.6f; // rad (149 degrees)

using SetFovFn = SSPC_SYSV_ABI void (*)(u64 camera, float vfov);
SetFovFn g_set_fov = nullptr;
u64 g_fov_base = 0;
std::atomic<u32> g_fov_percent{100};

SSPC_SYSV_ABI void SetFovProbe(u64 camera, float vfov) {
    const u32 p = g_fov_percent.load(std::memory_order_relaxed);
    if (p != 100 && reinterpret_cast<u64>(__builtin_return_address(0)) == g_fov_base + kPlayerSetFovReturn) {
        vfov = std::min(vfov * static_cast<float>(p) / 100.0f, kMaxFov);
    }
    g_set_fov(camera, vfov);
}

} // namespace

bool InstallFovControl(u64 eboot_base) {
    std::string error;
    void* original = InstallEntryProbe(eboot_base, kSetFovSite, reinterpret_cast<const void*>(&SetFovProbe), error);
    if (original == nullptr) {
        SSPC_LOG(Engine, Error, "Field of view control not installed (the game's own FOV): {}", error);
        return false;
    }
    g_fov_base = eboot_base;
    g_set_fov = reinterpret_cast<SetFovFn>(original);
    SSPC_LOG(Engine, Info, "Field of view: entry probe on Camera::SetFov {:#x} (player camera {} %)",
             kSetFovSite.function.rva, g_fov_percent.load());
    return true;
}

void SetFovPercent(u32 percent) {
    g_fov_percent.store(std::clamp<u32>(percent, 80, 130), std::memory_order_relaxed);
}

} // namespace SecondSon::Engine::Render
