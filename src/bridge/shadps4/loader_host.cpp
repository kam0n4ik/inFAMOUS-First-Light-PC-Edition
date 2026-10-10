// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY Phase 1-2 bridge: Loader::Host on top of the shadPS4 core
// (third_party/shadPS4 @ 0fe263a). The calls and arguments are the ones shadPS4's own
// Core::Module::LoadModuleToMemory and Core::Linker make (src/core/module.cpp, linker.cpp), so the
// guest address space ends up laid out exactly like in the Phase 0 oracle. Guest memory only
// through Core::GuestVirtualMemory / GuestPhysicalMemory (patch 0009, P2.3.3a).
#include "bridge/shadps4/loader_host.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "base/lifecycle.h"
#include "base/log.h"
#include "bridge/shadps4/memory_host.h"
#include "common/elf_info.h"
#include "common/thread.h"
#include "core/cpu_patches.h"
#include "core/guest_memory.h"
#include "core/libraries/kernel/threads.h"
#include "core/tls.h"
#include "app/config.h"
#include "runtime/cpu/exceptions.h"
#include "runtime/cpu/fs_patcher.h"
#include "runtime/cpu/sse4a.h"
#include "runtime/cpu/tcb_slot.h"
#include "runtime/kernel/thread.h"

namespace SecondSon::Bridge {

namespace {

/// SecondSonPC's FS patcher owns the guest's code (InstallGuestCpu; never changes after start-up).
bool g_native_fs = false;

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
            Lifecycle::Exit(1, "--native kernel-dmem: direct memory budget mismatch");
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

    void RegisterPatchModule(u64 module, u64 module_size, u64 trampoline, u64 trampoline_size,
                             std::string_view name) override {
        // Guest code for the exception dispatcher (P3.4): signals, fatal reports with module + offset.
        Cpu::Exceptions::RegisterGuestCode(module, module + module_size, name);
        Cpu::Exceptions::RegisterGuestCode(trampoline, trampoline + trampoline_size, std::string(name) + " (trampolines)");
        if (g_native_fs) {
            if (!Cpu::RegisterModule(module, module_size, trampoline, trampoline_size, name)) {
                SSPC_LOG(Loader, Critical, "{}: SecondSonPC's FS patcher cannot take the module, stopping", name);
                Lifecycle::Exit(1, "SecondSonPC's FS patcher cannot take a module");
            }
            return;
        }
        Core::RegisterPatchModule(reinterpret_cast<void*>(module), module_size,
                                  reinterpret_cast<void*>(trampoline), trampoline_size);
    }

    void PrePatch(u64 segment, u64 size) override {
        if (!g_native_fs) {
            Core::PrePatchInstructions(segment, size); // nothing on Windows
        }
    }

    bool StaticPatchingEnabled() override {
        // P3.4: with SecondSonPC's dispatcher the functions are decoded at load time for the red-zone marks
        // even when the bridge's patcher owns the code.
        return g_native_fs || NativeExceptions() || Core::WindowsGuestRedZoneProtection::IsStaticPatchingEnabled();
    }

    Loader::StaticPatchSummary StaticPatch(u64 segment, u64 size, std::span<const uintptr_t> function_starts,
                                           std::span<const Loader::CodeRange> functions) override {
        if (g_native_fs) {
            std::vector<Cpu::FunctionRange> ranges;
            ranges.reserve(functions.size());
            for (const auto& f : functions) {
                ranges.push_back({f.begin, f.end});
            }
            const auto r = Cpu::PatchModuleStatically(segment, size, ranges);
            if (r.undecodable != 0) {
                SSPC_LOG(Loader, Info,
                         "FS patcher: {} of {} functions at {:#x} not decoded to their end (data such as jump "
                         "tables: trusted up to their last ret/jmp); {} FS instructions past that left to first use",
                         r.undecodable, r.functions, segment, r.left_to_runtime);
            }
            SSPC_LOG(Loader, Info, "{} of {} functions at {:#x} keep data below rsp (red zone)", r.red_zone_functions,
                     r.functions, segment);
            return {r.functions, r.patched, r.unsupported};
        }
        if (NativeExceptions() && !Core::WindowsGuestRedZoneProtection::IsStaticPatchingEnabled()) {
            // The bridge's patcher patches at run time only: here just the red-zone marks.
            std::vector<Cpu::FunctionRange> ranges;
            for (const auto& f : functions) {
                if (f.begin >= segment && f.end <= segment + size) {
                    ranges.push_back({f.begin, f.end});
                }
            }
            const u64 marked = Cpu::AnalyzeRedZone(ranges);
            SSPC_LOG(Loader, Info, "{} of {} functions at {:#x} keep data below rsp (red zone)", marked, ranges.size(),
                     segment);
            return {ranges.size(), 0, 0};
        }
        const auto r = Core::WindowsGuestRedZoneProtection::IsStaticPatchingEnabled()
                           ? Core::PatchRedZoneMemoryInstructions(segment, size, function_starts)
                           : Core::PatchCpuInstructionsStatically(segment, size, function_starts);
        return {r.function_count, r.patched_cpu_patch_instruction_count + r.inplace_cpu_patch_instruction_count,
                r.unsupported_cpu_patch_instruction_count};
    }

