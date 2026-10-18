// SecondSonPC — `--gcn-inventory` (gcn_inventory.cpp): the instructions of every GCN program of the game.
#pragma once

#include <filesystem>
#include <string>

namespace SecondSon::Gpu::Gcn {

/// Decodes every program of the offline index (<cache>\<game>\native\game_programs.idx) from the game's packs and
/// writes the report to `report`. false: no index, or the report could not be written (`error`).
bool RunInventory(const std::filesystem::path& game_dir, const std::filesystem::path& cache_dir,
                  const std::filesystem::path& report, std::string& error);

/// `--gcn-translate-test`: every program of the index through SecondSonPC's own translator (decode, graph, structure,
/// emission) and every module it emits through spirv-val (E:/VulkanSDK/Bin, Vulkan 1.3): how many are emitted, how
/// many are valid, the instructions that stopped the others (by count), the validator's first messages.
bool RunTranslateTest(const std::filesystem::path& game_dir, const std::filesystem::path& cache_dir,
                      const std::filesystem::path& report, std::string& error);

} // namespace SecondSon::Gpu::Gcn
