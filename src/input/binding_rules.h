// SecondSonPC — rules of the keyboard/mouse layout the settings menu edits: reserved hotkeys, conflicts (one input
// on two actions), assigning and clearing a binding slot. Native code, no SDL calls.
#pragma once

#include <span>
#include <vector>

#include "input/settings.h"

namespace SecondSon::Input {

/// SecondSonPC's own hotkeys, handled before any action: F1 (settings menu), F10 (FPS counter), F11 (full screen).
/// No action may take them (they would never reach it).
std::span<const InputCode> ReservedInputs();
bool IsReservedInput(InputCode input);

/// Two actions meant to share an input: Esc is both Options (the game's pause) and ReleaseMouse (the cursor).
bool MayShareInput(Action a, Action b);

struct BindingConflict {
    Action first;  // first < second
    Action second;
    InputCode input;
};

/// Every pair of actions bound to the same input (each pair and input once), except the pairs that may share.
std::vector<BindingConflict> FindConflicts(const Settings& settings);

/// Actions bound to `input`.
std::vector<Action> ActionsUsing(const Settings& settings, InputCode input);

/// Binds `input` to `action` in slot `slot` (0 = primary, 1 = secondary): replaces that slot or, past the end,
/// appends; an input is listed once per action (an earlier copy in another slot is removed). Reserved inputs are
/// refused (false, nothing changes).
bool AssignBinding(Settings& settings, Action action, size_t slot, InputCode input);

/// Removes slot `slot` of `action` (nothing past the end).
void ClearBinding(Settings& settings, Action action, size_t slot);

} // namespace SecondSon::Input
