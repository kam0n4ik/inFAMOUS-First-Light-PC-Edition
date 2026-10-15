// SecondSonPC — the game's own menus (pause menu, options) as SecondSonPC sees them: the PC Edition opens its
// settings from the game's settings screen and gives the menus the mouse (docs/PC_EDITION_UI.md).
//
// inFAMOUS Second Son 01.07 (CUSA00004 eboot). The game's menus are compiled scripts of its own script VM
// (core_common.sprig.xpps, strings by name hash h*101+c); a screen that stops the game calls
//   0x27db10 PausingScreenOpened(screen): count DAT_01d1f7fc += 1, the game's time scale 0 when the first opens,
//            per-kind count DAT_07e00ba0 + 0x44 + 4 * kind (kind = screen + 0x620, < 10)
//   0x27e210 PausingScreenClosed(screen): the reverse (time scale 1 when the last closes).
// Both are observed by entry probes (src/engine/guest_probe.h); the game's code runs unchanged.
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::GameMenu {

/// The game's pausing screens now.
struct State {
    int open = 0;      // pausing screens open (0: the game runs)
    int top_kind = -1; // the kind of the screen opened last (+0x620; the settings screen Esc opens: 7), -1 if none
    u64 top_screen = 0;
    u64 changes = 0; // opens + closes since start (a change of screen)
};

/// Installs the probes on the game's screen open/close functions (call before the game starts). False when the
/// eboot is not the analysed one: State stays empty and the PC Edition's menu falls back to its own key.
bool Install(u64 image_base);
bool Installed();

State Current();

/// The game's settings screen (Esc / Options during play) is the screen shown on top.
bool SettingsScreenOnTop();

/// SSPC_DIAG_MEMSNAP=<s>[,<s>...] (diagnostics): at each second since start, the writable pages of the game's image
/// are copied to SSPC_DIAG_MEMSNAP_DIR\snap_<s>.bin with a page map snap_<s>.map ("<rva hex> <size hex>" per
/// region) - two game states compared find the state's flags (tools/pc_edition/memdiff.py).
void StartMemorySnapshotsFromEnvironment(u64 image_base, u64 image_size);

} // namespace SecondSon::Engine::GameMenu
