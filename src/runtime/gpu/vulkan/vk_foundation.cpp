// SecondSonPC — the Vulkan foundation (P4.1.4). See vk_foundation.h.
//
// Ownership: Foundation owns the loader module, the instance, the device; every other object is created and destroyed
// inside one call (CheckResources, ShowTestWindow). Destruction is in reverse order of creation and waits for the
// device first. Every create/destroy goes through Track() so LiveObjects() can tell a leak.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include "runtime/gpu/vulkan/vk_foundation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string_view>

#include <fmt/format.h>
#include <fmt/ranges.h>

#include "base/log.h"
#include "runtime/gpu/shader/shader_types.h"

// Vulkan's structures are filled with designated initializers naming only the fields that matter (pNext, flags and
// the rest stay zero), the usual way to write them.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Vk {

namespace {

std::atomic<s64> g_live{0};

void Track(s64 delta) {
    g_live.fetch_add(delta, std::memory_order_relaxed);
}

// The functions SecondSonPC uses, resolved from the loader / instance / device (C API; no global prototypes).
#define SSPC_VK_GLOBAL(X)                                                                                              \
    X(vkEnumerateInstanceVersion)                                                                                      \
    X(vkEnumerateInstanceExtensionProperties)                                                                          \
    X(vkEnumerateInstanceLayerProperties)                                                                              \
    X(vkCreateInstance)
#define SSPC_VK_INSTANCE(X)                                                                                            \
    X(vkDestroyInstance)                                                                                               \
    X(vkEnumeratePhysicalDevices)                                                                                      \
    X(vkGetPhysicalDeviceProperties)                                                                                   \
    X(vkGetPhysicalDeviceProperties2)                                                                                  \
    X(vkGetPhysicalDeviceFeatures2)                                                                                    \
    X(vkGetPhysicalDeviceQueueFamilyProperties)                                                                        \
    X(vkGetPhysicalDeviceMemoryProperties)                                                                             \
    X(vkEnumerateDeviceExtensionProperties)                                                                            \
    X(vkCreateDevice)                                                                                                  \
    X(vkGetDeviceProcAddr)                                                                                             \
    X(vkGetPhysicalDeviceWin32PresentationSupportKHR)                                                                  \
    X(vkCreateWin32SurfaceKHR)                                                                                         \
    X(vkDestroySurfaceKHR)                                                                                             \
    X(vkGetPhysicalDeviceSurfaceSupportKHR)                                                                            \
    X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                                                                       \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define SSPC_VK_DEVICE(X)                                                                                              \
    X(vkDestroyDevice)                                                                                                 \
    X(vkGetDeviceQueue)                                                                                                \
    X(vkDeviceWaitIdle)                                                                                                \
    X(vkCreateCommandPool)                                                                                             \
    X(vkDestroyCommandPool)                                                                                            \
    X(vkAllocateCommandBuffers)                                                                                        \
    X(vkBeginCommandBuffer)                                                                                            \
    X(vkEndCommandBuffer)                                                                                              \
    X(vkResetCommandBuffer)                                                                                            \
    X(vkCmdPipelineBarrier)                                                                                            \
    X(vkCmdClearColorImage)                                                                                            \
    X(vkCmdCopyImageToBuffer)                                                                                          \
    X(vkQueueSubmit)                                                                                                   \
    X(vkCreateFence)                                                                                                   \
    X(vkDestroyFence)                                                                                                  \
    X(vkWaitForFences)                                                                                                 \
    X(vkResetFences)                                                                                                   \
    X(vkCreateSemaphore)                                                                                               \
    X(vkDestroySemaphore)                                                                                              \
    X(vkCreateBuffer)                                                                                                  \
    X(vkDestroyBuffer)                                                                                                 \
    X(vkGetBufferMemoryRequirements)                                                                                   \
    X(vkCreateImage)                                                                                                   \
    X(vkDestroyImage)                                                                                                  \
    X(vkGetImageMemoryRequirements)                                                                                    \
    X(vkAllocateMemory)                                                                                                \
    X(vkFreeMemory)                                                                                                    \
    X(vkBindBufferMemory)                                                                                              \
    X(vkBindImageMemory)                                                                                               \
    X(vkMapMemory)                                                                                                     \
    X(vkUnmapMemory)                                                                                                   \
    X(vkCreateShaderModule)                                                                                            \
    X(vkDestroyShaderModule)
#define SSPC_VK_SWAPCHAIN(X)                                                                                           \
    X(vkCreateSwapchainKHR)                                                                                            \
    X(vkDestroySwapchainKHR)                                                                                           \
    X(vkGetSwapchainImagesKHR)                                                                                         \
    X(vkAcquireNextImageKHR)                                                                                           \
    X(vkQueuePresentKHR)

#define SSPC_VK_MEMBER(name) PFN_##name name = nullptr;

struct Fns {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
    SSPC_VK_GLOBAL(SSPC_VK_MEMBER)
    SSPC_VK_INSTANCE(SSPC_VK_MEMBER)
    SSPC_VK_DEVICE(SSPC_VK_MEMBER)
    SSPC_VK_SWAPCHAIN(SSPC_VK_MEMBER)
};

/// Required by SecondSonPC's backend: the bridge's shader translator emits SPIR-V for this binding model (one set,
/// push descriptors, robust buffer/image access with null descriptors, vertex attribute divisors), and the backend
/// records with dynamic rendering and synchronization2 against timeline semaphores.
constexpr const char* kRequiredExtensions[] = {
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME,
};
/// One of these (the KHR promotion of the EXT one).
constexpr const char* kDivisorExtensions[] = {"VK_KHR_vertex_attribute_divisor", "VK_EXT_vertex_attribute_divisor"};
/// Reported, not enabled: what later sub-stages may use (P4.2–P4.4).
constexpr const char* kOptionalExtensions[] = {
    "VK_EXT_external_memory_host",     "VK_EXT_graphics_pipeline_library", "VK_EXT_descriptor_buffer",
    "VK_EXT_extended_dynamic_state3",  "VK_EXT_vertex_input_dynamic_state", "VK_EXT_memory_budget",
    "VK_EXT_shader_object",            "VK_KHR_maintenance5",               "VK_KHR_maintenance6",
    "VK_EXT_custom_border_color",      "VK_KHR_fragment_shader_barycentric", "VK_EXT_depth_clip_control",
    "VK_EXT_primitive_topology_list_restart", "VK_EXT_provoking_vertex",  "VK_EXT_attachment_feedback_loop_layout",
};

std::string TypeName(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return "cpu";
    default:
        return "other";
    }
}

