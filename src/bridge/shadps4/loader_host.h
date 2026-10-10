// SecondSonPC — TEMPORARY Phase 1-2 bridge: Loader::Host on top of shadPS4's memory manager,
// CPU patcher and pthreads (Memory and Threading stay BRIDGE until Phase 2).
#pragma once

#include <memory>

#include "loader/host.h"

struct SDL_Window;

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

/// Opens the real controllers on shadPS4's SDL controller layer; the first check logs in the user
/// of slot 1 (src/bridge/shadps4/input_host.cpp). Needed when libScePad is NATIVE (its BRIDGE
/// RegisterLib, which does this, is then not started).
void StartControllers();

/// Native libScePad: its host (real controllers, clock) and SecondSonPC's keyboard/mouse on the
/// bridge window (patch 0006). `test_mode`: automated runs (see Input::KeyboardMouse).
void InstallNativeInput(const App::Config& cfg, SDL_Window* window, bool test_mode);

/// Native libkernel time functions: guest errno and process clock from the bridge, [Console]
/// neoMode with the title's PS4 Pro attribute (src/bridge/shadps4/kernel_host.cpp).
void InstallKernelHost(const App::Config& cfg, u32 psf_attributes);

/// --selftest (src/bridge/shadps4/selftest.cpp): exit code 0 = every check passed.
int RunSelfTest(const App::Config& cfg);

/// --patch-race-test (src/bridge/shadps4/selftest_patch_race.cpp, SSPC-002): many threads run guest
/// fs: accesses for the first time while the bridge patches them; 0 = every site patched, no crash.
int RunPatchRaceTest(const App::Config& cfg);

} // namespace SecondSon::Bridge
