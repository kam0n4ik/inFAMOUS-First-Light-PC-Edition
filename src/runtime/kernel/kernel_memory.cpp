// SecondSonPC — libkernel's memory functions on SecondSonPC's own memory manager (P3.5.2, EXPERIMENTAL:
// --native kernel-memory with --native memory). See kernel_memory.h.
//
// Each function is the oracle's (third_party/shadPS4/src/core/libraries/kernel/memory.cpp @ 0fe263a + patch 0012):
// the same checks in the same order with the same codes, then the manager call the bridge's adapter
// (bridge/shadps4/native_memory.cpp) makes for it. Differences, each safer and each with a self-test:
//  * a null `addr`/`addr_in_out` (MapDirectMemory, MapNamedFlexibleMemory): EINVAL; the oracle reads *addr first
//    (an access violation in host code);
//  * mprotect/mmap sizes that wrap when aligned up: EINVAL; the oracle wraps and would protect/map a small range.
// AllocateDirectMemory reports any refusal of the manager as EAGAIN, as the oracle (its adapter returns -1 for every
// error); the manager's own code (ENOMEM when the system refuses the commit, --memory-commit on-demand) goes to the
// log. ReleaseDirectMemory answers 0 whatever the release did, as the oracle (the unchecked release).
#include "runtime/kernel/kernel_memory.h"

#include <atomic>
#include <cstring>
#include <string_view>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"
#include "runtime/memory/virtual_memory.h"

