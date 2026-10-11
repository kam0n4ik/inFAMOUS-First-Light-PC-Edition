// SecondSonPC — the backend's presenter (P4.4). See vk_presenter.h.
#include <windows.h>

#include "runtime/gpu/vulkan/vk_presenter.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <vulkan/vulkan_win32.h>

#include "base/log.h"
#include "runtime/gpu/png_writer.h"
#include "runtime/gpu/vulkan/vk_device_context.h"
#include "runtime/gpu/vulkan/vk_resources.h"

#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

/// The raw-bits twin of a format: a frame keeps the buffer's bits (sRGB-encoded values stay encoded).
VkFormat Unorm(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default:
        return f;
    }
}

float HalfToFloat(u16 h) {
    const u32 s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) {
        v = std::ldexp(static_cast<float>(m), -24);
    } else if (e == 31) {
        v = m ? 0.0f : 65504.0f;
    } else {
        v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
    }
    return s ? -v : v;
}

u8 ToByte(float v) {
    return static_cast<u8>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

} // namespace

struct Presenter::Impl {
    DeviceContext& ctx;
    Timeline& timeline;
    SDL_Window* window = nullptr;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat swap_format = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D extent{};
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    std::vector<VkImage> images;
    std::vector<VkSemaphore> done; ///< per swapchain image: its commands finished (the present waits on it)

    struct Slot {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        s32 frame = -1; ///< the frame its commands read
    };
    VkCommandPool pool = VK_NULL_HANDLE;
    std::array<Slot, 2> slots{};
    u32 slot = 0;

    struct Frame {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkFormat format = VK_FORMAT_UNDEFINED;
        u32 width = 0, height = 0;
        std::atomic<u32> state{0}; ///< 0 free, 1 the GPU thread's, 2 waiting to be shown, 3 being shown
        VkFence fence = VK_NULL_HANDLE; ///< the presentation slot's fence while being shown
    };
    std::array<Frame, kFrames> frames{};
    std::mutex frames_mutex;

    std::mutex capture_mutex;
    u64 capture_flip = 0;
    std::filesystem::path capture_path;

    std::mutex stats_mutex;
    Stats stats;

    Impl(DeviceContext& c, Timeline& t) : ctx(c), timeline(t) {}

