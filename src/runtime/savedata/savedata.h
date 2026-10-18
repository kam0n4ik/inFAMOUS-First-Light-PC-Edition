// SecondSonPC — libSceSaveData (NATIVE, group "savedata"; docs/AUDIT.md Phase 3). The saves are the bridge's files
// exactly: <home>\<user>\savedata\<title>\<dir>\ with sce_sys\param.sfo (the same bytes, runtime/psf), icon0.png and
// the "corrupted" marker while a save is mounted for writing; a save written by either runs on the other.
#pragma once

#include <filesystem>
#include <span>
#include <string>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::SaveData {

struct Settings {
    std::filesystem::path home;   ///< <user data>\home
    std::string title_dir;        ///< the game's INSTALL_DIR_SAVEDATA, else its serial (CUSA00004)
    u32 firmware = 0;             ///< the game's SDK firmware (param.sfo SYSTEM_VER & 0xFFF00000): behaviour by version
    int language_id = 1;          ///< the console language: a new save's default title
    std::filesystem::path icon;   ///< /app0/sce_sys/save_data.png on the host (COPY_ICON)
};

/// Before the game runs (the bootstrap, beside the bridge's SaveData setup).
void Configure(const Settings& settings);

std::span<const Runtime::Export> Exports();

} // namespace SecondSon::SaveData
