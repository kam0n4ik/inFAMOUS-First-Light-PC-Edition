// SecondSonPC — the product this build is (CMake SSPC_PRODUCT: SecondSon -> SecondSonPC.exe, FirstLight ->
// FirstLightPC.exe; docs/PC_EDITION_UI.md). The runtime is one for both games; what tells the two products apart is
// here: the window's title, the names of the executable, the configuration, the log and the settings folder, and the
// game the build is for (its title id, compared with the game's param.sfo at start). Internal names (thread names,
// log categories, the namespace) stay SecondSon in both.
#pragma once

#include <string>
#include <string_view>

#if defined(SSPC_PRODUCT_FIRST_LIGHT)
#define SSPC_PRODUCT_NAME "FirstLightPC"
#else
#define SSPC_PRODUCT_NAME "SecondSonPC"
#endif

namespace SecondSon::App {

struct Product {
    std::string_view name;         ///< "SecondSonPC": exe, configuration, log, settings folder (%LOCALAPPDATA%\<name>)
    std::wstring_view wide_name;   ///< the same, for Win32
    std::string_view window_title; ///< UTF-8; the dash is U+2014 EM DASH
    std::string_view game;         ///< the game's name, for messages
    std::string_view brand;        ///< the launcher's brand line over its navigation: "SECOND SON"
    std::string_view title_id;     ///< the serial the build is for (param.sfo TITLE_ID)
    bool all_engine_hooks;         ///< every hook of src/engine has this game's sites (each still verifies its hash);
                                   ///< false: only those with a table for it (game menu, game fixes, mouse camera,
                                   ///< render size and HUD, FOV, anti-aliasing)
    bool settings_entry_measured;  ///< the game's Settings list (where its last item opens the PC settings, read from
                                   ///< the frame) was measured on this game; false: F1 opens the PC settings as well
};

#if defined(SSPC_PRODUCT_FIRST_LIGHT)
inline constexpr Product kProduct{
    .name = SSPC_PRODUCT_NAME,
    .wide_name = L"FirstLightPC",
    .window_title = "inFAMOUS: First Light \xE2\x80\x94 PC Edition",
    .game = "inFAMOUS: First Light",
    .brand = "FIRST LIGHT",
    .title_id = "CUSA00897",
    .all_engine_hooks = false, // effects quality, native engine, CITY-OPT tags: Second Son's sites only (PORTING_PLAN.md)
    .settings_entry_measured = false,
};
#else
inline constexpr Product kProduct{
    .name = SSPC_PRODUCT_NAME,
    .wide_name = L"SecondSonPC",
    .window_title = "inFAMOUS: Second Son \xE2\x80\x94 PC Edition",
    .game = "inFAMOUS: Second Son",
    .brand = "SECOND SON",
    .title_id = "CUSA00004",
    .all_engine_hooks = true,
    .settings_entry_measured = true,
};
#endif

/// "inFAMOUS: <game> — PC Edition" (UTF-8). The one definition of the game window's title: every path that creates the
/// window uses it (bridge/shadps4/bootstrap.cpp); it never changes while the game runs (full screen, a new swapchain,
/// a resize keep the window).
inline constexpr std::string_view kWindowTitle = kProduct.window_title;
static_assert(kWindowTitle.starts_with("inFAMOUS: ") && kWindowTitle.ends_with(" \xE2\x80\x94 PC Edition"),
              "the window title is \"inFAMOUS: <game> <U+2014> PC Edition\" in UTF-8");

/// "<name><extension>": ProductFile(".toml") is SecondSonPC.toml / FirstLightPC.toml
inline std::string ProductFile(std::string_view extension) {
    std::string s(kProduct.name);
    s += extension;
    return s;
}

} // namespace SecondSon::App
