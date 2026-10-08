// SecondSonPC — SSPC-001 diagnostics (see gpu_alloc_diag.h). Temporary, Phase 0-4.
#include "bridge/shadps4/gpu_alloc_diag.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "video_core/renderer_vulkan/vk_common.h"

#include <vk_mem_alloc.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "base/log.h"
#include "base/vram_monitor.h"

namespace SecondSon::GpuDiag {

namespace {

using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

// Who owns a VkDeviceMemory. Every shadPS4 Buffer has eShaderDeviceAddress usage and is therefore a
// dedicated VMA allocation; the category of a dedicated buffer comes from the thread's ContextScope.
enum Cat : u8 {
    kArenaRaw,       // vkAllocateMemory through the vulkan-hpp dispatcher (sparse buffer arena backing)
    kVmaBlock,       // VMA memory block (sub-allocated images and buffers)
    kImage,          // VMA dedicated image
    kBufDeviceLocal, // VMA dedicated buffer by shadPS4 MemoryType (ring/stream/named buffers)
    kBufHostUncached,
    kBufHostCached,
    kBufStream,
    kStagingDeviceLocal, // "StagingBufferPool:Dedicated" (large staging buffers > 16 MiB)
    kStagingHostUncached,
    kStagingHostCached,
    kStagingStream,
    kBufOther, // dedicated buffer without a context (not created through VideoCore::Buffer)
    kNumCats,
};

constexpr std::array<const char*, kNumCats> kCatNames = {
    "arena(raw)",            "vma-block",           "image(dedicated)", "buf-DeviceLocal",
    "buf-HostUncached",      "buf-HostCached",      "buf-Stream",       "stagingL-DeviceLocal",
    "stagingL-HostUncached", "stagingL-HostCached", "stagingL-Stream",  "buf-other",
};

constexpr std::array<const char*, 5> kMemTypeNames = {"DeviceLocal", "HostUncached", "HostCached",
                                                      "Stream", "Sparse"};

const char* MemTypeName(u32 mem_type) {
    return mem_type < kMemTypeNames.size() ? kMemTypeNames[mem_type] : "?";
}

enum class Kind : u8 { AllocOk, AllocFail, Free, CreateBufferFail, CreateImageFail, BindFail };

constexpr std::array<const char*, 6> kKindNames = {"alloc",          "ALLOC-FAIL",
                                                   "free",           "CREATEBUF-FAIL",
                                                   "CREATEIMG-FAIL", "BIND-FAIL"};

struct Context {
    bool active = false;
    bool is_image = false;
    u64 addr = 0;
    u64 size = 0;
    u32 mem_type = 0;
    std::string_view name;
};

struct FailedCall {
    const char* function = nullptr;
    VkResult result = VK_SUCCESS;
    u64 size = 0;
    u32 type = 0;
    s64 t_us = 0;
};

// BufferCache request on this thread: [0] guest address, [1] size, [2] bit 0 active, bit 1 written,
// bit 2 texel buffer.
struct Request {
    u64 addr = 0;
    u64 size = 0;
    u64 flags = 0;
};

constexpr u64 kLargeAllocation = 128ull << 20; // logged individually, with request and call stack

thread_local Context t_ctx;
thread_local Request t_req;
thread_local u64 t_fail_seq = 0;
thread_local FailedCall t_last_fail;

struct Live {
    u64 size;
    u32 type;
    Cat cat;
    s64 t_us;
};

struct Event {
    s64 t_us;
    u32 tid;
    u32 type;
    u64 size;
    s32 result;
    Cat cat;
    Kind kind;
};

constexpr size_t kRingSize = 4096;

using PerHeapCats = std::array<std::array<u64, kNumCats>, VK_MAX_MEMORY_HEAPS>;
using PerHeap = std::array<u64, VK_MAX_MEMORY_HEAPS>;

struct State {
    bool installed = false;
    bool budget_ext = false;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties props{};
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();

    PFN_vkAllocateMemory alloc = nullptr;
    PFN_vkFreeMemory free_memory = nullptr;
    PFN_vkCreateBuffer create_buffer = nullptr;
    PFN_vkCreateImage create_image = nullptr;
    PFN_vkBindBufferMemory bind_buffer = nullptr;
    PFN_vkBindImageMemory bind_image = nullptr;
    PFN_vkBindBufferMemory2 bind_buffer2 = nullptr;
    PFN_vkBindImageMemory2 bind_image2 = nullptr;

