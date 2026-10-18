// SecondSonPC — diagnostics a player asks for while playing (SSPC_DIAG_TEX_KEY=1: F8).
#pragma once

#include <atomic>

#include "base/abi.h"

namespace SecondSon::Gpu::Diag {

/// Bumped by F8 (SSPC_DIAG_TEX_KEY=1): the executor dumps the textures the next frame's draws read
/// (logs/frame_textures/<n>/: index.txt and each texture's bytes) - which picture of the game is on the screen.
inline std::atomic<u32> g_texture_dump_request{0};

} // namespace SecondSon::Gpu::Diag
