// SecondSonPC — video memory budget/usage of this process (DXGI QueryVideoMemoryInfo).
#include "base/vram_monitor.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <dxgi1_4.h>
#include <windows.h>
#include <psapi.h>

#include "base/crash.h"
#include "base/log.h"

namespace SecondSon::Base {

namespace {

IDXGIAdapter3* g_adapter = nullptr;
bool g_tried = false;
std::mutex g_adapter_mutex; ///< the first Adapter() (the log's thread and the Performance Inspector's)
std::atomic<bool> g_ready{false};
std::atomic<VramSampleHook> g_hook{nullptr};

IDXGIAdapter3* Adapter() {
    if (g_ready.load(std::memory_order_acquire)) {
        return g_adapter; // (no lock once made: the crash handler's sample)
    }
    std::scoped_lock lock{g_adapter_mutex};
    if (g_tried) {
        return g_adapter;
    }
    struct Ready {
        ~Ready() { g_ready.store(true, std::memory_order_release); }
    } ready;
    g_tried = true;
    IDXGIFactory4* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory)))) {
        return nullptr;
    }
    // The adapter with the most dedicated video memory (the GPU Vulkan uses on this machine).
    SIZE_T best_memory = 0;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        IDXGIAdapter3* a3 = nullptr;
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 && desc.DedicatedVideoMemory > best_memory &&
            SUCCEEDED(adapter->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&a3)))) {
            if (g_adapter != nullptr) {
                g_adapter->Release();
            }
            g_adapter = a3;
            best_memory = desc.DedicatedVideoMemory;
        }
        adapter->Release();
    }
    factory->Release();
    return g_adapter;
}

constexpr u64 kMiB = 1ull << 20;

} // namespace

void LogVramSample(const char* reason, bool allow_init) {
    if (allow_init || g_tried) {
        if (auto* adapter = Adapter(); adapter == nullptr) {
            SSPC_LOG(App, Info, "VRAM {}: no DXGI adapter", reason);
        } else {
            DXGI_QUERY_VIDEO_MEMORY_INFO local{};
            DXGI_QUERY_VIDEO_MEMORY_INFO nonlocal{};
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local);
            adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal);
            SSPC_LOG(App, Info,
                     "VRAM {}: local usage {} MiB / budget {} MiB (available for reservation {} MiB), "
                     "non-local usage {} MiB / budget {} MiB",
                     reason, local.CurrentUsage / kMiB, local.Budget / kMiB,
                     local.AvailableForReservation / kMiB, nonlocal.CurrentUsage / kMiB,
                     nonlocal.Budget / kMiB);
        }
    }
    // System commit: without a page file the commit limit equals physical RAM, and the driver fails
    // vkAllocateMemory in every heap once it is reached (SSPC-001). No allocation, safe in a crash.
    PROCESS_MEMORY_COUNTERS_EX process{};
    PERFORMANCE_INFORMATION system{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&process),
                             sizeof(process)) &&
        GetPerformanceInfo(&system, sizeof(system))) {
        const u64 page = system.PageSize;
        SSPC_LOG(App, Info,
                 "commit {}: process private {} MiB (peak {} MiB), working set {} MiB; system "
                 "committed {} / limit {} MiB (peak {} MiB), physical available {} MiB",
                 reason, process.PrivateUsage / kMiB, process.PeakPagefileUsage / kMiB,
                 process.WorkingSetSize / kMiB, system.CommitTotal * page / kMiB,
                 system.CommitLimit * page / kMiB, system.CommitPeak * page / kMiB,
                 system.PhysicalAvailable * page / kMiB);
    }
    LogFirstChanceStats(reason);
    if (const VramSampleHook hook = g_hook.load(); hook != nullptr) {
        hook(reason);
    }
}

bool QueryVideoMemory(VideoMemoryInfo& out) {
    IDXGIAdapter3* adapter = Adapter();
    if (adapter == nullptr) {
        return false;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO local{}, nonlocal{};
    if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &local)) ||
        FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &nonlocal))) {
        return false;
    }
    out = {local.CurrentUsage, local.Budget, nonlocal.CurrentUsage, nonlocal.Budget};
    return true;
}

void SetVramSampleHook(VramSampleHook hook) {
    g_hook.store(hook);
}

void StartVramLog(u32 interval_seconds) {
    if (interval_seconds == 0) {
        return;
    }
    LogVramSample("start");
    std::thread([interval_seconds] {
        for (;;) {
            Sleep(interval_seconds * 1000);
            LogVramSample("sample");
        }
    }).detach();
}

} // namespace SecondSon::Base
