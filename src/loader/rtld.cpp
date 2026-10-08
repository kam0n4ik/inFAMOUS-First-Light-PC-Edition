// SecondSonPC — run-time linker / process functions of libkernel (NATIVE, owner: Loader).
// On the console these live in libkernel's rtld; they read the module list, so they belong to
// the Loader, not to a kernel bridge.
#include <array>
#include <cstdlib>
#include <cstring>
#include <span>

#include "base/log.h"
#include "loader/linker.h"
#include "runtime/exports.h"

namespace SecondSon::Loader {

namespace {

constexpr s32 ORBIS_OK = 0;
constexpr s32 ORBIS_KERNEL_ERROR_ESRCH = static_cast<s32>(0x80020003);
constexpr s32 ORBIS_KERNEL_ERROR_EFAULT = static_cast<s32>(0x8002000E);
constexpr s32 ORBIS_KERNEL_ERROR_EINVAL = static_cast<s32>(0x80020016);

struct TlsIndex {
    u64 ti_module;
    u64 ti_offset;
};

/// OrbisKernelModuleInfoEx
struct ModuleInfoEx {
    u64 st_size;
    std::array<char, 256> name;
    s32 id;
    u32 tls_index;
    u64 tls_init_addr;
    u32 tls_init_size;
    u32 tls_size;
    u32 tls_offset;
    u32 tls_align;
    u64 init_proc_addr;
    u64 fini_proc_addr;
    u64 reserved1;
    u64 reserved2;
    u64 eh_frame_hdr_addr;
    u64 eh_frame_addr;
    u32 eh_frame_hdr_size;
    u32 eh_frame_size;
    std::array<SegmentInfo, 4> segments;
    u32 segment_count;
};
static_assert(sizeof(ModuleInfoEx) == 0x1A8);

/// OrbisModuleInfoForUnwind
struct ModuleInfoForUnwind {
    u64 st_size;
    std::array<char, 256> name;
    u64 eh_frame_hdr_addr;
    u64 eh_frame_addr;
    u64 eh_frame_size;
    u64 seg0_addr;
    u64 seg0_size;
};
static_assert(sizeof(ModuleInfoForUnwind) == 0x130);

ModuleInfoEx MakeInfoEx(const Module& m) {
    ModuleInfoEx info{};
    info.st_size = sizeof(ModuleInfoEx);
    info.name = m.info_name;
    info.id = m.id;
    info.tls_index = m.tls.modid;
    info.tls_init_addr = m.tls.image_vaddr;
    info.tls_init_size = m.tls.init_image_size;
    info.tls_size = m.tls.image_size;
    info.tls_offset = m.tls.offset;
    info.tls_align = m.tls.align;
    info.init_proc_addr = m.Base() + m.init_addr;
    info.fini_proc_addr = m.Base() + m.fini_addr;
    info.eh_frame_hdr_addr = m.Base() + m.eh_frame_hdr_addr;
    info.eh_frame_addr = m.Base() + m.eh_frame_addr;
    info.eh_frame_hdr_size = m.eh_frame_hdr_size;
    info.eh_frame_size = m.eh_frame_size;
    info.segments = m.segments;
    info.segment_count = m.segment_count;
    return info;
}

// __tls_get_addr
SSPC_SYSV_ABI void* TlsGetAddr(TlsIndex* index) {
    return Linker::Active()->TlsGetAddr(index->ti_module, index->ti_offset);
}

// _sceKernelRtldSetApplicationHeapAPI (libc.prx registers its malloc family)
SSPC_SYSV_ABI void RtldSetApplicationHeapApi(void* functions[]) {
    Linker::Active()->SetHeapApi(functions);
}

SSPC_SYSV_ABI void* GetProcParam() {
    return Linker::Active()->GetProcParam();
}

SSPC_SYSV_ABI s32 GetModuleInfoForUnwind(u64 addr, s32 flags, ModuleInfoForUnwind* info) {
    if (flags >= 3) {
        std::memset(info, 0, sizeof(ModuleInfoForUnwind));
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    if (info->st_size < sizeof(ModuleInfoForUnwind)) {
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    SSPC_LOG(Loader, Debug, "sceKernelGetModuleInfoForUnwind addr {:#x} flags {:#x}", addr, flags);
    const Module* m = Linker::Active()->FindByAddress(addr);
    if (m == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    const auto ex = MakeInfoEx(*m);
    std::memset(info, 0, sizeof(ModuleInfoForUnwind));
    info->name = ex.name;
    info->eh_frame_hdr_addr = ex.eh_frame_hdr_addr;
    info->eh_frame_addr = ex.eh_frame_addr;
    info->eh_frame_size = ex.eh_frame_size;
    info->seg0_addr = ex.segments[0].address;
    info->seg0_size = ex.segments[0].size;
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 GetModuleInfoFromAddr(u64 addr, s32 flags, ModuleInfoEx* info) {
    if (flags >= 3) {
        std::memset(info, 0, sizeof(ModuleInfoEx));
        return ORBIS_KERNEL_ERROR_EINVAL;
    }
    if (info == nullptr) {
        return ORBIS_KERNEL_ERROR_EFAULT;
    }
    SSPC_LOG(Loader, Debug, "sceKernelGetModuleInfoFromAddr addr {:#x} flags {:#x}", addr, flags);
    const Module* m = Linker::Active()->FindByAddress(addr);
    if (m == nullptr) {
        return ORBIS_KERNEL_ERROR_ESRCH;
    }
    *info = MakeInfoEx(*m);
    return ORBIS_OK;
}

// Stack protector and process name (libkernel process objects).
u64 g_stack_chk_guard = 0xDEADBEEF54321ABC; // same value as the Phase 0 oracle
const char* g_progname = "eboot.bin";

SSPC_SYSV_ABI void StackChkFail() {
    SSPC_LOG(Loader, Critical, "__stack_chk_fail: guest stack corruption detected");
    Log::Flush();
    std::abort();
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kRtldExports[] = {
    {"vNe1w4diLCs", "libkernel", "libkernel", "__tls_get_addr", Fn(&TlsGetAddr), nullptr, "loader"},
    {"p5EcQeEeJAE", "libkernel", "libkernel", "_sceKernelRtldSetApplicationHeapAPI",
     Fn(&RtldSetApplicationHeapApi), nullptr, "loader"},
    {"959qrazPIrg", "libkernel", "libkernel", "sceKernelGetProcParam", Fn(&GetProcParam), nullptr,
     "loader"},
    {"RpQJJVKTiFM", "libkernel", "libkernel", "sceKernelGetModuleInfoForUnwind",
     Fn(&GetModuleInfoForUnwind), nullptr, "loader"},
    {"f7KBOafysXo", "libkernel", "libkernel", "sceKernelGetModuleInfoFromAddr",
     Fn(&GetModuleInfoFromAddr), nullptr, "loader"},
    {"Ou3iL1abvng", "libkernel", "libkernel", "__stack_chk_fail", Fn(&StackChkFail), nullptr, "loader"},
    {"f7uOxY9mM1U", "libkernel", "libkernel", "__stack_chk_guard", &g_stack_chk_guard, nullptr,
     "loader"},
    {"djxxOmW6-aw", "libkernel", "libkernel", "__progname", static_cast<void*>(&g_progname), nullptr,
     "loader"},
};

} // namespace

std::span<const Runtime::Export> RtldExports() {
    return kRtldExports;
}

} // namespace SecondSon::Loader
