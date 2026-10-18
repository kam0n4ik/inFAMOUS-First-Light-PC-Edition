// SecondSonPC — install layout.
#pragma once

#include <filesystem>

namespace SecondSon::App {

/// Directory that contains SecondSonPC.toml (searched upwards from the executable, max 4 levels);
/// falls back to the executable directory. Safe to call before main().
const std::filesystem::path& RootDir();

/// TEMPORARY (Phase 0-4 bridge): working directory for the shadPS4 core, <root>\runtime\sspc.
/// shadPS4 picks its "portable" user dir as current_path()\user at static-init time, so this must
/// run before shadPS4's static initializers. Never falls back to %APPDATA%\shadPS4: aborts instead.
void PrepareBridgeWorkingDirectory();

/// The working directory the process was started in (before PrepareBridgeWorkingDirectory changed it): what a relative
/// path on the command line means to the user.
const std::filesystem::path& LaunchDir();

} // namespace SecondSon::App
