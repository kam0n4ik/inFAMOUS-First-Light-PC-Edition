// SecondSonPC — self-tests of the PC settings (see settings_checks.h).
#include "settings/settings_checks.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <fmt/format.h>

#include "app/config.h"
#include "input/binding_rules.h"
#include "input/device_tracker.h"
#include "input/gamepad_tuning.h"
#include "input/keyboard_mouse.h"
#include "settings/registry.h"
#include "settings/settings_service.h"
#include "settings/user_store.h"
#include "ui/settings_menu.h"

namespace SecondSon::Settings {

namespace {

namespace fs = std::filesystem;
using Input::InputCode;

App::Config TestConfig() {
    App::Config c;
    c.language_locale = "ru-RU";
    c.language_id = 8;
    return c;
}

std::string ReadFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void WriteText(const fs::path& p, std::string_view text) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

InputCode Key(SDL_Scancode sc) {
    return {InputCode::Kind::Key, static_cast<u16>(sc)};
}

SDL_Event KeyEvent(SDL_Scancode sc, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.scancode = sc;
    e.key.down = down;
    return e;
}

void Registry(const CheckFn& check) {
    const auto g = "Settings.registry";
    const auto all = All();
    std::set<std::string_view> ids;
    std::set<std::pair<std::string_view, std::string_view>> keys;
    bool unique_ids = true, unique_keys = true, accessors = true, labels = true, reasons = true;
    for (const auto& d : all) {
        unique_ids = unique_ids && ids.insert(d.id).second;
        if (!d.table.empty()) {
            unique_keys = unique_keys && keys.insert({d.table, d.key}).second;
            accessors = accessors && d.get != nullptr && d.set != nullptr;
        }
        labels = labels && !d.en.empty() && !d.ru.empty();
        if (d.apply == Apply::Unavailable) {
            reasons = reasons && !d.why_en.empty() && !d.why_ru.empty() && d.table.empty();
        }
    }
    check(g, "ids unique", unique_ids, fmt::format("{} settings", all.size()));
    check(g, "toml keys unique", unique_keys, fmt::format("{} stored", keys.size()));
    check(g, "stored settings have accessors", accessors, "");
    check(g, "labels in both languages", labels, "");
    check(g, "unavailable settings say why and store nothing", reasons, "");
    check(g, "every keyboard action has a setting",
          std::ranges::count_if(all, [](const Desc& d) { return d.type == Type::Binding; }) ==
              static_cast<long long>(Input::kActionCount),
          fmt::format("{} actions", Input::kActionCount));

    // the defaults are valid values, and get(set(get)) is the identity
    auto cfg = TestConfig();
    std::string bad;
    for (const auto& d : all) {
        if (d.get == nullptr) {
            continue;
        }
        std::string error;
        const Value v = d.get(cfg, d);
        if (!Validate(d, v, error)) {
            bad += fmt::format("{}: {} ({}); ", d.id, Describe(v), error);
        }
        auto copy = cfg;
        d.set(copy, d, v);
        if (!Equal(d.get(copy, d), v)) {
            bad += fmt::format("{}: round trip; ", d.id);
        }
    }
    check(g, "defaults valid, accessors round-trip", bad.empty(), bad);

    const Desc* vol = Find("audio.volume");
    std::string e;
    check(g, "range: volume 101 refused", vol != nullptr && !Validate(*vol, Value{s64{101}}, e), e);
    const Desc* sens = Find("input.mouseSensitivity");
    check(g, "range: NaN sensitivity refused", sens != nullptr && !Validate(*sens, Value{std::nan("")}, e), e);
    check(g, "range: sensitivity 0.5 accepted", sens != nullptr && Validate(*sens, Value{0.5}, e), e);
    const Desc* pm = Find("display.presentMode");
    check(g, "choice: unknown value refused", pm != nullptr && !Validate(*pm, Value{std::string("vsync")}, e), e);
    const Desc* cross = Find("keyboard.cross");
    check(g, "binding: F10 (hotkey) refused",
          cross != nullptr && !Validate(*cross, Value{Bindings{Key(SDL_SCANCODE_F10)}}, e), e);
    check(g, "binding: empty list accepted (unbound)", cross != nullptr && Validate(*cross, Value{Bindings{}}, e), e);
}

void UserFile(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.file";
    Overrides o;
    o["display.fullscreen"] = true;
    o["audio.volume"] = s64{35};
    o["input.mouseSensitivity"] = 1.0;
    o["gamepad.cameraSensitivity"] = 1.25;
    o["display.presentMode"] = std::string("fifo");
    o["display.monitor"] = std::string("Монитор \"DELL\" \\ 27");
    o["keyboard.cross"] = Bindings{Key(SDL_SCANCODE_SPACE), {InputCode::Kind::MouseButton, SDL_BUTTON_X1}};
    o["keyboard.dpadUp"] = Bindings{{InputCode::Kind::MouseWheel, 0}};
    o["keyboard.shake"] = Bindings{};
    const std::string text = SerializeUserFile(o);
    Overrides back;
    std::vector<std::string> warnings;
    std::string perr;
    const bool parsed = ParseUserText(text, back, warnings, perr);
    check(g, "save -> load round trip (every type, Cyrillic, quotes, mouse X1, wheel, unbound)",
          parsed && back == o && warnings.empty(), parsed ? fmt::format("{} keys, {} warnings", back.size(), warnings.size()) : perr);
    check(g, "a float stays a float (1.0, not 1)", text.find("mouseSensitivity = 1.0") != std::string::npos, "");

    // lenient reading: bad lines skipped, the rest used
    const std::string mixed = "[Display]\nfullscreen = true\nunknownKey = 3\n[Audio]\nvolume = 400\n"
                              "[Input]\ninvertY = \"yes\"\n[Keyboard]\ncross = \"F10\"\ncircle = \"NoSuchKey\"\n"
                              "square = \"Q\"\n[Nonsense]\nx = 1\n";
    back.clear();
    warnings.clear();
    const bool ok = ParseUserText(mixed, back, warnings, perr);
    check(g, "lenient: 2 good keys kept, 6 bad ones skipped with warnings",
          ok && back.size() == 2 && back.contains("display.fullscreen") && back.contains("keyboard.square") &&
              warnings.size() == 6,
          fmt::format("kept {}, warnings {}", back.size(), warnings.size()));
    check(g, "BOM accepted", ParseUserText("\xEF\xBB\xBF[Audio]\nvolume = 10\n", back, warnings, perr) &&
                                 back.size() == 1, perr);
    check(g, "broken TOML detected", !ParseUserText("[Display\nfullscreen = ", back, warnings, perr), perr);

    // files
    std::error_code ec;
    fs::create_directories(dir, ec);
    const auto file = dir / "settings.toml";
    fs::remove(file, ec);
    Overrides loaded;
    check(g, "missing file: SecondSonPC.toml alone",
          LoadUserFile(file, loaded).status == LoadReport::Status::Missing && loaded.empty(), "");
    std::string werr;
    const bool w1 = WriteFileAtomic(file, "[Audio]\nvolume = 10\n", werr);
    const bool w2 = WriteFileAtomic(file, text, werr);
    auto bak = file;
    bak += ".bak";
    auto tmp = file;
    tmp += ".tmp";
    check(g, "atomic write: new content, previous kept as .bak, no .tmp left",
          w1 && w2 && ReadFile(file) == text && ReadFile(bak) == "[Audio]\nvolume = 10\n" && !fs::exists(tmp, ec),
          werr);
    const auto report = LoadUserFile(file, loaded);
    check(g, "load from disk = what was saved", report.status == LoadReport::Status::Loaded && loaded == o,
          fmt::format("{} keys", loaded.size()));
    WriteText(file, "[Display]\nfullscreen = tru");
    const auto corrupt = LoadUserFile(file, loaded);
    check(g, "corrupt file: moved aside, nothing used, the original name free for the next save",
          corrupt.status == LoadReport::Status::Corrupt && loaded.empty() && fs::exists(corrupt.moved_to, ec) &&
              !fs::exists(file, ec),
          corrupt.moved_to.string());

    // the background writer coalesces a burst (a dragged slider) and writes the newest
    {
        AsyncWriter writer(dir / "burst.toml");
        for (int i = 0; i <= 50; ++i) {
            writer.Save(fmt::format("[Audio]\nvolume = {}\n", i));
        }
        const bool flushed = writer.Flush(5000);
        check(g, "async writer: 51 saves -> the last one on disk, writes coalesced",
              flushed && ReadFile(dir / "burst.toml") == "[Audio]\nvolume = 50\n" && writer.Writes() < 51,
              fmt::format("{} writes", writer.Writes()));
    }
}

void ServiceChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.service";
    const auto defaults = TestConfig();
    Service s;
    const auto file = dir / "service.toml";
    std::error_code ec;
    fs::remove(file, ec);
    s.Init(defaults, {}, defaults, file, Capability::Window | Capability::BridgePresenter | Capability::LiveVolume |
                                              Capability::RealController);
    std::vector<std::string> applied;
    s.AddApplier([&applied](const App::Config&, std::span<const Desc* const> changed) {
        for (const auto* d : changed) {
            applied.emplace_back(d->id);
        }
    });
    const Desc& vol = *Find("audio.volume");
    const Desc& inv = *Find("input.invertY");
    const Desc& pm = *Find("display.presentMode");
    const Desc& rs = *Find("graphics.renderScale");
    const Desc& sens = *Find("input.mouseSensitivity");

