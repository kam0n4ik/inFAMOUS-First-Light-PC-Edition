// SecondSonPC — libkernel direct memory (Phase 2, P2.3.1, EXPERIMENTAL; docs/PHASE2_P23_MEMORY.md).
// sceKernelGetDirectMemorySize / AllocateDirectMemory / ReleaseDirectMemory on SecondSonPC's own
// map of the physical direct memory (runtime/memory/direct_memory.h). Group "kernel-dmem": bound only
// with `--native kernel-dmem`, which also makes the bridge's memory manager account its direct
// mappings in the same map (patch 0007) — one switch, decided before anything is allocated.
// Parameter checks, their order and the error codes are the Phase 0 oracle's
// (third_party/shadPS4/src/core/libraries/kernel/memory.cpp @ 0fe263a, not verified on a PS4); the
// differences are documented in direct_memory.cpp. sceKernelMapDirectMemory stays BRIDGE: the
// virtual side (address space, GPU notifications) is the bridge's until P2.3.2.
#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"
#include "runtime/memory/direct_memory.h"

namespace SecondSon::Kernel {

namespace {

using Memory::kErrEagain;
using Memory::kErrEinval;
using Memory::kOk;
using Memory::kPageSize;

constexpr s32 kMaxMemoryType = 10; // ORBIS_KERNEL_WB_GARLIC, the last memory type

SSPC_SYSV_ABI u64 GetDirectMemorySize() {
    return Memory::GameDirectMemory().Size();
}

SSPC_SYSV_ABI s32 AllocateDirectMemory(s64 search_start, s64 search_end, u64 len, u64 alignment,
                                       s32 memory_type, s64* phys_out) {
    s32 result = kOk;
    if (search_start < 0 || search_end < 0 || len == 0 || len % kPageSize != 0 ||
        alignment % kPageSize != 0 || memory_type > kMaxMemoryType || phys_out == nullptr) {
        result = kErrEinval;
    } else if (search_end <= search_start || static_cast<u64>(search_end) < len ||
               static_cast<u64>(search_end - search_start) < len) {
        result = kErrEagain;
    }
    u64 phys = 0;
    if (result == kOk) {
        result = Memory::GameDirectMemory().Allocate(static_cast<u64>(search_start),
                                                     static_cast<u64>(search_end), len, alignment,
                                                     memory_type, &phys);
    }
    if (result != kOk) {
        SSPC_LOG(Memory, Error,
                 "sceKernelAllocateDirectMemory(search [{:#x}, {:#x}), len {:#x}, align {:#x}, type {}) "
                 "-> {:#x}",
                 search_start, search_end, len, alignment, memory_type, static_cast<u32>(result));
        return result;
    }
    *phys_out = static_cast<s64>(phys);
    SSPC_LOG(Memory, Info,
             "sceKernelAllocateDirectMemory(search [{:#x}, {:#x}), len {:#x}, align {:#x}, type {}) -> {:#x}",
             search_start, search_end, len, alignment, memory_type, phys);
    return kOk;
}

SSPC_SYSV_ABI s32 ReleaseDirectMemory(u64 start, u64 len) {
    const s32 result = Memory::GameDirectMemory().Release(start, len, false, Memory::GetUnmapper());
    if (result != kOk) {
        SSPC_LOG(Memory, Error, "sceKernelReleaseDirectMemory({:#x}, {:#x}) -> {:#x}", start, len,
                 static_cast<u32>(result));
    } else {
        SSPC_LOG(Memory, Info, "sceKernelReleaseDirectMemory({:#x}, {:#x})", start, len);
    }
    return result;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"pO96TwzOm5E", "libkernel", "libkernel", "sceKernelGetDirectMemorySize", Fn(&GetDirectMemorySize),
     nullptr, "kernel-dmem", false, true},
    {"rTXw65xmLIA", "libkernel", "libkernel", "sceKernelAllocateDirectMemory", Fn(&AllocateDirectMemory),
     nullptr, "kernel-dmem", false, true},
    {"MBuItvba6z8", "libkernel", "libkernel", "sceKernelReleaseDirectMemory", Fn(&ReleaseDirectMemory),
     nullptr, "kernel-dmem", false, true},
};

} // namespace

std::span<const Runtime::Export> DirectMemoryExports() {
    return kExports;
}

} // namespace SecondSon::Kernel
