// SecondSonPC — a TOML value of SecondSonPC.toml or of the user settings file as a typed setting value (one parser
// for both files: the registry's type and range). Own code; toml11 comes with the build (fmt/toml11 targets).
#pragma once

#include <string>

#include <toml.hpp>

#include "settings/setting_types.h"

namespace SecondSon::Settings {

/// Type and range checked (Validate). Binding: a key name or an array of key names (ParseInput names).
bool FromToml(const Desc& desc, const toml::value& value, Value& out, std::string& error);

} // namespace SecondSon::Settings
