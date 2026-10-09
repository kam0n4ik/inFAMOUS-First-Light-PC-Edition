// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY Phase 1-2 bridge: Loader::Host on top of the shadPS4 core
// (third_party/shadPS4 @ 0fe263a). The calls and arguments are the ones shadPS4's own
// Core::Module::LoadModuleToMemory and Core::Linker make (src/core/module.cpp, linker.cpp), so the
// guest address space ends up laid out exactly like in the Phase 0 oracle. Guest memory only
// through Core::GuestVirtualMemory / GuestPhysicalMemory (patch 0009, P2.3.3a).
#include "bridge/shadps4/loader_host.h"

#include <cstdlib>
#include <string>

#include "base/log.h"
#include "bridge/shadps4/memory_host.h"
#include "common/elf_info.h"
#include "common/thread.h"
#include "core/cpu_patches.h"
#include "core/guest_memory.h"
#include "core/libraries/kernel/threads.h"
#include "core/tls.h"

namespace SecondSon::Bridge {

namespace {

using Core::MemoryMapFlags;
using Core::MemoryProt;
using Core::VMAType;

Core::MemoryProt ProtFromElf(u32 flags) {
    auto prot = MemoryProt::NoAccess;
    if ((flags & 0x4) != 0) { // PF_R
        prot |= MemoryProt::CpuRead;
    }
    if ((flags & 0x2) != 0) { // PF_W
        prot |= MemoryProt::CpuWrite;
    }
    if ((flags & 0x1) != 0) { // PF_X
        prot |= MemoryProt::CpuExec;
    }
    return prot;
}

class LoaderHost final : public Loader::Host {
public:
    LoaderHost() : memory_{Core::GetGuestVirtualMemory()}, physical_{Core::GetGuestPhysicalMemory()} {}

    u64 ReserveModule(u64 desired, u64 size, std::string_view name) override {
        void* out = nullptr;
        const s32 r = memory_.Map(&out, {.addr = desired,
                                         .size = size,
                                         .prot = MemoryProt::NoAccess,
                                         .flags = MemoryMapFlags::NoFlags,
                                         .type = VMAType::Reserved,
                                         .name = name});
        return r == 0 ? reinterpret_cast<u64>(out) : 0;
    }

    bool MapTrampoline(u64 address, u64 size, std::string_view name) override {
        void* out = reinterpret_cast<void*>(address);
        return memory_.Map(&out, {.addr = address,
                                  .size = size,
                                  .prot = MemoryProt::CpuReadWrite | MemoryProt::CpuExec,
                                  .flags = MemoryMapFlags::Fixed,
                                  .type = VMAType::Code,
                                  .name = name}) == 0;
    }

    bool MapSegment(u64 address, u64 size, u32 elf_flags, bool system_module,
                    std::string_view name) override {
        void* out = reinterpret_cast<void*>(address);
        return memory_.Map(&out, {.addr = address,
                                  .size = size,
                                  .prot = ProtFromElf(elf_flags),
                                  .flags = MemoryMapFlags::Fixed,
                                  .type = system_module ? VMAType::Code : VMAType::Flexible,
                                  .name = name}) == 0;
    }

    void SetupMemoryRegions(u64 flexible_size, bool extended1, bool extended2) override {
        physical_.SetupRegions(flexible_size, extended1, extended2);
        if (!ConfigureNativeDirectMemory(flexible_size, extended1, extended2)) {
            SSPC_LOG(Loader, Critical, "--native kernel-dmem: direct memory budget mismatch, stopping");
            Log::Flush();
            std::quick_exit(1);
        }
    }