    bool CreateSwapchain(std::string& error) {
        const auto& f = Fn();
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        VkSurfaceCapabilitiesKHR caps{};
        if (f.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.Physical(), surface, &caps) != VK_SUCCESS) {
            error = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
            return false;
        }
        VkExtent2D ext = caps.currentExtent;
        if (ext.width == 0xFFFFFFFFu) {
            ext = {static_cast<u32>(std::max(w, 1)), static_cast<u32>(std::max(h, 1))};
        }
        if (ext.width == 0 || ext.height == 0) {
            extent = ext;
            return true; // minimized: no swapchain until restored
        }
        u32 nf = 0;
        f.vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.Physical(), surface, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(nf);
        f.vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.Physical(), surface, &nf, formats.data());
        VkSurfaceFormatKHR fmt = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                      VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                 : formats[0];
        for (const auto& sf : formats) {
            if (sf.format == VK_FORMAT_B8G8R8A8_UNORM && sf.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                fmt = sf;
            }
        }
        u32 nm = 0;
        f.vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.Physical(), surface, &nm, nullptr);
        std::vector<VkPresentModeKHR> modes(nm);
        f.vkGetPhysicalDeviceSurfacePresentModesKHR(ctx.Physical(), surface, &nm, modes.data());
        // the VideoOut thread paces at the vblank: a present must not block it (mailbox, else immediate, else FIFO)
        mode = VK_PRESENT_MODE_FIFO_KHR;
        if (std::ranges::find(modes, VK_PRESENT_MODE_MAILBOX_KHR) != modes.end()) {
            mode = VK_PRESENT_MODE_MAILBOX_KHR;
        } else if (std::ranges::find(modes, VK_PRESENT_MODE_IMMEDIATE_KHR) != modes.end()) {
            mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        u32 count = std::max(caps.minImageCount + 1, 3u);
        if (caps.maxImageCount != 0) {
            count = std::min(count, caps.maxImageCount);
        }
        const VkSwapchainKHR old = swapchain;
        const VkSwapchainCreateInfoKHR sci{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                           .surface = surface,
                                           .minImageCount = count,
                                           .imageFormat = fmt.format,
                                           .imageColorSpace = fmt.colorSpace,
                                           .imageExtent = ext,
                                           .imageArrayLayers = 1,
                                           .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                                         VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                           .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                           .preTransform = caps.currentTransform,
                                           .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                           .presentMode = mode,
                                           .clipped = VK_TRUE,
                                           .oldSwapchain = old};
        VkSwapchainKHR created = VK_NULL_HANDLE;
        if (const VkResult r = f.vkCreateSwapchainKHR(ctx.Device(), &sci, nullptr, &created); r != VK_SUCCESS) {
            error = fmt::format("vkCreateSwapchainKHR failed ({})", static_cast<int>(r));
            return false;
        }
        DestroySwapchainObjects();
        if (old != VK_NULL_HANDLE) {
            f.vkDestroySwapchainKHR(ctx.Device(), old, nullptr);
        }
        swapchain = created;
        swap_format = fmt.format;
        extent = ext;
        u32 ni = 0;
        f.vkGetSwapchainImagesKHR(ctx.Device(), swapchain, &ni, nullptr);
        images.resize(ni);
        f.vkGetSwapchainImagesKHR(ctx.Device(), swapchain, &ni, images.data());
        done.resize(ni);
        for (auto& s : done) {
            const VkSemaphoreCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            f.vkCreateSemaphore(ctx.Device(), &ci, nullptr, &s);
        }
        std::scoped_lock lock{stats_mutex};
        stats.window_width = ext.width;
        stats.window_height = ext.height;
        return true;
    }

    void DestroySwapchainObjects() {
        for (auto& s : done) {
            if (s != VK_NULL_HANDLE) {
                Fn().vkDestroySemaphore(ctx.Device(), s, nullptr);
            }
        }
        done.clear();
        images.clear();
    }

    bool Recreate() {
        // every presentation's commands done before the images change
        for (auto& s : slots) {
            Fn().vkWaitForFences(ctx.Device(), 1, &s.fence, VK_TRUE, 2'000'000'000ull);
        }
        std::string error;
        if (!CreateSwapchain(error)) {
            std::scoped_lock lock{stats_mutex};
            stats.last_error = error;
            SSPC_LOG(GPU, Error, "native presenter: swapchain: {}", error);
            return false;
        }
        std::scoped_lock lock{stats_mutex};
        ++stats.swapchain_recreated;
        return true;
    }

    void DestroyFrame(Frame& fr) {
        if (fr.image != VK_NULL_HANDLE) {
            Fn().vkDestroyImage(ctx.Device(), fr.image, nullptr);
        }
        if (fr.memory != VK_NULL_HANDLE) {
            Fn().vkFreeMemory(ctx.Device(), fr.memory, nullptr);
        }
        fr.image = VK_NULL_HANDLE;
        fr.memory = VK_NULL_HANDLE;
        fr.width = fr.height = 0;
        fr.format = VK_FORMAT_UNDEFINED;
    }

    bool MakeFrame(Frame& fr, VkFormat format, u32 w, u32 h) {
        if (fr.image != VK_NULL_HANDLE && fr.format == format && fr.width == w && fr.height == h) {
            return true;
        }
        DestroyFrame(fr);
        const auto& f = Fn();
        const VkImageCreateInfo ici{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    .imageType = VK_IMAGE_TYPE_2D,
                                    .format = format,
                                    .extent = {w, h, 1},
                                    .mipLevels = 1,
                                    .arrayLayers = 1,
                                    .samples = VK_SAMPLE_COUNT_1_BIT,
                                    .tiling = VK_IMAGE_TILING_OPTIMAL,
                                    .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (f.vkCreateImage(ctx.Device(), &ici, nullptr, &fr.image) != VK_SUCCESS) {
            fr.image = VK_NULL_HANDLE;
            return false;
        }
        VkMemoryRequirements req{};
        f.vkGetImageMemoryRequirements(ctx.Device(), fr.image, &req);
        const s32 type = ctx.FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = req.size,
                                       .memoryTypeIndex = static_cast<u32>(std::max(type, 0))};
        if (type < 0 || f.vkAllocateMemory(ctx.Device(), &mai, nullptr, &fr.memory) != VK_SUCCESS) {
            fr.memory = VK_NULL_HANDLE;
            DestroyFrame(fr);
            return false;
        }
        f.vkBindImageMemory(ctx.Device(), fr.image, fr.memory, 0);
        fr.format = format;
        fr.width = w;
        fr.height = h;
        return true;
    }

    void Capture(const Frame& fr, const std::filesystem::path& png) {
        const auto& f = Fn();
        u32 bpp = 4;
        if (fr.format == VK_FORMAT_R16G16B16A16_SFLOAT || fr.format == VK_FORMAT_R16G16B16A16_UNORM) {
            bpp = 8;
        }
        const u64 bytes = u64{fr.width} * fr.height * bpp;
        OwnedBuffer buf;
        std::string error;
        if (!CreateBuffer(ctx, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, buf, error)) {
            SSPC_LOG(GPU, Error, "native presenter: capture buffer: {}", error);
            return;
        }
        VkCommandBuffer cb = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
        f.vkAllocateCommandBuffers(ctx.Device(), &ai, &cb);
        const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        f.vkBeginCommandBuffer(cb, &bi);
        const VkBufferImageCopy region{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                       .imageExtent = {fr.width, fr.height, 1}};
        Fn().vkCmdCopyImageToBuffer(cb, fr.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1, &region);
        const VkMemoryBarrier2 mb{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                  .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
        const VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                   .pMemoryBarriers = &mb};
        f.vkCmdPipelineBarrier2(cb, &dep);
        f.vkEndCommandBuffer(cb);
        VkFence fence = VK_NULL_HANDLE;
        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        f.vkCreateFence(ctx.Device(), &fci, nullptr, &fence);
        const VkCommandBufferSubmitInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
        const VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1,
                               .pCommandBufferInfos = &cbi};
        {
            std::scoped_lock lock{ctx.QueueMutex()};
            f.vkQueueSubmit2(ctx.Queue(), 1, &si, fence);
        }
        f.vkWaitForFences(ctx.Device(), 1, &fence, VK_TRUE, 5'000'000'000ull);
        f.vkDestroyFence(ctx.Device(), fence, nullptr);
        f.vkFreeCommandBuffers(ctx.Device(), pool, 1, &cb);
        std::vector<u8> rgba(u64{fr.width} * fr.height * 4);
        const u8* src = buf.mapped;
        for (u64 i = 0, n = u64{fr.width} * fr.height; i < n; ++i) {
            u8* d = rgba.data() + i * 4;
            switch (fr.format) {
            case VK_FORMAT_B8G8R8A8_UNORM:
                d[0] = src[i * 4 + 2], d[1] = src[i * 4 + 1], d[2] = src[i * 4 + 0], d[3] = src[i * 4 + 3];
                break;
            case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
            case VK_FORMAT_A2R10G10B10_UNORM_PACK32: {
                u32 v;
                std::memcpy(&v, src + i * 4, 4);
                const u32 c0 = v & 0x3FF, c1 = (v >> 10) & 0x3FF, c2 = (v >> 20) & 0x3FF;
                const bool bgr = fr.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32; // R in the high bits
                d[0] = static_cast<u8>((bgr ? c2 : c0) >> 2);
                d[1] = static_cast<u8>(c1 >> 2);
                d[2] = static_cast<u8>((bgr ? c0 : c2) >> 2);
                d[3] = 255;
                break;
            }
            case VK_FORMAT_R16G16B16A16_SFLOAT: {
                u16 h[4];
                std::memcpy(h, src + i * 8, 8);
                for (int c = 0; c < 3; ++c) {
                    d[c] = ToByte(HalfToFloat(h[c]));
                }
                d[3] = 255;
                break;
            }
            default: // R8G8B8A8 and the like: as stored
                std::memcpy(d, src + i * bpp, 4);
                break;
            }
            d[3] = 255; // the displayed image is opaque
        }
        DestroyBuffer(ctx, buf);
        const bool ok = WritePng(png, fr.width, fr.height, rgba);
        SSPC_LOG(GPU, Info, "native presenter: frame {}x{} (format {}) captured to {}: {}", fr.width, fr.height,
                 static_cast<int>(fr.format), png.string(), ok ? "written" : "FAILED");
        std::scoped_lock lock{stats_mutex};
        stats.captures += ok ? 1 : 0;
    }

};