    check(g, "live change applied and recorded",
          s.Set(vol, Value{s64{40}}) == SetResult::Ok && std::get<s64>(s.Get(vol)) == 40 && s.IsOverridden(vol) &&
              applied == std::vector<std::string>{"audio.volume"},
          "");
    check(g, "same value: unchanged, no applier call", s.Set(vol, Value{s64{40}}) == SetResult::Unchanged &&
                                                           applied.size() == 1, "");
    check(g, "out of range refused, value kept",
          s.Set(vol, Value{s64{140}}) == SetResult::Invalid && std::get<s64>(s.Get(vol)) == 40, "");
    check(g, "unavailable setting refused (never a dead switch)",
          s.Set(rs, Value{true}) == SetResult::Unavailable && !s.Available(rs).available, "");
    check(g, "back to the default: override dropped", s.Set(vol, Value{s64{100}}) == SetResult::Ok &&
                                                          !s.IsOverridden(vol), "");
    applied.clear();
    check(g, "restart setting: stored, marked, no live applier",
          s.Set(pm, Value{std::string("fifo")}) == SetResult::Ok && s.NeedsRestart(pm) && applied.empty(), "");
    s.Set(pm, Value{std::string("mailbox")});
    check(g, "restart setting back to the started value: not marked", !s.NeedsRestart(pm), "");

