// SecondSonPC — NVIDIA Reflex low latency (see vk_reflex.h, upscale/reflex.h).
#include "runtime/gpu/vulkan/vk_reflex.h"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <vector>

#include "base/log.h"
#include "runtime/gpu/upscale/upscale_settings.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Reflex {

namespace {

using Vk::Fn;

std::mutex g_mutex; // the swapchain: attached / replaced / detached while the game's thread marks and sleeps
VkDevice g_device = VK_NULL_HANDLE;
bool g_available = false;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkSemaphore g_sleep = VK_NULL_HANDLE; // a timeline semaphore vkLatencySleepNV signals when the next frame may start
u64 g_sleep_value = 0;
int g_mode = -1; // the sleep mode set on the attached swapchain (-1: none yet)

// statistics (the game's thread)
std::chrono::steady_clock::time_point g_window{};
double g_slept_ms = 0.0;
u32 g_sleeps = 0, g_frames = 0;
u64 g_last_reported_id = 0;

void Mark(VkLatencyMarkerNV marker, u64 id) {
    const VkSetLatencyMarkerInfoNV info{.sType = VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV,
                                        .presentID = id,
                                        .marker = marker};
    Fn().vkSetLatencyMarkerNV(g_device, g_swapchain, &info);
}

void SetMode(bool on) {
    const VkLatencySleepModeInfoNV mode{.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV,
                                        .lowLatencyMode = on ? VK_TRUE : VK_FALSE,
                                        .lowLatencyBoost = VK_FALSE,
                                        .minimumIntervalUs = 0};
    const VkResult r = Fn().vkSetLatencySleepModeNV(g_device, g_swapchain, &mode);
    g_mode = on ? 1 : 0;
    SSPC_LOG(GPU, Info, "[Reflex] low latency mode {} ({})", on ? "on" : "off", static_cast<int>(r));
}

/// Every 5 s: the driver's timings of the frames since the last report (under g_mutex, a swapchain attached).
void Report(bool on) {
    VkGetLatencyMarkerInfoNV gi{.sType = VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV};
    Fn().vkGetLatencyTimingsNV(g_device, g_swapchain, &gi);
    std::vector<VkLatencyTimingsFrameReportNV> t(gi.timingCount,
                                                 {.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV});
    gi.pTimings = t.data();
    if (!t.empty()) {
        Fn().vkGetLatencyTimingsNV(g_device, g_swapchain, &gi);
    }
    double gpu_sum = 0.0, present_sum = 0.0, gpu_max = 0.0;
    u32 n = 0, np = 0;
    for (u32 i = 0; i < gi.timingCount; ++i) {
        const VkLatencyTimingsFrameReportNV& f = t[i];
        if (f.presentID <= g_last_reported_id || f.simStartTimeUs == 0 || f.gpuRenderEndTimeUs <= f.simStartTimeUs) {
            continue;
        }
        const double gpu = static_cast<double>(f.gpuRenderEndTimeUs - f.simStartTimeUs) / 1000.0;
        gpu_sum += gpu;
        gpu_max = std::max(gpu_max, gpu);
        ++n;
        if (f.presentEndTimeUs > f.simStartTimeUs) {
            present_sum += static_cast<double>(f.presentEndTimeUs - f.simStartTimeUs) / 1000.0;
            ++np;
        }
        g_last_reported_id = std::max<u64>(g_last_reported_id, f.presentID);
    }
    SSPC_LOG(GPU, Info,
             "[Reflex] {}: latency from the game's frame start to its GPU end avg {:.1f} ms (max {:.1f}), to its "
             "present {:.1f} ms ({} frames timed); the game slept {:.2f} ms/frame ({} of {} frames)",
             on ? "on" : "off", n ? gpu_sum / n : 0.0, gpu_max, np ? present_sum / np : 0.0, n,
             g_frames ? g_slept_ms / g_frames : 0.0, g_sleeps, g_frames);
}

} // namespace