Presenter::Presenter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

std::unique_ptr<Presenter> Presenter::Create(DeviceContext& ctx, Timeline& timeline, void* sdl_window,
                                             std::string& error) {
    auto impl = std::make_unique<Impl>(ctx, timeline);
    impl->window = static_cast<SDL_Window*>(sdl_window);
    if (impl->window == nullptr) {
        error = "no game window";
        return nullptr;
    }
    const SDL_PropertiesID props = SDL_GetWindowProperties(impl->window);
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd == nullptr) {
        error = "the game window has no Win32 handle";
        return nullptr;
    }
    const auto create_surface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(
        Fn().vkGetInstanceProcAddr(ctx.Instance(), "vkCreateWin32SurfaceKHR"));
    if (create_surface == nullptr) {
        error = "vkCreateWin32SurfaceKHR is not available";
        return nullptr;
    }
    const VkWin32SurfaceCreateInfoKHR sci{.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                          .hinstance = GetModuleHandleW(nullptr),
                                          .hwnd = hwnd};
    if (create_surface(ctx.Instance(), &sci, nullptr, &impl->surface) != VK_SUCCESS) {
        error = "vkCreateWin32SurfaceKHR failed";
        return nullptr;
    }
    VkBool32 supported = VK_FALSE;
    Fn().vkGetPhysicalDeviceSurfaceSupportKHR(ctx.Physical(), ctx.QueueFamily(), impl->surface, &supported);
    if (!supported) {
        error = "the device's queue cannot present to the game window";
        Fn().vkDestroySurfaceKHR(ctx.Instance(), impl->surface, nullptr);
        return nullptr;
    }
    const VkCommandPoolCreateInfo pci{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = ctx.QueueFamily()};
    Fn().vkCreateCommandPool(ctx.Device(), &pci, nullptr, &impl->pool);
    for (auto& s : impl->slots) {
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = impl->pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
        Fn().vkAllocateCommandBuffers(ctx.Device(), &ai, &s.cb);
        const VkFenceCreateInfo fci{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        Fn().vkCreateFence(ctx.Device(), &fci, nullptr, &s.fence);
        const VkSemaphoreCreateInfo semi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        Fn().vkCreateSemaphore(ctx.Device(), &semi, nullptr, &s.acquired);
    }
    if (!impl->CreateSwapchain(error)) {
        Presenter tmp(std::move(impl)); // destroys what was made
        return nullptr;
    }
    SSPC_LOG(GPU, Info, "native presenter: swapchain {}x{} on the game window, format {}, present mode {}",
             impl->extent.width, impl->extent.height, static_cast<int>(impl->swap_format), static_cast<int>(impl->mode));
    return std::unique_ptr<Presenter>(new Presenter(std::move(impl)));
}