    std::timed_mutex mutex;
    std::unordered_map<VkDeviceMemory, Live> live;
    PerHeapCats live_bytes{};
    PerHeapCats live_count{};
    PerHeap heap_live{};
    PerHeap heap_peak{};
    u64 allocs = 0;
    u64 frees = 0;
    u64 failures = 0;
    std::array<Event, kRingSize> ring{};
    u64 ring_pos = 0;
};

State& S() {
    static State state;
    return state;
}

s64 NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                                 S().t0)
        .count();
}

double Seconds(s64 t_us) {
    return static_cast<double>(t_us) / 1e6;
}

double MiB(u64 bytes) {
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

u32 HeapOf(u32 type) {
    return type < S().props.memoryTypeCount ? S().props.memoryTypes[type].heapIndex : 0;
}

std::string ThreadName() {
    using GetThreadDescriptionFn = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_description = reinterpret_cast<GetThreadDescriptionFn>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
    if (get_description == nullptr) {
        return {};
    }
    PWSTR wide = nullptr;
    if (FAILED(get_description(GetCurrentThread(), &wide)) || wide == nullptr) {
        return {};
    }
    std::string name;
    const int length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (length > 1) {
        name.resize(static_cast<size_t>(length - 1));
        WideCharToMultiByte(CP_UTF8, 0, wide, -1, name.data(), length, nullptr, nullptr);
    }
    LocalFree(wide);
    return name;
}

// Caller holds the mutex.
void PushEvent(Kind kind, Cat cat, u32 type, u64 size, VkResult result) {
    auto& s = S();
    s.ring[s.ring_pos++ % kRingSize] = Event{
        .t_us = NowUs(),
        .tid = static_cast<u32>(GetCurrentThreadId()),
        .type = type,
        .size = size,
        .result = static_cast<s32>(result),
        .cat = cat,
        .kind = kind,
    };
}

void NoteFailure(Kind kind, const char* function, VkResult result, u64 size, u32 type) {
    ++t_fail_seq;
    t_last_fail = FailedCall{function, result, size, type, NowUs()};
    std::lock_guard lock{S().mutex};
    ++S().failures;
    PushEvent(kind, kNumCats, type, size, result);
}

Cat ClassifyVma(const VkMemoryAllocateInfo* info) {
    for (auto* next = static_cast<const VkBaseInStructure*>(info->pNext); next != nullptr;
         next = next->pNext) {
        if (next->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO) {
            continue;
        }
        const auto* dedicated = reinterpret_cast<const VkMemoryDedicatedAllocateInfo*>(next);
        if (dedicated->image != VK_NULL_HANDLE) {
            return kImage;
        }
        if (dedicated->buffer == VK_NULL_HANDLE || !t_ctx.active || t_ctx.is_image ||
            t_ctx.mem_type > 3) {
            return kBufOther;
        }
        const bool staging = t_ctx.name.starts_with("StagingBufferPool");
        return static_cast<Cat>((staging ? kStagingDeviceLocal : kBufDeviceLocal) + t_ctx.mem_type);
    }
    return kVmaBlock;
}

// Return addresses inside SecondSonPC.exe as exe+offset (symbolize with the PDB in build\bin).
std::string CallStack() {
    static const u64 base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
    static const u64 image_size = [] {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        return static_cast<u64>(nt->OptionalHeader.SizeOfImage);
    }();
    void* frames[24]{};
    const USHORT count = RtlCaptureStackBackTrace(2, 24, frames, nullptr);
    std::string out;
    for (USHORT i = 0; i < count; ++i) {
        const u64 address = reinterpret_cast<u64>(frames[i]);
        if (address >= base && address < base + image_size) {
            out += fmt::format(" exe+{:#x}", address - base);
        }
    }
    return out;
}

void LogLargeAllocation(bool raw, Cat cat, const VkMemoryAllocateInfo* info, VkResult result) {
    std::string request = "none";
    if (t_req.flags & 1) {
        request = fmt::format("guest VA {:#x}..{:#x} ({:.2f} MiB){}{}", t_req.addr,
                              t_req.addr + t_req.size, MiB(t_req.size),
                              (t_req.flags & 2) ? " written" : "", (t_req.flags & 4) ? " texel" : "");
    }
    std::string context = "none";
    if (t_ctx.active) {
        context = fmt::format("{} {:#x}+{:#x} '{}'", t_ctx.is_image ? "image" : MemTypeName(t_ctx.mem_type),
                              t_ctx.addr, t_ctx.size, t_ctx.name);
    }
    SSPC_LOG(GPU, Warning,
             "large vkAllocateMemory {} at t={:.3f} s on '{}': {:.2f} MiB {} type {} heap {} -> {}; "
             "buffer-cache request: {}; object: {}; stack:{}",
             raw ? "(dispatcher)" : "(VMA)", Seconds(NowUs()), ThreadName(), MiB(info->allocationSize),
             kCatNames[cat], info->memoryTypeIndex, HeapOf(info->memoryTypeIndex),
             vk::to_string(vk::Result{result}), request, context, CallStack());
}

VkResult AllocCommon(bool raw, VkDevice device, const VkMemoryAllocateInfo* info,
                     const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    auto& s = S();
    const VkResult result = s.alloc(device, info, callbacks, memory);
    const Cat cat = raw ? kArenaRaw : ClassifyVma(info);
    if (info->allocationSize >= kLargeAllocation) {
        LogLargeAllocation(raw, cat, info, result);
    }
    if (result != VK_SUCCESS) {
        ++t_fail_seq;
        t_last_fail = FailedCall{raw ? "vkAllocateMemory (dispatcher, sparse arena)"
                                     : "vkAllocateMemory (VMA)",
                                 result, info->allocationSize, info->memoryTypeIndex, NowUs()};
    }
    std::lock_guard lock{s.mutex};
    if (result == VK_SUCCESS) {
        const u32 heap = HeapOf(info->memoryTypeIndex);
        s.live.emplace(*memory, Live{info->allocationSize, info->memoryTypeIndex, cat, NowUs()});
        s.live_bytes[heap][cat] += info->allocationSize;
        ++s.live_count[heap][cat];
        s.heap_live[heap] += info->allocationSize;
        s.heap_peak[heap] = std::max(s.heap_peak[heap], s.heap_live[heap]);
        ++s.allocs;
        PushEvent(Kind::AllocOk, cat, info->memoryTypeIndex, info->allocationSize, result);
    } else {
        ++s.failures;
        PushEvent(Kind::AllocFail, cat, info->memoryTypeIndex, info->allocationSize, result);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL AllocVma(VkDevice device, const VkMemoryAllocateInfo* info,
                                        const VkAllocationCallbacks* callbacks,
                                        VkDeviceMemory* memory) {
    return AllocCommon(false, device, info, callbacks, memory);
}

VKAPI_ATTR VkResult VKAPI_CALL AllocRaw(VkDevice device, const VkMemoryAllocateInfo* info,
                                        const VkAllocationCallbacks* callbacks,
                                        VkDeviceMemory* memory) {
    return AllocCommon(true, device, info, callbacks, memory);
}

VKAPI_ATTR void VKAPI_CALL FreeMemory(VkDevice device, VkDeviceMemory memory,
                                      const VkAllocationCallbacks* callbacks) {
    auto& s = S();
    if (memory != VK_NULL_HANDLE) {
        std::lock_guard lock{s.mutex};
        if (const auto it = s.live.find(memory); it != s.live.end()) {
            const u32 heap = HeapOf(it->second.type);
            s.live_bytes[heap][it->second.cat] -= it->second.size;
            --s.live_count[heap][it->second.cat];
            s.heap_live[heap] -= it->second.size;
            ++s.frees;
            PushEvent(Kind::Free, it->second.cat, it->second.type, it->second.size, VK_SUCCESS);
            s.live.erase(it);
        }
    }
    s.free_memory(device, memory, callbacks);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateBuffer(VkDevice device, const VkBufferCreateInfo* info,
                                            const VkAllocationCallbacks* callbacks,
                                            VkBuffer* buffer) {
    const VkResult result = S().create_buffer(device, info, callbacks, buffer);
    if (result != VK_SUCCESS) {
        NoteFailure(Kind::CreateBufferFail, "vkCreateBuffer", result, info->size, 0);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device, const VkImageCreateInfo* info,
                                           const VkAllocationCallbacks* callbacks, VkImage* image) {
    const VkResult result = S().create_image(device, info, callbacks, image);
    if (result != VK_SUCCESS) {
        NoteFailure(Kind::CreateImageFail, "vkCreateImage", result, 0, 0);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindBuffer(VkDevice device, VkBuffer buffer, VkDeviceMemory memory,
                                          VkDeviceSize offset) {
    const VkResult result = S().bind_buffer(device, buffer, memory, offset);
    if (result != VK_SUCCESS) {
        NoteFailure(Kind::BindFail, "vkBindBufferMemory", result, offset, 0);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindImage(VkDevice device, VkImage image, VkDeviceMemory memory,
                                         VkDeviceSize offset) {
    const VkResult result = S().bind_image(device, image, memory, offset);
    if (result != VK_SUCCESS) {
        NoteFailure(Kind::BindFail, "vkBindImageMemory", result, offset, 0);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindBuffer2(VkDevice device, u32 count,
                                           const VkBindBufferMemoryInfo* infos) {
    const VkResult result = S().bind_buffer2(device, count, infos);
    if (result != VK_SUCCESS) {
        NoteFailure(Kind::BindFail, "vkBindBufferMemory2", result, count, 0);
    }
    return result;
}

VKAPI_ATTR VkResult VKAPI_CALL BindImage2(VkDevice device, u32 count,
                                          const VkBindImageMemoryInfo* infos) {
    const VkResult result = S().bind_image2(device, count, infos);
    if (result != VK_SUCCESS) {
        NoteFailure(Kind::BindFail, "vkBindImageMemory2", result, count, 0);
    }
    return result;
}

const char* HeapFlags(VkMemoryHeapFlags flags) {
    return (flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "DEVICE_LOCAL" : "host";
}

std::string TypeFlags(VkMemoryPropertyFlags flags) {
    std::string out;
    const auto add = [&](VkMemoryPropertyFlags bit, const char* name) {
        if (flags & bit) {
            out += out.empty() ? "" : "|";
            out += name;
        }
    };
    add(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, "DL");
    add(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, "HV");
    add(VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, "HC");
    add(VK_MEMORY_PROPERTY_HOST_CACHED_BIT, "HCa");
    return out.empty() ? "-" : out;
}

struct DriverBudget {
    bool valid = false;
    PerHeap usage{};
    PerHeap budget{};
};

DriverBudget QueryDriverBudget() {
    DriverBudget out;
    auto& s = S();
    if (!s.budget_ext ||
        VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties2 == nullptr) {
        return out;
    }
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, .pNext = &budget};
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceMemoryProperties2(s.physical_device, &props);
    for (u32 i = 0; i < s.props.memoryHeapCount; ++i) {
        out.usage[i] = budget.heapUsage[i];
        out.budget[i] = budget.heapBudget[i];
    }
    out.valid = true;
    return out;
}

void LogHeapState(const char* reason, bool have_ledger, const PerHeapCats& bytes,
                  const PerHeapCats& counts, const PerHeap& heap_live, const PerHeap& heap_peak) {
    auto& s = S();
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> vma{};
    if (s.allocator != VK_NULL_HANDLE) {
        vmaGetHeapBudgets(s.allocator, vma.data());
    }
    const DriverBudget driver = QueryDriverBudget();
    for (u32 heap = 0; heap < s.props.memoryHeapCount; ++heap) {
        std::string parts = have_ledger ? "" : "busy";
        for (u32 cat = 0; have_ledger && cat < kNumCats; ++cat) {
            if (counts[heap][cat] == 0) {
                continue;
            }
            parts += fmt::format("{}{} {:.1f} MiB/{}", parts.empty() ? "" : ", ", kCatNames[cat],
                                 MiB(bytes[heap][cat]), counts[heap][cat]);
        }
        SSPC_LOG(GPU, Info,
                 "  {} heap{} ({} {:.0f} MiB): ledger live {:.1f} MiB (peak {:.1f}) [{}]; VMA "
                 "usage {:.1f} / budget {:.1f} MiB (blocks {:.1f} MiB/{}, allocations {}); driver "
                 "usage {} / budget {} MiB",
                 reason, heap, HeapFlags(s.props.memoryHeaps[heap].flags),
                 MiB(s.props.memoryHeaps[heap].size), MiB(heap_live[heap]), MiB(heap_peak[heap]),
                 parts, MiB(vma[heap].usage), MiB(vma[heap].budget),
                 MiB(vma[heap].statistics.blockBytes), vma[heap].statistics.blockCount,
                 vma[heap].statistics.allocationCount,
                 driver.valid ? fmt::format("{:.1f}", MiB(driver.usage[heap])) : "n/a",
                 driver.valid ? fmt::format("{:.1f}", MiB(driver.budget[heap])) : "n/a");
    }
}

void LogSummaryImpl(const char* reason, bool with_events) {
    auto& s = S();
    if (!s.installed) {
        return;
    }
    PerHeapCats bytes{};
    PerHeapCats counts{};
    PerHeap heap_live{};
    PerHeap heap_peak{};
    u64 allocs = 0, frees = 0, failures = 0, live_count = 0;
    std::vector<Event> events;
    std::vector<Live> top;
    // A crash handler may run while the faulting thread holds the mutex: never block for long.
    std::unique_lock lock{s.mutex, std::defer_lock};
    const bool locked = lock.try_lock_for(std::chrono::milliseconds(200));
    if (locked) {
        bytes = s.live_bytes;
        counts = s.live_count;
        heap_live = s.heap_live;
        heap_peak = s.heap_peak;
        allocs = s.allocs;
        frees = s.frees;
        failures = s.failures;
        live_count = s.live.size();
        if (with_events) {
            const u64 n = std::min<u64>(s.ring_pos, 48);
            for (u64 i = s.ring_pos - n; i < s.ring_pos; ++i) {
                events.push_back(s.ring[i % kRingSize]);
            }
            top.reserve(s.live.size());
            for (const auto& [memory, live] : s.live) {
                top.push_back(live);
            }
        }
        lock.unlock();
    }
    SSPC_LOG(GPU, Info,
             "GPU alloc ledger {} (t={:.3f} s): vkAllocateMemory ok {} / freed {} / live {} / "
             "failed Vulkan calls {}{}",
             reason, Seconds(NowUs()), allocs, frees, live_count, failures,
             locked ? "" : " (ledger busy, counters not read)");
    LogHeapState(reason, locked, bytes, counts, heap_live, heap_peak);
    if (!with_events || !locked) {
        return;
    }
    SSPC_LOG(GPU, Info, "  last {} memory events (oldest first):", events.size());
    for (const Event& e : events) {
        SSPC_LOG(GPU, Info, "    t={:.3f} s tid {} {} {} type {} heap {} size {:.2f} MiB result {}",
                 Seconds(e.t_us), e.tid, kKindNames[static_cast<u8>(e.kind)],
                 e.cat < kNumCats ? kCatNames[e.cat] : "-", e.type, HeapOf(e.type), MiB(e.size),
                 e.result);
    }
    std::ranges::sort(top, [](const Live& a, const Live& b) { return a.size > b.size; });
    const size_t n = std::min<size_t>(top.size(), 16);
    SSPC_LOG(GPU, Info, "  largest {} live allocations:", n);
    for (size_t i = 0; i < n; ++i) {
        SSPC_LOG(GPU, Info, "    {:.2f} MiB {} type {} heap {} allocated at t={:.3f} s",
                 MiB(top[i].size), kCatNames[top[i].cat], top[i].type, HeapOf(top[i].type),
                 Seconds(top[i].t_us));
    }
}

void LogFailingCall(u64 failure_seq_before) {
    if (t_fail_seq == failure_seq_before) {
        SSPC_LOG(GPU, Error,
                 "  failing Vulkan call: none — every Vulkan call made inside this vmaCreate* "
                 "succeeded, so VMA refused the request itself (VMA_ALLOCATION_CREATE_WITHIN_"
                 "BUDGET_BIT budget check)");
        return;
    }
    const FailedCall& f = t_last_fail;
    SSPC_LOG(GPU, Error,
             "  failing Vulkan call: {} -> {} (size {} = {:.2f} MiB, memoryTypeIndex {}, heap {}, "
             "t={:.3f} s); failed Vulkan calls on this thread inside vmaCreate*: {}",
             f.function, vk::to_string(vk::Result{f.result}), f.size, MiB(f.size), f.type,
             HeapOf(f.type), Seconds(f.t_us), t_fail_seq - failure_seq_before);
}

// The memory types VMA tries for this request, in VMA's order, with the VMA budget of each heap.
template <typename FindFn>
void LogCandidates(const VmaAllocationCreateInfo& alloc_ci, u64 request_size, FindFn&& find) {
    auto& s = S();
    std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> vma{};
    vmaGetHeapBudgets(s.allocator, vma.data());
    VmaAllocationCreateInfo probe = alloc_ci;
    u32 bits = alloc_ci.memoryTypeBits != 0 ? alloc_ci.memoryTypeBits : ~0u;
    for (u32 i = 0; i < VK_MAX_MEMORY_TYPES && bits != 0; ++i) {
        probe.memoryTypeBits = bits;
        u32 type = 0;
        if (find(probe, &type) != VK_SUCCESS) {
            break;
        }
        const u32 heap = HeapOf(type);
        const bool over = vma[heap].usage + request_size > vma[heap].budget;
        SSPC_LOG(GPU, Error,
                 "  VMA candidate #{}: memoryTypeIndex {} ({}) heapIndex {} heapSize {:.1f} MiB; "
                 "VMA usage {:.1f} + request {:.1f} = {:.1f} MiB vs VMA budget {:.1f} MiB -> {}",
                 i, type, TypeFlags(s.props.memoryTypes[type].propertyFlags), heap,
                 MiB(s.props.memoryHeaps[heap].size), MiB(vma[heap].usage), MiB(request_size),
                 MiB(vma[heap].usage + request_size), MiB(vma[heap].budget),
                 over ? "OVER (refused with WITHIN_BUDGET)" : "fits");
        bits &= ~(1u << type);
    }
}

void LogContext() {
    const Context& c = t_ctx;
    if (!c.active) {
        SSPC_LOG(GPU, Error, "  guest context: none");
        return;
    }
    SSPC_LOG(GPU, Error,
             "  guest context: {} guest VA {:#x}..{:#x} (size {:#x} = {:.2f} MiB), MemoryType {}, "
             "name '{}'",
             c.is_image ? "image" : "buffer", c.addr, c.addr + c.size, c.size, MiB(c.size),
             c.is_image ? "-" : MemTypeName(c.mem_type), c.name);
}

void LogFailureHeader(const char* what, VkResult result) {
    SSPC_LOG(GPU, Error, "SSPC-001 allocation failure: {} -> {} on thread {} '{}' at t={:.3f} s",
             what, vk::to_string(vk::Result{result}), GetCurrentThreadId(), ThreadName(),
             Seconds(NowUs()));
}

void LogRequirements(const VkMemoryRequirements2& reqs, const VkMemoryDedicatedRequirements& ded) {
    SSPC_LOG(GPU, Error,
             "  VkMemoryRequirements: size {} ({:.2f} MiB) alignment {:#x} memoryTypeBits {:#x}; "
             "prefersDedicatedAllocation {} requiresDedicatedAllocation {}",
             reqs.memoryRequirements.size, MiB(reqs.memoryRequirements.size),
             reqs.memoryRequirements.alignment, reqs.memoryRequirements.memoryTypeBits,
             ded.prefersDedicatedAllocation, ded.requiresDedicatedAllocation);
}

void LogProbe(VkResult probe_result, const VmaAllocationInfo& info, u64 seq_before) {
    if (probe_result == VK_SUCCESS) {
        SSPC_LOG(GPU, Error,
                 "  probe without WITHIN_BUDGET: VK_SUCCESS — driver allocated {:.2f} MiB in "
                 "memoryTypeIndex {} (heap {}); destroyed immediately",
                 MiB(info.size), info.memoryType, HeapOf(info.memoryType));
        return;
    }
    SSPC_LOG(GPU, Error, "  probe without WITHIN_BUDGET: {}", vk::to_string(vk::Result{probe_result}));
    LogFailingCall(seq_before);
}

} // namespace

ContextScope::ContextScope(bool is_image, u64 guest_addr, u64 guest_size, u32 mem_type,
                           std::string_view name) {
    t_ctx = Context{true, is_image, guest_addr, guest_size, mem_type, name};
}

ContextScope::~ContextScope() {
    t_ctx = Context{};
}

RequestScope::RequestScope(u64 guest_addr, u64 size, bool is_written, bool is_texel)
    : saved_{t_req.addr, t_req.size, t_req.flags} {
    t_req = Request{guest_addr, size, 1ull | (is_written ? 2ull : 0) | (is_texel ? 4ull : 0)};
}

RequestScope::~RequestScope() {
    t_req = Request{saved_[0], saved_[1], saved_[2]};
}

u64 ThreadFailureSeq() {
    return t_fail_seq;
}

void Install(VkInstance instance, VkPhysicalDevice physical_device, VkDevice device,
             VmaVulkanFunctions& functions, bool has_memory_budget) {
    (void)instance;
    auto& s = S();
    auto& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
    s.physical_device = physical_device;
    s.device = device;
    s.budget_ext = has_memory_budget;
    dispatcher.vkGetPhysicalDeviceMemoryProperties(physical_device, &s.props);

    const auto get = [&](const char* name) { return dispatcher.vkGetDeviceProcAddr(device, name); };
    s.alloc = dispatcher.vkAllocateMemory;
    s.free_memory = dispatcher.vkFreeMemory;
    s.create_buffer = reinterpret_cast<PFN_vkCreateBuffer>(get("vkCreateBuffer"));
    s.create_image = reinterpret_cast<PFN_vkCreateImage>(get("vkCreateImage"));
    s.bind_buffer = reinterpret_cast<PFN_vkBindBufferMemory>(get("vkBindBufferMemory"));
    s.bind_image = reinterpret_cast<PFN_vkBindImageMemory>(get("vkBindImageMemory"));
    s.bind_buffer2 = reinterpret_cast<PFN_vkBindBufferMemory2>(get("vkBindBufferMemory2"));
    s.bind_image2 = reinterpret_cast<PFN_vkBindImageMemory2>(get("vkBindImageMemory2"));
    if (s.alloc == nullptr || s.free_memory == nullptr || s.create_buffer == nullptr ||
        s.create_image == nullptr || s.bind_buffer == nullptr || s.bind_image == nullptr ||
        s.bind_buffer2 == nullptr || s.bind_image2 == nullptr) {
        SSPC_LOG(GPU, Warning, "GPU alloc diagnostics: Vulkan entry points missing, not installed");
        return;
    }

    // VMA: every pointer set here is used as is; the rest VMA loads itself.
    functions.vkAllocateMemory = &AllocVma;
    functions.vkFreeMemory = &FreeMemory;
    functions.vkCreateBuffer = &CreateBuffer;
    functions.vkCreateImage = &CreateImage;
    functions.vkBindBufferMemory = &BindBuffer;
    functions.vkBindImageMemory = &BindImage;
    functions.vkBindBufferMemory2KHR = &BindBuffer2;
    functions.vkBindImageMemory2KHR = &BindImage2;
    // Everything allocated through vulkan-hpp outside VMA (the sparse buffer arena backing).
    dispatcher.vkAllocateMemory = &AllocRaw;
    dispatcher.vkFreeMemory = &FreeMemory;
    s.installed = true;

    VkPhysicalDeviceMaintenance4Properties m4{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_4_PROPERTIES};
    VkPhysicalDeviceMaintenance3Properties m3{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_3_PROPERTIES, .pNext = &m4};
    VkPhysicalDeviceProperties2 props2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                       .pNext = &m3};
    dispatcher.vkGetPhysicalDeviceProperties2(physical_device, &props2);
    SSPC_LOG(GPU, Info,
             "GPU alloc diagnostics installed (SSPC-001): {} heaps, {} memory types, "
             "VK_EXT_memory_budget {} (VMA is created without EXT_MEMORY_BUDGET: VMA budget = 80% "
             "of heap size, VMA usage = its own blocks only), maxMemoryAllocationCount {}, "
             "maxMemoryAllocationSize {:.0f} MiB, maxBufferSize {:.0f} MiB",
             s.props.memoryHeapCount, s.props.memoryTypeCount,
             has_memory_budget ? "enabled" : "absent",
             props2.properties.limits.maxMemoryAllocationCount, MiB(m3.maxMemoryAllocationSize),
             MiB(m4.maxBufferSize));
    for (u32 i = 0; i < s.props.memoryHeapCount; ++i) {
        SSPC_LOG(GPU, Info, "  heap {}: {} {:.1f} MiB", i, HeapFlags(s.props.memoryHeaps[i].flags),
                 MiB(s.props.memoryHeaps[i].size));
    }
    for (u32 i = 0; i < s.props.memoryTypeCount; ++i) {
        SSPC_LOG(GPU, Info, "  type {}: heap {} {}", i, s.props.memoryTypes[i].heapIndex,
                 TypeFlags(s.props.memoryTypes[i].propertyFlags));
    }
    Base::SetVramSampleHook(&LogSummary);
}

void SetAllocator(VmaAllocator allocator) {
    S().allocator = allocator;
}

void ReportBufferFailure(VmaAllocator allocator, const VkBufferCreateInfo& buffer_ci,
                         const VmaAllocationCreateInfo& alloc_ci, VkResult result, u32 mem_type,
                         u64 failure_seq_before) {
    auto& s = S();
    LogFailureHeader("vmaCreateBuffer", result);
    SSPC_LOG(GPU, Error,
             "  VkBufferCreateInfo: size {} ({:.2f} MiB) usage {:#x} flags {:#x} sharingMode {}; "
             "shadPS4 MemoryType {}; VmaAllocationCreateInfo flags {:#x} (dedicated {}, within "
             "budget {}) usage {} requiredFlags {:#x} preferredFlags {:#x}",
             buffer_ci.size, MiB(buffer_ci.size), static_cast<u32>(buffer_ci.usage),
             static_cast<u32>(buffer_ci.flags), static_cast<u32>(buffer_ci.sharingMode),
             MemTypeName(mem_type), alloc_ci.flags,
             (alloc_ci.flags & VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT) != 0,
             (alloc_ci.flags & VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT) != 0,
             static_cast<u32>(alloc_ci.usage), alloc_ci.requiredFlags, alloc_ci.preferredFlags);
    LogContext();
    LogFailingCall(failure_seq_before);
    if (!s.installed || allocator == VK_NULL_HANDLE) {
        LogSummaryImpl("failure", true);
        Log::Flush();
        return;
    }

    VkMemoryDedicatedRequirements dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 reqs{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                               .pNext = &dedicated};
    const VkDeviceBufferMemoryRequirements req_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_BUFFER_MEMORY_REQUIREMENTS, .pCreateInfo = &buffer_ci};
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceBufferMemoryRequirements(s.device, &req_info, &reqs);
    LogRequirements(reqs, dedicated);
    LogCandidates(alloc_ci, reqs.memoryRequirements.size,
                  [&](const VmaAllocationCreateInfo& ci, u32* type) {
                      return vmaFindMemoryTypeIndexForBufferInfo(allocator, &buffer_ci, &ci, type);
                  });

    // Would the driver have satisfied the same request? Same call without the VMA budget check;
    // the buffer is destroyed at once and the original failure still goes to shadPS4's ASSERT.
    VmaAllocationCreateInfo probe = alloc_ci;
    probe.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    VkBuffer probe_buffer = VK_NULL_HANDLE;
    VmaAllocation probe_allocation = VK_NULL_HANDLE;
    VmaAllocationInfo probe_info{};
    const u64 seq = t_fail_seq;
    const VkResult probe_result = vmaCreateBuffer(allocator, &buffer_ci, &probe, &probe_buffer,
                                                  &probe_allocation, &probe_info);
    LogProbe(probe_result, probe_info, seq);
    if (probe_result == VK_SUCCESS) {
        vmaDestroyBuffer(allocator, probe_buffer, probe_allocation);
    }
    LogSummaryImpl("failure", true);
    Log::Flush();
}

void ReportImageFailure(VmaAllocator allocator, const VkImageCreateInfo& image_ci,
                        const VmaAllocationCreateInfo& alloc_ci, VkResult result,
                        u64 failure_seq_before) {
    auto& s = S();
    LogFailureHeader("vmaCreateImage", result);
    SSPC_LOG(GPU, Error,
             "  VkImageCreateInfo: type {} format {} extent {}x{}x{} mips {} layers {} samples {} "
             "tiling {} usage {:#x} flags {:#x}; VmaAllocationCreateInfo flags {:#x} usage {}",
             static_cast<u32>(image_ci.imageType), static_cast<u32>(image_ci.format),
             image_ci.extent.width, image_ci.extent.height, image_ci.extent.depth,
             image_ci.mipLevels, image_ci.arrayLayers, static_cast<u32>(image_ci.samples),
             static_cast<u32>(image_ci.tiling), static_cast<u32>(image_ci.usage),
             static_cast<u32>(image_ci.flags), alloc_ci.flags, static_cast<u32>(alloc_ci.usage));
    LogContext();
    LogFailingCall(failure_seq_before);
    if (!s.installed || allocator == VK_NULL_HANDLE) {
        LogSummaryImpl("failure", true);
        Log::Flush();
        return;
    }

    VkMemoryDedicatedRequirements dedicated{
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 reqs{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2,
                               .pNext = &dedicated};
    const VkDeviceImageMemoryRequirements req_info{
        .sType = VK_STRUCTURE_TYPE_DEVICE_IMAGE_MEMORY_REQUIREMENTS, .pCreateInfo = &image_ci};
    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceImageMemoryRequirements(s.device, &req_info, &reqs);
    LogRequirements(reqs, dedicated);
    LogCandidates(alloc_ci, reqs.memoryRequirements.size,
                  [&](const VmaAllocationCreateInfo& ci, u32* type) {
                      return vmaFindMemoryTypeIndexForImageInfo(allocator, &image_ci, &ci, type);
                  });

    VmaAllocationCreateInfo probe = alloc_ci;
    probe.flags &= ~VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT;
    VkImage probe_image = VK_NULL_HANDLE;
    VmaAllocation probe_allocation = VK_NULL_HANDLE;
    VmaAllocationInfo probe_info{};
    const u64 seq = t_fail_seq;
    const VkResult probe_result = vmaCreateImage(allocator, &image_ci, &probe, &probe_image,
                                                 &probe_allocation, &probe_info);
    LogProbe(probe_result, probe_info, seq);
    if (probe_result == VK_SUCCESS) {
        vmaDestroyImage(allocator, probe_image, probe_allocation);
    }
    LogSummaryImpl("failure", true);
    Log::Flush();
}

void LogSummary(const char* reason) {
    LogSummaryImpl(reason, std::strcmp(reason, "crash") == 0);
}

} // namespace SecondSon::GpuDiag
