// SecondSonPC — `--gcn-inventory` (gcn_inventory.cpp): the instructions of every GCN program of the game.
#pragma once

#include <filesystem>
#include <string>

namespace SecondSon::Gpu::Gcn {

/// Decodes every program of the offline index (<cache>\<game>\native\game_programs.idx) from the game's packs and
/// writes the report to `report`. false: no index, or the report could not be written (`error`).
bool RunInventory(const std::filesystem::path& game_dir, const std::filesystem::path& cache_dir,
                  const std::filesystem::path& report, std::string& error);

} // namespace SecondSon::Gpu::Gcn