Presenter::~Presenter() {
    if (!impl_) {
        return;
    }
    Impl& m = *impl_;
    const auto& f = Fn();
    if (m.ctx.Device() == VK_NULL_HANDLE) {
        return;
    }
    {
        std::scoped_lock lock{m.ctx.QueueMutex()};
        f.vkQueueWaitIdle(m.ctx.Queue());
    }
    for (auto& fr : m.frames) {
        m.DestroyFrame(fr);
    }
    m.DestroySwapchainObjects();
    if (m.swapchain != VK_NULL_HANDLE) {
        f.vkDestroySwapchainKHR(m.ctx.Device(), m.swapchain, nullptr);
    }
    for (auto& s : m.slots) {
        if (s.fence != VK_NULL_HANDLE) {
            f.vkDestroyFence(m.ctx.Device(), s.fence, nullptr);
        }
        if (s.acquired != VK_NULL_HANDLE) {
            f.vkDestroySemaphore(m.ctx.Device(), s.acquired, nullptr);
        }
    }
    if (m.pool != VK_NULL_HANDLE) {
        f.vkDestroyCommandPool(m.ctx.Device(), m.pool, nullptr);
    }
    if (m.surface != VK_NULL_HANDLE) {
        f.vkDestroySurfaceKHR(m.ctx.Instance(), m.surface, nullptr);
    }
}

