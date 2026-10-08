// SecondSonPC — what the Loader needs from subsystems that are still BRIDGE in Phase 1.
// Memory (Phase 2), CPU patches/TCB access (Phase 2) and threads (Phase 2) stay in the temporary
// shadPS4 bridge; the Loader reaches them only through this interface
// (implemented in src/bridge/shadps4/loader_host.cpp). No shadPS4 header is visible here.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Loader {

struct StaticPatchSummary {
    u64 functions = 0;
    u64 patched = 0;
    u64 unsupported = 0;
};

class Host {
public:
    virtual ~Host() = default;

    // --- guest memory -----------------------------------------------------------------------
    /// Reserves `size` bytes (no access) for a module at or above `desired`. Returns the base or 0.
    virtual u64 ReserveModule(u64 desired, u64 size, std::string_view name) = 0;
    /// Maps the RWX trampoline area of a module at the fixed address `address`.
    virtual bool MapTrampoline(u64 address, u64 size, std::string_view name) = 0;
    /// Maps one PT_LOAD/PT_SCE_RELRO segment at the fixed address `address`.
    /// `elf_flags` are the segment's PF_* bits; `system_module` selects the memory type.
    virtual bool MapSegment(u64 address, u64 size, u32 elf_flags, bool system_module,
                            std::string_view name) = 0;
    /// Configures the flexible/direct memory regions (from the eboot's SceKernelMemParam).
    virtual void SetupMemoryRegions(u64 flexible_size, bool extended1, bool extended2) = 0;
    /// The primary thread's TLS+TCB block (mapped from flexible memory, like libkernel does).
    virtual void* MapPrimaryTls(u64 size) = 0;
    /// libSceGnmDriver's start-up mapping of a direct memory chunk (some games depend on it).
    virtual bool InitGnmDriverMemory() = 0;

    // --- CPU patches (guest TCB access through FS etc.) -------------------------------------
    virtual void RegisterPatchModule(u64 module, u64 module_size, u64 trampoline,
                                     u64 trampoline_size) = 0;
    virtual void PrePatch(u64 segment, u64 size) = 0;
    virtual bool StaticPatchingEnabled() = 0;
    virtual StaticPatchSummary StaticPatch(u64 segment, u64 size,
                                           std::span<const uintptr_t> function_starts) = 0;

    // --- threads ----------------------------------------------------------------------------
    /// Runs `body` on a new guest thread (default attributes). The thread's TCB is created by
    /// the bridge's pthread code through Loader::Linker::CreateTcb (initial = first thread).
    virtual void StartGuestThread(std::function<void()> body) = 0;
    virtual void SetCurrentThreadName(std::string_view name) = 0;
    /// TCB of the calling guest thread.
    virtual void* CurrentTcb() = 0;
};

} // namespace SecondSon::Loader
