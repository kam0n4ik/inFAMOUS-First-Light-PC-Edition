// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY Phase 1-2 bridge: Loader::Host on top of the shadPS4 core
// (third_party/shadPS4 @ 0fe263a). The calls and arguments are the ones shadPS4's own
// Core::Module::LoadModuleToMemory and Core::Linker make (src/core/module.cpp, linker.cpp), so the
// guest address space ends up laid out exactly like in the Phase 0 oracle.
#include "bridge/shadps4/loader_host.h"

#include <string>

#include "common/thread.h"
#include "core/cpu_patches.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/threads.h"
#include "core/memory.h"
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
    LoaderHost() : memory_{Core::Memory::Instance()} {}

    u64 ReserveModule(u64 desired, u64 size, std::string_view name) override {
        void* out = nullptr;
        const s32 r = memory_->MapMemory(&out, desired, size, MemoryProt::NoAccess,
                                         MemoryMapFlags::NoFlags, VMAType::Reserved, name);
        return r == 0 ? reinterpret_cast<u64>(out) : 0;
    }

    bool MapTrampoline(u64 address, u64 size, std::string_view name) override {
        void* out = reinterpret_cast<void*>(address);
        return memory_->MapMemory(&out, address, size, MemoryProt::CpuReadWrite | MemoryProt::CpuExec,
                                  MemoryMapFlags::Fixed, VMAType::Code, name) == 0;
    }

    bool MapSegment(u64 address, u64 size, u32 elf_flags, bool system_module,
                    std::string_view name) override {
        void* out = reinterpret_cast<void*>(address);
        return memory_->MapMemory(&out, address, size, ProtFromElf(elf_flags), MemoryMapFlags::Fixed,
                                  system_module ? VMAType::Code : VMAType::Flexible, name) == 0;
    }

    void SetupMemoryRegions(u64 flexible_size, bool extended1, bool extended2) override {
        memory_->SetupMemoryRegions(flexible_size, extended1, extended2);
    }

    void* MapPrimaryTls(u64 size) override {
        // libkernel maps the primary thread's block with addr = 0 (system-managed area, which
        // already holds a mapping, hence this address).
        void* addr = reinterpret_cast<void*>(0x881000000ULL);
        const s32 ret = Libraries::Kernel::sceKernelMapNamedFlexibleMemory(&addr, size, 3, 0,
                                                                           "SceKernelPrimaryTcbTls");
        return ret == 0 ? addr : nullptr;
    }

    bool InitGnmDriverMemory() override {
        s64 phys_addr{};
        s32 result = Libraries::Kernel::sceKernelAllocateDirectMemory(
            0, Libraries::Kernel::sceKernelGetDirectMemorySize(), 0x10000, 0x10000, 3, &phys_addr);
        if (result == 0) {
            void* addr{reinterpret_cast<void*>(0xfe0000000)};
            result = Libraries::Kernel::sceKernelMapNamedDirectMemory(&addr, 0x10000, 0x13, 0, phys_addr,
                                                                      0x10000, "SceGnmDriver");
        }
        return result == 0;
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
    Core::MemoryManager* memory_;
    Libraries::Kernel::Thread main_thread_;
};

} // namespace

std::unique_ptr<Loader::Host> CreateLoaderHost() {
    return std::make_unique<LoaderHost>();
}

} // namespace SecondSon::Bridge
