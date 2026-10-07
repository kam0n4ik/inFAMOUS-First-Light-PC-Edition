// SecondSonPC — SecondSonPC.toml parser. Wrong types are errors, never silent defaults.
#include "app/config.h"

#include <fmt/format.h>
#include <toml.hpp>

#include "runtime/language.h"

namespace SecondSon::App {

namespace {

std::filesystem::path Utf8Path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

class Reader {
public:
    Reader(const toml::value& root, std::vector<std::string>& errors) : root_{root}, errors_{errors} {}

    const toml::value* Find(const char* table, const char* key) const {
        if (!root_.is_table() || !root_.contains(table)) {
            return nullptr;
        }
        const auto& t = root_.at(table);
        if (!t.is_table() || !t.contains(key)) {
            return nullptr;
        }
        return &t.at(key);
    }

    void String(const char* table, const char* key, std::string& out) const {
        if (const auto* v = Find(table, key)) {
            if (v->is_string()) {
                out = v->as_string();
            } else {
                Bad(table, key, "a string");
            }
        }
    }

    void Bool(const char* table, const char* key, bool& out) const {
        if (const auto* v = Find(table, key)) {
            if (v->is_boolean()) {
                out = v->as_boolean();
            } else {
                Bad(table, key, "true/false");
            }
        }
    }

    void UInt(const char* table, const char* key, u32& out, u32 min, u32 max) const {
        if (const auto* v = Find(table, key)) {
            if (v->is_integer() && v->as_integer() >= min && v->as_integer() <= max) {
                out = static_cast<u32>(v->as_integer());
            } else {
                Bad(table, key, fmt::format("an integer in [{}, {}]", min, max));
            }
        }
    }

    void Number(const char* table, const char* key, double& out, double min, double max) const {
        if (const auto* v = Find(table, key)) {
            double d = 0.0;
            if (v->is_floating()) {
                d = v->as_floating();
            } else if (v->is_integer()) {
                d = static_cast<double>(v->as_integer());
            } else {
                Bad(table, key, "a number");
                return;
            }
            if (d < min || d > max) {
                Bad(table, key, fmt::format("a number in [{}, {}]", min, max));
                return;
            }
            out = d;
        }
    }

    void Error(std::string message) const {
        errors_.push_back(std::move(message));
    }

private:
    void Bad(const char* table, const char* key, std::string_view expected) const {
        errors_.push_back(fmt::format("[{}] {} must be {}", table, key, expected));
    }

    const toml::value& root_;
    std::vector<std::string>& errors_;
};

} // namespace

std::optional<Config> LoadConfig(const std::filesystem::path& root, std::vector<std::string>& errors) {
    Config c;
    c.root = root;
    c.file = root / "SecondSonPC.toml";

    toml::value data;
    try {
        data = toml::parse(c.file);
    } catch (const std::exception& e) {
        errors.push_back(fmt::format("cannot parse {}: {}", c.file.string(), e.what()));
        return std::nullopt;
    }
    const Reader r{data, errors};

    std::string game = "E:\\shdps4\\CUSA00004";
    r.String("Game", "path", game);
    c.game_dir = Utf8Path(game);
    if (!std::filesystem::is_regular_file(c.game_dir / "eboot.bin")) {
        r.Error(fmt::format("[Game] path: {} does not contain eboot.bin", game));
    }

    c.language_locale = "en-US";
    r.String("Language", "system", c.language_locale);
    if (const auto* lang = Runtime::FindLanguageByLocale(c.language_locale)) {
        c.language_id = lang->id;
        c.language_locale = std::string(lang->locale);
    } else {
        r.Error(fmt::format("[Language] system = \"{}\" is not a PS4 system language "
                            "(use \"en-US\" or \"ru-RU\")",
                            c.language_locale));
    }

    r.String("System", "name", c.system_name);
    if (c.system_name.size() > 64) {
        r.Error("[System] name must be 0..64 bytes (PS4 system name limit)");
    }
    std::string enter = "cross";
    r.String("System", "enterButton", enter);
    if (enter == "cross" || enter == "circle") {
        c.enter_button_cross = enter == "cross";
    } else {
        r.Error("[System] enterButton must be \"cross\" or \"circle\"");
    }

    std::string user_data = "userdata";
    std::string logs = "logs";
    r.String("Paths", "userData", user_data);
    r.String("Paths", "logs", logs);
    const auto resolve = [&](const std::string& p) {
        const auto path = Utf8Path(p);
        return path.is_absolute() ? path : root / path;
    };
    c.user_data_dir = resolve(user_data);
    c.logs_dir = resolve(logs);

    r.UInt("Display", "width", c.window_width, 640, 7680);
    r.UInt("Display", "height", c.window_height, 360, 4320);
    r.Bool("Display", "fullscreen", c.fullscreen);
    r.UInt("Display", "vblankFrequency", c.vblank_hz, 30, 360);

    r.Number("Input", "mouseSensitivity", c.mouse_sensitivity, 0.01, 100.0);
    r.Bool("Input", "invertY", c.invert_y);

    std::string level = "info";
    r.String("Log", "level", level);
    if (const auto parsed = Log::ParseLevel(level)) {
        c.log_level = *parsed;
    } else {
        r.Error("[Log] level must be trace|debug|info|warning|error|critical|off");
    }
    r.String("Log", "bridgeFilter", c.bridge_log_filter);

    r.Bool("Trace", "importCounter", c.import_counter);
    r.UInt("Trace", "dumpIntervalSeconds", c.dump_interval_s, 1, 3600);

    if (!errors.empty()) {
        return std::nullopt;
    }
    return c;
}

} // namespace SecondSon::App