s32 Presenter::AcquireFrame(u32 timeout_ms) {
    Impl& m = *impl_;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        {
            std::scoped_lock lock{m.frames_mutex};
            for (u32 i = 0; i < kFrames; ++i) {
                auto& fr = m.frames[i];
                u32 st = fr.state.load(std::memory_order_acquire);
                if (st == 3 && fr.fence != VK_NULL_HANDLE &&
                    Fn().vkWaitForFences(m.ctx.Device(), 1, &fr.fence, VK_TRUE, 0) == VK_SUCCESS) {
                    fr.state.store(0, std::memory_order_release);
                    st = 0;
                }
                if (st == 0) {
                    fr.state.store(1, std::memory_order_release);
                    return static_cast<s32>(i);
                }
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void Presenter::ReleaseFrame(s32 frame) {
    if (frame >= 0 && frame < static_cast<s32>(kFrames)) {
        impl_->frames[frame].state.store(0, std::memory_order_release);
    }
}

bool Presenter::RecordCopy(VkCommandBuffer cb, s32 frame, VkImage src, VkFormat format, u32 width, u32 height) {
    Impl& m = *impl_;
    if (frame < 0 || frame >= static_cast<s32>(kFrames) || src == VK_NULL_HANDLE || width == 0 || height == 0) {
        return false;
    }
    auto& fr = m.frames[frame];
    if (!m.MakeFrame(fr, Unorm(format), width, height)) {
        SSPC_LOG(GPU, Error, "native presenter: a {}x{} frame of format {} could not be created", width, height,
                 static_cast<int>(format));
        return false;
    }
    const auto& f = Fn();
    const VkImageMemoryBarrier2 to_dst{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                       .srcAccessMask = 0,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                       .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = fr.image,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                         .pImageMemoryBarriers = &to_dst};
    f.vkCmdPipelineBarrier2(cb, &dep);
    const VkImageCopy copy{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                           .extent = {width, height, 1}};
    f.vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, fr.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                     &copy);
    const VkImageMemoryBarrier2 to_src{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                       .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                       .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                       .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                       .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = fr.image,
                                       .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    dep.pImageMemoryBarriers = &to_src;
    f.vkCmdPipelineBarrier2(cb, &dep);
    fr.state.store(2, std::memory_order_release);
    std::scoped_lock lock{m.stats_mutex};
    ++m.stats.copies;
    m.stats.frame_width = width;
    m.stats.frame_height = height;
    return true;
}

void Presenter::Present(s32 frame, u64 flip_id) {
    Impl& m = *impl_;
    const auto& f = Fn();
    Impl::Frame* fr = (frame >= 0 && frame < static_cast<s32>(kFrames)) ? &m.frames[frame] : nullptr;
    const auto release = [&] {
        if (fr != nullptr) {
            fr->state.store(0, std::memory_order_release);
        }
    };
    int w = 0, h = 0;
    SDL_GetWindowSizeInPixels(m.window, &w, &h);
    if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(m.window) & SDL_WINDOW_MINIMIZED)) {
        release();
        std::scoped_lock lock{m.stats_mutex};
        ++m.stats.minimized_skips;
        return;
    }
    if (m.swapchain == VK_NULL_HANDLE || static_cast<u32>(w) != m.extent.width ||
        static_cast<u32>(h) != m.extent.height) {
        if (!m.Recreate() || m.swapchain == VK_NULL_HANDLE) {
            release();
            return;
        }
    }
    Impl::Slot& s = m.slots[m.slot];
    m.slot = (m.slot + 1) % m.slots.size();
    f.vkWaitForFences(m.ctx.Device(), 1, &s.fence, VK_TRUE, 2'000'000'000ull);
    u32 index = 0;
    VkResult r = f.vkAcquireNextImageKHR(m.ctx.Device(), m.swapchain, 100'000'000ull, s.acquired, VK_NULL_HANDLE,
                                         &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        m.Recreate();
        release();
        return;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        std::scoped_lock lock{m.stats_mutex};
        ++m.stats.acquire_failures;
        if (r == VK_ERROR_DEVICE_LOST) {
            m.stats.device_lost = true;
            m.stats.last_error = "VK_ERROR_DEVICE_LOST at acquire";
        }
        release();
        return;
    }
    f.vkResetFences(m.ctx.Device(), 1, &s.fence);
    f.vkResetCommandBuffer(s.cb, 0);
    const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    f.vkBeginCommandBuffer(s.cb, &bi);
    const VkImage swap = m.images[index];
    VkImageMemoryBarrier2 b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                            .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                            .srcAccessMask = 0,
                            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                            .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = swap,
                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    // the frame was written by an earlier submission of this queue (its completion seen on the host): a memory
    // dependency on everything before in submission order makes those writes visible to the blit
    const VkMemoryBarrier2 earlier{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                   .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                   .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                                   .dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                                   .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo dep{.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                         .pMemoryBarriers = &earlier, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    f.vkCmdPipelineBarrier2(s.cb, &dep);
    dep.memoryBarrierCount = 0;
    dep.pMemoryBarriers = nullptr;
    const VkClearColorValue black{.float32 = {0.0f, 0.0f, 0.0f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    f.vkCmdClearColorImage(s.cb, swap, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    const bool show = fr != nullptr && fr->image != VK_NULL_HANDLE && fr->state.load() == 2;
    if (show) {
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        f.vkCmdPipelineBarrier2(s.cb, &dep); // clear -> blit on the same image
        // the frame's aspect ratio kept, centred
        const double sx = static_cast<double>(m.extent.width) / fr->width;
        const double sy = static_cast<double>(m.extent.height) / fr->height;
        const double sc = std::min(sx, sy);
        const s32 dw = static_cast<s32>(fr->width * sc), dh = static_cast<s32>(fr->height * sc);
        const s32 dx = (static_cast<s32>(m.extent.width) - dw) / 2, dy = (static_cast<s32>(m.extent.height) - dh) / 2;
        const VkImageBlit blit{.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .srcOffsets = {{0, 0, 0}, {static_cast<s32>(fr->width), static_cast<s32>(fr->height), 1}},
                               .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               .dstOffsets = {{dx, dy, 0}, {dx + dw, dy + dh, 1}}};
        f.vkCmdBlitImage(s.cb, fr->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swap,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    f.vkCmdPipelineBarrier2(s.cb, &dep);
    f.vkEndCommandBuffer(s.cb);

    const VkSemaphoreSubmitInfo waits[1] = {
        {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = s.acquired,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT},
    };
    const VkSemaphoreSubmitInfo signal{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = m.done[index],
                                       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    const VkCommandBufferSubmitInfo cbi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s.cb};
    const VkSubmitInfo2 si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                           .waitSemaphoreInfoCount = 1,
                           .pWaitSemaphoreInfos = waits,
                           .commandBufferInfoCount = 1,
                           .pCommandBufferInfos = &cbi,
                           .signalSemaphoreInfoCount = 1,
                           .pSignalSemaphoreInfos = &signal};
    VkResult pr = VK_SUCCESS;
    {
        std::scoped_lock lock{m.ctx.QueueMutex()};
        r = f.vkQueueSubmit2(m.ctx.Queue(), 1, &si, s.fence);
        if (r == VK_SUCCESS) {
            const VkPresentInfoKHR pi{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                      .waitSemaphoreCount = 1,
                                      .pWaitSemaphores = &m.done[index],
                                      .swapchainCount = 1,
                                      .pSwapchains = &m.swapchain,
                                      .pImageIndices = &index};
            pr = f.vkQueuePresentKHR(m.ctx.Queue(), &pi);
        }
    }
    if (r != VK_SUCCESS) {
        std::scoped_lock lock{m.stats_mutex};
        m.stats.last_error = fmt::format("present submission failed ({})", static_cast<int>(r));
        m.stats.device_lost |= r == VK_ERROR_DEVICE_LOST;
        release();
        return;
    }
    if (fr != nullptr) {
        fr->fence = s.fence;
        fr->state.store(show ? 3 : 0, std::memory_order_release);
    }
    {
        std::scoped_lock lock{m.stats_mutex};
        ++m.stats.presented;
        m.stats.black += show ? 0 : 1;
    }
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
        m.Recreate();
    } else if (pr == VK_ERROR_DEVICE_LOST) {
        std::scoped_lock lock{m.stats_mutex};
        m.stats.device_lost = true;
        m.stats.last_error = "VK_ERROR_DEVICE_LOST at present";
    }
    std::filesystem::path capture;
    {
        std::scoped_lock lock{m.capture_mutex};
        if (m.capture_flip != 0 && flip_id >= m.capture_flip && show) {
            capture = m.capture_path;
            m.capture_flip = 0;
        }
    }
    if (!capture.empty() && fr != nullptr) {
        m.Capture(*fr, capture);
    }
}

void Presenter::RequestCapture(u64 flip_id, const std::filesystem::path& png) {
    std::scoped_lock lock{impl_->capture_mutex};
    impl_->capture_flip = flip_id;
    impl_->capture_path = png;
}

Presenter::Stats Presenter::GetStats() {
    std::scoped_lock lock{impl_->stats_mutex};
    return impl_->stats;
}

} // namespace SecondSon::Gpu::Vk