    s.Set(inv, Value{true});
    s.Set(sens, Value{2.5});
    s.ResetCategory(Category::Controls);
    check(g, "reset a category: its settings back to the defaults",
          !std::get<bool>(s.Get(inv)) && std::get<double>(s.Get(sens)) == 1.0 && !s.IsOverridden(inv) &&
              !s.IsOverridden(sens),
          "");

    // all-or-nothing
    const Desc& w = *Find("display.width");
    const Desc& h = *Find("display.height");
    const std::pair<const Desc*, Value> bad_pair[] = {{&w, Value{s64{1600}}}, {&h, Value{s64{99999}}}};
    check(g, "several settings: one invalid, none changed",
          s.SetMany(bad_pair) == SetResult::Invalid && std::get<s64>(s.Get(w)) == 1280, "");

    // capability missing (--gpu-backend sspc: no bridge presenter)
    s.SetCapabilities(Capability::Window | Capability::LiveVolume | Capability::RealController);
    const Desc& fps = *Find("display.showFps");
    const auto a = s.Available(fps);
    check(g, "without the bridge's presenter: FPS counter unavailable, with the reason",
          !a.available && !a.why_ru.empty() && s.Set(fps, Value{false}) == SetResult::Unavailable,
          std::string(a.why_en));
    s.SetCapabilities(Capability::Window | Capability::BridgePresenter | Capability::LiveVolume |
                      Capability::RealController);