std::string Version(u32 v) {
    return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
}

bool Has(const std::vector<VkExtensionProperties>& exts, std::string_view name) {
    return std::ranges::any_of(exts, [&](const VkExtensionProperties& e) { return name == e.extensionName; });
}

} // namespace

struct Foundation::Impl {
    Options options;
    HMODULE module = nullptr;
    Fns f{};
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue graphics = VK_NULL_HANDLE;
    DeviceInfo info{};
    VkPhysicalDeviceMemoryProperties memory{};

    ~Impl() {
        if (device != VK_NULL_HANDLE && f.vkDestroyDevice != nullptr) {
            if (f.vkDeviceWaitIdle != nullptr) {
                f.vkDeviceWaitIdle(device);
            }
            f.vkDestroyDevice(device, nullptr);
            Track(-1);
        }
        if (instance != VK_NULL_HANDLE && f.vkDestroyInstance != nullptr) {
            f.vkDestroyInstance(instance, nullptr);
            Track(-1);
        }
        if (module != nullptr) {
            FreeLibrary(module);
        }
    }

    bool LoadLoader(Report& report, std::string& error) {
        module = LoadLibraryW(L"vulkan-1.dll");
        if (module == nullptr) {
            error = "the Vulkan loader (vulkan-1.dll) is not installed";
            return false;
        }
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        char narrow[MAX_PATH * 3] = {};
        WideCharToMultiByte(CP_UTF8, 0, path, -1, narrow, sizeof(narrow), nullptr, nullptr);
        report.loader = narrow;
        f.vkGetInstanceProcAddr =
            reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module, "vkGetInstanceProcAddr"));
        if (f.vkGetInstanceProcAddr == nullptr) {
            error = "vulkan-1.dll has no vkGetInstanceProcAddr";
            return false;
        }
#define SSPC_VK_LOAD_GLOBAL(name) f.name = reinterpret_cast<PFN_##name>(f.vkGetInstanceProcAddr(nullptr, #name));
        SSPC_VK_GLOBAL(SSPC_VK_LOAD_GLOBAL)
#undef SSPC_VK_LOAD_GLOBAL
        if (f.vkCreateInstance == nullptr || f.vkEnumerateInstanceExtensionProperties == nullptr) {
            error = "the Vulkan loader lacks the global functions";
            return false;
        }
        report.loader_api = VK_API_VERSION_1_0;
        if (f.vkEnumerateInstanceVersion != nullptr) {
            f.vkEnumerateInstanceVersion(&report.loader_api);
        }
        return true;
    }

    bool CreateInstance(Report& report, std::string& error) {
        if (report.loader_api < VK_API_VERSION_1_3) {
            error = fmt::format("the Vulkan loader offers API {}; 1.3 is required", Version(report.loader_api));
            return false;
        }
        u32 n = 0;
        f.vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        f.vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());
        std::vector<const char*> enable;
        if (options.presentation) {
            for (const char* e : {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME}) {
                if (!Has(exts, e)) {
                    error = fmt::format("the instance extension {} is missing", e);
                    return false;
                }
                enable.push_back(e);
            }
        } else if (Has(exts, VK_KHR_SURFACE_EXTENSION_NAME) && Has(exts, VK_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
            // for vkGetPhysicalDeviceWin32PresentationSupportKHR (present support of the queue family)
            enable.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
            enable.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
        }
        std::vector<const char*> layers;
        if (options.validation && f.vkEnumerateInstanceLayerProperties != nullptr) {
            u32 nl = 0;
            f.vkEnumerateInstanceLayerProperties(&nl, nullptr);
            std::vector<VkLayerProperties> props(nl);
            f.vkEnumerateInstanceLayerProperties(&nl, props.data());
            for (const auto& p : props) {
                if (std::string_view(p.layerName) == "VK_LAYER_KHRONOS_validation") {
                    layers.push_back("VK_LAYER_KHRONOS_validation");
                }
            }
        }
        for (const char* e : enable) {
            report.instance_extensions.emplace_back(e);
        }
        for (const char* l : layers) {
            report.layers.emplace_back(l);
        }
        if (options.fail_at == FailPoint::Instance) {
            error = "vkCreateInstance: VK_ERROR_INITIALIZATION_FAILED (injected by the self-test)";
            return false;
        }
        const VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                    .pApplicationName = "SecondSonPC",
                                    .applicationVersion = 1,
                                    .pEngineName = "SecondSonPC",
                                    .engineVersion = 1,
                                    .apiVersion = VK_API_VERSION_1_3};
        const VkInstanceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                      .pApplicationInfo = &app,
                                      .enabledLayerCount = static_cast<u32>(layers.size()),
                                      .ppEnabledLayerNames = layers.data(),
                                      .enabledExtensionCount = static_cast<u32>(enable.size()),
                                      .ppEnabledExtensionNames = enable.data()};
        const VkResult r = f.vkCreateInstance(&ci, nullptr, &instance);
        if (r != VK_SUCCESS) {
            instance = VK_NULL_HANDLE;
            error = fmt::format("vkCreateInstance failed ({})", static_cast<int>(r));
            return false;
        }
        Track(+1);
#define SSPC_VK_LOAD_INSTANCE(name) f.name = reinterpret_cast<PFN_##name>(f.vkGetInstanceProcAddr(instance, #name));
        SSPC_VK_INSTANCE(SSPC_VK_LOAD_INSTANCE)
