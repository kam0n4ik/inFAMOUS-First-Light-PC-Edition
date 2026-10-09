// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — TEMPORARY bridge for the native direct memory (Phase 2, P2.3.1, EXPERIMENTAL;
// docs/PHASE2_P23_MEMORY.md): Core::DirectMemoryBackend (patch 0007) on Memory::GameDirectMemory(),
// the release's Unmapper on Core::MemoryManager::UnmapDirectPhysical, and the memory map dump.
#include "bridge/shadps4/memory_host.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <shared_mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "app/config.h"
#include "base/log.h"
#include "bridge/shadps4/native_memory.h"
#include "common/thread.h"
#include "core/memory.h"
#include "runtime/kernel/kernel.h"
#include "runtime/memory/direct_memory.h"

namespace Core {

/// Patch 0007: read access to the bridge's memory maps for SecondSonPC (diagnostics, self-test).
class SecondSonMemoryAccess {
public:
    static u64 DirectSize(MemoryManager& m) {
        return m.total_direct_size;
    }

    static bool DmemUntouched(MemoryManager& m) {
        std::shared_lock lk{m.mutex};
        return m.dmem_map.size() == 1 && m.dmem_map.begin()->second.dma_type == PhysicalMemoryType::Free;
    }

    static bool WriterLocked(MemoryManager& m) {
        if (!m.mutex.try_lock_shared()) {
            return true;
        }
        m.mutex.unlock_shared();
        return false;
    }

    static bool DmemFreeAt(MemoryManager& m, PAddr phys) {
        std::shared_lock lk{m.mutex};
        const auto it = std::prev(m.dmem_map.upper_bound(phys));
        return it->second.dma_type == PhysicalMemoryType::Free;
    }

    /// The bridge's dmem_map in the format of the native map (a mapped area counts one view: the
    /// bridge does not count them).
    static std::string Dmem(MemoryManager& m) {
        std::shared_lock lk{m.mutex};
        std::string out;
        for (const auto& [base, area] : m.dmem_map) {
            const bool free = area.dma_type == PhysicalMemoryType::Free;
            out += fmt::format("{:#012x} {:#012x} {:#11x} {:<9} type {:2} views {}\n", base, base + area.size,
                               area.size, free ? "free" : "allocated", free ? 0 : area.memory_type,
                               area.dma_type == PhysicalMemoryType::Mapped ? 1 : 0);
        }
        return out;
    }
};

} // namespace Core

namespace SecondSon::Bridge {

namespace {

class Backend final : public Core::DirectMemoryBackend {
public:
    u64 TotalSize() override {
        return Memory::GameDirectMemory().Size();
    }

    s32 Allocate(PAddr search_start, PAddr search_end, u64 size, u64 alignment, s32 memory_type,
                 PAddr* out) override {
        u64 phys = 0;
        const s32 result =
            Memory::GameDirectMemory().Allocate(search_start, search_end, size, alignment, memory_type, &phys);
        if (result == Memory::kOk) {
            *out = phys;
        }
        return result;
    }

    s32 Release(PAddr start, u64 size, bool checked) override {
        return Memory::GameDirectMemory().Release(start, size, checked, Memory::GetUnmapper());
    }

    s32 BeginMap(PAddr phys, u64 size, bool exclusive, std::vector<Slice>& out) override {
        std::vector<Memory::PhysSlice> slices;
        const s32 result = Memory::GameDirectMemory().BeginMap(phys, size, exclusive, &slices);
        out.clear();
        for (const auto& s : slices) {
            out.push_back({s.base, s.size, s.memory_type});
        }
        return result;
    }