    // persistence: the file holds exactly the overrides
    s.Set(vol, Value{s64{55}});
    s.Set(*Find("keyboard.cross"), Value{Bindings{Key(SDL_SCANCODE_J)}});
    const bool flushed = s.Flush(5000);
    Overrides on_disk;
    const auto rep = LoadUserFile(file, on_disk);
    check(g, "saved off-thread: the file = the overrides",
          flushed && rep.status == LoadReport::Status::Loaded && on_disk == s.CurrentOverrides(),
          fmt::format("{} keys, error \"{}\"", on_disk.size(), s.LastSaveError()));
    // restart: a new process with these overrides starts with them
    auto restarted = defaults;
    ApplyOverrides(restarted, on_disk);
    check(g, "restored at the next start", restarted.volume == 55 &&
                                               restarted.input.bindings[0] == Bindings{Key(SDL_SCANCODE_J)}, "");
}

void ConfigChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Settings.config";
    std::error_code ec;
    const auto root = dir / "root";
    fs::create_directories(root / "game", ec);
    WriteText(root / "game" / "eboot.bin", "x");
    const auto toml = [&](std::string_view extra) {
        WriteText(root / "SecondSonPC.toml",
                  fmt::format("[Game]\npath = '{}'\n[Language]\nsystem = \"en-US\"\n{}", (root / "game").string(),
                              extra));
        std::vector<std::string> errors;
        auto c = App::LoadConfig(root, errors);
        return std::make_pair(std::move(c), errors);
    };
    auto [c1, e1] = toml("[Gamepad]\nvibration = 40\ninvertCameraY = true\n[Interface]\nlanguage = \"en\"\nscale = 125\n"
                         "[Graphics]\nfsr = true\n[Keyboard]\ntouchTwoFingers = [\"C\", \"Wheel Down\"]\n");
    check(g, "SecondSonPC.toml: new sections parsed through the registry",
          c1 && c1->pc.vibration == 40 && c1->pc.invert_camera_y && c1->pc.ui_language == "en" &&
              c1->pc.ui_scale == 125 && c1->pc.fsr &&
              c1->input.bindings[static_cast<size_t>(Input::Action::TouchTwoFingers)].size() == 2,
          e1.empty() ? "" : e1.front());
    auto [c2, e2] = toml("[Gamepad]\nvibration = 400\n");
    check(g, "SecondSonPC.toml: a bad new value is an error (strict as before)", !c2 && !e2.empty(),
          e2.empty() ? "" : e2.front());
    auto [c3, e3] = toml("[Gamepad]\nvibrations = 40\n");
    check(g, "SecondSonPC.toml: a misspelt new key is an error", !c3 && !e3.empty(), e3.empty() ? "" : e3.front());
    auto [c4, e4] = toml("");
    check(g, "SecondSonPC.toml without the new sections: defaults", c4 && c4->pc.vibration == 100 && !c4->pc.fsr,
          e4.empty() ? "" : e4.front());
}