namespace SecondSon::Kernel::MemoryApi {

namespace {

using Memory::kOk;
using Memory::kPageSize;
using Memory::VmaType;

constexpr s32 kErrEinval = Memory::kErrEinval;
constexpr s32 kErrEagain = Memory::kErrEagain;
constexpr s32 kErrEacces = Memory::kErrEacces;
constexpr s32 kErrEfault = static_cast<s32>(0x8002000E);
constexpr s32 kErrEnametoolong = static_cast<s32>(0x8002003F);
constexpr s32 kErrnoBase = static_cast<s32>(0x80020000); // libkernel code = 0x80020000 + errno
constexpr s32 kMaxMemoryType = 10;                        // ORBIS_KERNEL_WB_GARLIC, the last memory type
constexpr u64 kMaxNameLength = 32;                        // ORBIS_KERNEL_MAXIMUM_NAME_LENGTH (with the terminator)
constexpr u32 kSdk170 = 0x1700000;                        // Common::ElfInfo::FW_170
constexpr u32 kSdk250 = 0x2500000;                        // Common::ElfInfo::FW_250
// libkernel's map flags and protection bits (Core::MemoryMapFlags, Core::MemoryProt).
constexpr u32 kMapFixed = 0x10;
constexpr u32 kMapVoid = 0x100;
constexpr u32 kMapStack = 0x400;
constexpr u32 kMapAnon = 0x1000;
constexpr u32 kMapSystem = 0x2000;
constexpr u32 kProtCpuExec = 0x4;

void* const kMapFailed = reinterpret_cast<void*>(~uintptr_t{0});

Log::Level Outcome(s32 r) {
    return r == kOk ? Log::Level::Info : Log::Level::Error;
}

/// `value` up to a multiple of `alignment`; false when that wraps.
bool AlignUp(u64 value, u64 alignment, u64& out) {
    const u64 rest = value % alignment;
    if (rest == 0) {
        out = value;
        return true;
    }
    if (value > ~0ull - (alignment - rest)) {
        return false;
    }
    out = value + (alignment - rest);
    return true;
}

bool IsPowerOfTwo(u64 v) {
    return v != 0 && (v & (v - 1)) == 0;
}

/// Fixed at address 0: dropped before SDK 1.70, refused (false) from then on.
bool FixedAtZero(u32 sdk, u64 addr, u32& flags) {
    if ((flags & kMapFixed) != 0 && addr == 0) {
        if (sdk >= kSdk170) {
            return false;
        }
        flags &= ~kMapFixed;
    }
    return true;
}

s32 Map(Memory::VirtualMemory& vm, void** out, u64 addr, u64 size, u32 prot, u32 flags, VmaType type,
        std::string_view name, u64 phys = Memory::kNoPhys, bool exclusive = false, u64 alignment = 0) {
    u64 where = 0;
    const s32 r = vm.Map({.addr = addr,
                          .size = size,
                          .prot = prot,
                          .flags = flags,
                          .type = type,
                          .name = name,
                          .phys = phys,
                          .exclusive = exclusive,
                          .alignment = alignment},
                         &where);
    if (r == kOk) {
        *out = reinterpret_cast<void*>(where);
    }
    return r;
}

std::atomic<Api*> g_api{nullptr};

} // namespace

// ---------------------------------------------------------------------------- Api

u64 Api::GetDirectMemorySize() const {
    return vm_.DirectSize();
}

s32 Api::AllocateDirectMemory(s64 search_start, s64 search_end, u64 len, u64 alignment, s32 memory_type,
                              s64* phys_out) {
    s32 result = kOk;
    if (search_start < 0 || search_end < 0 || len == 0 || len % kPageSize != 0 || alignment % kPageSize != 0 ||
        memory_type > kMaxMemoryType || phys_out == nullptr) {
        result = kErrEinval;
    } else if (search_end <= search_start || static_cast<u64>(search_end) < len ||
               static_cast<u64>(search_end - search_start) < len) {
        result = kErrEagain;
    }
    u64 phys = 0;
    if (result == kOk) {
        const s32 r = vm_.AllocateDirect(static_cast<u64>(search_start), static_cast<u64>(search_end), len, alignment,
                                         memory_type, &phys);
        if (r != kOk) {
            SSPC_LOG(Memory, Error, "sceKernelAllocateDirectMemory: the manager refused ({:#x}), EAGAIN as the oracle",
                     static_cast<u32>(r));
            result = kErrEagain;
        }
    }
    if (result != kOk) {
        SSPC_LOG(Memory, Error,
                 "sceKernelAllocateDirectMemory(search [{:#x}, {:#x}), len {:#x}, align {:#x}, type {}) -> {:#x}",
                 search_start, search_end, len, alignment, memory_type, static_cast<u32>(result));
        return result;
    }
    *phys_out = static_cast<s64>(phys);
    SSPC_LOG(Memory, Info, "sceKernelAllocateDirectMemory(search [{:#x}, {:#x}), len {:#x}, align {:#x}, type {}) -> {:#x}",
             search_start, search_end, len, alignment, memory_type, phys);
    return kOk;
}

s32 Api::ReleaseDirectMemory(u64 start, u64 len) {
    if (start % kPageSize != 0 || len % kPageSize != 0) {
        SSPC_LOG(Memory, Error, "sceKernelReleaseDirectMemory({:#x}, {:#x}): not page aligned", start, len);
        return kErrEinval;
    }
    if (len == 0) {
        return kOk;
    }
    const s32 r = vm_.ReleaseDirect(start, len, false);
    if (r != kOk) {
        SSPC_LOG(Memory, Error, "sceKernelReleaseDirectMemory({:#x}, {:#x}): the manager answered {:#x} (unchecked: 0)",
                 start, len, static_cast<u32>(r));
    } else {
        SSPC_LOG(Memory, Info, "sceKernelReleaseDirectMemory({:#x}, {:#x})", start, len);
    }
    return kOk;
}

s32 Api::MapNamedDirectMemory(void** addr, u64 len, s32 prot, s32 flags, s64 phys, u64 alignment, const char* name) {
    if (addr == nullptr) {
        SSPC_LOG(Memory, Error, "sceKernelMapDirectMemory: no address pointer");
        return kErrEinval;
    }
    const u64 in_addr = reinterpret_cast<u64>(*addr);
    s32 result = kOk;
    if (len == 0 || len % kPageSize != 0 || static_cast<u64>(phys) % kPageSize != 0 ||
        (alignment != 0 && !IsPowerOfTwo(alignment) && alignment % kPageSize != 0)) {
        result = kErrEinval;
    } else if (std::strlen(name) >= kMaxNameLength) {
        result = kErrEnametoolong;
    } else if ((static_cast<u32>(prot) & kProtCpuExec) != 0) {
        result = kErrEacces;
    }
    u32 map_flags = static_cast<u32>(flags);
    // From SDK 2.50 a direct mapping may not alias mapped direct memory unless the game enabled aliasing
    // (sceKernelEnableDmemAliasing, which this game does not import) or maps a stack.
    const bool exclusive = sdk_ >= kSdk250 && (map_flags & kMapStack) == 0;
    if (result == kOk && !FixedAtZero(sdk_, in_addr, map_flags)) {
        result = kErrEinval;
    }
    if (result == kOk) {
        result = Map(vm_, addr, in_addr, len, static_cast<u32>(prot), map_flags, VmaType::Direct, name,
                     static_cast<u64>(phys), exclusive, alignment);
    }
    if (result != kOk) {
        SSPC_LOG(Memory, Error,
                 "sceKernelMapDirectMemory(addr {:#x}, len {:#x}, prot {:#x}, flags {:#x}, phys {:#x}, align {:#x}) -> "
                 "{:#x}",
                 in_addr, len, prot, flags, phys, alignment, static_cast<u32>(result));
        return result;
    }
    SSPC_LOG(Memory, Info,
             "sceKernelMapDirectMemory(addr {:#x}, len {:#x}, prot {:#x}, flags {:#x}, phys {:#x}, align {:#x}) -> {}",
             in_addr, len, prot, flags, phys, alignment, *addr);
    return kOk;
}

s32 Api::MapDirectMemory(void** addr, u64 len, s32 prot, s32 flags, s64 phys, u64 alignment) {
    return MapNamedDirectMemory(addr, len, prot, flags, phys, alignment, "anon");
}

s32 Api::MapNamedFlexibleMemory(void** addr, u64 len, s32 prot, s32 flags, const char* name) {
    if (addr == nullptr) {
        SSPC_LOG(Memory, Error, "sceKernelMapNamedFlexibleMemory: no address pointer");
        return kErrEinval;
    }
    const u64 in_addr = reinterpret_cast<u64>(*addr);
    s32 result = kOk;
    u32 map_flags = static_cast<u32>(flags);
    if (len == 0 || len % kPageSize != 0) {
        result = kErrEinval;
    } else if (name == nullptr) {
        result = kErrEfault;
    } else if (std::strlen(name) >= kMaxNameLength) {
        result = kErrEnametoolong;
    } else if (!FixedAtZero(sdk_, in_addr, map_flags)) {
        result = kErrEinval;
    }
    if (result == kOk) {
        result = Map(vm_, addr, in_addr, len, static_cast<u32>(prot), map_flags, VmaType::Flexible, name);
    }
    if (result != kOk) {
        SSPC_LOG(Memory, Error, "sceKernelMapNamedFlexibleMemory(addr {:#x}, len {:#x}, prot {:#x}, flags {:#x}) -> {:#x}",
                 in_addr, len, prot, flags, static_cast<u32>(result));
        return result;
    }
    SSPC_LOG(Memory, Info, "sceKernelMapNamedFlexibleMemory(addr {:#x}, len {:#x}, prot {:#x}, flags {:#x}, '{}') -> {}",
             in_addr, len, prot, flags, name, *addr);
    return kOk;
}

s32 Api::Munmap(void* addr, u64 len) {
    if (len == 0) {
        return kErrEinval;
    }
    const s32 r = vm_.Unmap(reinterpret_cast<u64>(addr), len);
    Log::Logf(Log::Category::Memory, Outcome(r), "sceKernelMunmap({}, {:#x}) -> {:#x}", addr, len, static_cast<u32>(r));
    return r;
}

s32 Api::Mprotect(const void* addr, u64 size, s32 prot) {
    // The address down and the size up to whole pages, over what the address was moved down.
    const u64 in_addr = reinterpret_cast<u64>(addr);
    const u64 aligned_addr = in_addr - in_addr % kPageSize;
    const u64 shift = in_addr - aligned_addr;
    u64 aligned_size = 0;
    if (size > ~0ull - shift || !AlignUp(size + shift, kPageSize, aligned_size)) {
        SSPC_LOG(Memory, Error, "sceKernelMprotect({}, {:#x}): the size wraps", addr, size);
        return kErrEinval;
    }
    if (aligned_size == 0) {
        return kOk; // nothing to do
    }
    const s32 r = vm_.Protect(aligned_addr, aligned_size, static_cast<u32>(prot));
    if (r == kOk) {
        vm_.NotifyHostWrite(aligned_addr, aligned_size); // the GPU drops what it cached of the range (as the oracle)
    }
    Log::Logf(Log::Category::Memory, Outcome(r), "sceKernelMprotect({}, {:#x}, {:#x}) -> {:#x}", addr, size, prot,
              static_cast<u32>(r));
    return r;
}

s32 Api::QueryMemoryProtection(void* addr, void** start, void** end, u32* prot) {
    u64 s = 0;
    u64 e = 0;
    u32 p = 0;
    const s32 r = vm_.QueryProtection(reinterpret_cast<u64>(addr), &s, &e, &p);
    if (r != kOk) {
        return r;
    }
    if (start != nullptr) {
        *start = reinterpret_cast<void*>(s);
    }
    if (end != nullptr) {
        *end = reinterpret_cast<void*>(e);
    }
    if (prot != nullptr) {
        *prot = p;
    }
    return kOk;
}

void* Api::Mmap(void* addr, u64 len, s32 prot, s32 flags, s32 fd, s64 offset, s32* error) {
    const auto fail = [error](s32 code) {
        *error = code - kErrnoBase;
        return kMapFailed;
    };
    if (len == 0) {
        SSPC_LOG(Memory, Error, "mmap({}, 0): no length", addr);
        return fail(kErrEinval);
    }
    // Address and size both up to whole pages (the oracle; a fixed address must already be aligned).
    const u64 vaddr = reinterpret_cast<u64>(addr);
    const u32 map_flags = static_cast<u32>(flags);
    u64 aligned_addr = 0;
    u64 aligned_size = 0;
    if (!AlignUp(vaddr, kPageSize, aligned_addr) || !AlignUp(len, kPageSize, aligned_size) ||
        ((map_flags & kMapFixed) != 0 && vaddr != aligned_addr)) {
        SSPC_LOG(Memory, Error, "mmap({}, {:#x}, flags {:#x}): misaligned fixed address or a size that wraps", addr, len,
                 flags);
        return fail(kErrEinval);
    }
    void* out = nullptr;
    s32 r = kOk;
    if ((map_flags & kMapAnon) != 0) {
        r = Map(vm_, &out, aligned_addr, aligned_size, static_cast<u32>(prot), map_flags,
                (map_flags & kMapSystem) != 0 ? VmaType::System : VmaType::Flexible, "anon");
    } else if ((map_flags & kMapStack) != 0) {
        r = Map(vm_, &out, aligned_addr, aligned_size, static_cast<u32>(prot), map_flags, VmaType::Stack, "anon");
    } else if ((map_flags & kMapVoid) != 0) {
        r = Map(vm_, &out, aligned_addr, aligned_size, Memory::Prot::kNone, map_flags, VmaType::Reserved, "anon");
    } else {
        // A file mapping: SecondSonPC's manager has none (the bridge's adapter refuses the same way).
        SSPC_LOG(Memory, Critical,
                 "mmap of fd {} [{:#x}, +{:#x}): file mappings are not supported by SecondSonPC's memory manager", fd,
                 offset, len);
        r = kErrEinval;
    }
    if (r != kOk) {
        SSPC_LOG(Memory, Error, "mmap({}, {:#x}, prot {:#x}, flags {:#x}, fd {}) -> {:#x}", addr, len, prot, flags, fd,
                 static_cast<u32>(r));
        return fail(r);
    }
    SSPC_LOG(Memory, Info, "mmap({}, {:#x}, prot {:#x}, flags {:#x}) -> {}", addr, len, prot, flags, out);
    return out;
}

// ---------------------------------------------------------------------------- the game's exports

namespace {

Api* Active(std::string_view function) {
    Api* const api = g_api.load(std::memory_order_acquire);
    if (api == nullptr) {
        SSPC_LOG(Memory, Critical, "{}: --native kernel-memory without SecondSonPC's memory manager", function);
    }
    return api;
}

SSPC_SYSV_ABI u64 GetDirectMemorySize() {
    Api* const a = Active("sceKernelGetDirectMemorySize");
    return a != nullptr ? a->GetDirectMemorySize() : 0;
}

SSPC_SYSV_ABI s32 AllocateDirectMemory(s64 search_start, s64 search_end, u64 len, u64 alignment, s32 memory_type,
                                       s64* phys_out) {
    Api* const a = Active("sceKernelAllocateDirectMemory");
    return a != nullptr ? a->AllocateDirectMemory(search_start, search_end, len, alignment, memory_type, phys_out)
                        : kErrEinval;
}

SSPC_SYSV_ABI s32 ReleaseDirectMemory(u64 start, u64 len) {
    Api* const a = Active("sceKernelReleaseDirectMemory");
    return a != nullptr ? a->ReleaseDirectMemory(start, len) : kErrEinval;
}

SSPC_SYSV_ABI s32 MapDirectMemory(void** addr, u64 len, s32 prot, s32 flags, s64 phys, u64 alignment) {
    Api* const a = Active("sceKernelMapDirectMemory");
    return a != nullptr ? a->MapDirectMemory(addr, len, prot, flags, phys, alignment) : kErrEinval;
}

SSPC_SYSV_ABI s32 MapNamedFlexibleMemory(void** addr, u64 len, s32 prot, s32 flags, const char* name) {
    Api* const a = Active("sceKernelMapNamedFlexibleMemory");
    return a != nullptr ? a->MapNamedFlexibleMemory(addr, len, prot, flags, name) : kErrEinval;
}

SSPC_SYSV_ABI s32 Munmap(void* addr, u64 len) {
    Api* const a = Active("sceKernelMunmap");
    return a != nullptr ? a->Munmap(addr, len) : kErrEinval;
}

SSPC_SYSV_ABI s32 Mprotect(const void* addr, u64 size, s32 prot) {
    Api* const a = Active("sceKernelMprotect");
    return a != nullptr ? a->Mprotect(addr, size, prot) : kErrEinval;
}

SSPC_SYSV_ABI s32 QueryMemoryProtection(void* addr, void** start, void** end, u32* prot) {
    Api* const a = Active("sceKernelQueryMemoryProtection");
    return a != nullptr ? a->QueryMemoryProtection(addr, start, end, prot) : kErrEinval;
}

SSPC_SYSV_ABI void* Mmap(void* addr, u64 len, s32 prot, s32 flags, s32 fd, s64 offset) {
    Api* const a = Active("mmap");
    s32 error = kErrEinval - kErrnoBase;
    void* const r = a != nullptr ? a->Mmap(addr, len, prot, flags, fd, offset, &error) : kMapFailed;
    if (r == kMapFailed) {
        if (Host* host = GetHost()) {
            *host->Errno() = error; // the guest errno's owner (the bridge's, or SecondSonPC's with --native errno)
        }
    }
    return r;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

// The game's import keys (docs/generated/imports.csv, prx_imports.csv).
const Runtime::Export kExports[] = {
    {"pO96TwzOm5E", "libkernel", "libkernel", "sceKernelGetDirectMemorySize", Fn(&GetDirectMemorySize), nullptr,
     "kernel-memory", false, true},
    {"rTXw65xmLIA", "libkernel", "libkernel", "sceKernelAllocateDirectMemory", Fn(&AllocateDirectMemory), nullptr,
     "kernel-memory", false, true},
    {"MBuItvba6z8", "libkernel", "libkernel", "sceKernelReleaseDirectMemory", Fn(&ReleaseDirectMemory), nullptr,
     "kernel-memory", false, true},
    {"L-Q3LEjIbgA", "libkernel", "libkernel", "sceKernelMapDirectMemory", Fn(&MapDirectMemory), nullptr,
     "kernel-memory", false, true},
    {"mL8NDH86iQI", "libkernel", "libkernel", "sceKernelMapNamedFlexibleMemory", Fn(&MapNamedFlexibleMemory), nullptr,
     "kernel-memory", false, true},
    {"cQke9UuBQOk", "libkernel", "libkernel", "sceKernelMunmap", Fn(&Munmap), nullptr, "kernel-memory", false, true},
    {"vSMAm3cxYTY", "libkernel", "libkernel", "sceKernelMprotect", Fn(&Mprotect), nullptr, "kernel-memory", false,
     true},
    {"WFcfL2lzido", "libkernel", "libkernel", "sceKernelQueryMemoryProtection", Fn(&QueryMemoryProtection), nullptr,
     "kernel-memory", false, true},
    {"BPE9s9vQQXo", "libkernel", "libkernel", "mmap", Fn(&Mmap), nullptr, "kernel-memory", false, true},
};

} // namespace

void Install(Memory::VirtualMemory& vm, u32 sdk_version) {
    static Api* const api = new Api(vm, sdk_version); // never destroyed: guest threads call it until the process ends
    g_api.store(api, std::memory_order_release);
    SSPC_LOG(Memory, Warning,
             "EXPERIMENTAL: libkernel's memory functions are SecondSonPC's (--native kernel-memory), on SecondSonPC's "
             "memory manager; SDK {:#x}",
             sdk_version);
}

bool Installed() {
    return g_api.load(std::memory_order_acquire) != nullptr;
}

std::span<const Runtime::Export> Exports() {
    return kExports;
}

} // namespace SecondSon::Kernel::MemoryApi