#undef SSPC_VK_LOAD_INSTANCE
        if (f.vkEnumeratePhysicalDevices == nullptr || f.vkCreateDevice == nullptr || f.vkGetDeviceProcAddr == nullptr) {
            error = "the instance lacks the core functions";
            return false;
        }
        return true;
    }

    DeviceInfo Examine(VkPhysicalDevice pd, u32 index) {
        DeviceInfo d;
        d.index = index;
        VkPhysicalDeviceProperties p{};
        f.vkGetPhysicalDeviceProperties(pd, &p);
        d.name = p.deviceName;
        d.vendor_id = p.vendorID;
        d.device_id = p.deviceID;
        d.driver_version = p.driverVersion;
        d.api_version = p.apiVersion;
        d.type = TypeName(p.deviceType);
        VkPhysicalDeviceMemoryProperties mp{};
        f.vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (u32 i = 0; i < mp.memoryHeapCount; ++i) {
            if (mp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
                d.device_local_bytes += mp.memoryHeaps[i].size;
            }
        }
        if (p.apiVersion < VK_API_VERSION_1_3) {
            d.missing.push_back(fmt::format("Vulkan 1.3 (device has {})", Version(p.apiVersion)));
        }
        u32 n = 0;
        f.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        f.vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, exts.data());
        for (const char* e : kRequiredExtensions) {
            if (!Has(exts, e)) {
                d.missing.emplace_back(e);
            }
        }
        if (!Has(exts, kDivisorExtensions[0]) && !Has(exts, kDivisorExtensions[1])) {
            d.missing.emplace_back("VK_KHR/EXT_vertex_attribute_divisor");
        }
        if (options.presentation && !Has(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            d.missing.emplace_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }
        for (const auto& e : options.extra_device_extensions) {
            if (!Has(exts, e)) {
                d.missing.push_back(e);
            }
        }
        for (const char* e : kOptionalExtensions) {
            if (Has(exts, e)) {
                d.optional.emplace_back(e);
            }
        }
        // features
        VkPhysicalDeviceRobustness2FeaturesEXT rob{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
        VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        v12.pNext = &v13;
        if (Has(exts, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
            v13.pNext = &rob;
        }
        VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
        if (p.apiVersion >= VK_API_VERSION_1_3) {
            f.vkGetPhysicalDeviceFeatures2(pd, &f2);
            const auto need = [&](VkBool32 have, const char* name) {
                if (!have) {
                    d.missing.emplace_back(name);
                }
            };
            need(f2.features.robustBufferAccess, "robustBufferAccess");
            need(v13.dynamicRendering, "dynamicRendering");
            need(v13.synchronization2, "synchronization2");
            need(v12.timelineSemaphore, "timelineSemaphore");
            need(v12.bufferDeviceAddress, "bufferDeviceAddress");
            if (Has(exts, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME)) {
                need(rob.robustBufferAccess2, "robustBufferAccess2");
                need(rob.robustImageAccess2, "robustImageAccess2");
                need(rob.nullDescriptor, "nullDescriptor");
            }
        }
        // queue families
        u32 nq = 0;
        f.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qf(nq);
        f.vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf.data());
        for (u32 i = 0; i < nq; ++i) {
            const VkQueueFlags fl = qf[i].queueFlags;
            const bool present = f.vkGetPhysicalDeviceWin32PresentationSupportKHR == nullptr ||
                                 f.vkGetPhysicalDeviceWin32PresentationSupportKHR(pd, i) == VK_TRUE;
            if (d.graphics_family == ~0u && (fl & VK_QUEUE_GRAPHICS_BIT) && (fl & VK_QUEUE_COMPUTE_BIT) && present) {
                d.graphics_family = i;
            } else if (d.compute_family == ~0u && (fl & VK_QUEUE_COMPUTE_BIT) && !(fl & VK_QUEUE_GRAPHICS_BIT)) {
                d.compute_family = i;
            } else if (d.transfer_family == ~0u && (fl & VK_QUEUE_TRANSFER_BIT) &&
                       !(fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) {
                d.transfer_family = i;
            }
        }
        if (d.graphics_family == ~0u) {
            d.missing.emplace_back("a queue family with graphics, compute and presentation");
        }
        d.suitable = d.missing.empty();
        return d;
    }

    bool CreateDevice(Report& report, std::string& error) {
        u32 n = 0;
        f.vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> pds(n);
        f.vkEnumeratePhysicalDevices(instance, &n, pds.data());
        if (n == 0) {
            error = "no Vulkan physical device";
            return false;
        }
        for (u32 i = 0; i < n; ++i) {
            report.devices.push_back(Examine(pds[i], i));
        }
        s32 pick = -1;
        if (options.device_index >= 0) {
            if (static_cast<u32>(options.device_index) >= n) {
                error = fmt::format("--gpu-device {}: there are {} devices (0..{})", options.device_index, n, n - 1);
                return false;
            }
            const auto& d = report.devices[options.device_index];
            if (!d.suitable) {
                std::string what;
                for (const auto& m : d.missing) {
                    what += (what.empty() ? "" : ", ") + m;
                }
                error = fmt::format("device {} ({}) lacks: {}", d.index, d.name, what);
                return false;
            }
            pick = options.device_index;
        } else {
            const auto rank = [](const DeviceInfo& d) {
                return d.type == "discrete" ? 2 : d.type == "integrated" ? 1 : 0;
            };
            for (const auto& d : report.devices) {
                if (!d.suitable) {
                    continue;
                }
                if (pick < 0 || rank(d) > rank(report.devices[pick]) ||
                    (rank(d) == rank(report.devices[pick]) &&
                     d.device_local_bytes > report.devices[pick].device_local_bytes)) {
                    pick = static_cast<s32>(d.index);
                }
            }
            if (pick < 0) {
                std::string what;
                for (const auto& d : report.devices) {
                    for (const auto& m : d.missing) {
                        what += fmt::format("{}{}: {}", what.empty() ? "" : "; ", d.name, m);
                    }
                }
                error = "no device has what SecondSonPC's backend needs (" + what + ")";
                return false;
            }
        }
        report.selected = pick;
        info = report.devices[pick];
        physical = pds[pick];
        f.vkGetPhysicalDeviceMemoryProperties(physical, &memory);
        if (options.fail_at == FailPoint::Device) {
            error = "vkCreateDevice: VK_ERROR_INITIALIZATION_FAILED (injected by the self-test)";
            return false;
        }

        // the device's extensions and features
        u32 ne = 0;
        f.vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        f.vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, exts.data());
        std::vector<const char*> enable(std::begin(kRequiredExtensions), std::end(kRequiredExtensions));
        enable.push_back(Has(exts, kDivisorExtensions[0]) ? kDivisorExtensions[0] : kDivisorExtensions[1]);
        if (options.presentation) {
            enable.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        }
        for (const auto& e : options.extra_device_extensions) {
            enable.push_back(e.c_str());
        }
        for (const char* e : enable) {
            report.enabled_device_extensions.emplace_back(e);
        }
        VkPhysicalDeviceRobustness2FeaturesEXT rob{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT,
                                                   .robustBufferAccess2 = VK_TRUE,
                                                   .robustImageAccess2 = VK_TRUE,
                                                   .nullDescriptor = VK_TRUE};
        VkPhysicalDeviceVulkan13Features v13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                             .pNext = &rob,
                                             .synchronization2 = VK_TRUE,
                                             .dynamicRendering = VK_TRUE};
        VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                             .pNext = &v13,
                                             .timelineSemaphore = VK_TRUE,
                                             .bufferDeviceAddress = VK_TRUE};
        VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
        f2.features.robustBufferAccess = VK_TRUE; // robustBufferAccess2 requires it

        const float priority = 1.0f;
        std::vector<VkDeviceQueueCreateInfo> queues;
        for (const u32 family : {info.graphics_family, info.compute_family, info.transfer_family}) {
            if (family != ~0u) {
                queues.push_back(VkDeviceQueueCreateInfo{.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                                         .queueFamilyIndex = family,
                                                         .queueCount = 1,
                                                         .pQueuePriorities = &priority});
            }
        }
        const VkDeviceCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                    .pNext = &f2,
                                    .queueCreateInfoCount = static_cast<u32>(queues.size()),
                                    .pQueueCreateInfos = queues.data(),
                                    .enabledExtensionCount = static_cast<u32>(enable.size()),
                                    .ppEnabledExtensionNames = enable.data()};
        const VkResult r = f.vkCreateDevice(physical, &ci, nullptr, &device);
        if (r != VK_SUCCESS) {
            device = VK_NULL_HANDLE;
            error = fmt::format("vkCreateDevice failed ({})", static_cast<int>(r));
            return false;
        }
        Track(+1);