void BindingChecks(const CheckFn& check) {
    const auto g = "Input.bindings";
    auto s = Input::DefaultSettings();
    check(g, "default layout: no conflicts (Esc = Options + cursor is allowed)", Input::FindConflicts(s).empty(), "");
    Input::AssignBinding(s, Input::Action::Cross, 0, Key(SDL_SCANCODE_E));
    const auto conflicts = Input::FindConflicts(s);
    check(g, "E on Cross: conflict with Circle found",
          conflicts.size() == 1 && conflicts[0].first == Input::Action::Cross &&
              conflicts[0].second == Input::Action::Circle,
          fmt::format("{} conflicts", conflicts.size()));
    check(g, "F11 refused", !Input::AssignBinding(s, Input::Action::Square, 0, Key(SDL_SCANCODE_F11)), "");
    auto t = Input::DefaultSettings();
    Input::AssignBinding(t, Input::Action::R3, 1, Key(SDL_SCANCODE_X));     // replaces Z
    Input::AssignBinding(t, Input::Action::R3, 0, Key(SDL_SCANCODE_X));     // the same input moves, not doubled
    const auto& r3 = t.bindings[static_cast<size_t>(Input::Action::R3)];
    check(g, "assign: an input listed once per action", r3.size() == 1 && r3[0] == Key(SDL_SCANCODE_X),
          fmt::format("{} inputs", r3.size()));
    Input::ClearBinding(t, Input::Action::R3, 0);
    check(g, "clear a slot", t.bindings[static_cast<size_t>(Input::Action::R3)].empty(), "");
    InputCode code;
    check(g, "names: Wheel Up, Mouse X2 parse and print back",
          Input::ParseInput("wheel up", code) && Input::InputName(code) == "Wheel Up" &&
              Input::ParseInput("Mouse X2", code) && Input::InputName(code) == "Mouse X2",
          "");
    check(g, "touchTwoFingers default: C", Input::DefaultSettings().bindings[static_cast<size_t>(
                                               Input::Action::TouchTwoFingers)] == Bindings{Key(SDL_SCANCODE_C)},
          "");
}

void KeyboardMouseChecks(const CheckFn& check) {
    const auto g = "Input.keyboard_mouse";
    Input::KeyboardMouse km;
    auto settings = Input::DefaultSettings();
    settings.bindings[static_cast<size_t>(Input::Action::DpadUp)].push_back({InputCode::Kind::MouseWheel, 0});
    km.Configure(settings);
    km.AttachWindow(nullptr, true); // logical capture, no system cursor
    u64 t = 1'000'000;
    const auto read = [&km, &t] {
        t += 16'667;
        return km.Read(t);
    };
    read();
    km.OnEvent(KeyEvent(SDL_SCANCODE_C, true));
    auto s = read();
    check(g, "C: two fingers on the touch pad and its click",
          (s.buttons & Input::Button::TouchPad) != 0 && s.touch_active && s.touch[0].down && s.touch[1].down &&
              s.touch[0].x == 480 && s.touch[1].x == 1440 && s.touch[0].y == 471 && s.touch[0].id != s.touch[1].id,
          fmt::format("buttons {:#x} ({},{}) ({},{})", s.buttons, s.touch[0].x, s.touch[0].y, s.touch[1].x,
                      s.touch[1].y));
    km.OnEvent(KeyEvent(SDL_SCANCODE_C, false));
    s = read();
    check(g, "C released: fingers lifted", !s.touch_active && (s.buttons & Input::Button::TouchPad) == 0, "");

    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 1.0f;
    wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    km.OnEvent(wheel);
    km.OnEvent(wheel); // two notches before one reading: still one press
    s = read();
    const bool pressed = (s.buttons & Input::Button::Up) != 0;
    s = read();
    check(g, "wheel up bound to D-pad up: pressed for one reading", pressed && (s.buttons & Input::Button::Up) == 0, "");

    km.OnEvent(KeyEvent(SDL_SCANCODE_W, true));
    s = read();
    const bool walking = s.ly == 0;
    km.SetMenuOpen(true);
    km.OnEvent(KeyEvent(SDL_SCANCODE_W, false)); // the release happened while the menu had the keyboard
    km.SetMenuOpen(false);
    s = read();
    check(g, "menu opened while W held: nothing stays pressed afterwards", walking && s.ly == 128 && s.lx == 128,
          fmt::format("ly {}", s.ly));

    Input::KeyboardMouse focus;
    focus.Configure(Input::DefaultSettings());
    focus.AttachWindow(nullptr, false);
    focus.OnEvent(KeyEvent(SDL_SCANCODE_G, true));
    focus.OnEvent(KeyEvent(SDL_SCANCODE_G, false));
    u64 ft = 2'000'000;
    const bool motion_on = focus.Read(ft).info.motion_mode;
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    focus.OnEvent(lost);
    ft += 20'000;
    check(g, "focus lost: motion mode (G) ends", motion_on && !focus.Read(ft).info.motion_mode, "");
}

