// SecondSonPC — self-tests of the PC settings (no game, no window): registry, user settings file (save, restore,
// corrupt files, atomic writes), the settings service (ranges, restart, reset, appliers), key bindings and conflicts,
// keyboard/mouse additions, controller tuning, device switching, the menu model. Run by --selftest (group Settings)
// and by the standalone tests/settings_tests program. Own code.
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace SecondSon::Settings {

using CheckFn = std::function<void(std::string_view group, std::string_view what, bool ok, const std::string& detail)>;

/// `scratch`: an empty writable folder for the file tests (created; left with the test files for inspection).
void RunChecks(const CheckFn& check, const std::filesystem::path& scratch);

} // namespace SecondSon::Settings
