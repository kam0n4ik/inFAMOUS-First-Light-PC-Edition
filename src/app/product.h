// SecondSonPC — the product's visible name (PC Edition, docs/PC_EDITION_UI.md). The one definition of the game
// window's title: every path that creates the window uses it (bridge/shadps4/bootstrap.cpp); it never changes while
// the game runs (full screen, a new swapchain, a resize keep the window). Save paths, the title id (CUSA00004) and
// internal names are not this.
#pragma once

#include <string_view>

namespace SecondSon::App {

/// "inFAMOUS: Second Son — PC Edition" (UTF-8; the dash is U+2014 EM DASH).
inline constexpr std::string_view kWindowTitle = "inFAMOUS: Second Son \xE2\x80\x94 PC Edition";
static_assert(kWindowTitle.size() == 35 && kWindowTitle.substr(21, 3) == "\xE2\x80\x94" &&
                  kWindowTitle.starts_with("inFAMOUS: Second Son ") && kWindowTitle.ends_with(" PC Edition"),
              "the window title is exactly \"inFAMOUS: Second Son <U+2014> PC Edition\" in UTF-8");

} // namespace SecondSon::App
