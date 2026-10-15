// SecondSonPC — AMD FSR 3.1 frame generation (see fsr_framegen.h).
#include "runtime/gpu/upscale/fsr_framegen.h"

#include <algorithm>
#include <filesystem>
#include <limits>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "runtime/gpu/upscale/upscalers.h"

#if defined(SSPC_HAVE_FFX_API)
#include <ffx_api/ffx_framegeneration.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
#endif

namespace SecondSon::Gpu::Upscale {

#if defined(SSPC_HAVE_FFX_API)

namespace {

PFN_vkGetDeviceProcAddr g_fg_gdpa = nullptr;

// as the upscaler's: the FFX Vulkan backend asks for some functions by their extension names
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL FgDeviceProcAddr(VkDevice device, const char* name) {
    if (PFN_vkVoidFunction f = g_fg_gdpa(device, name)) {
        return f;
    }
    const std::string_view v(name);
    for (const std::string_view suffix : {"KHR", "EXT"}) {
        if (v.ends_with(suffix)) {
            const std::string core(v.substr(0, v.size() - suffix.size()));
            if (PFN_vkVoidFunction f = g_fg_gdpa(device, core.c_str())) {
                return f;
            }
        }
    }
    return nullptr;
}

FfxApiResource Resource(VkImage image, VkFormat format, u32 w, u32 h, u32 state) {
    FfxApiResource r{};
    r.resource = reinterpret_cast<void*>(image);
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D;
    r.description.format = ffxApiGetSurfaceFormatVK(format);
    r.description.width = w;
    r.description.height = h;
    r.description.depth = 1;
    r.description.mipCount = 1;
    r.description.flags = FFX_API_RESOURCE_FLAGS_NONE;
    r.description.usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    r.state = state;
    return r;
}

ffxReturnCode_t Generate(ffxDispatchDescFrameGeneration* params, void* user);

} // namespace

struct FsrFrameGen::Impl {
    HMODULE module = nullptr;
    PfnFfxCreateContext create = nullptr;
    PfnFfxDestroyContext destroy = nullptr;
    PfnFfxConfigure configure = nullptr;
    PfnFfxQuery query = nullptr;
    PfnFfxDispatch dispatch = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueueInfoFFXAPI game{}, present{}, acquire{}, compute{};
    ffxContext swapchain_context = nullptr;
    ffxQueryDescSwapchainReplacementFunctionsVK fn{};
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    ffxContext context = nullptr;
    VkExtent2D context_display{};
    u32 context_max_w = 0, context_max_h = 0;
    VkFormat context_format = VK_FORMAT_UNDEFINED;
    bool context_inverted = false;
    u64 frame_id = 0;
    bool configured_on = false;
    u64 last_present_count = 0;

    void DestroyContext() {
        if (context != nullptr) {
            // the swapchain may still present frames it generated
            Vk::Fn().vkDeviceWaitIdle(device);
            destroy(&context, nullptr);
            context = nullptr;
            configured_on = false;
        }
    }