#define SSPC_VK_LOAD_DEVICE(name) f.name = reinterpret_cast<PFN_##name>(f.vkGetDeviceProcAddr(device, #name));
        SSPC_VK_DEVICE(SSPC_VK_LOAD_DEVICE)
        if (options.presentation) {
            SSPC_VK_SWAPCHAIN(SSPC_VK_LOAD_DEVICE)
        }
#undef SSPC_VK_LOAD_DEVICE
        if (f.vkGetDeviceQueue == nullptr || f.vkQueueSubmit == nullptr) {
            error = "the device lacks the core functions";
            return false;
        }
        f.vkGetDeviceQueue(device, info.graphics_family, 0, &graphics);
        report.queues.push_back(fmt::format("graphics+compute family {}", info.graphics_family));
        if (info.compute_family != ~0u) {
            report.queues.push_back(fmt::format("asynchronous compute family {}", info.compute_family));
        }
        if (info.transfer_family != ~0u) {
            report.queues.push_back(fmt::format("transfer family {}", info.transfer_family));
        }
        return true;
    }

    s32 MemoryType(u32 bits, VkMemoryPropertyFlags want) const {
        for (u32 i = 0; i < memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) {
                return static_cast<s32>(i);
            }
        }
        return -1;
    }

    /// One-shot command buffer on the graphics queue: record, submit, wait.
    bool Submit(VkCommandPool pool, auto&& record, std::string& detail) {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
        if (f.vkAllocateCommandBuffers(device, &ai, &cb) != VK_SUCCESS) {
            detail = "vkAllocateCommandBuffers failed";
            return false;
        }
        const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        if (f.vkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) {
            detail = "vkBeginCommandBuffer failed";
            return false;
        }
        record(cb);
        if (f.vkEndCommandBuffer(cb) != VK_SUCCESS) {
            detail = "vkEndCommandBuffer failed";
            return false;
        }
        VkFence fence = VK_NULL_HANDLE;
        const VkFenceCreateInfo fi{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (f.vkCreateFence(device, &fi, nullptr, &fence) != VK_SUCCESS) {
            detail = "vkCreateFence failed";
            return false;
        }
        Track(+1);
        const VkSubmitInfo si{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
        bool ok = f.vkQueueSubmit(graphics, 1, &si, fence) == VK_SUCCESS &&
                  f.vkWaitForFences(device, 1, &fence, VK_TRUE, 5'000'000'000ull) == VK_SUCCESS;
        if (!ok) {
            detail = "the submission did not complete within 5 s";
            f.vkDeviceWaitIdle(device);
        }
        f.vkDestroyFence(device, fence, nullptr);
        Track(-1);
        return ok;
    }

    /// P4.3: what the selected device supports that a shader translation depends on (the bridge's Profile rules).
    void QueryProfile(Gpu::Shader::DeviceProfile& out) {
        u32 ne = 0;
        f.vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        f.vkEnumerateDeviceExtensionProperties(physical, nullptr, &ne, exts.data());
        VkPhysicalDeviceVulkan12Properties p12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceVulkan11Properties p11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
                                               .pNext = &p12};
        VkPhysicalDeviceProperties2 p2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11};
        f.vkGetPhysicalDeviceProperties2(physical, &p2);
        VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceFeatures2 f2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v12};
        f.vkGetPhysicalDeviceFeatures2(physical, &f2);
        const VkPhysicalDeviceLimits& lim = p2.properties.limits;
        const bool nvidia = p12.driverID == VK_DRIVER_ID_NVIDIA_PROPRIETARY;
        out = {};
        out.max_viewport_width = lim.maxViewportDimensions[0];
        out.max_viewport_height = lim.maxViewportDimensions[1];
        out.max_shared_memory_size = lim.maxComputeSharedMemorySize;
        out.supported_spirv = 0x00010600;
        out.subgroup_size = p11.subgroupSize;
        out.sparse_page_shift = 16; // the bridge's buffer cache: the sparse binding granularity (64 KiB on this device class)
        out.support_int8 = v12.shaderInt8 != 0;
        out.support_int16 = f2.features.shaderInt16 != 0;
        out.support_int64 = f2.features.shaderInt64 != 0;
        out.support_float16 = v12.shaderFloat16 != 0;
        out.support_float64 = f2.features.shaderFloat64 != 0;
        out.supports_denorm_behavior_independence =
            p12.denormBehaviorIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
        out.supports_rounding_mode_independence =
            p12.roundingModeIndependence != VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_NONE;
        out.support_fp16_denorm_preserve = p12.shaderDenormPreserveFloat16 != 0;
        out.support_fp16_denorm_flush = p12.shaderDenormFlushToZeroFloat16 != 0;
        out.support_fp16_round_to_zero = p12.shaderRoundingModeRTZFloat16 != 0;
        out.support_fp32_denorm_preserve = p12.shaderDenormPreserveFloat32 != 0;
        out.support_fp32_denorm_flush = p12.shaderDenormFlushToZeroFloat32 != 0;
        out.support_fp32_round_to_zero = p12.shaderRoundingModeRTZFloat32 != 0;
        out.support_fp64_denorm_preserve = p12.shaderDenormPreserveFloat64 != 0;
        out.support_fp64_denorm_flush = p12.shaderDenormFlushToZeroFloat64 != 0;
        out.support_fp64_round_to_zero = p12.shaderRoundingModeRTZFloat64 != 0;
        out.support_fp16_signed_zero_inf_nan_preserve = p12.shaderSignedZeroInfNanPreserveFloat16 != 0;
        out.support_fp32_signed_zero_inf_nan_preserve = p12.shaderSignedZeroInfNanPreserveFloat32 != 0;
        out.support_fp64_signed_zero_inf_nan_preserve = p12.shaderSignedZeroInfNanPreserveFloat64 != 0;
        out.supports_image_load_store_lod = Has(exts, "VK_AMD_shader_image_load_store_lod");
        out.supports_native_cube_calc = Has(exts, "VK_AMD_gcn_shader");
        out.supports_trinary_minmax = Has(exts, "VK_AMD_shader_trinary_minmax");
        // VK_EXT_shader_atomic_float2's min/max features are not queried here (reported against the bridge's)
        out.supports_buffer_int64_atomics = v12.shaderBufferInt64Atomics != 0;
        out.supports_shared_int64_atomics = v12.shaderSharedInt64Atomics != 0;
        out.supports_workgroup_explicit_memory_layout = Has(exts, "VK_KHR_workgroup_memory_explicit_layout");
        out.supports_amd_shader_explicit_vertex_parameter = Has(exts, "VK_AMD_shader_explicit_vertex_parameter");
        out.supports_fragment_shader_barycentric = Has(exts, "VK_KHR_fragment_shader_barycentric");
        out.supports_shader_subgroup_clock = Has(exts, "VK_KHR_shader_clock");
        out.needs_manual_interpolation = out.supports_fragment_shader_barycentric && nvidia;
        out.needs_lds_barriers = nvidia;
        out.needs_buffer_offsets = lim.minStorageBufferOffsetAlignment > 4;
        out.needs_clip_distance_emulation = nvidia;
        out.supports_shader_stencil_export = Has(exts, "VK_EXT_shader_stencil_export");
        out.depth_clip_control = Has(exts, "VK_EXT_depth_clip_control");
        out.source = fmt::format("{} (driver {:#x})", info.name, info.driver_version);
    }

    /// P4.3: every module through vkCreateShaderModule (and destroyed): the driver's acceptance of the SPIR-V.
    void CreateModules(const std::vector<std::vector<u32>>& spirv, u32& accepted, u32& refused, std::string& detail) {
        for (size_t i = 0; i < spirv.size(); ++i) {
            const auto& code = spirv[i];
            const VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                              .codeSize = code.size() * 4,
                                              .pCode = code.data()};
            VkShaderModule module = VK_NULL_HANDLE;
            const VkResult r = code.empty() ? VK_ERROR_INITIALIZATION_FAILED
                                            : f.vkCreateShaderModule(device, &ci, nullptr, &module);
            if (r == VK_SUCCESS) {
                Track(+1);
                f.vkDestroyShaderModule(device, module, nullptr);
                Track(-1);
                ++accepted;
            } else {
                ++refused;
                if (detail.empty()) {
                    detail = fmt::format("module {}: VkResult {}", i, static_cast<int>(r));
                }
            }
        }
    }
};

