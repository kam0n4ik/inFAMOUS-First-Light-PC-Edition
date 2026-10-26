// SecondSonPC — starting the game from the launcher: the launch profiles (play.cmd's measured preset, and
// play_experimental.cmd's queue experiments on top) as environment variables, the launcher's own choices
// (%LOCALAPPDATA%\SecondSonPC\launcher.toml), the checks before a start (eboot.bin, a running SecondSonPC.exe),
// the saves' backup play.cmd makes, and the game as a child process. Own code.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/abi.h"

namespace SecondSon::Launcher {

enum class Profile : u8 { Standard, Experimental };

/// play.cmd's preset (the variables it sets for every start).
std::span<const std::pair<std::string_view, std::string_view>> StandardEnv();
/// play_experimental.cmd's additions (SSPC_PLAY_EXPERIMENTAL in play.cmd).
std::span<const std::pair<std::string_view, std::string_view>> ExperimentalEnv();
/// This process's environment as `profile` starts the game (the child inherits it): the preset set, the
/// experimental variables set or removed.
void ApplyProfileEnv(Profile profile);

struct LauncherState {
    Profile profile = Profile::Standard;
    bool close_after_launch = false;
};
std::filesystem::path LauncherFile(); // %LOCALAPPDATA%\SecondSonPC\launcher.toml
/// false: no file yet (`out` keeps the defaults; play_experimental.cmd's SSPC_PLAY_EXPERIMENTAL picks that profile).
bool LoadLauncherState(LauncherState& out);
bool SaveLauncherState(const LauncherState& s, std::string& error);

/// SecondSonPC.exe's folder (the game's own in a player's install; the game starts there).
std::filesystem::path GameDir();
/// "" if the game can start; else why not (Russian). `game_dir`: SecondSonPC.toml's [Game] path ("": not read).
std::string CheckGame(const std::filesystem::path& game_dir);
/// <user_data_dir>\home copied to <user_data_dir>\savedata_backups\<yyyyMMdd_HHmmss>, as play.cmd does (its
/// userdata\ is SecondSonPC.toml's [Paths] userData). "" or the error.
std::string BackupSaves(const std::filesystem::path& user_data_dir);

/// The game: SecondSonPC.exe in its folder, in its own console, `--no-launcher` + `args`. "" or the error.
struct GameProcess {
    void* process = nullptr; // HANDLE
    u32 pid = 0;
};
std::string StartGame(std::span<const std::string> args, GameProcess& out);
bool GameRunning(GameProcess& p); // false once it exited (the handle is closed)

} // namespace SecondSon::Launcher
