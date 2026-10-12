// SecondSonPC — the registry of PC settings: every setting the in-game menu shows, with its SecondSonPC.toml key,
// type, range, how it applies and its labels (docs/PC_SETTINGS_IMPLEMENTATION.md §3). Own code.
#pragma once

#include <span>
#include <string>
#include <string_view>

#include "settings/setting_types.h"

namespace SecondSon::Settings {

/// All settings, in menu order (category by category).
std::span<const Desc> All();
const Desc* Find(std::string_view id);
const Desc* FindByKey(std::string_view table, std::string_view key);

std::string_view CategoryId(Category category); // "display"
std::string_view CategoryLabel(Category category, bool ru);
std::string_view ActionLabel(Input::Action action, bool ru);
/// The label of a Choice value ("" if it is not one of the choices).
std::string_view ChoiceLabel(const Desc& desc, std::string_view value, bool ru);

/// Type, range, choice membership, text and binding limits (no reserved hotkey). `error` says what is wrong.
bool Validate(const Desc& desc, const Value& value, std::string& error);
bool Equal(const Value& a, const Value& b);
/// For logs and tests: true, 1.5, "mailbox", [Space, Mouse Left].
std::string Describe(const Value& value);

} // namespace SecondSon::Settings