Foundation::Foundation(std::unique_ptr<Impl> impl) : impl_{std::move(impl)} {}

Foundation::~Foundation() = default;

const DeviceInfo& Foundation::Selected() const {
    return impl_->info;
}

std::unique_ptr<Foundation> Foundation::Create(const Options& options, Report& report, std::string& error) {
    auto impl = std::make_unique<Impl>();
    impl->options = options;
    if (!impl->LoadLoader(report, error) || !impl->CreateInstance(report, error) ||
        !impl->CreateDevice(report, error)) {
        return nullptr; // ~Impl destroys what was created
    }
    SSPC_LOG(GPU, Info, "SecondSonPC Vulkan foundation: {} ({}, driver {:#x}, API {}), queues: {}", impl->info.name,
             impl->info.type, impl->info.driver_version, Version(impl->info.api_version),
             fmt::join(report.queues, ", "));
    return std::unique_ptr<Foundation>(new Foundation(std::move(impl)));
}

bool Foundation::CheckResources(u32 rgba, std::string& detail) {
    Impl& m = *impl_;
    const auto& f = m.f;
    VkDevice dev = m.device;
    constexpr u32 kSize = 64;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory buffer_memory = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE;
    bool ok = false;
    const auto cleanup = [&] {
        if (image != VK_NULL_HANDLE) {
            f.vkDestroyImage(dev, image, nullptr);
            Track(-1);
        }
        if (image_memory != VK_NULL_HANDLE) {
            f.vkFreeMemory(dev, image_memory, nullptr);
            Track(-1);
        }
        if (buffer != VK_NULL_HANDLE) {
            f.vkDestroyBuffer(dev, buffer, nullptr);
            Track(-1);
        }
        if (buffer_memory != VK_NULL_HANDLE) {
            f.vkFreeMemory(dev, buffer_memory, nullptr);
            Track(-1);
        }
        if (pool != VK_NULL_HANDLE) {
            f.vkDestroyCommandPool(dev, pool, nullptr);
            Track(-1);
        }
    };
    do {
        const VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                         .queueFamilyIndex = m.info.graphics_family};
        if (f.vkCreateCommandPool(dev, &pi, nullptr, &pool) != VK_SUCCESS) {
            pool = VK_NULL_HANDLE;
            detail = "vkCreateCommandPool failed";
            break;
        }
        Track(+1);
        const VkBufferCreateInfo bi{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                    .size = kSize * kSize * 4,
                                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        if (f.vkCreateBuffer(dev, &bi, nullptr, &buffer) != VK_SUCCESS) {
            buffer = VK_NULL_HANDLE;
            detail = "vkCreateBuffer failed";
            break;
        }
        Track(+1);
        if (m.options.fail_at == FailPoint::Resources) {
            detail = "vkAllocateMemory: VK_ERROR_OUT_OF_DEVICE_MEMORY (injected by the self-test)";
            break;
        }
        VkMemoryRequirements br{};
        f.vkGetBufferMemoryRequirements(dev, buffer, &br);
        const s32 bt = m.MemoryType(br.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        const VkMemoryAllocateInfo bai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = br.size,
                                       .memoryTypeIndex = static_cast<u32>(bt)};
        if (bt < 0 || f.vkAllocateMemory(dev, &bai, nullptr, &buffer_memory) != VK_SUCCESS) {
            buffer_memory = VK_NULL_HANDLE;
            detail = "no host-visible memory for the read-back buffer";
            break;
        }
        Track(+1);
        void* pre = nullptr;
        if (f.vkBindBufferMemory(dev, buffer, buffer_memory, 0) != VK_SUCCESS ||
            f.vkMapMemory(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, &pre) != VK_SUCCESS) {
            detail = "the read-back buffer cannot be bound or mapped";
            break;
        }
        for (u32 i = 0; i < kSize * kSize; ++i) { // a sentinel: a copy that did not happen cannot pass
            static_cast<u32*>(pre)[i] = ~rgba;
        }
        f.vkUnmapMemory(dev, buffer_memory);
        const VkImageCreateInfo ii{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                   .imageType = VK_IMAGE_TYPE_2D,
                                   .format = VK_FORMAT_R8G8B8A8_UNORM,
                                   .extent = {kSize, kSize, 1},
                                   .mipLevels = 1,
                                   .arrayLayers = 1,
                                   .samples = VK_SAMPLE_COUNT_1_BIT,
                                   .tiling = VK_IMAGE_TILING_OPTIMAL,
                                   .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                   .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
        if (f.vkCreateImage(dev, &ii, nullptr, &image) != VK_SUCCESS) {
            image = VK_NULL_HANDLE;
            detail = "vkCreateImage failed";
            break;
        }
        Track(+1);
        VkMemoryRequirements ir{};
        f.vkGetImageMemoryRequirements(dev, image, &ir);
        const s32 it = m.MemoryType(ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const VkMemoryAllocateInfo iai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                       .allocationSize = ir.size,
                                       .memoryTypeIndex = static_cast<u32>(it)};
        if (it < 0 || f.vkAllocateMemory(dev, &iai, nullptr, &image_memory) != VK_SUCCESS) {
            image_memory = VK_NULL_HANDLE;
            detail = "no device-local memory for the image";
            break;
        }
        Track(+1);
        if (f.vkBindImageMemory(dev, image, image_memory, 0) != VK_SUCCESS) {
            detail = "vkBindImageMemory failed";
            break;
        }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        const VkClearColorValue color{.float32 = {(rgba & 0xFF) / 255.0f, ((rgba >> 8) & 0xFF) / 255.0f,
                                                  ((rgba >> 16) & 0xFF) / 255.0f, ((rgba >> 24) & 0xFF) / 255.0f}};
        const bool submitted = m.Submit(
            pool,
            [&](VkCommandBuffer cb) {
                VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                       .srcAccessMask = 0,
                                       .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                       .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                       .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                       .image = image,
                                       .subresourceRange = range};
                f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                       nullptr, 0, nullptr, 1, &b);
                f.vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                                       nullptr, 0, nullptr, 1, &b);
                const VkBufferImageCopy copy{.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                             .imageExtent = {kSize, kSize, 1}};
                f.vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
                const VkBufferMemoryBarrier hb{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                               .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                               .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                               .buffer = buffer,
                                               .size = VK_WHOLE_SIZE};
                f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr,
                                       1, &hb, 0, nullptr);
            },
            detail);
        if (!submitted) {
            break;
        }
        void* mapped = nullptr;
        if (f.vkMapMemory(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
            detail = "vkMapMemory failed";
            break;
        }
        const u32* px = static_cast<const u32*>(mapped);
        u32 wrong = 0;
        for (u32 i = 0; i < kSize * kSize; ++i) {
            wrong += px[i] != rgba ? 1 : 0;
        }
        const u32 first = px[0];
        f.vkUnmapMemory(dev, buffer_memory);
        ok = wrong == 0;
        detail = fmt::format("{}x{} RGBA8 image cleared to {:08x} on the graphics queue, copied to a host buffer: {} "
                             "of {} pixels differ (first {:08x})",
                             kSize, kSize, rgba, wrong, kSize * kSize, first);
    } while (false);
    cleanup();
    return ok;
}

