// SecondSonPC — the game's high-resolution city textures ([Graphics] textureQuality).
//
// What the engine has (eboot.bin of CUSA00004; the file's bytes and Ghidra):
//   the city's blocks have two texture packs each: <block>.xpps (every texture at half its size, ~10-12 MiB) and
//   <block>.hires.xpps (the same textures at full size with one more mip, ~47 MiB). The streaming update 0xfd890
//   (every frame) ranks the blocks around the camera and keeps the hires packs of the nearest few in its four slots:
//     0xfe9a0  mov r12d, 4        the number of hires blocks
//     0xfe9b0  cmp eax, 4 ; cmovle r12d, eax   (fewer when fewer candidates)
//   the first r12 ranked blocks get (or keep) theirs (0xff910 loads "%s.hires.xpp"), the other slots are released.
//   Characters, powers and the rest have no hires packs and are not affected.
//   Levels: "high" 4 (the game's) | "medium" 2 | "low" 0 (the city at the half-size textures alone). The two
//   immediates are written before the game starts; takes effect at the next start.
#pragma once

#include <string_view>

#include "base/abi.h"

namespace SecondSon::Engine::Render::TextureQuality {

/// The loader's window before the guest runs: writes the hires block count of `level` when the eboot's streaming
/// update and its bytes are the analysed ones. false (logged): the game's own.
bool Install(u64 eboot_base, std::string_view level);

} // namespace SecondSon::Engine::Render::TextureQuality