    bool EnsureContext(VkExtent2D display, u32 render_w, u32 render_h, bool inverted) {
        if (context != nullptr && context_display.width == display.width && context_display.height == display.height &&
            context_format == format && context_inverted == inverted && render_w <= context_max_w &&
            render_h <= context_max_h) {
            return true;
        }
        DestroyContext();
        ffxCreateBackendVKDesc backend{};
        backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
        backend.vkDevice = device;
        backend.vkPhysicalDevice = physical;
        backend.vkDeviceProcAddr = &FgDeviceProcAddr;
        ffxCreateContextDescFrameGenerationHudless hudless{};
        hudless.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_HUDLESS;
        hudless.hudlessBackBufferFormat = ffxApiGetSurfaceFormatVK(format);
        backend.header.pNext = &hudless.header;
        ffxCreateContextDescFrameGeneration create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
        create.header.pNext = &backend.header;
        create.flags = inverted ? FFX_FRAMEGENERATION_ENABLE_DEPTH_INVERTED | FFX_FRAMEGENERATION_ENABLE_DEPTH_INFINITE
                                : 0u;
        create.displaySize = {display.width, display.height};
        context_max_w = std::max(render_w, display.width);
        context_max_h = std::max(render_h, display.height);
        create.maxRenderSize = {context_max_w, context_max_h};
        create.backBufferFormat = ffxApiGetSurfaceFormatVK(format);
        const ffxReturnCode_t r = this->create(&context, &create.header, nullptr);
        if (r != FFX_API_RETURN_OK) {
            context = nullptr;
            SSPC_LOG(GPU, Error, "[FSR-FG] context creation failed ({})", r);
            return false;
        }
        context_display = display;
        context_format = format;
        context_inverted = inverted;
        SSPC_LOG(GPU, Info, "[FSR-FG] context {}x{} (render up to {}x{})", display.width, display.height,
                 context_max_w, context_max_h);
        return true;
    }
};

namespace {
ffxReturnCode_t Generate(ffxDispatchDescFrameGeneration* params, void* user) {
    auto* impl = static_cast<FsrFrameGen::Impl*>(user);
    return impl->dispatch(&impl->context, &params->header);
}
} // namespace

std::unique_ptr<FsrFrameGen> FsrFrameGen::Create(VkPhysicalDevice physical, VkDevice device, const Queues& q,
                                                 PFN_vkGetDeviceProcAddr gdpa, std::string& why) {
    if (q.present.queue == VK_NULL_HANDLE || q.acquire.queue == VK_NULL_HANDLE) {
        why = "the GPU has no second queue that can present (FFX frame generation presents on its own queue)";
        return nullptr;
    }
    const std::filesystem::path dll = std::filesystem::path(ExecutableDirectory()) / L"amd_fidelityfx_vk.dll";
    auto impl = std::make_unique<Impl>();
    impl->module = LoadLibraryW(dll.c_str());
    if (impl->module == nullptr) {
        why = "amd_fidelityfx_vk.dll is not next to SecondSonPC.exe";
        return nullptr;
    }
    const auto get = [&](const char* n) { return reinterpret_cast<void*>(GetProcAddress(impl->module, n)); };
    impl->create = reinterpret_cast<PfnFfxCreateContext>(get("ffxCreateContext"));
    impl->destroy = reinterpret_cast<PfnFfxDestroyContext>(get("ffxDestroyContext"));
    impl->configure = reinterpret_cast<PfnFfxConfigure>(get("ffxConfigure"));
    impl->query = reinterpret_cast<PfnFfxQuery>(get("ffxQuery"));
    impl->dispatch = reinterpret_cast<PfnFfxDispatch>(get("ffxDispatch"));
    if (!impl->create || !impl->destroy || !impl->configure || !impl->query || !impl->dispatch) {
        FreeLibrary(impl->module);
        why = "amd_fidelityfx_vk.dll lacks the FidelityFX API entry points";
        return nullptr;
    }
    g_fg_gdpa = gdpa;
    impl->physical = physical;
    impl->device = device;
    const auto info = [](const Queue& x) {
        return VkQueueInfoFFXAPI{x.queue, x.family == ~0u ? 0u : x.family, nullptr};
    };
    impl->game = info(q.game);
    impl->present = info(q.present);
    impl->acquire = info(q.acquire);
    impl->compute = info(q.compute);
    std::unique_ptr<FsrFrameGen> fg(new FsrFrameGen());
    fg->impl_ = std::move(impl);
    SSPC_LOG(GPU, Info, "[FSR-FG] ready: present queue family {}, acquire family {}, compute family {}",
             q.present.family, q.acquire.family, static_cast<s32>(q.compute.family));
    return fg;
}

FsrFrameGen::~FsrFrameGen() {
    if (!impl_) {
        return;
    }
    impl_->DestroyContext();
    if (impl_->swapchain_context != nullptr) {
        impl_->destroy(&impl_->swapchain_context, nullptr);
    }
    FreeLibrary(impl_->module);
}

VkResult FsrFrameGen::CreateSwapchain(const VkSwapchainCreateInfoKHR& info, VkSwapchainKHR& out) {
    Impl& s = *impl_;
    VkSwapchainKHR handle = VK_NULL_HANDLE;
    if (s.swapchain_context == nullptr) {
        ffxCreateContextDescFrameGenerationSwapChainVK create{};
        create.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        create.physicalDevice = s.physical;
        create.device = s.device;
        create.swapchain = &handle;
        create.allocator = nullptr;
        create.createInfo = info;
        create.gameQueue = s.game;
        create.asyncComputeQueue = s.compute;
        create.presentQueue = s.present;
        create.imageAcquireQueue = s.acquire;
        const ffxReturnCode_t r = s.create(&s.swapchain_context, &create.header, nullptr);
        if (r != FFX_API_RETURN_OK) {
            s.swapchain_context = nullptr;
            SSPC_LOG(GPU, Error, "[FSR-FG] swapchain creation failed ({})", r);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        s.fn = {};
        s.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
        s.query(&s.swapchain_context, &s.fn.header);
    } else {
        const VkResult r = s.fn.pOutCreateSwapchainFFXAPI(s.device, &info, nullptr, &handle, s.swapchain_context);
        if (r != VK_SUCCESS) {
            return r;
        }
    }
    out = handle;
    s.swapchain = handle;
    s.format = info.imageFormat;
    s.last_present_count = s.fn.pOutGetLastPresentCountFFXAPI ? s.fn.pOutGetLastPresentCountFFXAPI(handle) : 0;
    SSPC_LOG(GPU, Info, "[FSR-FG] swapchain {}x{} format {}", info.imageExtent.width, info.imageExtent.height,
             static_cast<int>(info.imageFormat));
    return VK_SUCCESS;
}

void FsrFrameGen::DestroySwapchain(VkSwapchainKHR swapchain) {
    Impl& s = *impl_;
    s.DestroyContext();
    if (s.fn.pOutDestroySwapchainFFXAPI != nullptr) {
        s.fn.pOutDestroySwapchainFFXAPI(s.device, swapchain, nullptr, s.swapchain_context);
    }
    if (swapchain == s.swapchain) {
        s.swapchain = VK_NULL_HANDLE;
    }
}

VkResult FsrFrameGen::GetImages(VkSwapchainKHR swapchain, std::vector<VkImage>& images) {
    u32 n = 0;
    VkResult r = impl_->fn.pOutGetSwapchainImagesKHR(impl_->device, swapchain, &n, nullptr);
    if (r != VK_SUCCESS) {
        return r;
    }
    images.resize(n);
    r = impl_->fn.pOutGetSwapchainImagesKHR(impl_->device, swapchain, &n, images.data());
    images.resize(n);
    return r;
}

VkResult FsrFrameGen::Acquire(VkSwapchainKHR swapchain, u64 timeout, VkSemaphore semaphore, VkFence fence, u32* index) {
    return impl_->fn.pOutAcquireNextImageKHR(impl_->device, swapchain, timeout, semaphore, fence, index);
}

VkResult FsrFrameGen::Present(VkQueue queue, const VkPresentInfoKHR& info) {
    return impl_->fn.pOutQueuePresentKHR(queue, &info);
}

void FsrFrameGen::SetFrame(VkCommandBuffer cb, const Inputs* in, VkExtent2D backbuffer, float frame_ms) {
    Impl& s = *impl_;
    if (s.swapchain == VK_NULL_HANDLE) {
        return;
    }
    if (in != nullptr && !s.EnsureContext(backbuffer, in->render_w, in->render_h, in->depth_inverted)) {
        in = nullptr;
    }
    if (s.context == nullptr) {
        generating_ = false;
        return;
    }
    ++s.frame_id;
    ffxConfigureDescFrameGeneration config{};
    config.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    config.swapChain = s.swapchain;
    config.frameGenerationCallback = &Generate;
    config.frameGenerationCallbackUserContext = &s;
    config.frameGenerationEnabled = in != nullptr && !in->reset;
    config.allowAsyncWorkloads = false;
    if (in != nullptr && in->hudless != VK_NULL_HANDLE) {
        config.HUDLessColor = Resource(in->hudless, in->hudless_format, backbuffer.width, backbuffer.height,
                                       FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    }
    config.generationRect = {0, 0, static_cast<int>(backbuffer.width), static_cast<int>(backbuffer.height)};
    config.frameID = s.frame_id;
    if (config.frameGenerationEnabled || s.configured_on) {
        if (const ffxReturnCode_t r = s.configure(&s.context, &config.header); r != FFX_API_RETURN_OK) {
            SSPC_LOG(GPU, Warning, "[FSR-FG] configure failed ({})", r);
        }
        if (config.frameGenerationEnabled != s.configured_on) {
            SSPC_LOG(GPU, Info, "[FSR-FG] generation {}", config.frameGenerationEnabled ? "on" : "off");
        }
        s.configured_on = config.frameGenerationEnabled;
    }
    generating_ = config.frameGenerationEnabled;
    if (!config.frameGenerationEnabled) {
        return;
    }
    ffxDispatchDescFrameGenerationPrepare prepare{};
    prepare.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE;
    prepare.frameID = s.frame_id;
    prepare.commandList = cb;
    prepare.renderSize = {in->render_w, in->render_h};
    prepare.jitterOffset = {in->jitter_x, in->jitter_y};
    prepare.motionVectorScale = {in->mv_scale_x, in->mv_scale_y};
    prepare.frameTimeDelta = frame_ms;
    prepare.cameraNear = in->camera_near;
    prepare.cameraFar = in->camera_far;
    prepare.cameraFovAngleVertical = in->fov_y;
    prepare.viewSpaceToMetersFactor = 1.0f;
    prepare.depth = Resource(in->depth, VK_FORMAT_R32_SFLOAT, in->depth_w, in->depth_h,
                             FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prepare.motionVectors = Resource(in->motion, VK_FORMAT_R16G16_SFLOAT, in->motion_w, in->motion_h,
                                     FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    if (const ffxReturnCode_t r = s.dispatch(&s.context, &prepare.header); r != FFX_API_RETURN_OK) {
        SSPC_LOG(GPU, Warning, "[FSR-FG] prepare failed ({})", r);
    }
}

u32 FsrFrameGen::TakeShown() {
    Impl& s = *impl_;
    if (s.swapchain == VK_NULL_HANDLE || s.fn.pOutGetLastPresentCountFFXAPI == nullptr) {
        return 1;
    }
    const u64 count = s.fn.pOutGetLastPresentCountFFXAPI(s.swapchain);
    const u64 shown = count - s.last_present_count;
    s.last_present_count = count;
    return static_cast<u32>(std::min<u64>(shown, 8));
}

#else

struct FsrFrameGen::Impl {};
std::unique_ptr<FsrFrameGen> FsrFrameGen::Create(VkPhysicalDevice, VkDevice, const Queues&, PFN_vkGetDeviceProcAddr,
                                                 std::string& why) {
    why = "built without the FidelityFX API headers";
    return nullptr;
}
FsrFrameGen::~FsrFrameGen() = default;
VkResult FsrFrameGen::CreateSwapchain(const VkSwapchainCreateInfoKHR&, VkSwapchainKHR&) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
void FsrFrameGen::DestroySwapchain(VkSwapchainKHR) {}
VkResult FsrFrameGen::GetImages(VkSwapchainKHR, std::vector<VkImage>&) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VkResult FsrFrameGen::Acquire(VkSwapchainKHR, u64, VkSemaphore, VkFence, u32*) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
VkResult FsrFrameGen::Present(VkQueue, const VkPresentInfoKHR&) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}
void FsrFrameGen::SetFrame(VkCommandBuffer, const Inputs*, VkExtent2D, float) {}
u32 FsrFrameGen::TakeShown() {
    return 1;
}

#endif

} // namespace SecondSon::Gpu::Upscale