void GamepadChecks(const CheckFn& check) {
    const auto g = "Input.gamepad";
    const Input::GamepadTuning neutral;
    bool identity = true;
    for (int x = 0; x < 256; x += 3) {
        for (int y = 0; y < 256; y += 5) {
            u8 tx = static_cast<u8>(x), ty = static_cast<u8>(y);
            Input::TuneRightStick(tx, ty, neutral);
            identity = identity && tx == x && ty == y;
        }
    }
    check(g, "neutral tuning: every stick value unchanged", identity, "");
    Input::GamepadTuning t;
    t.camera_sensitivity = 2.0;
    u8 x = 160, y = 128;
    Input::TuneRightStick(x, y, t);
    check(g, "sensitivity 2: deflection doubled, the resting centre stays 128", x == 192 && y == 128,
          fmt::format("{} {}", x, y));
    x = 255;
    y = 192;
    Input::TuneRightStick(x, y, t);
    check(g, "sensitivity 2 past the edge: clamped, direction kept (y/x ratio)", x == 255 && y == 192,
          fmt::format("{} {}", x, y));
    x = 64;
    y = 128;
    t.camera_sensitivity = 0.5;
    Input::TuneRightStick(x, y, t);
    check(g, "sensitivity 0.5: deflection halved", x == 96 && y == 128, fmt::format("{} {}", x, y));
    Input::GamepadTuning inv;
    inv.invert_camera_y = true;
    x = 40;
    y = 10;
    Input::TuneRightStick(x, y, inv);
    check(g, "invert Y: mirrored, X unchanged", x == 40 && y == 245, fmt::format("{} {}", x, y));
    x = 128;
    y = 128;
    inv.invert_camera_x = true;
    Input::TuneRightStick(x, y, inv);
    check(g, "inverted resting stick stays at the centre (128, 128)", x == 128 && y == 128, fmt::format("{} {}", x, y));
    check(g, "vibration 50 %: half, 0 %: off, 100 %: unchanged",
          Input::ScaleMotor(255, 50) == 127 && Input::ScaleMotor(200, 0) == 0 && Input::ScaleMotor(77, 100) == 77,
          "");
}

void DeviceChecks(const CheckFn& check) {
    const auto g = "Input.device";
    Input::DeviceTracker d;
    constexpr u64 ms = 1'000'000;
    d.PadButton(Input::Device::PlayStation, 1000 * ms);
    const bool to_pad = d.Active() == Input::Device::PlayStation;
    d.MouseMotion(3, 2, 1100 * ms);
    const bool nudge_kept = d.Active() == Input::Device::PlayStation;
    d.MouseMotion(60, 10, 1200 * ms);
    const bool hold_kept = d.Active() == Input::Device::PlayStation; // within 1.5 s of the switch
    d.MouseMotion(60, 10, 2700 * ms);
    const bool moved = d.Active() == Input::Device::KeyboardMouse;
    d.PadAxis(Input::Device::Xbox, 0.2, 2800 * ms);
    const bool jitter_kept = d.Active() == Input::Device::KeyboardMouse;
    d.PadButton(Input::Device::Xbox, 2850 * ms);
    const bool button_now = d.Active() == Input::Device::Xbox;
    d.KeyOrMouseButton(2900 * ms);
    check(g, "switching: buttons at once, nudges/jitter never, mouse movement after the hold",
          to_pad && nudge_kept && hold_kept && moved && jitter_kept && button_now &&
              d.Active() == Input::Device::KeyboardMouse,
          fmt::format("{} switches", d.Switches()));
    check(g, "hints: auto follows the device, a fixed style wins",
          Input::ResolvePrompts("auto", Input::Device::PlayStation) == Input::PromptStyle::PlayStation &&
              Input::ResolvePrompts("auto", Input::Device::OtherGamepad) == Input::PromptStyle::Xbox &&
              Input::ResolvePrompts("keyboard", Input::Device::PlayStation) == Input::PromptStyle::Keyboard,
          "");
}

