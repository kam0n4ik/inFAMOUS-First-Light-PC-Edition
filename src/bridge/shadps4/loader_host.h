// SecondSonPC — TEMPORARY Phase 1-2 bridge: Loader::Host on top of shadPS4's memory manager,
// CPU patcher and pthreads (Memory and Threading stay BRIDGE until Phase 2).
#pragma once

#include <memory>

#include "loader/host.h"

namespace SecondSon::Filesystem {
class FsHost;
}
namespace SecondSon::App {
struct Config;
}

namespace SecondSon::Bridge {

std::unique_ptr<Loader::Host> CreateLoaderHost();

/// Filesystem::FsHost on the shadPS4 core (src/bridge/shadps4/fs_host.cpp).
std::unique_ptr<Filesystem::FsHost> CreateFsHost();

/// Native UserService: routes the input layer's logins to it, passes taken events on to NpManager
/// (src/bridge/shadps4/user_host.cpp). Must run before the first controller check (window).
void InstallUserHost();

/// --selftest (src/bridge/shadps4/selftest.cpp): exit code 0 = every check passed.
int RunSelfTest(const App::Config& cfg);

} // namespace SecondSon::Bridge
