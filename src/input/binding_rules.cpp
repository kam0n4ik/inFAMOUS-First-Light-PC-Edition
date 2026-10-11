// SecondSonPC — rules of the keyboard/mouse layout (see binding_rules.h).
#include "input/binding_rules.h"

#include <algorithm>
#include <array>

#include <SDL3/SDL_scancode.h>

namespace SecondSon::Input {

namespace {

constexpr std::array<InputCode, 3> kReserved{{
    {InputCode::Kind::Key, static_cast<u16>(SDL_SCANCODE_F1)},
    {InputCode::Kind::Key, static_cast<u16>(SDL_SCANCODE_F10)},
    {InputCode::Kind::Key, static_cast<u16>(SDL_SCANCODE_F11)},
}};

std::vector<InputCode>& Slots(Settings& s, Action a) {
    return s.bindings[static_cast<size_t>(a)];
}

} // namespace

std::span<const InputCode> ReservedInputs() {
    return kReserved;
}

bool IsReservedInput(InputCode input) {
    return std::ranges::find(kReserved, input) != kReserved.end();
}

bool MayShareInput(Action a, Action b) {
    const auto is = [&](Action x, Action y) { return (a == x && b == y) || (a == y && b == x); };
    return is(Action::Options, Action::ReleaseMouse);
}

std::vector<BindingConflict> FindConflicts(const Settings& settings) {
    std::vector<BindingConflict> out;
    for (size_t a = 0; a < kActionCount; ++a) {
        for (size_t b = a + 1; b < kActionCount; ++b) {
            const auto first = static_cast<Action>(a), second = static_cast<Action>(b);
            if (MayShareInput(first, second)) {
                continue;
            }
            for (const auto& in : settings.bindings[a]) {
                const auto& other = settings.bindings[b];
                const bool shared = std::ranges::find(other, in) != other.end();
                const bool listed = std::ranges::any_of(out, [&](const BindingConflict& c) {
                    return c.first == first && c.second == second && c.input == in;
                });
                if (shared && !listed) {
                    out.push_back({first, second, in});
                }
            }
        }
    }
    return out;
}

std::vector<Action> ActionsUsing(const Settings& settings, InputCode input) {
    std::vector<Action> out;
    for (size_t a = 0; a < kActionCount; ++a) {
        if (std::ranges::find(settings.bindings[a], input) != settings.bindings[a].end()) {
            out.push_back(static_cast<Action>(a));
        }
    }
    return out;
}

bool AssignBinding(Settings& settings, Action action, size_t slot, InputCode input) {
    if (IsReservedInput(input)) {
        return false;
    }
    auto& slots = Slots(settings, action);
    // The same input in another slot of this action goes away first (no "Space, Space").
    for (size_t i = 0; i < slots.size(); ++i) {
        if (slots[i] == input && i != slot) {
            slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(i));
            if (i < slot) {
                --slot;
            }
            break;
        }
    }
    if (slot < slots.size()) {
        slots[slot] = input;
    } else if (std::ranges::find(slots, input) == slots.end()) {
        slots.push_back(input);
    }
    return true;
}

void ClearBinding(Settings& settings, Action action, size_t slot) {
    auto& slots = Slots(settings, action);
    if (slot < slots.size()) {
        slots.erase(slots.begin() + static_cast<std::ptrdiff_t>(slot));
    }
}

} // namespace SecondSon::Input