void MenuChecks(const CheckFn& check, const fs::path& dir) {
    const auto g = "Ui.menu";
    const auto defaults = TestConfig();
    Service s;
    s.Init(defaults, {}, defaults, {}, Capability::Window | Capability::BridgePresenter | Capability::LiveVolume |
                                          Capability::RealController);
    (void)dir;
    Ui::SettingsMenu menu(s);
    Ui::MenuEnv env;
    env.window_sizes = {{1280, 720}, {1600, 900}, {1920, 1080}};
    env.now_ns = 1'000'000'000;
    menu.SetEnv(env);
    menu.Open();
    Ui::EstimateMeasure measure;
    Ui::DrawList list;

    // layout at the usual window sizes: the panel inside the window, scaled with it
    std::string layout;
    float prev_w = 0;
    bool fits = true;
    for (const auto& [w, h] : {std::pair{1280.0f, 720.0f}, {1920.0f, 1080.0f}, {2560.0f, 1440.0f}, {3840.0f, 2160.0f},
                               {800.0f, 600.0f}}) {
        auto e = env;
        e.width = w;
        e.height = h;
        menu.SetEnv(e);
        menu.Build(measure, list);
        const auto r = list.Surface();
        fits = fits && r.x >= 0 && r.y >= 0 && r.Right() <= w && r.Bottom() <= h && !list.Commands().empty();
        layout += fmt::format("{}x{}: {:.0f}x{:.0f}; ", w, h, r.w, r.h);
        if (h == 2160.0f) {
            fits = fits && r.w > prev_w * 1.4f; // 4K: about twice 1080p
        }
        if (h == 1080.0f) {
            prev_w = r.w;
        }
    }
    check(g, "layout fits and scales (720p..4K, 800x600)", fits, layout);
    menu.SetEnv(env);

    check(g, "opens on Display, first row focused", menu.CurrentCategory() == Category::Display &&
                                                       menu.FocusedId() == "display.fullscreen", menu.FocusedId());
    menu.OnNav(Ui::Nav::NextTab);
    menu.OnNav(Ui::Nav::NextTab);
    menu.OnNav(Ui::Nav::NextTab);
    check(g, "tabs: Display -> Graphics -> Audio -> Mouse", menu.CurrentCategory() == Category::Controls, "");
    menu.Focus("input.invertY");
    menu.OnNav(Ui::Nav::Accept);
    const bool toggled = std::get<bool>(s.Get(*Find("input.invertY")));
    menu.OnNav(Ui::Nav::Reset);
    check(g, "toggle with Enter, back to the default with Del",
          toggled && !std::get<bool>(s.Get(*Find("input.invertY"))), "");
    menu.Focus("input.mouseSensitivity");
    menu.OnNav(Ui::Nav::Right);
    menu.OnNav(Ui::Nav::Right);
    const double sens = std::get<double>(s.Get(*Find("input.mouseSensitivity")));
    check(g, "slider: two steps right = +0.10", std::abs(sens - 1.10) < 1e-9, fmt::format("{}", sens));

    menu.SelectCategory(Category::Graphics);
    menu.Focus("graphics.renderScale");
    menu.OnNav(Ui::Nav::Accept);
    check(g, "unavailable row: nothing changes, the reason is shown", !menu.Status().empty() &&
                                                                          s.CurrentOverrides().size() == 1,
          menu.Status());

    menu.SelectCategory(Category::Keyboard);
    menu.Focus("keyboard.cross");
    menu.OnNav(Ui::Nav::Accept);
    const bool capturing = menu.Capturing();
    menu.OnCaptureInput(Key(SDL_SCANCODE_E));
    const auto cross = std::get<Bindings>(s.Get(*Find("keyboard.cross")));
    check(g, "rebind: Enter, then E -> Cross = E, the conflict with Circle reported",
          capturing && !menu.Capturing() && cross == Bindings{Key(SDL_SCANCODE_E)} && !menu.Status().empty(),
          menu.Status());
    menu.OnNav(Ui::Nav::Accept);
    menu.OnCaptureInput(Key(SDL_SCANCODE_F10));
    check(g, "rebind to F10 refused (a SecondSonPC hotkey)",
          std::get<Bindings>(s.Get(*Find("keyboard.cross"))) == Bindings{Key(SDL_SCANCODE_E)}, menu.Status());
    menu.OnNav(Ui::Nav::Accept);
    menu.OnNav(Ui::Nav::Back);
    check(g, "Esc during capture cancels it", !menu.Capturing(), "");
    menu.OnNav(Ui::Nav::Reset);
    check(g, "Del: the action's default keys", std::get<Bindings>(s.Get(*Find("keyboard.cross"))) ==
                                                  Bindings{Key(SDL_SCANCODE_SPACE)}, "");

    // display changes are confirmed or reverted
    menu.SelectCategory(Category::Display);
    menu.Focus("display.fullscreen");
    menu.OnNav(Ui::Nav::Accept);
    const bool asked = menu.ConfirmPending() && std::get<bool>(s.Get(*Find("display.fullscreen")));
    menu.Tick(env.now_ns + Ui::SettingsMenu::kConfirmNs + 1);
    check(g, "display change: confirmation asked, reverted after 15 s without an answer",
          asked && !menu.ConfirmPending() && !std::get<bool>(s.Get(*Find("display.fullscreen"))), menu.Status());
    menu.OnNav(Ui::Nav::Accept);
    menu.OnNav(Ui::Nav::Accept); // "Keep" is focused first
    check(g, "display change: kept when confirmed",
          !menu.ConfirmPending() && std::get<bool>(s.Get(*Find("display.fullscreen"))), menu.Status());
    menu.Focus("window.size");
    menu.OnNav(Ui::Nav::Right);
    const auto cfg = s.Current();
    menu.OnNav(Ui::Nav::Accept);
    check(g, "window size: next preset sets width and height together",
          cfg.window_width == 1600 && cfg.window_height == 900, fmt::format("{}x{}", cfg.window_width, cfg.window_height));

    menu.Focus("reset.category");
    menu.OnNav(Ui::Nav::Accept);
    const bool reset_asked = menu.ResetPending();
    menu.OnNav(Ui::Nav::Left);
    menu.OnNav(Ui::Nav::Accept);
    const auto after = s.Current();
    menu.OnNav(Ui::Nav::Accept); // keep the reset display settings
    check(g, "reset the section: asked, then defaults", reset_asked && !after.fullscreen && after.window_width == 1280,
          fmt::format("fullscreen {}, {}x{}", after.fullscreen, after.window_width, after.window_height));

    menu.OnNav(Ui::Nav::Back);
    check(g, "Esc at the top asks the host to close", menu.TakeCloseRequest(), "");

    // without the bridge's presenter (sspc): the bridge-only rows are greyed with the reason
    s.SetCapabilities(Capability::Window | Capability::LiveVolume | Capability::RealController);
    menu.SelectCategory(Category::Graphics);
    menu.Focus("graphics.fsr");
    menu.OnNav(Ui::Nav::Accept);
    check(g, "--gpu-backend sspc: FSR unavailable, says why", !std::get<bool>(s.Get(*Find("graphics.fsr"))) &&
                                                                  !menu.Status().empty(), menu.Status());
}

} // namespace

void RunChecks(const CheckFn& check, const std::filesystem::path& scratch) {
    std::error_code ec;
    std::filesystem::create_directories(scratch, ec);
    Registry(check);
    UserFile(check, scratch);
    ServiceChecks(check, scratch);
    ConfigChecks(check, scratch);
    BindingChecks(check);
    KeyboardMouseChecks(check);
    GamepadChecks(check);
    DeviceChecks(check);
    MenuChecks(check, scratch);
}

} // namespace SecondSon::Settings