    void* MapPrimaryTls(u64 size) override {
        // libkernel maps the primary thread's block with sceKernelMapNamedFlexibleMemory (RW, no
        // flags); its search starts at 0x881000000 (the system-managed area already holds a mapping
        // there). The function's argument checks pass for these arguments (the size is a page
        // multiple, the name shorter than 32), so this is the memory manager call it makes.
        void* addr = nullptr;
        const s32 ret = memory_.Map(&addr, {.addr = 0x881000000ULL,
                                            .size = size,
                                            .prot = MemoryProt::CpuReadWrite,
                                            .flags = MemoryMapFlags::NoFlags,
                                            .type = VMAType::Flexible,
                                            .name = "SceKernelPrimaryTcbTls"});
        SSPC_LOG(Loader, Info, "primary thread TLS: {:#x} bytes of flexible memory at {} ({:#x})", size,
                 addr, static_cast<u32>(ret));
        return ret == 0 ? addr : nullptr;
    }

    bool InitGnmDriverMemory() override {
        // libSceGnmDriver's start-up: sceKernelAllocateDirectMemory(0, the direct memory size,
        // 64 KiB, 64 KiB, type 3) and sceKernelMapNamedDirectMemory(0xfe0000000, 64 KiB, 0x13,
        // flags 0, phys, 64 KiB, "SceGnmDriver"). Their argument checks pass for these constants;
        // the mapping is exclusive (EBUSY over a mapped page) from SDK 2.50 on unless the guest
        // enabled direct memory aliasing, which no module of this game imports. The SDK version is
        // the one libkernel's RegisterLib read (ElfInfo, filled by the bootstrap before the bridge's
        // libraries start; this runs on the game's main thread, after them).
        const PAddr phys = physical_.AllocateDirect(0, physical_.DirectSize(), 0x10000, 0x10000, 3);
        if (phys == -1) {
            SSPC_LOG(Loader, Error, "libSceGnmDriver start-up: no direct memory");
            return false;
        }
        void* addr = nullptr;
        const s32 ret =
            memory_.Map(&addr, {.addr = 0xfe0000000ULL,
                                .size = 0x10000,
                                .prot = static_cast<MemoryProt>(0x13), // CPU read/write, GPU read
                                .flags = MemoryMapFlags::NoFlags,
                                .type = VMAType::Direct,
                                .name = "SceGnmDriver",
                                .phys = phys,
                                .exclusive = Common::ElfInfo::Instance().CompiledSdkVer() >=
                                             Common::ElfInfo::FW_250,
                                .alignment = 0x10000});
        SSPC_LOG(Loader, Info, "libSceGnmDriver start-up: direct memory {:#x} mapped at {} ({:#x})", phys, addr,
                 static_cast<u32>(ret));
        return ret == 0;
    }

    void RegisterPatchModule(u64 module, u64 module_size, u64 trampoline,
                             u64 trampoline_size) override {
        Core::RegisterPatchModule(reinterpret_cast<void*>(module), module_size,
                                  reinterpret_cast<void*>(trampoline), trampoline_size);
    }

    void PrePatch(u64 segment, u64 size) override {
        Core::PrePatchInstructions(segment, size);
    }

    bool StaticPatchingEnabled() override {
        return Core::WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    }

    Loader::StaticPatchSummary StaticPatch(u64 segment, u64 size,
                                           std::span<const uintptr_t> function_starts) override {
        const auto r = Core::WindowsGuestRedZoneProtection::IsStaticPatchingEnabled()
                           ? Core::PatchRedZoneMemoryInstructions(segment, size, function_starts)
                           : Core::PatchCpuInstructionsStatically(segment, size, function_starts);
        return {r.function_count, r.patched_cpu_patch_instruction_count + r.inplace_cpu_patch_instruction_count,
                r.unsupported_cpu_patch_instruction_count};
    }

    void StartGuestThread(std::function<void()> body) override {
        main_thread_.Run([body = std::move(body)](std::stop_token) { body(); });
    }

    void SetCurrentThreadName(std::string_view name) override {
        Common::SetCurrentThreadName(std::string(name).c_str());
    }

    void* CurrentTcb() override {
        return Core::GetTcbBase();
    }

private:
    Core::GuestVirtualMemory& memory_;
    Core::GuestPhysicalMemory& physical_;
    Libraries::Kernel::Thread main_thread_;
};

} // namespace

std::unique_ptr<Loader::Host> CreateLoaderHost() {
    return std::make_unique<LoaderHost>();
}

} // namespace SecondSon::Bridge
