// SecondSonPC — the game's shadow map sizes ([Graphics] shadowQuality).
//
// What the engine has (eboot.bin of CUSA00004; Ghidra and the file's bytes):
//   the sizes are .data values, read where the maps are made and where each frame's cascades are set up:
//     0xdf9d2c  2048  the sun's target (one 2048x2048 D32 shared by its cascades, drawn in turn) and cascades 1..3
//     0xdf9d38  2048  cascade 0 (a view over the same memory: never larger than 0xdf9d2c)
//     0xdf9d30  1024  the two rain maps (double-buffered by frame parity) and their cascades
//     0xdf9d3c  2048  the light slots (a 2048x2048x4 array) and every reader's fallback - not changed
//   The targets are made once at start (0x18e4c0, from 0x29c75e: RenderTarget::Init 0x2ffeb0); each frame the update
//   0x43b7b0 writes each cascade's size (+0xa8) from them, and its readers take that size: the view 0x439830, the
//   viewport 0x439ab0, the sampling constants size / 1/size 0x43e3e0 and the cascade matrices' texel snap 0x441080.
//   So the values written before the game starts make the whole chain at the other size.
//   Levels: "original" 2048 / 1024 (the game's) | "low" 1024 / 512 | "high" 4096 / 2048 (+64 MiB of GPU memory).
//   Takes effect at the next start (the targets are made at start).
#pragma once

#include <string_view>

#include "base/abi.h"

namespace SecondSon::Engine::Render::ShadowQuality {

/// The loader's window before the guest runs: writes the sizes of `level` when the eboot's values and the functions
/// using them are the analysed ones. false (logged): the game's own sizes.
bool Install(u64 eboot_base, std::string_view level);

} // namespace SecondSon::Engine::Render::ShadowQuality