void Init(VkDevice device, bool available) {
    std::scoped_lock lock{g_mutex};
    g_device = device;
    g_available = available && Fn().vkSetLatencySleepModeNV != nullptr && Fn().vkLatencySleepNV != nullptr &&
                  Fn().vkSetLatencyMarkerNV != nullptr && Fn().vkGetLatencyTimingsNV != nullptr;
    if (g_available && g_sleep == VK_NULL_HANDLE) {
        const VkSemaphoreTypeCreateInfo type{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                             .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
                                             .initialValue = 0};
        const VkSemaphoreCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
        if (Fn().vkCreateSemaphore(device, &ci, nullptr, &g_sleep) != VK_SUCCESS) {
            g_available = false;
        }
    }
    SSPC_LOG(GPU, Info, "[Reflex] {}", g_available ? "available (VK_NV_low_latency2)"
                                                   : "unavailable: the device has no VK_NV_low_latency2");
}

bool Available() {
    std::scoped_lock lock{g_mutex};
    return g_available;
}

void Attach(VkSwapchainKHR swapchain) {
    std::scoped_lock lock{g_mutex};
    if (!g_available) {
        return;
    }
    g_swapchain = swapchain;
    g_mode = -1; // (a new swapchain: its mode is set at the game's next frame)
}

void Detach(VkSwapchainKHR swapchain) {
    std::scoped_lock lock{g_mutex};
    if (g_swapchain == swapchain) {
        g_swapchain = VK_NULL_HANDLE;
    }
}

bool Attached() {
    std::scoped_lock lock{g_mutex};
    return g_swapchain != VK_NULL_HANDLE;
}

void GameFrameSubmitted(u64 flip_id) {
    const bool want = Upscale::GetSettings().reflex;
    u64 wait_value = 0;
    {
        std::scoped_lock lock{g_mutex};
        if (g_swapchain == VK_NULL_HANDLE || flip_id == 0) {
            return;
        }
        if (g_mode != (want ? 1 : 0)) {
            SetMode(want);
        }
        Mark(VK_LATENCY_MARKER_SIMULATION_END_NV, flip_id);
        Mark(VK_LATENCY_MARKER_RENDERSUBMIT_START_NV, flip_id);
        if (want) {
            const VkLatencySleepInfoNV sleep{.sType = VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV,
                                             .signalSemaphore = g_sleep,
                                             .value = ++g_sleep_value};
            if (Fn().vkLatencySleepNV(g_device, g_swapchain, &sleep) == VK_SUCCESS) {
                wait_value = g_sleep_value;
            }
        }
    }
    const auto now = std::chrono::steady_clock::now();
    if (wait_value != 0) {
        // the driver signals when the next frame should start (at most 100 ms: never a stuck game)
        const VkSemaphoreWaitInfo wi{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                     .semaphoreCount = 1,
                                     .pSemaphores = &g_sleep,
                                     .pValues = &wait_value};
        Fn().vkWaitSemaphores(g_device, &wi, 100'000'000ull);
        g_slept_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - now).count();
        ++g_sleeps;
    }
    ++g_frames;
    std::scoped_lock lock{g_mutex};
    if (g_swapchain == VK_NULL_HANDLE) {
        return;
    }
    Mark(VK_LATENCY_MARKER_SIMULATION_START_NV, flip_id + 1);
    Mark(VK_LATENCY_MARKER_INPUT_SAMPLE_NV, flip_id + 1);
    if (g_window.time_since_epoch().count() == 0) {
        g_window = now;
    } else if (now - g_window >= std::chrono::seconds(5)) {
        Report(want);
        g_window = now;
        g_slept_ms = 0.0;
        g_sleeps = g_frames = 0;
    }
}

void RenderSubmitEnd(u64 flip_id) {
    std::scoped_lock lock{g_mutex};
    if (g_swapchain != VK_NULL_HANDLE && flip_id != 0) {
        Mark(VK_LATENCY_MARKER_RENDERSUBMIT_END_NV, flip_id);
    }
}

void PresentMarker(bool start, u64 flip_id) {
    std::scoped_lock lock{g_mutex};
    if (g_swapchain != VK_NULL_HANDLE && flip_id != 0) {
        Mark(start ? VK_LATENCY_MARKER_PRESENT_START_NV : VK_LATENCY_MARKER_PRESENT_END_NV, flip_id);
    }
}

const void* SubmitChain(u64 flip_id, VkLatencySubmissionPresentIdNV& storage) {
    std::scoped_lock lock{g_mutex};
    if (g_swapchain == VK_NULL_HANDLE || flip_id == 0) {
        return nullptr;
    }
    storage = {.sType = VK_STRUCTURE_TYPE_LATENCY_SUBMISSION_PRESENT_ID_NV, .presentID = flip_id};
    return &storage;
}

} // namespace SecondSon::Gpu::Reflex
