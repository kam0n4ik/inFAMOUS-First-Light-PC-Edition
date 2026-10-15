// SecondSonPC — where the game's menu shows its chosen item, read from the presented frame (PC Edition mouse support,
// docs/PC_EDITION_UI.md).
//
// The game's menus are compiled scripts of its own script VM: SecondSonPC has no list of their items or positions.
// What every menu of the game does show is its chosen item written in yellow (the settings screen: items white, the
// chosen one ~#F5B800). While the main thread asks for it (SetWanted: the game's Settings screen is open), the native
// backend copies a narrow strip of the displayed frame at each flip (the item column of the Settings list: frame x
// kStripX .. +kStripW, y kStripY .. +kStripH of 1920x1080) to host memory and FindFocus finds the yellow row there.
// The mouse then moves the game's own focus with d-pad presses until the yellow row is under the cursor - the game's
// code and its menu stay unchanged. Thread-safe.
#pragma once

#include "base/abi.h"

namespace SecondSon::Gpu::MenuFocus {

/// The strip read back (coordinates of the game's 1920x1080 frame).
constexpr u32 kStripX = 410;
constexpr u32 kStripW = 128;
constexpr u32 kStripY = 180;
constexpr u32 kStripH = 820;
/// The list's scroll bar (same rows): its thumb is a grey (~79) block, at the bottom when the list's end is shown.
constexpr u32 kBarX = 1028;
constexpr u32 kBarW = 8;

struct Row {
    bool valid = false;        // a frame was read while wanted
    bool found = false;        // it shows a yellow row
    float top = 0, bottom = 0; // the yellow text's rows (frame y, 1920x1080)
    bool bar = false;          // the scroll bar's thumb was found
    float thumb_top = 0, thumb_bottom = 0;
    u64 seq = 0;               // frames read so far (a newer reading has a larger one)
};

/// Main thread: the readback is wanted (the game's Settings screen is open) or not (nothing is copied).
void SetWanted(bool wanted);
bool Wanted();

/// GPU thread: one reading (RGBA8): the item strip `w` x `h` and the scroll bar strip `bw` x `h`; their first row is
/// frame row `y0`, `scale` frame rows per strip row.
void Publish(const u8* rgba, u32 w, const u8* bar, u32 bw, u32 h, float y0, float scale);
/// The latest reading.
Row Latest();

/// The yellow row of an RGBA8 strip (tests): rows with at least 3 yellow texels, the largest cluster of them.
Row FindFocus(const u8* rgba, u32 w, u32 h, float y0, float scale);
/// The scroll bar's thumb in `row` (tests).
void FindThumb(const u8* bar, u32 bw, u32 h, float y0, float scale, Row& row);

} // namespace SecondSon::Gpu::MenuFocus
