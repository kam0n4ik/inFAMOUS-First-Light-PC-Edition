// SecondSonPC — TEMPORARY bridge for the native direct memory (Phase 2, P2.3.1, EXPERIMENTAL;
// docs/PHASE2_P23_MEMORY.md). With --native kernel-dmem shadPS4's memory manager accounts the guest's
// physical direct memory in Memory::GameDirectMemory() (patch 0007) and keeps the virtual side
// (address space, views, protection, GPU notifications); a release unmaps the views through it.
#pragma once

#include <filesystem>
#include <string>

#include "base/abi.h"

namespace SecondSon::App {
struct Config;
}

namespace SecondSon::Bridge {

/// --native kernel-dmem, before anything is allocated. false (nothing changed) when the bridge has
/// already allocated or mapped direct memory.
bool InstallNativeDirectMemory(const App::Config& cfg);
/// Self-test: back to the bridge's own accounting; only when nothing native is allocated or mapped.
bool UninstallNativeDirectMemory();
bool NativeDirectMemoryInstalled();

/// Loader::Host::SetupMemoryRegions, after the bridge's: sizes the native map from the same inputs
/// and compares it with the bridge's budget. false = they differ (the run must not go on). Does
/// nothing (true) without --native kernel-dmem.
bool ConfigureNativeDirectMemory(u64 flexible_size, bool extended1, bool extended2);

/// The bridge's own direct memory budget, and whether its own map (dmem_map) was never used.
u64 BridgeDirectMemorySize();
bool BridgeDirectMemoryUntouched();

/// Self-test (P2.3.2, GPU notifications): whether the bridge memory manager's writer lock is held
/// right now (by anyone), and whether `phys` is free in its own map (dmem_map; takes the reader
/// lock, so only call it while the caller does not hold the writer lock).
bool BridgeMemoryWriterLocked();
bool BridgeDirectMemoryFreeAt(u64 phys);

/// The guest's memory maps as text: the bridge's virtual memory areas, then the direct memory map
/// (the native one when installed, else the bridge's dmem_map) in the same format for both.
std::string MemoryMapText();
/// --memory-map <seconds>: writes MemoryMapText() to `file` that many seconds after start.
void StartMemoryMapDump(const std::filesystem::path& file, u32 seconds);

} // namespace SecondSon::Bridge
