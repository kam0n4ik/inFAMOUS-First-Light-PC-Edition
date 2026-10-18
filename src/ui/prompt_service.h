// SecondSonPC — the game's button icons follow the input device (docs/PC_INPUT_PROMPTS.md).
//
// The icon atlas of the game's language table (ui/prompt_icons.h) is replaced as the game reads the table (the file
// on disk is unchanged), and rewritten where the game keeps it in its memory whenever the style changes: the last
// device used ([Interface] prompts = auto) or the style chosen, and for the keyboard the current key bindings. A
// rewrite checks first that those bytes are still exactly what was written there (the game may have freed or reused
// the memory: then the copy is forgotten, never written), and tells the GPU's caches about the write (the texture is
// uploaded again from the new bytes). PlayStation = the game's own atlas, byte for byte.
#pragma once

#include "input/device_tracker.h"

namespace SecondSon::Ui::PromptService {

/// Hooks the language tables (once, before the game opens them). Main thread.
void Install();

/// The style the game's icons should have now (setting + active device + layout); rewritten when it changed, at
/// most once per kMinGapNs (a later call applies the last wish). Main thread.
void Update(u64 now_ns);
constexpr u64 kMinGapNs = 300'000'000;

/// The style now in the game's memory, and how many rewrites were done (diagnostics, tests).
Input::PromptStyle Applied();
u32 Rewrites();

} // namespace SecondSon::Ui::PromptService
