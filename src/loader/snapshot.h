// SecondSonPC — relocation snapshot: the value of every relocation slot of the loaded game
// modules, classified (module+offset / binding name / raw). Written by both loader paths at the
// same point, so `fc` / `diff` of two snapshots proves both loaders produced the same image.
#pragma once

#include <filesystem>
#include <span>
#include <string>

#include "base/abi.h"

namespace SecondSon::Loader {

class Bindings;

struct SnapshotModule {
    std::string name;
    std::filesystem::path host_path;
    u64 base = 0;
    u64 size = 0; // aligned image size (0 = computed from the file)
};

/// `bindings` (optional) names binding addresses; thunks of the call counter are followed.
bool WriteRelocationSnapshot(const std::filesystem::path& file, std::span<const SnapshotModule> modules,
                             const Bindings* bindings, std::string& error);

} // namespace SecondSon::Loader
