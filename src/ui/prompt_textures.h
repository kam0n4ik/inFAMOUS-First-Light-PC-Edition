// SecondSonPC — the game's button pictures that are not glyphs of its icon font (docs/PC_INPUT_PROMPTS.md): the
// graffiti tutorial's hand holding the controller (sideways, rattled) in every graffiti location pack, the touch pad
// print of the drain prompt and the d-pad wheels in core_common.sprig.xpps. For the keyboard each is drawn again with
// the keys bound now; for a controller the game's own bytes stay. BC3 (DXT5) textures in the console layout (tiling
// mode 13 on 4x4 blocks, rows stored bottom-up), found at fixed places checked by the hash of their original bytes.
#pragma once

#include <string_view>
#include <vector>

#include "base/abi.h"
#include "input/device_tracker.h"
#include "input/settings.h"

namespace SecondSon::Ui::PromptTextures {

enum class PictureKind : u8 {
    kSideways256,
    kSideways128,
    kRattle1024,
    kRattle512,
    kTouchPrint256,
    kDpad256,
    kDpadKarma256,
    kDpad64,
};

/// A picture in a pack: the bytes of its texture data at `offset` of the file named `pack` (the game's file name).
struct Location {
    const char* pack;
    u64 offset;
    PictureKind kind;
};

/// The places of the pictures in a pack (`pack`: its file name, e.g. "a8.hires.xpps"); empty for most packs.
std::vector<const Location*> InPack(std::string_view pack);

/// The size of a picture's texture data (BC3: one byte per pixel).
u64 Bytes(PictureKind kind);

/// True when `data` (Bytes(kind)) are the game's own bytes of that picture (version 01.07).
bool IsOriginal(PictureKind kind, const u8* data);

/// The picture for `style`: the keyboard's drawn from `original` with the bindings of `layout`; any other style (or a
/// picture that cannot be drawn) gives the original bytes back.
std::vector<u8> Build(PictureKind kind, const u8* original, Input::PromptStyle style, const Input::Settings& layout);

/// BC3 texture in the console layout <-> RGBA rows as stored (tests).
void DecodeBc3(const u8* tiled, u32 w, u32 h, u8* rgba);
void EncodeBc3(const u8* rgba, u32 w, u32 h, u8* tiled);

} // namespace SecondSon::Ui::PromptTextures