namespace {

LRESULT CALLBACK ProbeWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLOSE) {
        // only noted: the window outlives its swapchain and surface, it is destroyed after them
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 1);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

bool Foundation::ShowTestWindow(u32 seconds, u32 rgba, u32& frames, std::string& detail) {
    frames = 0;
    Impl& m = *impl_;
    const auto& f = m.f;
    VkDevice dev = m.device;
    if (!m.options.presentation || f.vkCreateSwapchainKHR == nullptr || f.vkCreateWin32SurfaceKHR == nullptr) {
        detail = "the foundation was created without presentation";
        return false;
    }
    const HINSTANCE hinst = GetModuleHandleW(nullptr);
    static const wchar_t* kClass = L"SecondSonPC.GpuProbe";
    WNDCLASSW wc{};
    wc.lpfnWndProc = &ProbeWindowProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    RegisterClassW(&wc); // a second registration fails harmlessly
    RECT rc{0, 0, 960, 540};
    AdjustWindowRect(&rc, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    HWND hwnd = CreateWindowExW(0, kClass,
                                L"SecondSonPC GPU probe — diagnostic colour from SecondSonPC's own Vulkan device, "
                                L"NOT a game frame",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, hinst, nullptr);
    if (hwnd == nullptr) {
        detail = "CreateWindowExW failed";
        return false;
    }
    ShowWindow(hwnd, SW_SHOW);

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquired = VK_NULL_HANDLE;
    std::vector<VkSemaphore> rendered;
    bool ok = false;
    std::string format_text;
    do {
        const VkWin32SurfaceCreateInfoKHR si{.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
                                             .hinstance = hinst,
                                             .hwnd = hwnd};
        if (f.vkCreateWin32SurfaceKHR(m.instance, &si, nullptr, &surface) != VK_SUCCESS) {
            surface = VK_NULL_HANDLE;
            detail = "vkCreateWin32SurfaceKHR failed";
            break;
        }
        Track(+1);
        VkBool32 supported = VK_FALSE;
        f.vkGetPhysicalDeviceSurfaceSupportKHR(m.physical, m.info.graphics_family, surface, &supported);
        if (!supported) {
            detail = "the graphics queue family cannot present to the window";
            break;
        }
        VkSurfaceCapabilitiesKHR caps{};
        f.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m.physical, surface, &caps);
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
            detail = "the surface's images cannot be cleared (no TRANSFER_DST)";
            break;
        }
        u32 nf = 0;
        f.vkGetPhysicalDeviceSurfaceFormatsKHR(m.physical, surface, &nf, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(nf);
        f.vkGetPhysicalDeviceSurfaceFormatsKHR(m.physical, surface, &nf, formats.data());
        if (nf == 0) {
            detail = "the surface has no format";
            break;
        }
        VkSurfaceFormatKHR fmt0 = formats[0];
        for (const auto& sf : formats) {
            if (sf.format == VK_FORMAT_B8G8R8A8_UNORM) {
                fmt0 = sf;
            }
        }
        format_text = fmt::format("format {}, colour space {}", static_cast<int>(fmt0.format),
                                  static_cast<int>(fmt0.colorSpace));
        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu) {
            extent = {960, 540};
        }
        if (extent.width == 0 || extent.height == 0) {
            detail = "the window has no area (minimized)";
            break;
        }
        u32 count = std::max(caps.minImageCount + 1, 2u);
        if (caps.maxImageCount != 0) {
            count = std::min(count, caps.maxImageCount);
        }
        const VkSwapchainCreateInfoKHR sc{.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                          .surface = surface,
                                          .minImageCount = count,
                                          .imageFormat = fmt0.format,
                                          .imageColorSpace = fmt0.colorSpace,
                                          .imageExtent = extent,
                                          .imageArrayLayers = 1,
                                          .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                                          .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                          .preTransform = caps.currentTransform,
                                          .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                          .presentMode = VK_PRESENT_MODE_FIFO_KHR,
                                          .clipped = VK_TRUE};
        if (f.vkCreateSwapchainKHR(dev, &sc, nullptr, &swapchain) != VK_SUCCESS) {
            swapchain = VK_NULL_HANDLE;
            detail = "vkCreateSwapchainKHR failed";
            break;
        }
        Track(+1);
        u32 ni = 0;
        f.vkGetSwapchainImagesKHR(dev, swapchain, &ni, nullptr);
        std::vector<VkImage> images(ni);
        f.vkGetSwapchainImagesKHR(dev, swapchain, &ni, images.data());
        const VkCommandPoolCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                         .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                         .queueFamilyIndex = m.info.graphics_family};
        if (f.vkCreateCommandPool(dev, &pi, nullptr, &pool) != VK_SUCCESS) {
            pool = VK_NULL_HANDLE;
            detail = "vkCreateCommandPool failed";
            break;
        }
        Track(+1);
        VkCommandBuffer cb = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                             .commandPool = pool,
                                             .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                             .commandBufferCount = 1};
        if (f.vkAllocateCommandBuffers(dev, &ai, &cb) != VK_SUCCESS) {
            detail = "vkAllocateCommandBuffers failed";
            break;
        }
        const VkFenceCreateInfo fi{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        const VkSemaphoreCreateInfo smi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (f.vkCreateFence(dev, &fi, nullptr, &fence) != VK_SUCCESS) {
            fence = VK_NULL_HANDLE;
            detail = "vkCreateFence failed";
            break;
        }
        Track(+1);
        if (f.vkCreateSemaphore(dev, &smi, nullptr, &acquired) != VK_SUCCESS) {
            acquired = VK_NULL_HANDLE;
            detail = "vkCreateSemaphore failed";
            break;
        }
        Track(+1);
        bool sems = true;
        for (u32 i = 0; i < ni; ++i) { // one per swapchain image: reused only after that image is acquired again
            VkSemaphore s = VK_NULL_HANDLE;
            if (f.vkCreateSemaphore(dev, &smi, nullptr, &s) != VK_SUCCESS) {
                sems = false;
                break;
            }
            Track(+1);
            rendered.push_back(s);
        }
        if (!sems) {
            detail = "vkCreateSemaphore failed";
            break;
        }
        const auto t0 = std::chrono::steady_clock::now();
        bool clean = false; // stopped by the time limit or the user, not by an error
        std::string stop = "time is up";
        for (;;) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            if (GetWindowLongPtrW(hwnd, GWLP_USERDATA) != 0) {
                stop = "the window was closed by the user";
                clean = true;
                break;
            }
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (t >= seconds) {
                clean = true;
                break;
            }
            u32 index = 0;
            const VkResult ar = f.vkAcquireNextImageKHR(dev, swapchain, 1'000'000'000ull, acquired, VK_NULL_HANDLE,
                                                        &index);
            if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) {
                stop = fmt::format("vkAcquireNextImageKHR returned {}", static_cast<int>(ar));
                break;
            }
            const float pulse = 0.6f + 0.4f * static_cast<float>(std::sin(t * 3.0));
            const VkClearColorValue color{.float32 = {(rgba & 0xFF) / 255.0f * pulse,
                                                      ((rgba >> 8) & 0xFF) / 255.0f * pulse,
                                                      ((rgba >> 16) & 0xFF) / 255.0f * pulse, 1.0f}};
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            f.vkResetCommandBuffer(cb, 0);
            const VkCommandBufferBeginInfo bi{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                              .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            f.vkBeginCommandBuffer(cb, &bi);
            VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .srcAccessMask = 0,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = images[index],
                                   .subresourceRange = range};
            f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                                   0, nullptr, 1, &b);
            f.vkCmdClearColorImage(cb, images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = 0;
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            f.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                                   nullptr, 0, nullptr, 1, &b);
            f.vkEndCommandBuffer(cb);
            const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            const VkSubmitInfo sub{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                   .waitSemaphoreCount = 1,
                                   .pWaitSemaphores = &acquired,
                                   .pWaitDstStageMask = &wait_stage,
                                   .commandBufferCount = 1,
                                   .pCommandBuffers = &cb,
                                   .signalSemaphoreCount = 1,
                                   .pSignalSemaphores = &rendered[index]};
            if (f.vkQueueSubmit(m.graphics, 1, &sub, fence) != VK_SUCCESS) {
                stop = "vkQueueSubmit failed";
                break;
            }
            const VkPresentInfoKHR pres{.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                        .waitSemaphoreCount = 1,
                                        .pWaitSemaphores = &rendered[index],
                                        .swapchainCount = 1,
                                        .pSwapchains = &swapchain,
                                        .pImageIndices = &index};
            const VkResult pr = f.vkQueuePresentKHR(m.graphics, &pres);
            f.vkWaitForFences(dev, 1, &fence, VK_TRUE, 5'000'000'000ull);
            f.vkResetFences(dev, 1, &fence);
            if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) {
                stop = fmt::format("vkQueuePresentKHR returned {}", static_cast<int>(pr));
                break;
            }
            ++frames;
        }
        ok = frames > 0 && clean;
        detail = fmt::format("window 960x540, swapchain {} images {}x{} ({}), FIFO, {} frames presented, stopped: {}",
                             ni, extent.width, extent.height, format_text, frames, stop);
    } while (false);
    if (dev != VK_NULL_HANDLE) {
        f.vkDeviceWaitIdle(dev);
    }
    for (VkSemaphore s : rendered) {
        f.vkDestroySemaphore(dev, s, nullptr);
        Track(-1);
    }
    if (acquired != VK_NULL_HANDLE) {
        f.vkDestroySemaphore(dev, acquired, nullptr);
        Track(-1);
    }
    if (fence != VK_NULL_HANDLE) {
        f.vkDestroyFence(dev, fence, nullptr);
        Track(-1);
    }
    if (pool != VK_NULL_HANDLE) {
        f.vkDestroyCommandPool(dev, pool, nullptr);
        Track(-1);
    }
    if (swapchain != VK_NULL_HANDLE) {
        f.vkDestroySwapchainKHR(dev, swapchain, nullptr);
        Track(-1);
    }
    if (surface != VK_NULL_HANDLE) {
        f.vkDestroySurfaceKHR(m.instance, surface, nullptr);
        Track(-1);
    }
    if (IsWindow(hwnd)) {
        DestroyWindow(hwnd);
    }
    return ok;
}

