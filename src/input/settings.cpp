// SecondSonPC — keyboard/mouse layout: default bindings and input names.
#include "input/settings.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

namespace SecondSon::Input {

namespace {

constexpr std::array<std::string_view, kActionCount> kActionNames{
    "cross",       "circle",     "square",   "triangle",  "l1",        "r1",
    "l2",          "r2",         "l3",       "r3",        "options",   "touchPad",
    "dpadUp",      "dpadDown",   "dpadLeft", "dpadRight", "moveForward", "moveBack",
    "moveLeft",    "moveRight",  "walk",     "releaseMouse", "motionMode", "shake",
    "touchDrag",   "touchTwoFingers",
};

struct MouseName {
    std::string_view name;
    u16 button;
};
constexpr MouseName kMouseNames[] = {
    {"mouse left", SDL_BUTTON_LEFT},   {"mouse right", SDL_BUTTON_RIGHT},
    {"mouse middle", SDL_BUTTON_MIDDLE}, {"mouse x1", SDL_BUTTON_X1},
    {"mouse x2", SDL_BUTTON_X2},
};

/// MouseWheel codes 0..3.
constexpr std::string_view kWheelNames[] = {"wheel up", "wheel down", "wheel left", "wheel right"};

/// "mouse left" -> "Mouse Left".
std::string TitleCase(std::string_view lower) {
    std::string s(lower);
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 0 || s[i - 1] == ' ') {
            s[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(s[i])));
        }
    }
    return s;
}

/// Short names people type for keys whose SDL name is longer.
constexpr std::pair<std::string_view, std::string_view> kAliases[] = {
    {"enter", "Return"},       {"esc", "Escape"},          {"shift", "Left Shift"},
    {"lshift", "Left Shift"},  {"rshift", "Right Shift"},  {"ctrl", "Left Ctrl"},
    {"lctrl", "Left Ctrl"},    {"rctrl", "Right Ctrl"},    {"alt", "Left Alt"},
    {"lalt", "Left Alt"},      {"ralt", "Right Alt"},
};

std::string Lower(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return std::tolower(c); });
    return out;
}

InputCode Key(SDL_Scancode sc) {
    return {InputCode::Kind::Key, static_cast<u16>(sc)};
}

InputCode Mouse(u16 button) {
    return {InputCode::Kind::MouseButton, button};
}

} // namespace

std::string_view ActionName(Action action) {
    return kActionNames[static_cast<size_t>(action)];
}

Settings DefaultSettings() {
    Settings s;
    auto bind = [&s](Action a, std::initializer_list<InputCode> inputs) {
        s.bindings[static_cast<size_t>(a)] = inputs;
    };
    bind(Action::Cross, {Key(SDL_SCANCODE_SPACE)});
    bind(Action::Circle, {Key(SDL_SCANCODE_E)});
    bind(Action::Square, {Key(SDL_SCANCODE_V)});
    bind(Action::Triangle, {Key(SDL_SCANCODE_F)});
    bind(Action::L1, {Key(SDL_SCANCODE_Q)});
    bind(Action::R1, {Key(SDL_SCANCODE_R)});
    bind(Action::L2, {Mouse(SDL_BUTTON_RIGHT)});
    bind(Action::R2, {Mouse(SDL_BUTTON_LEFT)});
    bind(Action::L3, {Key(SDL_SCANCODE_LSHIFT)});
    bind(Action::R3, {Mouse(SDL_BUTTON_MIDDLE), Key(SDL_SCANCODE_Z)});
    bind(Action::Options, {Key(SDL_SCANCODE_RETURN), Key(SDL_SCANCODE_ESCAPE)});
    bind(Action::TouchPad, {Key(SDL_SCANCODE_TAB)});
    bind(Action::DpadUp, {Key(SDL_SCANCODE_UP)});
    bind(Action::DpadDown, {Key(SDL_SCANCODE_DOWN)});
    bind(Action::DpadLeft, {Key(SDL_SCANCODE_LEFT)});
    bind(Action::DpadRight, {Key(SDL_SCANCODE_RIGHT)});
    bind(Action::MoveForward, {Key(SDL_SCANCODE_W)});
    bind(Action::MoveBack, {Key(SDL_SCANCODE_S)});
    bind(Action::MoveLeft, {Key(SDL_SCANCODE_A)});
    bind(Action::MoveRight, {Key(SDL_SCANCODE_D)});
    bind(Action::Walk, {Key(SDL_SCANCODE_LCTRL)});
    bind(Action::ReleaseMouse, {Key(SDL_SCANCODE_ESCAPE)});
    bind(Action::MotionMode, {Key(SDL_SCANCODE_G)});
    bind(Action::Shake, {Key(SDL_SCANCODE_X)});
    bind(Action::TouchDrag, {Key(SDL_SCANCODE_T)});
    bind(Action::TouchTwoFingers, {Key(SDL_SCANCODE_C)});
    return s;
}

bool ParseInput(std::string_view name, InputCode& out) {
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.front()))) {
        name.remove_prefix(1);
    }
    while (!name.empty() && std::isspace(static_cast<unsigned char>(name.back()))) {
        name.remove_suffix(1);
    }
    if (name.empty()) {
        return false;
    }
    const std::string lower = Lower(name);
    for (const auto& m : kMouseNames) {
        if (lower == m.name) {
            out = Mouse(m.button);
            return true;
        }
    }
    for (size_t w = 0; w < std::size(kWheelNames); ++w) {
        if (lower == kWheelNames[w]) {
            out = {InputCode::Kind::MouseWheel, static_cast<u16>(w)};
            return true;
        }
    }
    std::string sdl_name(name);
    for (const auto& [alias, real] : kAliases) {
        if (lower == alias) {
            sdl_name = real;
            break;
        }
    }
    const SDL_Scancode sc = SDL_GetScancodeFromName(sdl_name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) {
        return false;
    }
    out = Key(sc);
    return true;
}

std::string InputName(InputCode input) {
    if (input.kind == InputCode::Kind::MouseButton) {
        for (const auto& m : kMouseNames) {
            if (m.button == input.code) {
                return TitleCase(m.name);
            }
        }
        return "Mouse " + std::to_string(input.code);
    }
    if (input.kind == InputCode::Kind::MouseWheel) {
        return input.code < std::size(kWheelNames) ? TitleCase(kWheelNames[input.code])
                                                   : "Wheel " + std::to_string(input.code);
    }
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(input.code));
    return (n != nullptr && *n != '\0') ? n : "Scancode " + std::to_string(input.code);
}

} // namespace SecondSon::Input
