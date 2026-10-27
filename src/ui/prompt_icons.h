// SecondSonPC — the game's button icons for the device in use (docs/PC_INPUT_PROMPTS.md).
//
// The game draws every button hint with one icon font: a character of the Private Use Area in its strings (U+F083 is
// the cross of "Press U+F083 to jump") and a 512x512 RGBA8 atlas of white shapes in lang_<language>_text.xpps. Codes the
// game picks at run time (yes/no, the current button of an action) end in the same physical cells, so redrawing the
// cells of the physical buttons (U+F080..U+F08D, U+F090..U+F094) changes every hint of the game, static and composed,
// in every language, without touching a string.
//
// This file: the atlas in its console layout (tiling mode 13 "1D thin": 8x8 micro tiles in Morton order, tile rows in
// order, the image stored bottom-up), where it is in the language table, and the cells drawn for the keyboard and mouse
// (the keys the player bound to each DualShock 4 button) and for an Xbox controller. PlayStation is the game's own
// atlas, byte for byte. Drawing: Direct2D/DirectWrite in software into a WIC bitmap, Roboto Condensed (assets/fonts),
// white with the symbol cut out, as the game's own icons are drawn. Any thread that is a host thread (not a guest one).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "input/device_tracker.h"
#include "input/settings.h"

namespace SecondSon::Ui::PromptIcons {

constexpr u32 kAtlasSide = 512;
constexpr u64 kAtlasBytes = u64{kAtlasSide} * kAtlasSide * 4;

/// The atlas as the console stores it -> RGBA rows top-down (the glyph table's coordinates). Both 1 MiB.
void Detile(const u8* tiled, u8* rgba);
/// The inverse of Detile.
void Tile(const u8* rgba, u8* tiled);

/// One glyph of the icon font: its code point and its cell in the atlas (top-down pixels).
struct Cell {
    u16 code = 0;
    u16 x = 0, y = 0, w = 0, h = 0;
};

/// Where the icon font is in a language table (lang_<language>_text.xpps, KCAP).
struct AtlasInfo {
    u64 data_offset = 0;  ///< the atlas's 1 MiB in the file
    u64 glyph_offset = 0; ///< the glyph table (32-byte records)
    std::vector<Cell> cells;
};

/// Finds the icon atlas by its texture id and texture descriptor (format, size, tiling) and checks that its bytes are
/// the game's (Second Son 01.07: the same atlas in all 15 languages; First Light 01.04: its own packing of the same
/// codes, the same in its 3 languages); the glyph table by its records. nullopt + `why`.
std::optional<AtlasInfo> Locate(std::string_view file, std::string* why);

/// 64-bit FNV-1a (the atlas's identity check).
u64 Fnv1a(const u8* data, u64 n);
#if defined(SSPC_PRODUCT_FIRST_LIGHT)
constexpr u64 kOriginalAtlasFnv = 0xe4854f8aa84b0129ull;
constexpr const char* kAtlasVersion = "First Light 01.04";
#else
constexpr u64 kOriginalAtlasFnv = 0x12138d8479e36e34ull;
constexpr const char* kAtlasVersion = "01.07";
#endif

/// What a cell shows for a style: drawn (`drawn` true) or the game's own (unbound key, d-pad on Xbox, other codes).
struct CellArt {
    bool drawn = false;
    std::vector<u8> alpha; ///< w * h, top-down
};

/// The cell of `cell.code` for `style` with the keyboard/mouse layout `layout` (the first input bound to the button).
CellArt Draw(const Cell& cell, Input::PromptStyle style, const Input::Settings& layout);

/// One input (a key, a mouse button) drawn as the keyboard icons are, scaled to a `side` x `side` alpha mask (top-down)
/// for the game's larger button pictures. Empty when Direct2D is unavailable.
std::vector<u8> DrawInputAlpha(Input::InputCode in, u32 side);

/// The atlas for `style`: the original (tiled, 1 MiB) with the cells redrawn; PlayStation: the original unchanged.
/// `drawn`: how many cells were redrawn. Empty when Direct2D is unavailable (`error`).
std::vector<u8> BuildAtlas(const u8* original_tiled, const std::vector<Cell>& cells, Input::PromptStyle style,
                           const Input::Settings& layout, u32* drawn, std::string* error);

/// The DualShock 4 button (or what a code shows) — the codes this module redraws.
std::string_view CodeName(u16 code);

/// RGBA top-down -> PNG (tests, diagnostics).
bool SavePng(const std::wstring& path, const u8* rgba, u32 w, u32 h);

} // namespace SecondSon::Ui::PromptIcons
