// SecondSonPC — temporal upscalers of the native backend (see upscalers.h).
//
// The FSR part follows the integration of shadPS4-Bloodborne-DLSS-FSR (IFreemz, vk_fsr_upscaler.cpp, GPL-2.0-or-later):
// the same FidelityFX API calls, the device-function fallbacks the FFX Vulkan backend needs, resource descriptions.
#include "runtime/gpu/upscale/upscalers.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "dlss_bridge/sspc_dlss_bridge.h"

#if defined(SSPC_HAVE_FFX_API)
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

namespace SecondSon::Gpu::Upscale {

float ScaleOf(Quality q) {
    switch (q) {
    case Quality::Native: return 1.0f;
    case Quality::Quality: return 1.5f;
    case Quality::Balanced: return 1.7f;
    case Quality::Performance: return 2.0f;
    case Quality::UltraPerformance: return 3.0f;
    }
    return 1.0f;
}

const char* BackendName(Backend b) {
    return b == Backend::Dlss ? "DLSS" : b == Backend::Fsr ? "FSR 3.1" : "off";
}

const char* QualityName(Quality q) {
    switch (q) {
    case Quality::Native: return "Native AA";
    case Quality::Quality: return "Quality";
    case Quality::Balanced: return "Balanced";
    case Quality::Performance: return "Performance";
    case Quality::UltraPerformance: return "Ultra Performance";
    }
    return "?";
}

std::wstring ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return std::filesystem::path(path).parent_path().wstring();
}

namespace {

std::mutex g_settings_mutex;
Settings g_settings;
std::atomic<u32> g_render_w{1920}, g_render_h{1080};
std::mutex g_status_mutex;
Status g_status;

} // namespace

void SetSettings(const Settings& s) {
    std::scoped_lock lock{g_settings_mutex};
    g_settings = s;
}

Settings GetSettings() {
    std::scoped_lock lock{g_settings_mutex};
    return g_settings;
}

bool ParseBackend(std::string_view s, Backend& out) {
    if (s == "off" || s == "none" || s == "0") {
        out = Backend::Off;
    } else if (s == "dlss") {
        out = Backend::Dlss;
    } else if (s == "fsr" || s == "fsr3") {
        out = Backend::Fsr;
    } else {
        return false;
    }
    return true;
}

bool ParseQuality(std::string_view s, Quality& out) {
    if (s == "native" || s == "dlaa" || s == "nativeaa") {
        out = Quality::Native;
    } else if (s == "quality") {
        out = Quality::Quality;
    } else if (s == "balanced") {
        out = Quality::Balanced;
    } else if (s == "performance") {
        out = Quality::Performance;
    } else if (s == "ultra" || s == "ultraperformance") {
        out = Quality::UltraPerformance;
    } else {
        return false;
    }
    return true;
}

bool ReserveCudaWindow() {
    static int done = -1;
    if (done >= 0) {
        return done == 1;
    }
    constexpr u64 kWindow = 0xF000000000ull, kTop = 0x10000000000ull;
    std::vector<void*> hold;
    MEMORY_BASIC_INFORMATION mi{};
    for (u64 a = 0x10000; a < kWindow && VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) != 0;) {
        const u64 base = reinterpret_cast<u64>(mi.BaseAddress), end = base + mi.RegionSize;
        if (mi.State == MEM_FREE) {
            const u64 b = (base + 0xFFFF) & ~0xFFFFull, e = std::min(end, kWindow);
            if (e > b) {
                if (void* p = VirtualAlloc(reinterpret_cast<void*>(b), e - b, MEM_RESERVE, PAGE_NOACCESS)) {
                    hold.push_back(p);
                }
            }
        }
        a = end;
    }
    int result = -1;
    HMODULE cuda = LoadLibraryW(L"nvcuda.dll");
    if (cuda != nullptr) {
        using CuInit = int (*)(unsigned);
        if (const auto init = reinterpret_cast<CuInit>(reinterpret_cast<void*>(GetProcAddress(cuda, "cuInit")))) {
            result = init(0);
        }
    }
    for (void* p : hold) {
        VirtualFree(p, 0, MEM_RELEASE);
    }
    u64 taken = 0, first = 0;
    for (u64 a = kWindow; a < kTop && VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof(mi)) != 0;) {
        if (mi.State != MEM_FREE) {
            taken += mi.RegionSize;
            first = first != 0 ? first : reinterpret_cast<u64>(mi.BaseAddress);
        }
        a = reinterpret_cast<u64>(mi.BaseAddress) + mi.RegionSize;
    }
    done = result == 0 ? 1 : 0;
    if (done == 1) {
        SSPC_LOG(GPU, Info, "DLSS: the driver's CUDA part initialized before the guest's address space: it holds {} MiB "
                            "from {:#x} (under 1 TiB, above the game's mappings)",
                 taken >> 20, first);
    } else {
        SSPC_LOG(GPU, Warning, "DLSS: CUDA could not be initialized ({}): DLSS will be unavailable",
                 cuda == nullptr ? std::string("no nvcuda.dll (not an NVIDIA driver)") : fmt::format("cuInit {}", result));
    }
    return done == 1;
}

