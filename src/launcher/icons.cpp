// SecondSonPC — the launcher's icons: the mockup's inline SVGs (app.js ICONS, index.html's title bar, the select's
// chevron), <rect> and <circle> written as the equivalent path data. Own code.
#include <string_view>
#include <unordered_map>

#include "launcher/canvas.h"

namespace SecondSon::Launcher {

std::string_view IconPath(std::string_view name) {
    static const std::unordered_map<std::string_view, std::string_view> kIcons = {
        // 24x24, stroked 1.8, round caps and joins
        {"home", "M3.5 10.5 12 3.5l8.5 7V20a1 1 0 0 1-1 1H15v-6H9v6H4.5a1 1 0 0 1-1-1z"},
        {"display", "M5 4h14a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2zM8.5 20h7M12 16v4"},
        {"graphics", "M12 3.2l2.3 6 6 2.3-6 2.3-2.3 6-2.3-6-6-2.3 6-2.3z"},
        {"effects", "M3 8.5h10.5a3 3 0 1 0-3-3M3 12.5h15a3 3 0 1 1-3 3M3 16.5h7"},
        {"controls", "M7.2 7.5h9.6a4.2 4.2 0 0 1 4.2 4.2v1.8a3.4 3.4 0 0 1-6.2 1.9l-.8-1.2h-4l-.8 1.2A3.4 3.4 0 0 1 3 "
                     "13.5v-1.8a4.2 4.2 0 0 1 4.2-4.2zM7.5 10.2v3.2M5.9 11.8h3.2M15.8 11h.01M17.9 13h.01"},
        {"audio", "M4 9.5h3.5l4.5-4v13l-4.5-4H4zM15.5 9a4 4 0 0 1 0 6M18.2 6.5a7.6 7.6 0 0 1 0 11"},
        {"advanced", "M4 6h9M17 6h3M4 12h3M11 12h9M4 18h11M19 18h1M13 6a2 2 0 1 0 4 0a2 2 0 1 0-4 0zM7 12a2 2 0 1 0 4 "
                     "0a2 2 0 1 0-4 0zM15 18a2 2 0 1 0 4 0a2 2 0 1 0-4 0z"},
        {"gear", "M9 12a3 3 0 1 0 6 0a3 3 0 1 0-6 0zM19.4 15a1.7 1.7 0 0 0 .3 1.8l.1.1a2 2 0 1 1-2.8 2.8l-.1-.1a1.7 1.7 "
                 "0 0 0-1.8-.3 1.7 1.7 0 0 0-1 1.5V21a2 2 0 1 1-4 0v-.1a1.7 1.7 0 0 0-1.1-1.5 1.7 1.7 0 0 0-1.8.3l-.1.1a2 "
                 "2 0 1 1-2.8-2.8l.1-.1a1.7 1.7 0 0 0 .3-1.8 1.7 1.7 0 0 0-1.5-1H3a2 2 0 1 1 0-4h.1a1.7 1.7 0 0 0 "
                 "1.5-1.1 1.7 1.7 0 0 0-.3-1.8l-.1-.1a2 2 0 1 1 2.8-2.8l.1.1a1.7 1.7 0 0 0 1.8.3H9a1.7 1.7 0 0 0 "
                 "1-1.5V3a2 2 0 1 1 4 0v.1a1.7 1.7 0 0 0 1 1.5 1.7 1.7 0 0 0 1.8-.3l.1-.1a2 2 0 1 1 2.8 2.8l-.1.1a1.7 "
                 "1.7 0 0 0-.3 1.8V9a1.7 1.7 0 0 0 1.5 1H21a2 2 0 1 1 0 4h-.1a1.7 1.7 0 0 0-1.5 1z"},
        {"gpu", "M4 7h16a1.5 1.5 0 0 1 1.5 1.5v7a1.5 1.5 0 0 1-1.5 1.5H4a1.5 1.5 0 0 1-1.5-1.5v-7A1.5 1.5 0 0 1 4 "
                "7zM6.5 12a2.5 2.5 0 1 0 5 0a2.5 2.5 0 1 0-5 0zM15 10.5h3.5M15 13.5h3.5M5 17v2.5"},
        {"cpu", "M7.5 6h9A1.5 1.5 0 0 1 18 7.5v9a1.5 1.5 0 0 1-1.5 1.5h-9A1.5 1.5 0 0 1 6 16.5v-9A1.5 1.5 0 0 1 7.5 "
                "6zM9.5 9.5h5v5h-5zM9 3v3M15 3v3M9 18v3M15 18v3M3 9h3M3 15h3M18 9h3M18 15h3"},
        {"ram", "M3.5 7h17a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1h-17a1 1 0 0 1-1-1V8a1 1 0 0 1 1-1zM6.5 10v3M10.5 10v3M14.5 "
                "10v3M18 10v3M5 16v2.5M19 16v2.5"},
        {"arrow", "M5 12h13M13 6.5l5.5 5.5-5.5 5.5"},
        {"check", "M5 12.5l4.5 4.5L19 7.5"},
        {"warn", "M12 3.8 2.8 20h18.4zM12 10v4.5M12 17.3h.01"},
        {"chev", "M9 5.5l6.5 6.5L9 18.5"},
        {"folder", "M3 6.5A1.5 1.5 0 0 1 4.5 5h4.2l2 2.2h8.8A1.5 1.5 0 0 1 21 8.7v9.8a1.5 1.5 0 0 1-1.5 1.5h-15A1.5 1.5 "
                   "0 0 1 3 18.5z"},
        {"keyboard", "M4.5 6h15a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2h-15a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2zM6.5 10h.01M10 "
                     "10h.01M13.5 10h.01M17 10h.01M6.5 14h.01M17.5 14h.01M9.5 14h5"},
        {"gamepad", "M7.2 7.5h9.6a4.2 4.2 0 0 1 4.2 4.2v1.8a3.4 3.4 0 0 1-6.2 1.9l-.8-1.2h-4l-.8 1.2A3.4 3.4 0 0 1 3 "
                    "13.5v-1.8a4.2 4.2 0 0 1 4.2-4.2zM7.5 10.2v3.2M5.9 11.8h3.2M15.8 11h.01M17.9 13h.01"},
        {"reset", "M4.5 12a7.5 7.5 0 1 0 2.2-5.3L4.5 9M4.5 4.5V9H9"},
        {"save", "M5 4h11l3 3v12a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1zM8 4v5h7V4M8 20v-6h8v6"},
        // 24x24, filled
        {"play", "M7.5 4.8v14.4L19.5 12z"},
        {"mark", "M12 2.5 14.6 9H21l-5.2 4 2 6.5L12 15.6 6.2 19.5l2-6.5L3 9h6.4z"},
        // 12x12, stroked 1.2, butt caps (the window buttons); the select's chevron: 1.6, round
        {"win-min", "M1.5 6h9"},
        {"win-max", "M1.8 1.8h8.4v8.4H1.8z"},
        {"win-close", "M2 2l8 8M10 2l-8 8"},
        {"select-chev", "M2.5 4.5 6 8l3.5-3.5"},
        // the DualShock face buttons' marks (24x24, stroked 2)
        {"ps-cross", "M8.3 8.3l7.4 7.4M15.7 8.3l-7.4 7.4"},
        {"ps-circle", "M7.4 12a4.6 4.6 0 1 0 9.2 0a4.6 4.6 0 1 0-9.2 0z"},
        {"ps-square", "M7.8 7.8h8.4v8.4H7.8z"},
        {"ps-triangle", "M12 7.3 16.8 15.6H7.2z"},
    };
    const auto it = kIcons.find(name);
    return it != kIcons.end() ? it->second : std::string_view{};
}

} // namespace SecondSon::Launcher
