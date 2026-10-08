// SecondSonPC — TEMPORARY Phase 1-2 bridge: Loader::Host on top of shadPS4's memory manager,
// CPU patcher and pthreads (Memory and Threading stay BRIDGE until Phase 2).
#pragma once

#include <memory>

#include "loader/host.h"

namespace SecondSon::Filesystem {
class FsHost;
}

namespace SecondSon::Bridge {

std::unique_ptr<Loader::Host> CreateLoaderHost();

/// Filesystem::FsHost on the shadPS4 core (src/bridge/shadps4/fs_host.cpp).
std::unique_ptr<Filesystem::FsHost> CreateFsHost();

} // namespace SecondSon::Bridge