void ApplyEnvironmentOverrides() {
    static bool applied = false;
    if (applied) {
        return;
    }
    applied = true;
    const auto env = [](const char* n) {
        char v[64] = {};
        const DWORD k = GetEnvironmentVariableA(n, v, sizeof(v));
        return k > 0 && k < sizeof(v) ? std::string(v, k) : std::string();
    };
    Settings s = GetSettings();
    bool any = false;
    if (const std::string v = env("SSPC_UPSCALER"); !v.empty()) {
        any |= ParseBackend(v, s.backend);
    }
    if (const std::string v = env("SSPC_UPSCALE_QUALITY"); !v.empty()) {
        any |= ParseQuality(v, s.quality);
    }
    if (const std::string v = env("SSPC_UPSCALE_SHARPNESS"); !v.empty()) {
        s.sharpness = std::clamp(static_cast<float>(std::atof(v.c_str())), 0.0f, 1.0f);
        any = true;
    }
    if (env("SSPC_UPSCALE_JITTER") == "0") {
        s.jitter = false;
        any = true;
    }
    if (any) {
        SetSettings(s);
        SSPC_LOG(GPU, Warning, "Upscaling from the environment: {} {}, sharpness {:.2f}, jitter {}", BackendName(s.backend),
                 QualityName(s.quality), s.sharpness, s.jitter ? "on" : "OFF");
    }
}

void SetRenderSize(u32 width, u32 height) {
    g_render_w.store(width, std::memory_order_relaxed);
    g_render_h.store(height, std::memory_order_relaxed);
}

void GetRenderSize(u32& width, u32& height) {
    width = g_render_w.load(std::memory_order_relaxed);
    height = g_render_h.load(std::memory_order_relaxed);
}

Status GetStatus() {
    std::scoped_lock lock{g_status_mutex};
    return g_status;
}

void UpdateStatus(const std::function<void(Status&)>& fn) {
    std::scoped_lock lock{g_status_mutex};
    fn(g_status);
}