    void StartGuestThread(std::function<void()> body) override {
        if (Kernel::Threads::Enabled()) {
            // --native threads (P2.4): the main thread is SecondSonPC's like every other one.
            if (!Kernel::Threads::StartMainThread(std::move(body))) {
                Lifecycle::Exit(1, "the guest's main thread could not start");
            }
            return;
        }
        main_thread_.Run([body = std::move(body)](std::stop_token) { body(); });
    }

    void SetCurrentThreadName(std::string_view name) override {
        Common::SetCurrentThreadName(std::string(name).c_str());
    }

    void* CurrentTcb() override {
        return Cpu::CurrentTcb(); // the same slot the bridge's threads write (patch 0021)
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

bool InstallGuestCpu(const App::Config& cfg, std::string* error) {
    // P3.4: the instruction sets the game's code needs (tools/cpu_audit.py). Without one of them the game
    // would stop at its first such instruction; say so before it starts.
    if (std::string missing; !Cpu::HostMeetsGameRequirements(&missing)) {
        *error = fmt::format("This CPU lacks instruction sets the game's code uses: {}.\nSecondSonPC needs "
                             "SSE2-SSE4.2, POPCNT, MOVBE, AVX, F16C, BMI1 and LZCNT (an AMD Ryzen / FX or an Intel "
                             "Core of the 4th generation or later).",
                             missing);
        SSPC_LOG(Cpu, Critical, "CPU: missing {}", missing);
        return false;
    }
    SSPC_LOG(Cpu, Info, "CPU: SSE2-SSE4.2, POPCNT, MOVBE, AVX, F16C, BMI1, LZCNT present; SSE4a {}",
             Cpu::Sse4a::HostHasSse4a() ? "present" : "absent (emulated by the CPU patcher)");
    // One slot for every guest thread, owned by SecondSonPC: the bridge's TLS code (its threads'
    // SetTcbBase, its patcher's trampolines) is pointed at it before it allocates one of its own.
    if (!Cpu::InitTcbSlot()) {
        *error = "the guest TCB slot could not be set up (no TLS slot left), see SecondSonPC.log";
        return false;
    }
    if (!Core::UseTcbKey(Cpu::TcbSlot())) {
        SSPC_LOG(Cpu, Critical, "the bridge already has a TCB slot of its own ({}): one slot per thread is required",
                 Core::GetTcbKey());
        *error = "the guest TCB slot could not be set up, see SecondSonPC.log";
        return false;
    }
    const auto listed = [&cfg](std::string_view g) {
        return std::ranges::find(cfg.native_groups, g) != cfg.native_groups.end();
    };
    const bool exceptions = listed("exceptions");
    const bool wanted = listed("fs-patch") || listed("cpu");
    if (!wanted) {
        SSPC_LOG(Cpu, Info, "CPU patcher: the bridge's (core/cpu_patches.cpp, run-time patches with patch 0016); "
                            "TCB slot {} is SecondSonPC's",
                 Cpu::TcbSlot());
    } else if (!Cpu::InstallFsPatcher(!exceptions)) {
        SSPC_LOG(Cpu, Error, "--native cpu: the exception handler could not be added; the bridge's patcher stays");
    } else {
        std::string note;
        Cpu::CpuNeedsOnlyFsPatches(&note);
        Cpu::SetSse4aEmulation(cfg.cpu_emulate_sse4a);
        g_native_fs = true;
        SSPC_LOG(Cpu, Warning,
                 "EXPERIMENTAL: SecondSonPC's CPU patcher (FS: load-time patches, run-time fallback; SSE4a {}), TCB "
                 "slot {}{}",
                 !Cpu::Sse4aEmulated()        ? "native"
                 : cfg.cpu_emulate_sse4a      ? "emulated: FORCED on a CPU that has it (--cpu-emulate sse4a)"
                                              : "emulated",
                 Cpu::TcbSlot(), note.empty() ? std::string{} : "; " + note);
    }
    // P3.4: the dispatcher before anything creates the bridge's (its first Signals::Instance()).
    if (exceptions && !InstallExceptionRuntime(cfg, g_native_fs)) {
        SSPC_LOG(Cpu, Critical, "--native exceptions: SecondSonPC's exception dispatcher could not be installed");
        *error = "--native exceptions: SecondSonPC's exception dispatcher could not be installed, see SecondSonPC.log";
        return false;
    }
    return true;
}

bool NativeFsPatcher() {
    return g_native_fs;
}

} // namespace SecondSon::Bridge