    void EndMap(PAddr phys, u64 size) override {
        Memory::GameDirectMemory().EndMap(phys, size);
    }
};

class BridgeUnmapper final : public Memory::Unmapper {
public:
    s32 UnmapPhysical(std::span<const Memory::PhysRange> ranges) override {
        std::vector<std::pair<PAddr, u64>> list;
        list.reserve(ranges.size());
        for (const auto& r : ranges) {
            list.emplace_back(r.base, r.size);
        }
        return Core::Memory::Instance()->UnmapDirectPhysical(list);
    }
};

Backend g_backend;
BridgeUnmapper g_unmapper;
std::atomic<bool> g_installed{false};
// [Console] of SecondSonPC.toml, kept for the budget computed when the loader sets up the regions.
bool g_dev_kit = false;
u32 g_extra_dmem_mib = 0;
u32 g_extra_fmem_mib = 0;

const char* StateName(Memory::AreaState s) {
    switch (s) {
    case Memory::AreaState::Free:
        return "free";
    case Memory::AreaState::Allocated:
        return "allocated";
    case Memory::AreaState::Releasing:
        return "releasing";
    }
    return "?";
}

} // namespace

bool InstallNativeDirectMemory(const App::Config& cfg) {
    g_dev_kit = cfg.dev_kit;
    g_extra_dmem_mib = cfg.extra_dmem_mib;
    g_extra_fmem_mib = cfg.extra_fmem_mib;
    if (!Core::Memory::Instance()->SetDirectMemoryBackend(&g_backend)) {
        SSPC_LOG(Memory, Critical, "native direct memory: the bridge has already allocated or mapped "
                                   "direct memory, its accounting cannot be replaced now");
        return false;
    }
    Memory::SetUnmapper(&g_unmapper);
    g_installed = true;
    SSPC_LOG(Memory, Warning, "EXPERIMENTAL: direct memory accounted by SecondSonPC (--native kernel-dmem); "
                              "the bridge keeps the virtual side");
    return true;
}

bool UninstallNativeDirectMemory() {
    if (!Memory::GameDirectMemory().Pristine() || !Core::Memory::Instance()->SetDirectMemoryBackend(nullptr)) {
        return false;
    }
    Memory::SetUnmapper(nullptr);
    g_installed = false;
    return true;
}

bool NativeDirectMemoryInstalled() {
    return g_installed;
}

bool ConfigureNativeDirectMemory(u64 flexible_size, bool extended1, bool extended2) {
    if (!g_installed) {
        return true;
    }
    const Memory::Budget budget{
        .neo = Kernel::NeoMode(),
        .dev_kit = g_dev_kit,
        .extra_dmem_mib = g_extra_dmem_mib,
        .extra_fmem_mib = g_extra_fmem_mib,
        .flexible_size = flexible_size,
        .extended1 = extended1,
        .extended2 = extended2,
    };
    const u64 native = Memory::DirectMemorySize(budget);
    const u64 bridge = BridgeDirectMemorySize();
    if (native == 0 || native != bridge) {
        SSPC_LOG(Memory, Critical, "native direct memory budget {:#x} differs from the bridge's {:#x} "
                                   "(flexible {:#x}, extended {}/{}, neo {})",
                 native, bridge, flexible_size, extended1, extended2, budget.neo);
        return false;
    }
    if (!Memory::GameDirectMemory().Configure(native)) {
        SSPC_LOG(Memory, Critical, "native direct memory: already in use before the memory regions were set up");
        return false;
    }
    SSPC_LOG(Memory, Info, "native direct memory: budget {:#x} ({} MiB), the same as the bridge's", native,
             native >> 20);
    return true;
}

u64 BridgeDirectMemorySize() {
    return Core::SecondSonMemoryAccess::DirectSize(*Core::Memory::Instance());
}

bool BridgeDirectMemoryUntouched() {
    return Core::SecondSonMemoryAccess::DmemUntouched(*Core::Memory::Instance());
}

bool BridgeMemoryWriterLocked() {
    return Core::SecondSonMemoryAccess::WriterLocked(*Core::Memory::Instance());
}

bool BridgeDirectMemoryFreeAt(u64 phys) {
    return Core::SecondSonMemoryAccess::DmemFreeAt(*Core::Memory::Instance(), phys);
}

/// The virtual memory areas in use, one line each (the --memory-map format of P2.3.1).
static std::string VmaText(const std::vector<Core::GuestArea>& areas) {
    static constexpr const char* kTypes[] = {"free",   "reserved", "direct", "flexible", "pooled",
                                             "poolres", "stack",    "code",   "file",     "system"};
    std::string out;
    for (const auto& a : areas) {
        const auto type = static_cast<u32>(a.type);
        out += fmt::format("{:#014x} {:#014x} {:#12x} {:<8} prot {:#04x}", a.base, a.base + a.size, a.size,
                           type < std::size(kTypes) ? kTypes[type] : "?", static_cast<u32>(a.prot));
        if (a.phys_slices != 0) {
            out += fmt::format(" phys {:#x} ({} slice{})", a.phys, a.phys_slices, a.phys_slices == 1 ? "" : "s");
        }
        out += fmt::format(" \"{}\"\n", a.name);
    }
    return out;
}

std::string MemoryMapText() {
    if (const NativeGame game = GetNativeGame(); game.vm != nullptr) {
        // --native memory: the bridge's manager does not exist (creating it would take the address space again).
        std::string out = "# virtual memory areas (SecondSonPC's manager, --native memory; free areas left out)\n";
        out += "# start          end            size         type     prot  [phys first (slices)] \"name\"\n";
        out += VmaText(Core::GetGuestVirtualMemory().Areas());
        const auto& dmem = game.vm->Direct();
        out += fmt::format("# direct memory: SecondSonPC's manager, size {:#x}\n", dmem.Size());
        for (const auto& a : dmem.Snapshot()) {
            out += fmt::format("{:#012x} {:#012x} {:#11x} {:<9} type {:2} views {}\n", a.base, a.base + a.size,
                               a.size, StateName(a.state), a.memory_type, a.mappings);
        }
        const auto stats = game.vm->GetStats();
        out += fmt::format("# flexible memory: {:#x} of {:#x} in use; section {:#x} bytes, commit {}\n",
                           game.vm->FlexibleUsed(), game.vm->FlexibleBudget(), game.vm->Host().BackingSize(),
                           game.vm->Host().Commit() == Memory::CommitMode::OnDemand ? "on demand" : "upfront");
        out += fmt::format("# manager: maps {} unmaps {} protects {} failures {}; GPU unmap {} register {} map {} "
                           "write {}; host views {} private {} remaps {} protection restores {} protect failures "
                           "{} commits {}\n",
                           stats.maps, stats.unmaps, stats.protects, stats.failures, stats.gpu_unmap,
                           stats.gpu_register, stats.gpu_map, stats.gpu_write, stats.host.views, stats.host.privates,
                           stats.host.remaps, stats.host.protect_restores, stats.host.protect_failures,
                           stats.host.commits);
        return out;
    }
    auto& m = *Core::Memory::Instance();
    std::string out = "# virtual memory areas (the bridge's vma_map; free areas left out)\n";
    out += "# start          end            size         type     prot  [phys first (slices)] \"name\"\n";
    out += VmaText(Core::GetGuestVirtualMemory().Areas());
    if (g_installed) {
        const auto& dmem = Memory::GameDirectMemory();
        out += fmt::format("# direct memory: NATIVE (--native kernel-dmem), size {:#x}\n", dmem.Size());
        for (const auto& a : dmem.Snapshot()) {
            out += fmt::format("{:#012x} {:#012x} {:#11x} {:<9} type {:2} views {}\n", a.base, a.base + a.size,
                               a.size, StateName(a.state), a.memory_type, a.mappings);
        }
    } else {
        out += fmt::format("# direct memory: the bridge's dmem_map, size {:#x}\n", m.GetTotalDirectSize());
        out += Core::SecondSonMemoryAccess::Dmem(m);
    }
    return out;
}

void StartMemoryMapDump(const std::filesystem::path& file, u32 seconds) {
    if (seconds == 0) {
        return;
    }
    std::thread([file, seconds] {
        Common::SetCurrentThreadName("SecondSonPC:MemoryMap");
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const std::string text = MemoryMapText();
        std::ofstream(file, std::ios::binary) << text;
        SSPC_LOG(Memory, Info, "memory maps after {} s -> {}", seconds, file.string());
    }).detach();
}

} // namespace SecondSon::Bridge