namespace {

float Halton(u32 index, u32 base) {
    float f = 1.0f, r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

} // namespace

void Jitter(u32 index, u32 render_width, u32 display_width, float& x, float& y) {
    const float ratio = render_width > 0 ? static_cast<float>(display_width) / static_cast<float>(render_width) : 1.0f;
    const u32 phases = std::max<u32>(8, static_cast<u32>(std::ceil(8.0f * ratio * ratio)));
    const u32 i = index % phases + 1; // Halton from 1 (0 is the origin)
    x = Halton(i, 2) - 0.5f;
    y = Halton(i, 3) - 0.5f;
}

// --------------------------------------------------------------------------------------------------- DLSS (NGX bridge)

namespace {

struct Bridge {
    std::once_flag once;
    HMODULE module = nullptr;
    const SspcDlssApi* api = nullptr;
    std::string why; ///< why the bridge is not usable ("" when it is)
    bool instance_ready = false, device_ready = false;
} g_bridge;

void BridgeLog(int warning, const char* message) {
    if (warning) {
        SSPC_LOG(GPU, Warning, "[DLSS] {}", message);
    } else {
        SSPC_LOG(GPU, Info, "[DLSS] {}", message);
    }
}

void LoadBridge() {
    std::call_once(g_bridge.once, [] {
        const std::filesystem::path dir = ExecutableDirectory();
        if (char v[4] = {}; GetEnvironmentVariableA("SSPC_DLSS", v, sizeof(v)) == 1 && v[0] == '0') {
            g_bridge.why = "SSPC_DLSS=0";
            return;
        }
        if (!std::filesystem::is_regular_file(dir / L"sspc_dlss.dll") ||
            !std::filesystem::is_regular_file(dir / L"nvngx_dlss.dll")) {
            g_bridge.why = "sspc_dlss.dll / nvngx_dlss.dll are not next to SecondSonPC.exe";
            return;
        }
        g_bridge.module = LoadLibraryW((dir / L"sspc_dlss.dll").c_str());
        const auto get = g_bridge.module ? reinterpret_cast<SspcDlssGetApiFn>(reinterpret_cast<void*>(
                                               GetProcAddress(g_bridge.module, "SspcDlssGetApi")))
                                         : nullptr;
        g_bridge.api = get ? get() : nullptr;
        if (g_bridge.api == nullptr || g_bridge.api->abi != SSPC_DLSS_BRIDGE_ABI) {
            g_bridge.api = nullptr;
            g_bridge.why = "sspc_dlss.dll is missing or from another build";
            return;
        }
        // NGX writes its logs and model cache to a data folder
        const std::filesystem::path data = dir / L"ngx";
        std::error_code ec;
        std::filesystem::create_directories(data, ec);
        if (!g_bridge.api->Configure(dir.c_str(), data.c_str(), BridgeLog)) {
            g_bridge.api = nullptr;
            g_bridge.why = "the DLSS bridge could not be configured";
        }
    });
}

SspcDlssImage Wrap(const Image& i) {
    return SspcDlssImage{i.image, i.view,
                         VkImageSubresourceRange{.aspectMask = i.aspect, .levelCount = 1, .layerCount = 1}, i.format,
                         i.width, i.height};
}

} // namespace

void Dlss::AppendInstanceExtensions(std::vector<std::string>& out, std::string& why) {
    LoadBridge();
    if (g_bridge.api == nullptr) {
        why = g_bridge.why;
        return;
    }
    u32 n = 0;
    const VkExtensionProperties* req = nullptr;
    if (!g_bridge.api->InstanceExtensions(&n, &req)) {
        g_bridge.why = why = "NGX did not report its instance extensions";
        g_bridge.api = nullptr;
        return;
    }
    for (u32 i = 0; i < n; ++i) {
        out.emplace_back(req[i].extensionName);
    }
    g_bridge.instance_ready = true;
}

void Dlss::AppendDeviceExtensions(VkInstance instance, VkPhysicalDevice physical, u32 vendor_id,
                                  std::vector<std::string>& out, std::string& why) {
    if (g_bridge.api == nullptr || !g_bridge.instance_ready) {
        why = g_bridge.why.empty() ? "the DLSS bridge is not loaded" : g_bridge.why;
        return;
    }
    if (vendor_id != 0x10de) {
        g_bridge.why = why = "DLSS needs an NVIDIA GeForce RTX GPU";
        return;
    }
    u32 n = 0;
    const VkExtensionProperties* req = nullptr;
    if (!g_bridge.api->DeviceExtensions(instance, physical, &n, &req)) {
        g_bridge.why = why = "this GPU or driver does not support DLSS (GeForce RTX needed)";
        return;
    }
    for (u32 i = 0; i < n; ++i) {
        out.emplace_back(req[i].extensionName);
    }
    g_bridge.device_ready = true;
}

std::unique_ptr<Dlss> Dlss::Create(const DeviceInfo& d, std::string& why) {
    if (g_bridge.api == nullptr || !g_bridge.device_ready) {
        why = g_bridge.why.empty() ? "DLSS was not prepared for this device" : g_bridge.why;
        return nullptr;
    }
    if (!g_bridge.api->Initialize(d.instance, d.physical, d.device, d.gipa, d.gdpa)) {
        g_bridge.why = why = "NVIDIA NGX did not initialize (update the GPU driver)";
        return nullptr;
    }
    SSPC_LOG(GPU, Info, "[DLSS] ready");
    return std::unique_ptr<Dlss>(new Dlss());
}

Dlss::~Dlss() {
    if (g_bridge.api != nullptr) {
        g_bridge.api->Shutdown();
    }
}

bool Dlss::NeedsRecreate(u32 in_w, u32 in_h, u32 out_w, u32 out_h, Quality q, bool hdr, bool inverted) const {
    const Feature& f = feature_;
    return !has_feature_ || f.in_w != in_w || f.in_h != in_h || f.out_w != out_w || f.out_h != out_h || f.q != q ||
           f.hdr != hdr || f.inverted != inverted;
}

bool Dlss::CreateFeature(VkCommandBuffer cb, u32 in_w, u32 in_h, u32 out_w, u32 out_h, Quality q, bool hdr,
                         bool inverted, std::string& why) {
    has_feature_ = false;
    // quality: 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance (the bridge's order); preset 0 = the
    // driver's default model for the mode
    const SspcDlssFeature desc{in_w, in_h, out_w, out_h, static_cast<int32_t>(q), inverted ? 1 : 0, 0u, hdr ? 1 : 0};
    if (!g_bridge.api->CreateFeature(cb, &desc)) {
        why = "the DLSS feature could not be created";
        return false;
    }
    feature_ = {in_w, in_h, out_w, out_h, q, hdr, inverted};
    has_feature_ = true;
    return true;
}

bool Dlss::Evaluate(VkCommandBuffer cb, const Inputs& in) {
    if (!has_feature_) {
        return false;
    }
    const SspcDlssEvaluate e{Wrap(in.color), Wrap(in.depth), Wrap(in.motion), Wrap(in.output),
                             in.jitter_x,    in.jitter_y,    in.reset ? 1 : 0, in.frame_ms,
                             in.mv_scale_x,  in.mv_scale_y,  1.0f};
    return g_bridge.api->Evaluate(cb, &e) != 0;
}

void Dlss::ReleaseFeature() {
    if (has_feature_) {
        g_bridge.api->ReleaseFeature();
    }
    has_feature_ = false;
}

// ------------------------------------------------------------------------------------------------------------- FSR 3.1

#if defined(SSPC_HAVE_FFX_API)

namespace {

PFN_vkGetDeviceProcAddr g_gdpa = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL NoObjectName(VkDevice, const VkDebugUtilsObjectNameInfoEXT*) {
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL NoLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
VKAPI_ATTR void VKAPI_CALL NoLabelEnd(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL NoMarker(VkCommandBuffer, VkPipelineStageFlagBits, VkBuffer, VkDeviceSize, uint32_t) {}
VKAPI_ATTR void VKAPI_CALL NoMarker2(VkCommandBuffer, VkPipelineStageFlags2, VkBuffer, VkDeviceSize, uint32_t) {}

// The FFX Vulkan backend looks some functions up by their extension names and calls a few without checking: the core
// name stands in for the KHR/EXT one, and debug labels / buffer markers get no-op stand-ins when not enabled.
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL DeviceProcAddr(VkDevice device, const char* name) {
    if (PFN_vkVoidFunction f = g_gdpa(device, name)) {
        return f;
    }
    const std::string_view v(name);
    for (const std::string_view suffix : {"KHR", "EXT"}) {
        if (v.ends_with(suffix)) {
            const std::string core(v.substr(0, v.size() - suffix.size()));
            if (PFN_vkVoidFunction f = g_gdpa(device, core.c_str())) {
                return f;
            }
        }
    }
    if (v == "vkSetDebugUtilsObjectNameEXT") {
        return reinterpret_cast<PFN_vkVoidFunction>(&NoObjectName);
    }
    if (v == "vkCmdBeginDebugUtilsLabelEXT" || v == "vkCmdInsertDebugUtilsLabelEXT") {
        return reinterpret_cast<PFN_vkVoidFunction>(&NoLabel);
    }
    if (v == "vkCmdEndDebugUtilsLabelEXT") {
        return reinterpret_cast<PFN_vkVoidFunction>(&NoLabelEnd);
    }
    if (v == "vkCmdWriteBufferMarkerAMD") {
        return reinterpret_cast<PFN_vkVoidFunction>(&NoMarker);
    }
    if (v == "vkCmdWriteBufferMarker2AMD") {
        return reinterpret_cast<PFN_vkVoidFunction>(&NoMarker2);
    }
    SSPC_LOG(GPU, Warning, "[FSR] Vulkan function {} is not available", name);
    return nullptr;
}

void FfxMessage(uint32_t type, const wchar_t* message) {
    const std::string text = std::filesystem::path(message ? message : L"").string();
    if (type == FFX_API_MESSAGE_TYPE_ERROR) {
        SSPC_LOG(GPU, Error, "[FSR] {}", text);
    } else {
        SSPC_LOG(GPU, Warning, "[FSR] {}", text);
    }
}

FfxApiResource Resource(const Image& i, bool writable) {
    FfxApiResource r{};
    r.resource = reinterpret_cast<void*>(i.image);
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK(i.format);
    if (i.format == VK_FORMAT_D32_SFLOAT || i.format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        r.description.format = FFX_API_SURFACE_FORMAT_R32_FLOAT;
    }
    r.description.width = i.width;
    r.description.height = i.height;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = writable ? FFX_API_RESOURCE_USAGE_UAV
                                   : (i.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? FFX_API_RESOURCE_USAGE_DEPTHTARGET
                                                                            : FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = writable ? FFX_API_RESOURCE_STATE_UNORDERED_ACCESS : FFX_API_RESOURCE_STATE_COMPUTE_READ;
    return r;
}

} // namespace

struct Fsr::Impl {
    HMODULE module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    ffxContext context = nullptr;
    u32 max_w = 0, max_h = 0, out_w = 0, out_h = 0;
    bool hdr = false, inverted = false;
};

std::unique_ptr<Fsr> Fsr::Create(const DeviceInfo& d, std::string& why) {
    if (char v[4] = {}; GetEnvironmentVariableA("SSPC_FSR", v, sizeof(v)) == 1 && v[0] == '0') {
        why = "SSPC_FSR=0";
        return nullptr;
    }
    const std::filesystem::path dll = std::filesystem::path(ExecutableDirectory()) / L"amd_fidelityfx_vk.dll";
    if (!std::filesystem::is_regular_file(dll)) {
        why = "amd_fidelityfx_vk.dll is not next to SecondSonPC.exe";
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->module = LoadLibraryW(dll.c_str());
    if (impl->module == nullptr) {
        why = fmt::format("amd_fidelityfx_vk.dll could not be loaded ({})", GetLastError());
        return nullptr;
    }
    const auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(impl->module, n)); };
    impl->create = reinterpret_cast<PfnFfxCreateContext>(get("ffxCreateContext"));
    impl->destroy = reinterpret_cast<PfnFfxDestroyContext>(get("ffxDestroyContext"));
    impl->query = reinterpret_cast<PfnFfxQuery>(get("ffxQuery"));
    impl->dispatch = reinterpret_cast<PfnFfxDispatch>(get("ffxDispatch"));
    if (!impl->create || !impl->destroy || !impl->query || !impl->dispatch) {
        FreeLibrary(impl->module);
        why = "amd_fidelityfx_vk.dll lacks the FidelityFX API entry points";
        return nullptr;
    }
    g_gdpa = d.gdpa;
    impl->device = d.device;
    impl->physical = d.physical;
    std::unique_ptr<Fsr> fsr(new Fsr());
    fsr->impl_ = std::move(impl);
    SSPC_LOG(GPU, Info, "[FSR] ready (amd_fidelityfx_vk.dll)");
    return fsr;
}

Fsr::~Fsr() {
    if (impl_) {
        DestroyContext();
        FreeLibrary(impl_->module);
    }
}

bool Fsr::NeedsRecreate(u32 max_in_w, u32 max_in_h, u32 out_w, u32 out_h, bool hdr, bool inverted) const {
    const Impl& s = *impl_;
    return s.context == nullptr || max_in_w > s.max_w || max_in_h > s.max_h || out_w != s.out_w || out_h != s.out_h ||
           hdr != s.hdr || inverted != s.inverted;
}

bool Fsr::CreateContext(u32 max_in_w, u32 max_in_h, u32 out_w, u32 out_h, bool hdr, bool inverted,
                        std::string& why) {
    Impl& s = *impl_;
    DestroyContext();
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = s.device;
    backend.vkPhysicalDevice = s.physical;
    backend.vkDeviceProcAddr = &DeviceProcAddr;
    ffxCreateContextDescUpscale create{};
    create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    create.header.pNext = &backend.header;
    create.flags = (hdr ? (FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE)
                        : FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE) |
                   (inverted ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0u);
    create.maxRenderSize = {max_in_w, max_in_h};
    create.maxUpscaleSize = {out_w, out_h};
    create.fpMessage = &FfxMessage;
    const ffxReturnCode_t r = s.create(&s.context, &create.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        s.context = nullptr;
        why = fmt::format("FSR context creation failed ({})", r);
        return false;
    }
    s.max_w = max_in_w;
    s.max_h = max_in_h;
    s.out_w = out_w;
    s.out_h = out_h;
    s.hdr = hdr;
    s.inverted = inverted;
    ffxQueryGetProviderVersion version{};
    version.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    s.query(&s.context, &version.header);
    SSPC_LOG(GPU, Info, "[FSR] context up to {}x{} -> {}x{} ({}{})", max_in_w, max_in_h, out_w, out_h,
             version.versionName ? version.versionName : "?", hdr ? ", HDR input, auto exposure" : "");
    return true;
}

bool Fsr::Evaluate(VkCommandBuffer cb, const Inputs& in) {
    Impl& s = *impl_;
    if (s.context == nullptr) {
        return false;
    }
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = Resource(in.color, false);
    d.depth = Resource(in.depth, false);
    d.motionVectors = Resource(in.motion, false);
    d.output = Resource(in.output, true);
    d.jitterOffset = {in.jitter_x, in.jitter_y};
    d.motionVectorScale = {in.mv_scale_x, in.mv_scale_y};
    d.renderSize = {in.render_width, in.render_height};
    d.upscaleSize = {in.output.width, in.output.height};
    d.enableSharpening = in.sharpness > 0.0f;
    d.sharpness = std::clamp(in.sharpness, 0.0f, 1.0f);
    d.frameTimeDelta = in.frame_ms;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.camera_near;
    d.cameraFar = in.camera_far;
    d.cameraFovAngleVertical = in.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = in.hdr ? 0u : FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;
    const ffxReturnCode_t r = s.dispatch(&s.context, &d.header);
    if (r != FFX_API_RETURN_OK) {
        SSPC_LOG(GPU, Error, "[FSR] dispatch failed ({})", r);
        return false;
    }
    return true;
}

void Fsr::DestroyContext() {
    Impl& s = *impl_;
    if (s.context != nullptr) {
        s.destroy(&s.context, nullptr);
    }
    s.context = nullptr;
    s.max_w = s.max_h = s.out_w = s.out_h = 0;
}

#else // no FidelityFX API headers at build time

struct Fsr::Impl {};
std::unique_ptr<Fsr> Fsr::Create(const DeviceInfo&, std::string& why) {
    why = "built without the FidelityFX API headers";
    return nullptr;
}
Fsr::~Fsr() = default;
bool Fsr::NeedsRecreate(u32, u32, u32, u32, bool, bool) const {
    return false;
}
bool Fsr::CreateContext(u32, u32, u32, u32, bool, bool, std::string&) {
    return false;
}
bool Fsr::Evaluate(VkCommandBuffer, const Inputs&) {
    return false;
}
void Fsr::DestroyContext() {}

#endif

} // namespace SecondSon::Gpu::Upscale