s64 LiveObjects() {
    return g_live.load(std::memory_order_relaxed);
}

void Foundation::QueryShaderProfile(Gpu::Shader::DeviceProfile& out) const {
    impl_->QueryProfile(out);
}

void Foundation::CreateShaderModules(const std::vector<std::vector<u32>>& spirv, u32& accepted, u32& refused,
                                     std::string& detail) {
    impl_->CreateModules(spirv, accepted, refused, detail);
}

bool QueryDeviceProfile(s32 device_index, Gpu::Shader::DeviceProfile& out, std::string& error) {
    Options options;
    options.device_index = device_index;
    Report report;
    const auto foundation = Foundation::Create(options, report, error);
    if (foundation == nullptr) {
        return false;
    }
    foundation->QueryShaderProfile(out);
    return true;
}

bool CheckShaderModules(const std::vector<std::vector<u32>>& spirv, u32& accepted, u32& refused, std::string& detail) {
    Options options;
    Report report;
    const auto foundation = Foundation::Create(options, report, detail);
    if (foundation == nullptr) {
        return false;
    }
    foundation->CreateShaderModules(spirv, accepted, refused, detail);
    return true;
}

int RunProbe(const std::filesystem::path& logs_dir, s32 device_index, u32 window_seconds, bool validation) {
    std::string out = "SecondSonPC GPU probe (P4.1.4): SecondSonPC's own Vulkan foundation, on its own (no game).\n"
                      "Levels: A = device and queues created, resources used and destroyed; B = a test window cleared "
                      "to a diagnostic colour. Neither executes a GPU command of the game nor shows a game frame.\n\n";
    Report report;
    std::string error;
    const Options options{.device_index = device_index, .validation = validation,
                          .presentation = window_seconds > 0};
    const auto t0 = std::chrono::steady_clock::now();
    auto foundation = Foundation::Create(options, report, error);
    const double create_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    out += fmt::format("Loader: {} (instance API {})\n", report.loader.empty() ? "(not found)" : report.loader,
                       Version(report.loader_api));
    out += fmt::format("Instance extensions: {}; layers: {}\n", fmt::join(report.instance_extensions, ", "),
                       report.layers.empty() ? "none" : fmt::format("{}", fmt::join(report.layers, ", ")));
    for (const auto& d : report.devices) {
        out += fmt::format("Device {}: {} ({}, vendor {:#06x} device {:#06x}, driver {:#x}, API {}, {} MiB "
                           "device-local) — {}\n",
                           d.index, d.name, d.type, d.vendor_id, d.device_id, d.driver_version,
                           Version(d.api_version), d.device_local_bytes >> 20,
                           d.suitable ? "suitable" : "NOT suitable");
        if (!d.missing.empty()) {
            out += fmt::format("  missing: {}\n", fmt::join(d.missing, ", "));
        }
        out += fmt::format("  optional present: {}\n", d.optional.empty() ? "none"
                                                                           : fmt::format("{}", fmt::join(d.optional, ", ")));
    }
    bool level_a = false;
    bool level_b = window_seconds == 0;
    if (foundation) {
        out += fmt::format("Selected: device {} ({}); queues: {}; enabled device extensions: {}; created in {:.0f} ms\n",
                           report.selected, foundation->Selected().name, fmt::join(report.queues, ", "),
                           fmt::join(report.enabled_device_extensions, ", "), create_ms);
        std::string detail;
        const bool res = foundation->CheckResources(0xFF20C0F0u, detail);
        out += fmt::format("Resources: {} — {}\n", res ? "PASS" : "FAIL", detail);
        level_a = res;
        if (window_seconds > 0) {
            u32 frames = 0;
            level_b = foundation->ShowTestWindow(window_seconds, 0xFFFF00FFu, frames, detail);
            out += fmt::format("Test window (level B): {} — {}\n", level_b ? "PASS" : "FAIL", detail);
        }
        foundation.reset();
    } else {
        out += fmt::format("Level A: FAIL — {}\n", error);
    }
    out += fmt::format("Shutdown: live Vulkan objects of SecondSonPC after it: {} ({})\n", LiveObjects(),
                       LiveObjects() == 0 ? "none left" : "LEAK");
    const bool pass = level_a && level_b && LiveObjects() == 0;
    out += fmt::format("RESULT: level A {}, level B {} - {}\n", level_a ? "PASS" : "FAIL",
                       window_seconds == 0 ? "not asked" : (level_b ? "PASS" : "FAIL"), pass ? "PASS" : "FAIL");
    std::error_code ec;
    std::filesystem::create_directories(logs_dir, ec);
    std::ofstream file(logs_dir / "gpu_probe.txt", std::ios::binary);
    file << out;
    const bool written = static_cast<bool>(file);
    std::fwrite(out.data(), 1, out.size(), stdout);
    SSPC_LOG(GPU, Info, "--gpu-probe: {}{}", pass ? "PASS" : "FAIL",
             written ? "" : " (logs\\gpu_probe.txt could not be written)");
    return pass && written ? 0 : 1;
}

} // namespace SecondSon::Gpu::Vk
