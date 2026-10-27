// SecondSonPC — the launcher's pages (app.h). Layout, sizes and texts: the HTML mockup
// (styles.css, app.js); values, ranges and availability: the settings registry and a Settings::Service of its own on the
// user settings file. Own code.
#include "app/product.h"
#include "launcher/app.h"

#include <windows.h>

#include <shellapi.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <optional>

#include "app/config.h"
#include "app/paths.h"
#include "input/binding_rules.h"
#include "launcher/launch.h"
#include "launcher/system_info.h"
#include "settings/experiments.h"
#include "settings/registry.h"
#include "settings/settings_service.h"
#include "settings/user_store.h"

namespace SecondSon::Launcher {

namespace {

using namespace Theme;
namespace S = Settings;
using Input::Action;

// ---- metrics (styles.css)
constexpr float kTitleH = 36;          // .titlebar, its border-bottom included
constexpr float kNavW = 212;           // .sidenav, its border-right included
constexpr float kGutter = 15;          // .page-scroll: scrollbar-gutter: stable (Chrome's scrollbar on Windows)
constexpr float kActionsH = 59;        // .page-actions: 1 + 12 + 34 + 12
constexpr float kGroupMax = 820;       // .group
constexpr float kCtlW = 300;           // .row: minmax(0, 1fr) 300px
constexpr float kLabelLh = 14 * kLine; // 20.3
constexpr float kHelpLh = 12.5f * kLine;
constexpr float kGroupTitleH = 13 + 12 * kLine + 11 + 1; // .group > h2 with its border

constexpr float LineOf(float size) {
    return size * kLine;
}

const Font kBody = F(14);        // body
const Font kLabel = F(14, 500);  // .row-label
const Font kHelp = F(12.5f);     // .row-help, .row-reason

Font Tab(Font f) {
    f.tabular = true;
    return f;
}
Font Mono(float size) {
    return Font{.size = size, .mono = true};
}
Font Upper(float size, int weight, float tracking) {
    return F(size, weight, tracking);
}

// ---- the mockup's inline <b>
struct Rich {
    std::string text;
    std::vector<Span> spans;
};
Rich ParseRich(std::string_view m, int weight = 700, std::optional<Color> color = {}) {
    Rich r;
    u32 open = 0;
    for (size_t i = 0; i < m.size();) {
        if (m.substr(i, 3) == "<b>") {
            open = static_cast<u32>(r.text.size());
            i += 3;
        } else if (m.substr(i, 4) == "</b>") {
            Span s{.begin = open, .end = static_cast<u32>(r.text.size()), .weight = weight};
            if (color) {
                s.color = true;
                s.rgb = *color;
            }
            r.spans.push_back(s);
            i += 4;
        } else {
            r.text += m[i++];
        }
    }
    return r;
}

// ---- sections (app.js SECTIONS)
enum Sec : int { kHome, kDisplay, kGraphics, kEffects, kControls, kAudio, kAdvanced, kSecCount };
struct SecInfo {
    std::string_view id, label, icon, lead;
};
constexpr SecInfo kSecs[kSecCount] = {
    {"home", "Главная", "home", ""},
    {"display", "Экран", "display",
     "Окно, монитор и частота кадров. Значения применяются при запуске игры; те же пункты есть в игре: Esc → "
     "«Настройки» → «Дополнительные настройки»."},
    {"graphics", "Графика", "graphics",
     "Масштабирование, генерация кадров и камера. Пресетов качества нет: каждый пункт меняет только то, что описано."},
    {"effects", "Эффекты", "effects",
     "Пост-эффекты, дым и частицы. Огонь, искры и логика способностей не меняются."},
    {"controls", "Управление", "controls",
     "Клавиатура и мышь работают как контроллер PS4: каждое действие — кнопка или стик DualShock 4."},
    {"audio", "Звук", "audio",
     "Только то, что есть в SecondSonPC. Музыка, эффекты и речь настраиваются в самой игре."},
    {"advanced", "Дополнительно", "advanced",
     "Файлы, шейдеры, язык игры и восстановление настроек. Эксперименты — в отдельном закрытом разделе внизу."},
};

// The settings each section saves and resets (the registry's ids; the bindings and the experiments added at Init).
const std::vector<std::string_view> kSectionIds[kSecCount] = {
    {},
    {"display.fullscreen", "display.fullscreenMode", "display.width", "display.height", "display.monitor",
     "display.presentMode", "display.frameRateLimit", "display.showFps", "display.resolution", "display.aspectRatio",
     "display.hudSafeArea", "display.hudArea"},
    {"upscaling.upscaler", "upscaling.quality", "upscaling.sharpness", "graphics.renderScale",
     "upscaling.frameGeneration", "upscaling.frameGenMultiplier", "upscaling.reflex", "graphics.fov",
     "graphics.reflectionQuality", "graphics.anisotropy", "graphics.antiAliasing", "graphics.shadowQuality",
     "graphics.textureQuality"},
    {"graphics.motionBlur", "graphics.motionBlurStrength", "graphics.smokeQuality", "graphics.particleResolution",
     "graphics.rainShadows"},
    {"input.mouseSensitivity", "input.invertY", "input.mouseDeadzone", "input.captureMouse", "input.motionSensitivity",
     "input.sideways", "gamepad.vibration", "gamepad.cameraSensitivity", "gamepad.invertCameraX",
     "gamepad.invertCameraY", "interface.prompts", "game.enterButton", "gamepad.menuButton"},
    {"audio.volume", "audio.device"},
    {"shaders.predictAll", "shaders.precompile", "shaders.showProgress", "shaders.maxSize", "shaders.threads",
     "game.language"},
};

// ---- the key bindings (app.js ACTIONS: the mockup's order, labels and notes)
struct BindText {
    Action action;
    std::string_view label, help;
};
struct BindGroup {
    std::string_view title;
    std::vector<BindText> items;
};
const BindGroup kBindGroups[] = {
    {"Движение",
     {{Action::MoveForward, "Вперёд", ""},
      {Action::MoveBack, "Назад", ""},
      {Action::MoveLeft, "Влево", ""},
      {Action::MoveRight, "Вправо", ""},
      {Action::Walk, "Шаг (удерживать)", "Левый стик отклоняется наполовину, пока клавиша удерживается."}}},
    {"Кнопки контроллера",
     {{Action::Cross, "Крест (прыжок)", ""},
      {Action::Circle, "Круг", ""},
      {Action::Square, "Квадрат", ""},
      {Action::Triangle, "Треугольник", ""},
      {Action::L1, "L1", ""},
      {Action::R1, "R1", ""},
      {Action::L2, "L2", ""},
      {Action::R2, "R2", ""},
      {Action::L3, "L3 (нажатие левого стика)", ""},
      {Action::R3, "R3 (нажатие правого стика)", ""},
      {Action::Options, "Options (пауза)", ""},
      {Action::TouchPad, "Нажатие сенсорной панели",
       "Палец в центре панели и её нажатие, пока клавиша удерживается (поглощение, меню улучшений)."}}},
    {"Крестовина",
     {{Action::DpadUp, "Крестовина вверх", ""},
      {Action::DpadDown, "Крестовина вниз", ""},
      {Action::DpadLeft, "Крестовина влево", ""},
      {Action::DpadRight, "Крестовина вправо", ""}}},
    {"Особые действия",
     {{Action::ReleaseMouse, "Отпустить курсор",
       "Отпускает курсор мыши и снова захватывает его. По умолчанию делит Esc с Options."},
      {Action::MotionMode, "Контроллер «боком» (G)", "Режим граффити: мышь поворачивает контроллер, а не камеру."},
      {Action::Shake, "Встряхнуть контроллер", "Трясёт контроллер, пока клавиша удерживается (баллончик краски)."},
      {Action::TouchDrag, "Вести пальцем по панели",
       "Пока клавиша удерживается, мышь ведёт палец по сенсорной панели (свайпы)."},
      {Action::TouchTwoFingers, "Нажатие панели двумя пальцами",
       "Действия игры «двумя пальцами». В игре ещё не проверено."}}},
};
std::string_view ActionLabel(Action a) {
    for (const auto& g : kBindGroups) {
        for (const auto& b : g.items) {
            if (b.action == a) {
                return b.label;
            }
        }
    }
    return S::ActionLabel(a, true);
}
std::string BindingId(Action a) {
    return fmt::format("keyboard.{}", Input::ActionName(a));
}

/// The mockup's key names (app.js KEY_NAMES, MOUSE_NAMES) for the registry's (SDL's) names.
std::string KeyLabel(Input::InputCode c) {
    const std::string n = Input::InputName(c);
    static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
        {"Space", "Пробел"},          {"Return", "Enter"},          {"Escape", "Esc"},
        {"Up", "↑"},                  {"Down", "↓"},                {"Left", "←"},
        {"Right", "→"},               {"Mouse Left", "ЛКМ"},        {"Mouse Right", "ПКМ"},
        {"Mouse Middle", "СКМ"},      {"Mouse X1", "Мышь 4"},       {"Mouse X2", "Мышь 5"},
        {"Wheel Up", "Колесо вверх"}, {"Wheel Down", "Колесо вниз"}, {"Wheel Left", "Колесо влево"},
        {"Wheel Right", "Колесо вправо"},
    };
    for (const auto& [from, to] : kNames) {
        if (_stricmp(n.c_str(), std::string(from).c_str()) == 0) {
            return std::string(to);
        }
    }
    return n;
}

/// A keyboard scan code of WM_KEYDOWN (0xE0xx: extended) as an SDL scancode (USB HID usage); 0: none.
u16 SdlScancode(u32 scan) {
    const u32 s = scan & 0xFF;
    if ((scan & 0xE000) == 0xE000) {
        switch (s) {
        case 0x1C: return 88;  // keypad Enter
        case 0x1D: return 228; // right Ctrl
        case 0x35: return 84;  // keypad /
        case 0x37: return 70;  // Print Screen
        case 0x38: return 230; // right Alt
        case 0x45: return 83;  // Num Lock
        case 0x47: return 74;  // Home
        case 0x48: return 82;  // Up
        case 0x49: return 75;  // Page Up
        case 0x4B: return 80;  // Left
        case 0x4D: return 79;  // Right
        case 0x4F: return 77;  // End
        case 0x50: return 81;  // Down
        case 0x51: return 78;  // Page Down
        case 0x52: return 73;  // Insert
        case 0x53: return 76;  // Delete
        case 0x5B: return 227; // left Win
        case 0x5C: return 231; // right Win
        case 0x5D: return 101; // Menu
        default: return 0;
        }
    }
    static constexpr u8 kPlain[0x59] = {
        0,   41,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  45,  46,  42,  43,  // 00-0F
        20,  26,  8,   21,  23,  28,  24,  12,  18,  19,  47,  48,  40,  224, 4,   22,  // 10-1F
        7,   9,   10,  11,  13,  14,  15,  51,  52,  53,  225, 49,  29,  27,  6,   25,  // 20-2F
        5,   17,  16,  54,  55,  56,  229, 85,  226, 44,  57,  58,  59,  60,  61,  62,  // 30-3F
        63,  64,  65,  66,  67,  72,  71,  95,  96,  97,  86,  92,  93,  94,  87,  89,  // 40-4F
        90,  91,  98,  99,  0,   0,   100, 68,  69,                                     // 50-58
    };
    return s < std::size(kPlain) ? kPlain[s] : 0;
}

constexpr std::string_view kWindowSizes[] = {"1280x720",  "1366x768",  "1600x900", "1920x1080",
                                             "2560x1440", "3200x1800", "3840x2160"};

bool ParseWh(std::string_view s, u32& w, u32& h) {
    unsigned a = 0, b = 0;
    if (std::sscanf(std::string(s).c_str(), "%ux%u", &a, &b) != 2) {
        return false;
    }
    w = a;
    h = b;
    return true;
}
std::string SizeLabel(std::string_view s) {
    std::string out(s);
    if (const auto x = out.find('x'); x != std::string::npos) {
        out.replace(x, 1, " × ");
    }
    return out;
}
std::string Comma(std::string s) {
    std::ranges::replace(s, '.', ',');
    return s;
}

// ---- the experiments: the mockup's labels and notes where it has the variable (app.js EXPERIMENTS)
struct ExpText {
    std::string_view env, label, help;
};
constexpr ExpText kExpTexts[] = {
    {"SSPC_CP_MODE", "Режим обработчика команд",
     "«Нативный» — собственный обработчик SecondSonPC с данными движка. «Параллельный» и «Прежний» — старые пути, "
     "медленнее."},
    {"SSPC_CITY_PERFORMANCE", "Оптимизации города", "Замеренные оптимизации отрисовки города на процессоре."},
    {"SSPC_REA_MERGE_RUNS", "Объединять серии ресурсов", "Соседние диапазоны ресурсов подключаются одним."},
    {"SSPC_CP_ROUND_NODRAIN", "Без слива между раундами", "Обработчик не ждёт видеокарту между своими раундами."},
    {"SSPC_P411_ASYNC_ACQUIRE", "Асинхронное получение кадра",
     "Следующий кадр swapchain получается вне потока обработчика."},
    {"SSPC_P53_FRONTEND_THREAD", "Отдельный поток подготовки", "Подготовка отрисовки идёт в отдельном потоке."},
    {"SSPC_CP_ASYNC_WRITES", "Асинхронные записи в память",
     "Записи видеокарты, которые опрашивает игра, не останавливают обработчик."},
    {"SSPC_CP_ASYNC_FLIP", "Асинхронная смена кадра", "Готовый кадр выводится без остановки обработчика."},
    {"SSPC_CP_RING", "Кольцо команд", "Сколько подготовленных записей может ждать поток записи."},
    {"SSPC_NATIVE_GBUFFER", "Нативная отрисовка G-буфера", "Инстансные отрисовки G-буфера готовятся из данных движка."},
    {"SSPC_VK_EMIT_THREAD", "Второй поток записи", "Команды Vulkan передаются драйверу в отдельном потоке."},
    {"SSPC_VK_EMIT_BATCH", "Пакетная запись", "Поток записи публикует команды один раз на запись."},
    {"SSPC_R11_EARLY_SUBMIT", "Ранняя отправка батчей (R11)",
     "Батч уходит на видеокарту по 256 командам, если она простаивает. В одном из трёх длительных тестов был сбой "
     "драйвера (TDR), повторы чистые."},
    {"SSPC_R11_ASYNC_SUBMIT", "Отправка батчей в отдельном потоке (R11)",
     "Изменения FPS не замерено: поток записи не ограничивает кадр."},
    {"SSPC_ASYNC_PIPELINES", "Сборка конвейеров в фоне",
     "Новые графические конвейеры собираются в фоне, не останавливая кадр."},
    {"SSPC_R9_PCACHE", "Без фризов кэша конвейеров (F1)", "Кэш конвейеров драйвера сохраняется без остановки игры."},
    {"SSPC_R9_HEAP_WINDOW", "Без повторного импорта кучи вершин (F3)", "Большие кучи импортируются с запасом."},
    {"SSPC_PIPE_PREFETCH", "Предсборка compute-конвейеров (R10)",
     "Помогает только с пустым кэшем шейдеров (первое прохождение, новый драйвер)."},
    {"SSPC_REA_DEFER_WAIT_B", "REA-3: ожидание compute на видеокарте",
     "В одиночку делает кадры неровными; включать вместе с AQ и MAIN_FREE."},
    {"SSPC_FB_ASYNC_COMPUTE", "AQ: compute в отдельной очереди",
     "Compute игры идёт в отдельной очереди Vulkan, рядом с графикой."},
    {"SSPC_R10_AQ_MAIN_FREE", "MAIN_FREE: графика ждёт только свой compute (R10)",
     "Быстрее в бою и на сохранении, но в Neon Dash 1 % low ниже, чем в play.cmd."},
    {"SSPC_R11_AQ_IMAGES", "AQ: compute с текстурами в своей очереди (R11)",
     "Dispatch, который только читает готовые текстуры, идёт в очередь compute."},
    {"SSPC_R11_AQ_ORDER_EPOCH", "MAIN_FREE: ожидание порядка только между эпохами (R11)",
     "Замерено вместе с пунктом выше."},
    {"SSPC_R11_FAST_LABEL", "Готовый compute виден раньше (R11)",
     "Вместе с ранней отправкой батчей выигрыш только в бою."},
    {"SSPC_R11_LABEL_WAIT", "Ожидание compute в игре без спина (R11)", "Бой лучше, но Neon Dash хуже."},
    {"SSPC_R10_SCHED", "Общий пул сборки конвейеров (R10)", "С прогретым кэшем выигрыша нет."},
    {"SSPC_R10_SPREAD", "Разносить тяжёлые потоки по ядрам (R10)",
     "Выигрыша не замерено; может помочь на других процессорах."},
    {"SSPC_R10_RUN_PS", "Серии через смену пиксельного шейдера (R10)", "Картинка та же, но город медленнее."},
    {"SSPC_R10_RUN_PS_MATERIAL", "…и со своими текстурами (R10)",
     "В серию входят и отрисовки со своими текстурами и сэмплерами."},
    {"SSPC_R10_NATIVE_DRAWS", "Нативная отрисовка теней (R10)", "Расхождений нет, изменения FPS не замерено."},
    {"SSPC_R10_AJM", "Свой декодер звука (AJM / ATRAC9)", "Выключено — декодер shadPS4."},
    {"SSPC_R10_SYSRT", "Свои диалоги, сеть и libc", "Выключено — реализация shadPS4."},
};
const ExpText* FindExpText(std::string_view env) {
    for (const auto& t : kExpTexts) {
        if (t.env == env) {
            return &t;
        }
    }
    return nullptr;
}
bool InEnv(std::span<const std::pair<std::string_view, std::string_view>> env, std::string_view name) {
    return std::ranges::any_of(env, [&](const auto& p) { return p.first == name; });
}

constexpr std::string_view kProfileLabel[] = {"Стандартный (play.cmd)", "Экспериментальный (play_experimental.cmd)"};
constexpr std::string_view kProfileHelp[] = {
    "Замеренный пресет проекта. Экспериментальные режимы выключены.",
    "Пресет play.cmd + отдельная очередь compute (REA-3, AQ, MAIN_FREE). Быстрее в бою, но в Neon Dash 1 % low хуже — "
    "выбирайте осознанно.",
};

std::wstring Wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}
/// CSS text-transform: uppercase (Cyrillic included).
std::string UpperText(std::string_view s) {
    std::wstring w = Wide(s);
    if (w.empty()) {
        return {};
    }
    CharUpperBuffW(w.data(), static_cast<DWORD>(w.size()));
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// ---- rows

struct ChipSpec {
    std::string text;
    ChipKind kind = ChipKind::Plain;
};
/// The left column of a .row: .row-label (with its chips), .env, .path, .row-help, .row-reason, .capture-hint.
struct RowText {
    std::string label;
    std::vector<ChipSpec> chips;
    std::string env, path, help, reason, hint;
    Color reason_color = kAmber;
    bool disabled = false; // .is-disabled: the label muted, the control at 45 %
};
/// One child of a .group: its height for a width (without its border-bottom) and its drawing.
struct Item {
    std::function<float(float w)> height;
    std::function<void(Rect r)> draw;
    bool border = true; // border-bottom (none on the last child)
    Color bg{0, 0, 0, 0};
};
struct Group {
    enum class Kind : u8 { Box, NaList, Experiments };
    Kind kind = Kind::Box;
    std::string title; // Box: the h2 ("" none); NaList: the summary
    std::vector<Item> items;
    u64 id = 0;
};

struct Opt {
    std::string value, label;
    bool disabled = false;
};

struct Toast {
    std::string text;
    bool warn = false;
    double t0 = -1;
};

struct ModalAction {
    std::string label;
    ButtonKind kind = ButtonKind::Normal;
    std::string value;
};
struct Modal {
    std::string title;
    std::vector<std::string> paras; // <p> (with <b>)
    std::vector<std::string> notes; // <p class="demo-line">
    std::vector<ModalAction> actions;
    std::string dismiss = "cancel"; // Esc ("" : nothing)
    std::string initial;
    std::function<void(const std::string&)> done;
    bool launch = false;
    bool fresh = true;
};
struct Launch {
    int step = 0; // the active step (3: all done)
    double next = 0;
    bool finished = false;
    std::string error;
    double close_at = 0;
};

constexpr std::string_view kLaunchSteps[] = {"Проверка файлов игры", "Сохранение настроек", "Запуск SecondSonPC.exe"};

} // namespace

struct LauncherApp::Impl {
    AppOptions opt;
    Ui* u = nullptr;
    SystemInfo sys;
    LauncherState state;
    std::filesystem::path settings_file;
    SecondSon::App::Config cfg; // SecondSonPC.toml
    std::string config_error;
    std::unique_ptr<S::Service> svc;
    std::map<std::string, S::Value, std::less<>> draft;
    std::array<std::vector<std::string>, kSecCount> ids;
    int current = kHome;
    int tab = 0; // controls: 0 keyboard and mouse, 1 gamepad
    bool exp_open = false;
    std::map<u64, bool> lists_open; // the "Ещё недоступно" lists
    std::array<ScrollState, kSecCount> scroll{};
    int capture = -1; // the action whose key is being captured
    bool capture_armed = false;
    std::vector<Toast> toasts;
    std::optional<Modal> modal;
    Launch launch;
    GameProcess game;
    bool game_running = false;
    std::shared_ptr<Image> art;
    WindowRequest request = WindowRequest::None;
    float W = 0, H = 0, b = 1;

    Canvas& C() {
        return u->c;
    }
    float TW(std::string_view s, const Font& f) {
        return C().Measure(s, f, f.size * kLine, 1e5f, false).w;
    }

    // ------------------------------------------------------------------ settings

    u32 Caps() const {
        namespace Cap = S::Capability;
        u32 c = Cap::Window | Cap::LiveVolume | Cap::RealController | Cap::RenderScale | Cap::FrameLimiter |
                Cap::FpsCounter | Cap::PresentModeSwitch;
        if (sys.rtx) {
            c |= Cap::Dlss;
        }
        if (sys.rtx40) {
            c |= Cap::DlssFrameGen;
        }
        if (sys.fsr_dll) {
            c |= Cap::FsrFrameGen;
        }
        if (sys.gpu_vendor == 0x10DE) {
            c |= Cap::Reflex;
        }
        return c;
    }

    /// SecondSonPC.toml + this profile's experiments as the defaults, the user file over them: as the game starts.
    /// The drafts not changed since the last load follow the new values.
    void ReloadService(bool force_drafts = false) {
        std::vector<std::string> errors;
        cfg = SecondSon::App::Config{};
        config_error.clear();
        if (auto c = SecondSon::App::LoadConfig(SecondSon::App::RootDir(), errors)) {
            cfg = std::move(*c);
        } else {
            config_error = errors.empty() ? std::string("SecondSonPC.toml не читается") : errors.front();
        }
        S::ReadExperiments(cfg);
        S::Overrides overrides;
        S::KeptKeys kept;
        S::LoadReport report;
        if (!settings_file.empty()) {
            report = S::LoadUserFile(settings_file, overrides, &kept);
            if (report.status == S::LoadReport::Status::Corrupt) {
                ToastMsg(fmt::format("settings.toml не читается — он перемещён в {}", Utf8(report.moved_to)), true);
            } else if (report.status == S::LoadReport::Status::Unreadable) {
                ToastMsg(fmt::format("settings.toml не читается: {}", report.error), true);
            }
        }
        SecondSon::App::Config started = cfg;
        S::ApplyOverrides(started, overrides);
        auto next = std::make_unique<S::Service>();
        const bool save = !opt.read_only && !settings_file.empty() &&
                          report.status != S::LoadReport::Status::Unreadable;
        next->Init(cfg, std::move(overrides), started, save ? settings_file : std::filesystem::path{}, Caps(),
                   std::move(kept));
        for (const S::Desc& d : S::All()) {
            if (d.apply == S::Apply::Unavailable || d.get == nullptr) {
                continue;
            }
            const auto it = draft.find(d.id);
            if (it == draft.end()) {
                draft.emplace(std::string(d.id), next->Get(d));
            } else if (force_drafts || (svc != nullptr && S::Equal(it->second, svc->Get(d)))) {
                it->second = next->Get(d);
            }
        }
        if (svc != nullptr) {
            svc->Flush(3000);
        }
        svc = std::move(next);
    }

    const S::Desc& D(std::string_view id) const {
        return *S::Find(id);
    }
    S::Value& V(std::string_view id) {
        return draft.find(id)->second;
    }
    bool Bv(std::string_view id) {
        return std::get<bool>(V(id));
    }
    std::string Sv(std::string_view id) {
        return std::get<std::string>(V(id));
    }
    double Nv(std::string_view id) {
        const S::Value& v = V(id);
        return std::holds_alternative<s64>(v) ? static_cast<double>(std::get<s64>(v)) : std::get<double>(v);
    }
    void SetNum(std::string_view id, double v) {
        if (D(id).type == S::Type::Int) {
            V(id) = static_cast<s64>(std::llround(v));
        } else {
            V(id) = v;
        }
    }

    bool Dirty(int sec) {
        return std::ranges::any_of(ids[sec], [&](const std::string& id) { return !S::Equal(V(id), svc->Get(D(id))); });
    }
    bool IsDefault(int sec) {
        return std::ranges::all_of(ids[sec],
                                   [&](const std::string& id) { return S::Equal(V(id), svc->Default(D(id))); });
    }
    bool ExperimentsDefault() {
        for (const auto& e : S::Experiments()) {
            const std::string id = S::ExperimentId(e.env);
            if (!S::Equal(V(id), svc->Default(D(id)))) {
                return false;
            }
        }
        return true;
    }
    void Revert(int sec) {
        for (const auto& id : ids[sec]) {
            V(id) = svc->Get(D(id));
        }
    }
    bool SaveSection(int sec) {
        std::vector<std::pair<const S::Desc*, S::Value>> changes;
        for (const auto& id : ids[sec]) {
            if (!S::Equal(V(id), svc->Get(D(id)))) {
                changes.emplace_back(&D(id), V(id));
            }
        }
        if (changes.empty()) {
            return true;
        }
        std::string error;
        const S::SetResult r = svc->SetMany(changes, &error);
        if (r == S::SetResult::Invalid || r == S::SetResult::Unavailable) {
            ToastMsg(fmt::format("Не сохранено: {}", error.empty() ? "значение не подходит" : error), true);
            return false;
        }
        if (!svc->Flush(3000) && !svc->LastSaveError().empty()) {
            ToastMsg(fmt::format("settings.toml не записан: {}", svc->LastSaveError()), true);
            return false;
        }
        ToastMsg(fmt::format("Сохранено: «{}».", kSecs[sec].label));
        return true;
    }

    // ------------------------------------------------------------------ notices, dialogs

    void ToastMsg(std::string text, bool warn = false) {
        toasts.push_back({std::move(text), warn, -1});
    }

    void OpenModal(Modal m) {
        CancelCapture();
        if (u != nullptr) {
            u->ClosePopup();
        }
        modal = std::move(m);
    }
    void CloseModal(const std::string& value) {
        if (!modal) {
            return;
        }
        auto done = std::move(modal->done);
        modal.reset();
        if (done) {
            done(value);
        }
    }
    void Confirm(std::string title, std::string body, std::string ok, ButtonKind ok_kind,
                 std::function<void()> on_ok, std::vector<std::string> notes = {}) {
        Modal m;
        m.title = std::move(title);
        m.paras = {std::move(body)};
        m.notes = std::move(notes);
        m.actions = {{"Отмена", ButtonKind::Normal, "cancel"}, {std::move(ok), ok_kind, "ok"}};
        m.initial = "cancel";
        m.done = [on_ok = std::move(on_ok)](const std::string& v) {
            if (v == "ok") {
                on_ok();
            }
        };
        OpenModal(std::move(m));
    }

    void SwitchTo(int sec) {
        CancelCapture();
        current = sec;
        scroll[sec] = {};
    }
    void Go(int sec) {
        if (sec == current) {
            return;
        }
        if (current != kHome && Dirty(current)) {
            const int from = current;
            Modal m;
            m.title = "Сохранить изменения?";
            m.paras = {fmt::format("В разделе «{}» есть несохранённые изменения.", kSecs[from].label)};
            m.actions = {{"Отмена", ButtonKind::Normal, "cancel"},
                         {"Не сохранять", ButtonKind::Normal, "discard"},
                         {"Сохранить", ButtonKind::Primary, "save"}};
            m.initial = "save";
            m.done = [this, from, sec](const std::string& v) {
                if (v == "save") {
                    if (game_running || !SaveSection(from)) {
                        return;
                    }
                } else if (v == "discard") {
                    Revert(from);
                } else {
                    return;
                }
                SwitchTo(sec);
            };
            OpenModal(std::move(m));
            return;
        }
        SwitchTo(sec);
    }

    void ResetSection(int sec) {
        Confirm("Вернуть значения по умолчанию?",
                fmt::format("Раздел «{}» вернётся к значениям по умолчанию{}. Чтобы оставить их, нажмите «Сохранить».",
                            kSecs[sec].label, sec == kAdvanced ? ", эксперименты — к профилю запуска" : ""),
                "Вернуть по умолчанию", ButtonKind::Primary, [this, sec] {
                    for (const auto& id : ids[sec]) {
                        V(id) = svc->Default(D(id));
                    }
                    ToastMsg("Значения по умолчанию установлены — не забудьте сохранить.");
                });
    }

    void RestoreAll() {
        Confirm("Восстановить все настройки?",
                "Все разделы, клавиши и эксперименты вернутся к значениям по умолчанию, профиль запуска — "
                "«Стандартный».",
                "Восстановить", ButtonKind::Danger,
                [this] {
                    for (size_t c = 0; c < S::kCategoryCount; ++c) {
                        svc->ResetCategory(static_cast<S::Category>(c));
                    }
                    svc->Flush(3000);
                    state = {};
                    SaveState();
                    ApplyProfileEnv(state.profile);
                    ReloadService(true);
                    ToastMsg("Все настройки восстановлены по умолчанию.");
                },
                {"settings.toml будет перезаписан; файлы игры и сохранения не трогаются."});
    }

    void SaveState() {
        std::string error;
        if (!opt.read_only && !SaveLauncherState(state, error)) {
            ToastMsg(fmt::format("launcher.toml не записан: {}", error), true);
        }
    }
    void SetProfile(Profile p) {
        if (p == state.profile) {
            return;
        }
        state.profile = p;
        SaveState();
        ApplyProfileEnv(p);
        ReloadService();
    }

    // ------------------------------------------------------------------ the key capture

    void CancelCapture() {
        capture = -1;
    }
    void StartCapture(Action a) {
        capture = static_cast<int>(a);
        capture_armed = false;
    }
    void ApplyCapture(Input::InputCode code) {
        const Action a = static_cast<Action>(capture);
        if (Input::IsReservedInput(code)) {
            std::string_view what = "F11 — полноэкранный режим";
            const std::string n = Input::InputName(code);
            if (n == "F1") {
                what = "F1 — меню настроек";
            } else if (n == "F10") {
                what = "F10 — счётчик FPS";
            }
            ToastMsg(fmt::format("{}: клавиша зарезервирована SecondSonPC.", what), true);
            return;
        }
        Input::Settings tmp;
        for (size_t i = 0; i < Input::kActionCount; ++i) {
            tmp.bindings[i] = std::get<S::Bindings>(V(BindingId(static_cast<Action>(i))));
        }
        if (!Input::AssignBinding(tmp, a, 0, code)) {
            return;
        }
        V(BindingId(a)) = tmp.bindings[static_cast<size_t>(a)];
        std::string others;
        for (const Action o : Input::ActionsUsing(tmp, code)) {
            if (o != a && !Input::MayShareInput(a, o)) {
                others += fmt::format("{}«{}»", others.empty() ? "" : ", ", ActionLabel(o));
            }
        }
        const std::string name = KeyLabel(code);
        CancelCapture();
        if (others.empty()) {
            ToastMsg(fmt::format("«{}» — {}.", ActionLabel(a), name));
        } else {
            ToastMsg(fmt::format("{} назначена, но также используется: {}.", name, others), true);
        }
    }
    /// While capturing: the next key, mouse button or wheel notch is the action's first input (app.js keydown /
    /// mousedown / wheel handlers); nothing else gets them.
    void CaptureInput() {
        Ui& ui = *u;
        if (capture < 0 || modal) {
            return;
        }
        if (!capture_armed) {
            capture_armed = !ui.in.down; // the click on "Изменить" is over
        }
        for (const KeyEvent& k : ui.in.keys) {
            if (k.vk == VK_ESCAPE) {
                CancelCapture();
                break;
            }
            if (const u16 sc = SdlScancode(k.scan); sc != 0) {
                ApplyCapture({Input::InputCode::Kind::Key, sc});
                break;
            }
        }
        ui.in.keys.clear();
        if (capture >= 0 && capture_armed) {
            u16 button = 0;
            if (ui.in.pressed) {
                button = 1;
            } else if (ui.in.other_pressed & 1) {
                button = 3;
            } else if (ui.in.other_pressed & 2) {
                button = 2;
            } else if (ui.in.other_pressed & 4) {
                button = 4;
            } else if (ui.in.other_pressed & 8) {
                button = 5;
            }
            if (button != 0) {
                ApplyCapture({Input::InputCode::Kind::MouseButton, button});
            } else if (ui.in.wheel != 0) {
                ApplyCapture({Input::InputCode::Kind::MouseWheel, static_cast<u16>(ui.in.wheel > 0 ? 0 : 1)});
            }
        }
        if (capture >= 0 || ui.in.pressed) {
            ui.in.pressed = ui.in.released = false; // (the capturing click selects nothing)
            ui.in.wheel = 0;
        }
    }

    // ------------------------------------------------------------------ the left column of a row

    float LeftBlock(const RowText& t, float x, float y, float w, bool draw) {
        Canvas& c = C();
        const TextBox lb = c.Measure(t.label, kLabel, kLabelLh, w, true);
        // .row-label: a flex line of the label and its chips (gap 6px 8px, wrapping)
        struct Line {
            float top, h;
        };
        std::vector<Line> lines{{0, lb.h}};
        struct Placed {
            float x;
            size_t line;
        };
        std::vector<Placed> placed;
        float cx = lb.lines > 1 ? 1e9f : lb.w;
        for (const auto& chip : t.chips) {
            const float cw = ChipWidth(*u, chip.text, chip.kind);
            if (cx + 8 + cw > w) {
                lines.push_back({lines.back().top + lines.back().h + 6, 20});
                placed.push_back({0, lines.size() - 1});
                cx = cw;
            } else {
                lines.back().h = std::max(lines.back().h, 20.0f);
                placed.push_back({cx + 8, lines.size() - 1});
                cx += 8 + cw;
            }
        }
        if (draw) {
            c.Text(t.label, {x, y + (lines[0].h - lb.h) / 2, w + 1, lb.h}, kLabel, t.disabled ? kMuted : kText,
                   kLabelLh);
            for (size_t i = 0; i < t.chips.size(); ++i) {
                const Line& l = lines[placed[i].line];
                Chip(*u, x + placed[i].x, y + l.top + (l.h - 20) / 2, t.chips[i].text, t.chips[i].kind);
            }
        }
        float h = lines.back().top + lines.back().h;
        const auto block = [&](const std::string& s, const Font& f, float lh, Color col, float margin) {
            if (s.empty()) {
                return;
            }
            const TextBox tb = c.Measure(s, f, lh, w, true);
            if (draw) {
                c.Text(s, {x, y + h + margin, w + 1, tb.h}, f, col, lh);
            }
            h += margin + tb.h;
        };
        block(t.env, Mono(12), C().NormalLineHeight(Mono(12)), kDim, 0);
        block(t.path, Mono(12.5f), C().NormalLineHeight(Mono(12.5f)), kMuted, 0);
        block(t.help, kHelp, kHelpLh, kMuted, 3);
        block(t.reason, kHelp, kHelpLh, t.reason_color, 5);
        block(t.hint, kHelp, kHelpLh, kAccentHover, 0);
        return h;
    }

    /// A .row: the left column and a control `ctl_w` x `ctl_h` at the right (.control: justify-content: flex-end).
    Item Row(RowText text, float ctl_w, float ctl_h, std::function<void(Rect)> ctl) {
        auto t = std::make_shared<RowText>(std::move(text));
        Item it;
        it.height = [this, t, ctl_w, ctl_h](float w) {
            return 26 + std::max(LeftBlock(*t, 0, 0, w - 36 - 20 - ctl_w, false), ctl_h);
        };
        it.draw = [this, t, ctl_w, ctl_h, ctl = std::move(ctl)](Rect r) {
            const float lw = r.w - 36 - 20 - ctl_w;
            const float lh = LeftBlock(*t, 0, 0, lw, false), hh = std::max(lh, ctl_h);
            LeftBlock(*t, r.x + 18, r.y + 13 + (hh - lh) / 2, lw, true);
            if (t->disabled) {
                C().PushOpacity(0.45f);
            }
            ctl({r.R() - 18 - ctl_w, r.y + 13 + (hh - ctl_h) / 2, ctl_w, ctl_h});
            if (t->disabled) {
                C().PopOpacity();
            }
        };
        return it;
    }

    /// Why `id` cannot be changed: the page's own rule first (`rule`), else this run's availability.
    std::string ReasonFor(std::string_view id, std::string rule = {}) {
        if (!rule.empty()) {
            return rule;
        }
        const S::Availability a = svc->Available(D(id));
        return a.available ? std::string{} : std::string(a.why_ru);
    }
    RowText Text(std::string label, std::string help, std::string reason = {}) {
        RowText t;
        t.label = std::move(label);
        t.help = std::move(help);
        t.disabled = !reason.empty();
        t.reason = std::move(reason);
        return t;
    }

    std::vector<Opt> ChoiceOpts(std::string_view id) {
        std::vector<Opt> out;
        for (const auto& c : D(id).choices) {
            out.push_back({std::string(c.value), std::string(c.ru)});
        }
        return out;
    }

    Item SelectItem(RowText t, std::vector<Opt> opts, std::function<std::string()> get,
                    std::function<void(const std::string&)> set, u64 id, Color bg = kSurface2) {
        const std::string value = get();
        if (std::ranges::none_of(opts, [&](const Opt& o) { return o.value == value; })) {
            opts.insert(opts.begin(), {value, value.empty() ? std::string("—") : SizeLabel(value)});
        }
        const bool enabled = t.reason.empty();
        auto o = std::make_shared<std::vector<Opt>>(std::move(opts));
        return Row(std::move(t), kCtlW, 34, [this, o, value, set = std::move(set), id, enabled, bg](Rect r) {
            std::vector<SelectOption> so;
            int cur = 0;
            for (size_t i = 0; i < o->size(); ++i) {
                so.push_back({(*o)[i].label, (*o)[i].disabled});
                if ((*o)[i].value == value) {
                    cur = static_cast<int>(i);
                }
            }
            if (const int ch = Select(*u, id, r, so, cur, enabled, bg); ch >= 0) {
                set((*o)[ch].value);
            }
        });
    }
    Item SelectRow(std::string_view sid, RowText t, std::vector<Opt> opts) {
        const std::string id(sid);
        return SelectItem(
            std::move(t), std::move(opts), [this, id] { return Sv(id); },
            [this, id](const std::string& v) { V(id) = v; }, Id(id));
    }
    Item SelectRow(std::string_view id, std::string label, std::string help, std::string rule = {}) {
        return SelectRow(id, Text(std::move(label), std::move(help), ReasonFor(id, std::move(rule))), ChoiceOpts(id));
    }

    Item SwitchRow(std::string_view sid, std::string label, std::string help, std::string rule = {}) {
        const std::string id(sid);
        RowText t = Text(std::move(label), std::move(help), ReasonFor(id, std::move(rule)));
        const bool enabled = t.reason.empty();
        return Row(std::move(t), kCtlW, 24, [this, id, enabled](Rect r) {
            const bool on = Bv(id);
            const float sx = r.R() - 42, lh = LineOf(13);
            // .switch-state: 13px, min-width 40px, right-aligned, 10px before the switch
            C().Text(on ? "Вкл." : "Выкл.", {sx - 10 - 200, r.y + (24 - lh) / 2, 200, lh}, F(13), kMuted, lh,
                     Align::Right, false);
            if (Switch(*u, Id(id), {sx, r.y, 42, 24}, on, enabled)) {
                V(id) = !on;
            }
        });
    }

    Item RangeRow(std::string_view sid, std::string label, std::string help, double min, double max, double step,
                  std::function<std::string(double)> show, std::string rule = {}) {
        const std::string id(sid);
        RowText t = Text(std::move(label), std::move(help), ReasonFor(id, std::move(rule)));
        const bool enabled = t.reason.empty();
        return Row(std::move(t), kCtlW, 22, [this, id, min, max, step, show = std::move(show), enabled](Rect r) {
            double v = Nv(id);
            const Font vf = Tab(F(13));
            const float vw = std::max(64.0f, TW(show(v), vf));
            if (Range(*u, Id(id), {r.x, r.y, r.w - 12 - vw, 22}, v, min, max, step, enabled)) {
                SetNum(id, v);
            }
            const float lh = LineOf(13);
            C().Text(show(Nv(id)), {r.R() - vw, r.y + (22 - lh) / 2, vw, lh}, vf, kText, lh, Align::Right, false);
        });
    }

    Item LockedRow(std::string label, bool dev, std::string value, std::string reason) {
        RowText t;
        t.label = std::move(label);
        t.chips = {{dev ? "В разработке" : "Недоступно", dev ? ChipKind::Dev : ChipKind::Na}};
        t.reason = std::move(reason);
        t.reason_color = dev ? kAmber : kMuted;
        const u64 id = Id(t.label, 7);
        return Row(std::move(t), kCtlW, 34, [this, value, id](Rect r) {
            C().PushOpacity(0.5f);
            const SelectOption o[] = {{value}};
            Select(*u, id, r, o, 0, false);
            C().PopOpacity();
        });
    }

    Item ButtonRow(RowText t, std::string button, std::string icon, ButtonKind kind, std::function<void()> click,
                   bool enabled = true) {
        const u64 id = Id(t.label, 11);
        return Row(std::move(t), kCtlW, 34, [this, button, icon, kind, click = std::move(click), id, enabled](Rect r) {
            ButtonStyle s;
            s.kind = kind;
            const float bw = ButtonWidth(*u, button, icon, s);
            if (Button(*u, id, {r.R() - bw, r.y, bw, 34}, button, icon, s, enabled)) {
                click();
            }
        });
    }

    Item NoteItem(std::string markup) {
        auto rich = std::make_shared<Rich>(ParseRich(markup));
        Item it;
        it.height = [this, rich](float w) {
            return 24 + C().Measure(rich->text, F(13), LineOf(13), w - 36, true, rich->spans).h;
        };
        it.draw = [this, rich](Rect r) {
            C().Text(rich->text, {r.x + 18, r.y + 12, r.w - 36 + 1, r.h - 24}, F(13), kMuted, LineOf(13), Align::Left,
                     true, rich->spans);
        };
        return it;
    }

    Item SubheadItem(std::string title, bool border_top) {
        Item it;
        it.border = false;
        it.height = [border_top](float) { return (border_top ? 1.0f : 0.0f) + 14 + LineOf(12) + 6; };
        it.draw = [this, title, border_top](Rect r) {
            if (border_top) {
                C().FillRect({r.x, r.y, r.w, 1}, kBorder);
            }
            C().Text(UpperText(title), {r.x + 18, r.y + (border_top ? 1.0f : 0.0f) + 14, r.w - 36, LineOf(12)},
                     Upper(12, 600, 0.05f), kMuted, LineOf(12), Align::Left, false);
        };
        return it;
    }

    // ------------------------------------------------------------------ Display

    double MonitorHz() {
        const std::string m = Sv("display.monitor");
        for (const auto& d : sys.displays) {
            if (m.empty() ? d.primary : d.name == m) {
                return d.hz > 0 ? d.hz : 60.0;
            }
        }
        return 60.0;
    }
    std::pair<u32, u32> MonitorSize() {
        const std::string m = Sv("display.monitor");
        for (const auto& d : sys.displays) {
            if ((m.empty() ? d.primary : d.name == m) && d.width != 0) {
                return {d.width, d.height};
            }
        }
        return {7680, 4320};
    }
    bool IsWide() {
        const std::string a = Sv("display.aspectRatio");
        if (a == "21:9" || a == "32:9") {
            return true;
        }
        if (a != "auto") {
            return false;
        }
        u32 w = 0, h = 0;
        const std::string r = Sv("display.resolution");
        if (r == "auto" || !ParseWh(r, w, h)) {
            std::tie(w, h) = MonitorSize();
        }
        return h != 0 && static_cast<double>(w) / h >= 2.1;
    }

    /// .explain: what the vertical sync and the limit give together (app.js renderExplain).
    float Explain(float x, float y, float w, bool draw) {
        Canvas& c = C();
        const double hz = MonitorHz();
        const std::string vs = Sv("display.presentMode"), lim_s = Sv("display.frameRateLimit");
        const bool console = lim_s == "console", unlimited = lim_s == "unlimited";
        double lim = console ? 60.0 : unlimited ? 1e9 : std::atof(lim_s.c_str());
        if (lim <= 0) {
            lim = 60;
        }
        const auto up_to = [](double n) {
            return n >= 1e9 ? std::string("без ограничения") : fmt::format("до {:.0f} к/с", n);
        };
        double shown = std::min(lim, hz);
        bool tear = false;
        std::string text;
        if (vs == "fifo") {
            text = "Кадр ждёт ближайшего обновления монитора: разрывов нет, но опоздавший кадр показывается только на "
                   "следующем обновлении.";
        } else if (vs == "immediate") {
            shown = lim;
            tear = true;
            text = "Кадр выводится сразу, как готов: задержка минимальна, но возможны горизонтальные разрывы картинки.";
        } else {
            text = "Игра не ждёт монитор, а на каждом его обновлении показывается самый новый кадр: разрывов нет, лишние "
                   "кадры отбрасываются.";
        }
        if (console) {
            text = "Как на консоли: кадр показывается только на vblank 60 Гц, поэтому кадр, готовый за 17 мс, ждёт до "
                   "33 мс. " + text;
        }
        const std::string vs_label = vs == "fifo" ? "Вкл. (FIFO)" : vs == "immediate" ? "Выкл." : "Mailbox";
        const std::string cells[4][2] = {
            {"ЛИМИТ FPS", console ? std::string("60 · vblank PS4") : up_to(lim)},
            {"СИНХРОНИЗАЦИЯ", vs_label},
            {"МОНИТОР", fmt::format("{:.0f} Гц", hz)},
            {"НА ЭКРАНЕ", fmt::format("{} · {}", up_to(shown), tear ? "возможны разрывы" : "без разрывов")},
        };
        const float iw = w - 36, ix = x + 18;
        float h = 14;
        if (draw) {
            c.Text("Как сработают синхронизация и лимит", {ix, y + h, iw, LineOf(13)}, F(13, 600), kText, LineOf(13),
                   Align::Left, false);
        }
        h += LineOf(13) + 10;
        // grid: 1fr auto 1fr auto 1fr auto 1.2fr, gap 8
        const float fr = (iw - 3 * 14 - 6 * 8) / 4.2f;
        const float cw[4] = {fr, fr, fr, fr * 1.2f};
        const Font kf = Upper(11, 400, 0.05f), vf = Tab(F(13.5f, 600));
        float row_h = 0;
        for (int i = 0; i < 4; ++i) {
            row_h = std::max(row_h, 1 + 8 + LineOf(11) + 2 + c.Measure(cells[i][1], vf, LineOf(13.5f), cw[i] - 22).h +
                                        8 + 1);
        }
        if (draw) {
            float cx = ix;
            for (int i = 0; i < 4; ++i) {
                const Rect cell{cx, y + h, cw[i], row_h};
                const bool result = i == 3;
                c.FillRect(cell, result ? kAccentWeak : kSurface2, kRadius);
                c.Border(cell, result ? kAccentLine : kBorder, 1, kRadius);
                c.Text(cells[i][0], {cell.x + 11, cell.y + 9, cell.w - 22, LineOf(11)}, kf, kMuted, LineOf(11),
                       Align::Left, false);
                c.Text(cells[i][1], {cell.x + 11, cell.y + 9 + LineOf(11) + 2, cell.w - 22 + 1, row_h}, vf, kText,
                       LineOf(13.5f));
                cx += cw[i] + 8;
                if (i < 3) {
                    c.Icon(IconPath("arrow"), {cx, cell.y + (row_h - 14) / 2, 14, 14}, kDim);
                    cx += 14 + 8;
                }
            }
        }
        h += row_h + 10;
        const Rich rich = ParseRich(text + " <b>Это потолок, а не обещание</b>: реальная частота зависит от сцены. "
                                           "Ограничение 30 к/с в настройках самой игры тоже действует.",
                                    600, kText);
        const TextBox tb = c.Measure(rich.text, kHelp, kHelpLh, iw, true, rich.spans);
        if (draw) {
            c.Text(rich.text, {ix, y + h, iw + 1, tb.h}, kHelp, kMuted, kHelpLh, Align::Left, true, rich.spans);
        }
        return h + tb.h + 16;
    }

    std::vector<Group> DisplayGroups() {
        std::vector<Group> gs;
        const bool full = Bv("display.fullscreen");
        {
            Group g{.title = "Окно"};
            RowText wm = Text("Режим окна",
                              "«Без рамки» — окно на весь монитор поверх рабочего стола, быстрый Alt+Tab. F11 "
                              "переключает режим во время игры. Эксклюзивного режима в рендерере нет, поэтому он не "
                              "предлагается.",
                              ReasonFor("display.fullscreen"));
            const bool wm_enabled = wm.reason.empty();
            g.items.push_back(Row(std::move(wm), kCtlW, kSegmentedH, [this, full, wm_enabled](Rect r) {
                static const std::string labels[] = {"Оконный", "Без рамки"};
                if (const int ch = Segmented(*u, Id("display.windowMode"), r, labels, full ? 1 : 0, wm_enabled);
                    ch >= 0) {
                    V("display.fullscreen") = ch == 1;
                    if (ch == 1) {
                        V("display.fullscreenMode") = std::string("borderless");
                    }
                }
            }));
            const auto [mw, mh] = MonitorSize();
            std::vector<Opt> sizes;
            for (const auto s : kWindowSizes) {
                u32 w = 0, h = 0;
                ParseWh(s, w, h);
                if (w <= mw && h <= mh) {
                    sizes.push_back({std::string(s), SizeLabel(s)});
                }
            }
            g.items.push_back(SelectItem(
                Text("Размер окна", "Пресеты до размера монитора.",
                     ReasonFor("display.width", full ? "В режиме «Без рамки» окно занимает весь монитор." : "")),
                std::move(sizes),
                [this] { return fmt::format("{}x{}", std::get<s64>(V("display.width")), std::get<s64>(V("display.height"))); },
                [this](const std::string& v) {
                    u32 w = 0, h = 0;
                    if (ParseWh(v, w, h)) {
                        V("display.width") = static_cast<s64>(w);
                        V("display.height") = static_cast<s64>(h);
                    }
                },
                Id("display.windowSize")));
            std::vector<Opt> monitors{{"", sys.primary_label.empty() ? std::string("Основной монитор")
                                                                      : "Основной монитор — " + sys.primary_label}};
            for (const auto& d : sys.displays) {
                monitors.push_back({d.name, d.label});
            }
            const std::string cur = Sv("display.monitor");
            if (!cur.empty() && std::ranges::none_of(sys.displays, [&](const DisplayInfo& d) { return d.name == cur; })) {
                monitors.push_back({cur, cur + " (не подключён)"});
            }
            g.items.push_back(SelectRow("display.monitor",
                                        Text("Монитор", "Монитор, на котором откроется окно игры.",
                                             ReasonFor("display.monitor")),
                                        std::move(monitors)));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Кадры"};
            g.items.push_back(SelectRow(
                "display.presentMode", "Вертикальная синхронизация",
                "«Вкл.» (FIFO) — без разрывов, не чаще частоты монитора. «Выкл.» (IMMEDIATE) — кадр сразу, возможны "
                "разрывы. «Без разрывов» (MAILBOX) — на каждом обновлении монитора новейший кадр."));
            g.items.push_back(SelectRow("display.frameRateLimit", "Ограничение FPS",
                                        "Работает при любой синхронизации. Часы игры идут в реальном времени: при "
                                        "высоком FPS мир не ускоряется."));
            Item ex;
            ex.height = [this](float w) { return Explain(0, 0, w, false); };
            ex.draw = [this](Rect r) { Explain(r.x, r.y, r.w, true); };
            g.items.push_back(std::move(ex));
            g.items.push_back(SwitchRow("display.showFps", "Счётчик FPS",
                                        "Кадры в секунду и время кадра в левом верхнем углу. F10 переключает его во "
                                        "время игры."));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Картинка игры"};
            const auto [mw, mh] = MonitorSize();
            const std::string own = fmt::format("{}x{}", mw, mh);
            std::vector<Opt> res;
            bool own_listed = false;
            for (const auto& c : D("display.resolution").choices) {
                std::string label(c.ru);
                if (c.value == own) {
                    label += " — как у монитора";
                    own_listed = true;
                }
                res.push_back({std::string(c.value), std::move(label)});
            }
            if (!own_listed && mw < 7680) {
                res.insert(res.begin() + 1, {own, SizeLabel(own) + " — как у монитора"});
            }
            g.items.push_back(SelectRow(
                "display.resolution",
                Text("Разрешение",
                     "Собственное разрешение игры: картинка и интерфейс рисуются в этом размере. «Автоматически» — "
                     "размер окна, без рамки — монитора. Меньше 1080 строк игра всё равно рисует 1080, как PS4.",
                     ReasonFor("display.resolution")),
                std::move(res)));
            g.items.push_back(SelectRow("display.aspectRatio", "Соотношение сторон",
                                        "Шире — по бокам видно больше при той же высоте обзора (Hor+). Ничего не "
                                        "растягивается: остальное окно занимают полосы."));
            g.items.push_back(RangeRow("display.hudSafeArea", "Безопасная зона HUD",
                                       "Отступ интерфейса от краёв картинки, как «Область отображения» PS4: 100 % — у "
                                       "самых краёв.",
                                       85, 100, 1, [](double v) { return fmt::format("{:.0f} %", v); }));
            g.items.push_back(SelectRow("display.hudArea", "Область HUD",
                                        "На широкой картинке интерфейс можно собрать ближе к центру взгляда.",
                                        IsWide() ? "" : "Нужна широкая картинка: соотношение 21:9 или 32:9."));
            gs.push_back(std::move(g));
        }
        return gs;
    }

    // ------------------------------------------------------------------ Graphics, Effects

    std::vector<Group> GraphicsGroups() {
        std::vector<Group> gs;
        const bool up = Sv("upscaling.upscaler") != "off";
        const std::string fg = Sv("upscaling.frameGeneration");
        {
            Group g{.title = "Масштабирование"};
            auto ups = ChoiceOpts("upscaling.upscaler");
            for (auto& o : ups) {
                if (o.value == "dlss" && !sys.rtx) {
                    o.label += " — нужна GeForce RTX";
                    o.disabled = true;
                }
            }
            g.items.push_back(SelectRow(
                "upscaling.upscaler",
                Text("Технология масштабирования",
                     "Игра рендерит 3D-сцену в пониженном разрешении, апскейлер восстанавливает картинку в разрешении "
                     "игры; интерфейс рисуется поверх чётким. DLSS — GeForce RTX, FSR 3.1 — любая видеокарта.",
                     ReasonFor("upscaling.upscaler")),
                std::move(ups)));
            g.items.push_back(SelectRow("upscaling.quality", "Качество",
                                        "Ниже — быстрее там, где кадр упирается в видеокарту, и мягче картинка.",
                                        up ? "" : "Сначала выберите DLSS или FSR."));
            g.items.push_back(RangeRow("upscaling.sharpness", "Резкость изображения",
                                       "Контрастно-адаптивная резкость (AMD RCAS) после масштабирования. 0 — без неё.",
                                       0, 100, 5, [](double v) { return fmt::format("{:.0f} %", v); },
                                       up ? "" : "Сначала выберите DLSS или FSR."));
            g.items.push_back(SelectRow("graphics.renderScale", "Масштаб рендеринга",
                                        "Разрешение 3D-сцены относительно разрешения игры. «Автоматически» — не больше "
                                        "1080 строк, как на PS4.",
                                        up ? "С DLSS / FSR разрешение сцены задаёт «Качество»." : ""));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Генерация кадров"};
            g.items.push_back(NoteItem(fmt::format(
                "Между кадрами игры показываются сгенерированные: движение плавнее, но сама игра работает с прежней "
                "частотой, а ввод реагирует примерно на кадр позже. Совместимость определена по видеокарте этого ПК — "
                "<b>{}</b>.",
                sys.gpu.empty() ? std::string("не определена") : sys.gpu)));
            auto fgo = ChoiceOpts("upscaling.frameGeneration");
            for (auto& o : fgo) {
                if (o.value == "dlss" && !sys.rtx40) {
                    o.label += " — нужна RTX 40/50";
                    o.disabled = true;
                } else if (o.value == "fsr" && !sys.fsr_dll) {
                    o.label += " — нет amd_fidelityfx_vk.dll";
                    o.disabled = true;
                }
            }
            g.items.push_back(SelectRow(
                "upscaling.frameGeneration",
                Text("Генерация кадров", "В меню, на загрузках и при смене камеры приостанавливается.",
                     ReasonFor("upscaling.frameGeneration",
                               up ? "" : "Нужен апскейлер (DLSS или FSR): генерации нужны его глубина и векторы "
                                         "движения.")),
                std::move(fgo)));
            std::vector<Opt> mult{{"2", "×2"},
                                  {"3", "×3 — DLSS, нужна RTX 50", !sys.rtx50 || fg != "dlss"},
                                  {"4", "×4 — DLSS, нужна RTX 50", !sys.rtx50 || fg != "dlss"}};
            g.items.push_back(SelectRow(
                "upscaling.frameGenMultiplier",
                Text("Множитель генерации",
                     "FSR генерирует один кадр (×2). ×3 и ×4 — DLSS Multi Frame Generation на GeForce RTX 50.",
                     ReasonFor("upscaling.frameGenMultiplier",
                               up && fg != "off" ? "" : "Сначала включите генерацию кадров.")),
                std::move(mult)));
            g.items.push_back(SwitchRow("upscaling.reflex", "NVIDIA Reflex",
                                        "Кадр начинается ровно к освобождению видеокарты — ввод быстрее доходит до "
                                        "экрана. GeForce GTX 900 и новее.",
                                        up && fg == "fsr" ? "Не работает вместе с генерацией кадров FSR (у неё свой "
                                                            "swapchain)."
                                                          : ""));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Камера"};
            g.items.push_back(RangeRow("graphics.fov", "Поле зрения",
                                       "Относительно собственного поля зрения игры; при прицеливании и беге оно "
                                       "меняется как обычно. В кат-сценах — своё.",
                                       80, 130, 5, [](double v) { return fmt::format("{:.0f} %", v); }));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Тени, освещение и отражения"};
            g.items.push_back(SelectRow("graphics.shadowQuality", "Качество теней",
                                        "Размер теневых карт: солнце 2048 в игре. «Высокое» — 4096, чётче края теней, "
                                        "больше работы видеокарты и +64 МиБ видеопамяти; «Низкое» — 1024. Действует "
                                        "со следующего запуска."));
            g.items.push_back(LockedRow("Локальное освещение", false, "Как в игре",
                                        "R12: сам свет стоит мало (освещение — около 1,7 мс работы видеокарты), "
                                        "параметра качества у игры нет, поэтому пункта настроек нет."));
            g.items.push_back(SelectRow(
                "graphics.reflectionQuality", "Качество отражений",
                "«Низкое» — без экранных отражений: того, что видно на экране, в мокрой дороге и лужах. Сами источники "
                "света, блики, мокрота и собственное отражение воды остаются. Около 0,4 мс видеокарты за кадр (около "
                "2 % FPS в вечернем городе)."));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Текстуры и сглаживание"};
            g.items.push_back(SelectRow("graphics.textureQuality", "Качество текстур",
                                        "Игра грузит полноразмерные текстуры для 4 ближайших кварталов города. "
                                        "«Среднее» — для 2, «Низкое» — нигде: меньше видеопамяти и чтения с диска, "
                                        "вблизи размытее. Набора лучше, чем в игре, нет. Со следующего запуска."));
            g.items.push_back(SelectRow(
                "graphics.anisotropy",
                Text("Анизотропная фильтрация",
                     "Чёткость текстур под углом: дороги, стены и крыши вдаль. Игра просит 4x для большинства "
                     "материалов; выше — даль чётче. Тени, интерфейс и полноэкранные эффекты не меняются.",
                     ReasonFor("graphics.anisotropy")),
                ChoiceOpts("graphics.anisotropy")));
            g.items.push_back(SelectRow(
                "graphics.antiAliasing",
                Text("Сглаживание",
                     up ? "Сейчас сглаживает апскейлер (DLSS / FSR): здесь выбор между FXAA на его входе и ничем. "
                          "DLAA / FSR Native AA — «Качество» → «Нативное»."
                        : "«Как в игре» — SMAA T2x, а при нагрузке на видеокарту игра сама переходит на FXAA. "
                          "«Выключено» — чёткие, но зубчатые края.",
                     ReasonFor("graphics.antiAliasing")),
                ChoiceOpts("graphics.antiAliasing")));
            gs.push_back(std::move(g));
        }
        return gs;
    }

    std::vector<Group> EffectsGroups() {
        std::vector<Group> gs;
        {
            Group g{.title = "Размытие в движении"};
            g.items.push_back(SwitchRow("graphics.motionBlur", "Motion Blur",
                                        "Собственное размытие игры при быстром движении камеры и объектов. «Выкл.» — "
                                        "чёткая картинка; данные движения для DLSS / FSR не меняются."));
            g.items.push_back(RangeRow("graphics.motionBlurStrength", "Сила размытия", "Длина размытия: 100 % — как в игре.",
                                       10, 100, 10, [](double v) { return fmt::format("{:.0f} %", v); },
                                       Bv("graphics.motionBlur") ? "" : "Включите Motion Blur."));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Дым и частицы"};
            RowText smoke = Text("Качество дыма",
                                 "Сколько частиц создают эмиттеры дыма: костры, трубы, облака дымовых способностей. "
                                 "Меньше — реже дым и меньше работы видеокарты.",
                                 ReasonFor("graphics.smokeQuality"));
            smoke.chips = {{"Эксперимент", ChipKind::Exp}};
            g.items.push_back(SelectRow("graphics.smokeQuality", std::move(smoke), ChoiceOpts("graphics.smokeQuality")));
            RowText part = Text("Разрешение огня и дыма",
                                "Собственный режим игры: прозрачные частицы рисуются в половинном или четвертном "
                                "разрешении. В сценах с огнём заметно легче для видеокарты, края пламени мягче.",
                                ReasonFor("graphics.particleResolution"));
            part.chips = {{"Эксперимент", ChipKind::Exp}};
            g.items.push_back(
                SelectRow("graphics.particleResolution", std::move(part), ChoiceOpts("graphics.particleResolution")));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Погода"};
            g.items.push_back(SelectRow("graphics.rainShadows", "Тени дождя",
                                        "«Выключены» — карта укрытий от дождя пустая: земля под мостами, навесами и "
                                        "крышами становится такой же мокрой, как улица. Дождь, лужи и отражения "
                                        "остаются. Экономит время процессора в мокрую погоду, в сухую ничего не меняет."));
            g.items.push_back(LockedRow("Погода и мокрые поверхности", true, "Как в миссии",
                                        "R12: погода и время суток задаются миссией и сохранением и на лету не "
                                        "меняются. Сейчас есть только диагностический инструмент, пункта настроек нет."));
            gs.push_back(std::move(g));
        }
        {
            Group g{.kind = Group::Kind::NaList, .title = "Ещё недоступно: глубина резкости, зерно плёнки",
                    .id = Id("na-effects")};
            g.items.push_back(LockedRow("Глубина резкости", false, "Как в игре",
                                        "Выключатель у игры есть, но в сыгранных сценах он ничего видимого не менял. Не "
                                        "предлагается, пока не найдена сцена с эффектом."));
            g.items.push_back(LockedRow("Зерно плёнки", false, "Как в игре",
                                        "Проход в игре есть, но ни одна сыгранная сцена его не включает."));
            gs.push_back(std::move(g));
        }
        return gs;
    }

    // ------------------------------------------------------------------ Controls

    static std::string Mult(double v) {
        return Comma(fmt::format("{:.2f}", v)) + "×";
    }

    Item BindRow(const BindText& b) {
        const std::string id = BindingId(b.action);
        const bool capturing = capture == static_cast<int>(b.action);
        RowText t;
        t.label = std::string(b.label);
        t.help = std::string(b.help);
        if (capturing) {
            t.hint = "Нажмите клавишу или кнопку мыши… Esc — отмена";
        }
        std::vector<std::string> keys;
        for (const auto& k : std::get<S::Bindings>(V(id))) {
            keys.push_back(KeyLabel(k));
        }
        ButtonStyle bs;
        bs.small = true;
        const std::string label = capturing ? "Ожидание…" : "Изменить";
        float kw = 0;
        for (size_t i = 0; i < keys.size(); ++i) {
            kw += KeycapWidth(*u, keys[i]) + (i ? 6.0f : 0.0f);
        }
        const float bw = ButtonWidth(*u, label, {}, bs);
        const float cw = kw + (keys.empty() ? 0 : 10) + bw;
        const Action a = b.action;
        Item it = Row(std::move(t), cw, 30, [this, keys, kw, bw, bs, label, capturing, a](Rect r) {
            float x = r.x;
            for (size_t i = 0; i < keys.size(); ++i) {
                x += Keycap(*u, x, r.y + 2, keys[i], capturing && i == 0) + 6;
            }
            if (Button(*u, Id(BindingId(a), 3), {r.R() - bw, r.y, bw, 30}, label, {}, bs)) {
                if (capturing) {
                    CancelCapture();
                } else {
                    StartCapture(a);
                }
            }
        });
        if (capturing) {
            it.bg = kAccentWeak;
        }
        return it;
    }

    /// The PlayStation / Xbox / keyboard table under the button options (app.js renderPadMap).
    float PadMap(float x, float y, float w, bool draw) {
        Canvas& c = C();
        const std::string p = Sv("interface.prompts");
        const bool dim_ps = p == "xbox" || p == "keyboard", dim_xb = p == "playstation" || p == "keyboard",
                   dim_kb = p == "playstation" || p == "xbox";
        struct PadRow {
            Action a;
            std::string_view ps_icon, ps, xb_letter, xb;
            u32 xb_color;
        };
        static constexpr PadRow kRows[] = {
            {Action::Cross, "ps-cross", "Крест", "A", "A", 0x6cc04a},
            {Action::Circle, "ps-circle", "Круг", "B", "B", 0xe86060},
            {Action::Square, "ps-square", "Квадрат", "X", "X", 0x4a90e2},
            {Action::Triangle, "ps-triangle", "Треугольник", "Y", "Y", 0xe8c440},
            {Action::L1, "", "L1", "", "LB", 0},
            {Action::R1, "", "R1", "", "RB", 0},
            {Action::L2, "", "L2", "", "LT", 0},
            {Action::R2, "", "R2", "", "RT", 0},
            {Action::L3, "", "L3", "", "LS", 0},
            {Action::R3, "", "R3", "", "RS", 0},
            {Action::Options, "", "Options", "", "Menu", 0},
            {Action::TouchPad, "", "Сенсорная панель", "", "View", 0},
        };
        static constexpr u32 kPsColors[] = {0x7fa6f5, 0xf0787f, 0xe59bd9, 0x61d1aa};
        const Font cf = F(13), hf = Upper(11, 600, 0.05f);
        const auto glyph_w = [&](std::string_view text) { return 20 + 8 + TW(text, cf); };
        // column widths: max-content, the rest shared in proportion (table-layout: auto)
        float mx[3] = {TW("PLAYSTATION", hf), TW("XBOX", hf), TW("КЛАВИАТУРА И МЫШЬ", hf)};
        std::vector<std::vector<std::string>> kb(std::size(kRows));
        for (size_t i = 0; i < std::size(kRows); ++i) {
            const auto& r = kRows[i];
            mx[0] = std::max(mx[0], r.ps_icon.empty() ? KeycapWidth(*u, r.ps) : glyph_w(r.ps));
            mx[1] = std::max(mx[1], r.xb_letter.empty() ? KeycapWidth(*u, r.xb) : glyph_w(r.xb));
            float kw = 0;
            for (const auto& k : std::get<S::Bindings>(V(BindingId(r.a)))) {
                kb[i].push_back(KeyLabel(k));
                kw += KeycapWidth(*u, kb[i].back()) + (kb[i].size() > 1 ? TW(" ", cf) : 0.0f);
            }
            mx[2] = std::max(mx[2], kw);
        }
        float total = 0;
        for (float& m : mx) {
            m += 36;
            total += m;
        }
        const float extra = std::max(0.0f, w - total);
        float cw[3];
        for (int i = 0; i < 3; ++i) {
            cw[i] = mx[i] + extra * mx[i] / total;
        }
        float h = 14 + LineOf(12) + 6; // .subhead (the first of its type: no border)
        if (draw) {
            c.Text("ОБОЗНАЧЕНИЯ КНОПОК", {x + 18, y + 14, w - 36, LineOf(12)}, Upper(12, 600, 0.05f), kMuted,
                   LineOf(12), Align::Left, false);
        }
        const float head_h = 8 + LineOf(11) + 8;
        if (draw) {
            const std::string_view heads[3] = {"PLAYSTATION", "XBOX", "КЛАВИАТУРА И МЫШЬ"};
            const bool dims[3] = {dim_ps, dim_xb, dim_kb};
            float cx = x;
            for (int i = 0; i < 3; ++i) {
                c.Text(heads[i], {cx + 18, y + h + 8, cw[i] - 36 + 40, LineOf(11)}, hf, dims[i] ? kDim : kMuted,
                       LineOf(11), Align::Left, false);
                cx += cw[i];
            }
            c.FillRect({x, y + h + head_h, w, 1}, kBorder);
        }
        h += head_h + 1;
        for (size_t i = 0; i < std::size(kRows); ++i) {
            const auto& r = kRows[i];
            const float rh = 8 + 26 + 8;
            if (draw) {
                const float cy = y + h + 8;
                float cx = x + 18;
                const auto cell_glyph = [&](std::string_view text, bool dim, auto&& icon) {
                    icon(Rect{cx, cy + 3, 20, 20});
                    const float lh = LineOf(13);
                    c.Text(text, {cx + 28, cy + (26 - lh) / 2, 200, lh}, cf, dim ? kDim : kText, lh, Align::Left, false);
                };
                if (!r.ps_icon.empty()) {
                    const u32 col = kPsColors[i];
                    cell_glyph(r.ps, dim_ps, [&](Rect g) {
                        const float k = 20.0f / 24.0f;
                        c.FillCircle(g.x + 10, g.y + 10, 10 * k, Rgb(0x1d1f25));
                        c.StrokeCircle(g.x + 10, g.y + 10, 10 * k, Rgb(0x4a4e59), 1.2f * k);
                        c.Icon(IconPath(r.ps_icon), g, Rgb(col), 2.0f);
                    });
                } else {
                    Keycap(*u, cx, cy, r.ps, false, dim_ps ? kDim : kText);
                }
                cx = x + cw[0] + 18;
                if (!r.xb_letter.empty()) {
                    cell_glyph(r.xb, dim_xb, [&](Rect g) {
                        const float k = 20.0f / 24.0f;
                        c.FillCircle(g.x + 10, g.y + 10, 10 * k, Rgb(0x1d1f25));
                        c.StrokeCircle(g.x + 10, g.y + 10, 10 * k, Rgb(r.xb_color), 1.4f * k);
                        c.Text(r.xb_letter, {g.x, g.y + (20 - 11 * k * 1.2f) / 2, 20, 11 * k * 1.2f}, F(11 * k, 700),
                               Rgb(r.xb_color), 11 * k * 1.2f, Align::Center, false);
                    });
                } else {
                    Keycap(*u, cx, cy, r.xb, false, dim_xb ? kDim : kText);
                }
                cx = x + cw[0] + cw[1] + 18;
                for (const auto& k : kb[i]) {
                    cx += Keycap(*u, cx, cy, k, false, dim_kb ? kDim : kText) + TW(" ", cf);
                }
                if (i + 1 < std::size(kRows)) {
                    c.FillRect({x, y + h + rh, w, 1}, kBorder);
                }
            }
            h += rh + (i + 1 < std::size(kRows) ? 1.0f : 0.0f);
        }
        return h;
    }

    std::vector<Group> ControlsGroups() {
        std::vector<Group> gs;
        if (tab == 0) {
            {
                Group g{.title = "Мышь"};
                g.items.push_back(RangeRow("input.mouseSensitivity", "Чувствительность мыши",
                                           "Мышь управляет правым стиком игры, поэтому «Чувствительность прицела» в "
                                           "настройках игры тоже действует.",
                                           0.1, 5, 0.05, &Mult));
                g.items.push_back(SwitchRow("input.invertY", "Инвертировать мышь по вертикали",
                                            "Мышь вверх — камера вниз. Инверсия в настройках игры тоже действует на "
                                            "мышь."));
                g.items.push_back(RangeRow("input.mouseDeadzone", "Мёртвая зона мыши",
                                           "Отклонение стика, с которого начинается поворот при медленном движении "
                                           "мыши. Меньше — плавнее медленные движения.",
                                           0, 0.5, 0.01, [](double v) { return Comma(fmt::format("{:.2f}", v)); }));
                g.items.push_back(SwitchRow("input.captureMouse", "Захватывать курсор",
                                            "Курсор скрыт и удерживается в окне; Esc отпускает и снова захватывает. "
                                            "Выключено — только клавиатура."));
                gs.push_back(std::move(g));
            }
            {
                Group g{.title = "Режим G (граффити)"};
                g.items.push_back(RangeRow("input.motionSensitivity", "Чувствительность наклона",
                                           "Скорость, с которой мышь поворачивает контроллер, удерживаемый боком.", 0.1,
                                           5, 0.05, &Mult));
                g.items.push_back(SelectRow("input.sideways", "Положение контроллера", ""));
                gs.push_back(std::move(g));
            }
            {
                Group g{.title = "Клавиши"};
                g.items.push_back(NoteItem("Нажмите «Изменить», затем клавишу или кнопку мыши. <b>Esc</b> — отмена. "
                                           "F1, F10 и F11 заняты самим SecondSonPC. Новая клавиша заменяет первую "
                                           "клавишу действия."));
                for (const auto& bg : kBindGroups) {
                    g.items.push_back(SubheadItem(std::string(bg.title), true));
                    for (const auto& b : bg.items) {
                        g.items.push_back(BindRow(b));
                    }
                }
                gs.push_back(std::move(g));
            }
        } else {
            {
                Group g{.title = "Контроллер"};
                g.items.push_back(NoteItem(
                    sys.controller.empty()
                        ? std::string("Поддерживаются <b>DualShock 4</b> и другие контроллеры через SDL. Статус: "
                                      "контроллер не обнаружен.")
                        : fmt::format("Поддерживаются <b>DualShock 4</b> и другие контроллеры через SDL. Статус: "
                                      "подключён <b>{}</b>.",
                                      sys.controller)));
                g.items.push_back(RangeRow("gamepad.vibration", "Вибрация",
                                           "Сила вибрации относительно запроса игры; 0 — выключена.", 0, 100, 10,
                                           [](double v) { return v == 0 ? std::string("Выкл.") : fmt::format("{:.0f} %", v); }));
                g.items.push_back(RangeRow("gamepad.cameraSensitivity", "Чувствительность камеры (стик)",
                                           "Множитель правого стика поверх «Чувствительности прицела» игры.", 0.25, 2,
                                           0.05, &Mult));
                g.items.push_back(SwitchRow("gamepad.invertCameraX", "Инвертировать камеру по горизонтали",
                                            "В настройках игры есть своя инверсия — не включайте обе."));
                g.items.push_back(SwitchRow("gamepad.invertCameraY", "Инвертировать камеру по вертикали",
                                            "В настройках игры есть своя инверсия — не включайте обе."));
                gs.push_back(std::move(g));
            }
            {
                Group g{.title = "Кнопки"};
                g.items.push_back(SelectRow("interface.prompts", "Подсказки кнопок",
                                            "«Автоматически» — по последнему использованному устройству: Xbox — его "
                                            "кнопки, PlayStation — оригинальные значки игры."));
                g.items.push_back(
                    SelectRow("game.enterButton", "Кнопка подтверждения в меню игры", "Системная настройка PS4."));
                g.items.push_back(SelectRow("gamepad.menuButton", "Быстрый вызов настроек в игре",
                                            "Настройки всегда открываются из экрана «Настройки» игры. Кнопку Guide Xbox "
                                            "может перехватить Game Bar, кнопку PS — Steam."));
                Item map;
                map.border = false;
                map.height = [this](float w) { return PadMap(0, 0, w, false); };
                map.draw = [this](Rect r) { PadMap(r.x, r.y, r.w, true); };
                g.items.push_back(std::move(map));
                gs.push_back(std::move(g));
            }
        }
        return gs;
    }

    // ------------------------------------------------------------------ Audio, Advanced

    std::vector<Group> AudioGroups() {
        std::vector<Group> gs;
        {
            Group g{.title = "Громкость"};
            g.items.push_back(RangeRow("audio.volume", "Общая громкость",
                                       "Общая громкость SecondSonPC поверх настроек звука игры. Собственный аудиовывод "
                                       "(--native audio) её пока не поддерживает.",
                                       0, 100, 5, [](double v) { return fmt::format("{:.0f} %", v); }));
            g.items.push_back(NoteItem("Музыка, эффекты и речь — в игре: «Параметры» → «Звук». Отдельных ползунков для "
                                       "них в SecondSonPC нет."));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Вывод"};
            std::vector<Opt> devs{{"", "Устройство Windows по умолчанию"}};
            for (const auto& d : sys.audio_devices) {
                devs.push_back({d, d});
            }
            const std::string cur = Sv("audio.device");
            if (!cur.empty() && std::ranges::find(sys.audio_devices, cur) == sys.audio_devices.end()) {
                devs.push_back({cur, cur + " (не подключено)"});
            }
            g.items.push_back(SelectRow("audio.device",
                                        Text("Устройство вывода",
                                             "Применяется при запуске игры. Собственный аудиовывод (--native audio) "
                                             "всегда использует устройство Windows по умолчанию.",
                                             ReasonFor("audio.device")),
                                        std::move(devs)));
            gs.push_back(std::move(g));
        }
        return gs;
    }

    void OpenFolder(const std::filesystem::path& dir, bool create) {
        std::error_code ec;
        if (create) {
            std::filesystem::create_directories(dir, ec);
        }
        if (!std::filesystem::is_directory(dir, ec)) {
            ToastMsg("Этой папки ещё нет — она появится после первого запуска игры.", true);
            return;
        }
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    std::filesystem::path LogsDir() const {
        return cfg.logs_dir.is_absolute() ? cfg.logs_dir : SecondSon::App::RootDir() / cfg.logs_dir;
    }

    std::vector<Group> AdvancedGroups() {
        std::vector<Group> gs;
        {
            Group g{.title = "Файлы"};
            RowText s;
            s.label = "Папка настроек";
            s.path = Utf8(settings_file.parent_path());
            s.help = "settings.toml — ваши изменения поверх SecondSonPC.toml; launcher.toml — профиль лаунчера.";
            g.items.push_back(ButtonRow(std::move(s), "Открыть папку", "folder", ButtonKind::Normal,
                                        [this] { OpenFolder(settings_file.parent_path(), true); }));
            RowText l;
            l.label = "Папка логов";
            l.path = Utf8(LogsDir());
            l.help = "Журналы запусков — пригодятся для отчёта об ошибке.";
            g.items.push_back(ButtonRow(std::move(l), "Открыть папку", "folder", ButtonKind::Normal,
                                        [this] { OpenFolder(LogsDir(), false); }));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Шейдеры и кэш"};
            g.items.push_back(SwitchRow("shaders.predictAll", "Компилировать все шейдеры игры до запуска",
                                        "Окно консоли компилирует шейдеры из файлов игры: в первый раз — несколько "
                                        "минут, потом — около секунды."));
            g.items.push_back(SwitchRow("shaders.precompile", "Готовить шейдеры при запуске",
                                        "Конвейеры из прошлых запусков компилируются до начала игры, чтобы не "
                                        "останавливать её потом."));
            g.items.push_back(SwitchRow("shaders.showProgress", "Показывать прогресс подготовки",
                                        "Панель поверх окна; Esc на ней сразу запускает игру, остальное готовится в "
                                        "фоне.",
                                        Bv("shaders.precompile") ? "" : "Включите «Готовить шейдеры при запуске»."));
            g.items.push_back(RangeRow("shaders.maxSize", "Размер кэша шейдеров",
                                       "Файл кэша растёт до этого размера; дальше новое не добавляется.", 256, 8192,
                                       256, [](double v) { return fmt::format("{:.0f} МиБ", v); }));
            g.items.push_back(RangeRow("shaders.threads", "Потоков подготовки",
                                       "Авто — половина логических процессоров, не больше 6.", 0, 12, 1,
                                       [](double v) { return v == 0 ? std::string("Авто") : fmt::format("{:.0f}", v); }));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Игра"};
            g.items.push_back(
                SelectRow("game.language", "Язык игры", "Системный язык эмулируемой PS4: текст и озвучка игры."));
            gs.push_back(std::move(g));
        }
        {
            Group g{.title = "Восстановление"};
            RowText t;
            t.label = "Восстановить все настройки";
            t.help = "Все разделы и эксперименты вернутся к значениям по умолчанию.";
            g.items.push_back(ButtonRow(std::move(t), "Восстановить…", "reset", ButtonKind::Danger,
                                        [this] { RestoreAll(); }, !game_running && !opt.read_only));
            gs.push_back(std::move(g));
        }
        gs.push_back(ExperimentsGroup());
        return gs;
    }

    Group ExperimentsGroup() {
        Group g{.kind = Group::Kind::Experiments, .id = Id("experiments")};
        if (!exp_open) {
            return g;
        }
        // .warn
        Item warn;
        warn.border = false;
        auto rich = std::make_shared<Rich>(ParseRich(fmt::format(
            "Эти режимы влияют на стабильность и плавность. Значения по умолчанию берутся из профиля запуска — сейчас "
            "<b>{}</b>. Само ничего не включается; изменённый пункт помечается «Изменено».",
            kProfileLabel[static_cast<int>(state.profile)])));
        warn.height = [this, rich](float w) {
            return 14 + 1 + 10 + C().Measure(rich->text, F(13), LineOf(13), w - 36 - 2 - 24 - 28, true, rich->spans).h +
                   10 + 1 + 14;
        };
        warn.draw = [this, rich](Rect r) {
            const Rect box{r.x + 18, r.y + 14, r.w - 36, r.h - 28};
            C().FillRect(box, kAmberWeak, kRadius);
            C().Border(box, Rgb(0xf0a73a, 0.35f), 1, kRadius);
            C().Icon(IconPath("warn"), {box.x + 13, box.y + 12, 18, 18}, kAmber);
            C().Text(rich->text, {box.x + 13 + 28, box.y + 11, box.w - 26 - 28 + 1, box.h - 22}, F(13), Rgb(0xf6c98a),
                     LineOf(13), Align::Left, true, rich->spans);
        };
        g.items.push_back(std::move(warn));
        std::string_view group;
        for (const auto& e : S::Experiments()) {
            if (e.group_ru != group) {
                group = e.group_ru;
                g.items.push_back(SubheadItem(std::string(group), true));
            }
            g.items.push_back(ExperimentRow(e));
        }
        RowText foot;
        foot.label = "Вернуть эксперименты к профилю";
        foot.help = "Убирает изменения этого подраздела; остальные настройки не трогаются.";
        g.items.push_back(ButtonRow(std::move(foot), "Вернуть к профилю", "reset", ButtonKind::Normal,
                                    [this] {
                                        for (const auto& e : S::Experiments()) {
                                            const std::string id = S::ExperimentId(e.env);
                                            V(id) = svc->Default(D(id));
                                        }
                                        ToastMsg("Эксперименты возвращены к профилю — не забудьте сохранить.");
                                    },
                                    !ExperimentsDefault()));
        return g;
    }

    Item ExperimentRow(const S::Experiment& e) {
        const std::string id = S::ExperimentId(e.env);
        const ExpText* text = FindExpText(e.env);
        RowText t;
        t.label = text != nullptr ? std::string(text->label) : std::string(e.ru);
        t.help = text != nullptr ? std::string(text->help) : std::string(e.help_ru);
        t.env = std::string(e.env);
        if (InEnv(StandardEnv(), e.env)) {
            t.chips.push_back({"play.cmd", ChipKind::Preset});
        } else if (InEnv(ExperimentalEnv(), e.env)) {
            t.chips.push_back({"play_experimental", ChipKind::Dev});
        } else {
            t.chips.push_back({"Эксперимент, выкл.", ChipKind::Na});
        }
        if (!S::Equal(V(id), svc->Default(D(id)))) {
            t.chips.push_back({"Изменено", ChipKind::Changed});
        }
        if (const std::string_view parent = D(id).requires_on; !parent.empty() && !Bv(parent)) {
            const S::Experiment* pe = S::FindExperiment(D(parent).key);
            const ExpText* pt = pe != nullptr ? FindExpText(pe->env) : nullptr;
            t.reason = fmt::format("Нужен пункт «{}».", pt != nullptr ? pt->label : D(parent).ru);
            t.disabled = true;
        }
        if (e.type == S::Type::Choice) {
            std::vector<Opt> opts;
            for (const auto& c : e.choices) {
                opts.push_back({std::string(c.value), std::string(c.ru)});
            }
            return SelectRow(id, std::move(t), std::move(opts));
        }
        const bool enabled = !t.disabled;
        return Row(std::move(t), kCtlW, 24, [this, id, enabled](Rect r) {
            const bool on = Bv(id);
            const float sx = r.R() - 42, lh = LineOf(13);
            C().Text(on ? "Вкл." : "Выкл.", {sx - 10 - 200, r.y + (24 - lh) / 2, 200, lh}, F(13), kMuted, lh,
                     Align::Right, false);
            if (Switch(*u, Id(id), {sx, r.y, 42, 24}, on, enabled)) {
                V(id) = !on;
            }
        });
    }

    std::vector<Group> Groups(int sec) {
        switch (sec) {
        case kDisplay: return DisplayGroups();
        case kGraphics: return GraphicsGroups();
        case kEffects: return EffectsGroups();
        case kControls: return ControlsGroups();
        case kAudio: return AudioGroups();
        case kAdvanced: return AdvancedGroups();
        default: return {};
        }
    }

    // ------------------------------------------------------------------ drawing: groups

    float GroupHeight(const Group& g, float w) {
        float h = 2;
        bool open = true;
        if (g.kind == Group::Kind::Box) {
            h += g.title.empty() ? 0 : kGroupTitleH;
        } else if (g.kind == Group::Kind::NaList) {
            open = lists_open[g.id];
            h += 12 + LineOf(13) + 12 + (open ? 1 : 0);
        } else {
            open = exp_open;
            h += 14 + kLabelLh + 2 + kHelpLh + 14 + (open ? 1 : 0);
        }
        if (!open) {
            return h;
        }
        for (size_t i = 0; i < g.items.size(); ++i) {
            h += g.items[i].height(w - 2) + (g.items[i].border && i + 1 < g.items.size() ? 1.0f : 0.0f);
        }
        return h;
    }

    void DrawGroup(Group& g, float x, float y, float w) {
        Canvas& c = C();
        Ui& ui = *u;
        const float gh = GroupHeight(g, w);
        const Rect box{x, y, w, gh};
        c.FillRect(box, kSurface, kRadiusL);
        c.Border(box, kBorder, 1, kRadiusL);
        float cy = y + 1;
        const float ix = x + 1, iw = w - 2;
        bool open = true;
        if (g.kind == Group::Kind::Box) {
            if (!g.title.empty()) {
                c.Text(UpperText(g.title), {ix + 18, cy + 13, iw - 36, LineOf(12)}, Upper(12, 600, 0.06f), kMuted, LineOf(12),
                       Align::Left, false);
                c.FillRect({ix, cy + kGroupTitleH - 1, iw, 1}, kBorder);
                cy += kGroupTitleH;
            }
        } else if (g.kind == Group::Kind::NaList) {
            open = lists_open[g.id];
            const float sh = 12 + LineOf(13) + 12;
            const Ui::Hit h = ui.Interact(g.id, {ix, cy, iw, sh});
            const float rot = ui.Anim(Id(g.id, 5), open ? 1.0f : 0.0f, 0.12f);
            const Color col = h.hover ? kText : kMuted;
            const float ax = ix + 18;
            DrawChevron({ax, cy + (sh - 14) / 2, 14, 14}, col, rot);
            c.Text(g.title, {ax + 14 + 8, cy + 12, iw - 36 - 22, LineOf(13)}, F(13), col, LineOf(13), Align::Left,
                   false);
            if (h.focus_ring) {
                ui.FocusRing({ix, cy, iw, sh}, kRadiusL, 2, kAccent);
            }
            if (h.clicked) {
                lists_open[g.id] = !open;
            }
            cy += sh;
            if (open) {
                c.FillRect({ix, cy, iw, 1}, kBorder);
                cy += 1;
            }
        } else {
            open = exp_open;
            const float dh = 14 + kLabelLh + 2 + kHelpLh + 14;
            const Rect dr{ix, cy, iw, dh};
            const Ui::Hit h = ui.Interact(g.id, dr);
            if (h.hover) {
                if (open) {
                    c.FillRect4(dr, kSurface2, kRadiusL - 1, kRadiusL - 1, 0, 0);
                } else {
                    c.FillRect(dr, kSurface2, kRadiusL - 1);
                }
            }
            const float rot = ui.Anim(Id(g.id, 5), open ? 1.0f : 0.0f, 0.12f);
            DrawChevron({ix + 18, cy + (dh - 16) / 2, 16, 16}, kMuted, rot);
            c.Text("Эксперименты", {ix + 18 + 16 + 12, cy + 14, iw - 64, kLabelLh}, F(14, 600), kText, kLabelLh,
                   Align::Left, false);
            c.Text("Переменные SSPC_* для разработки и замеров. Закрыто по умолчанию.",
                   {ix + 18 + 16 + 12, cy + 14 + kLabelLh + 2, iw - 64, kHelpLh}, kHelp, kMuted, kHelpLh, Align::Left,
                   false);
            if (h.focus_ring) {
                ui.FocusRing(dr, kRadiusL, 2, kAccent);
            }
            if (h.clicked) {
                exp_open = !exp_open;
            }
            cy += dh;
            if (open) {
                c.FillRect({ix, cy, iw, 1}, kBorder);
                cy += 1;
            }
        }
        if (!open) {
            return;
        }
        for (size_t i = 0; i < g.items.size(); ++i) {
            Item& it = g.items[i];
            const float ih = it.height(iw);
            const bool sep = it.border && i + 1 < g.items.size();
            if (it.bg.a > 0) {
                c.FillRect({ix, cy, iw, ih + (sep ? 1.0f : 0.0f)}, it.bg);
            }
            it.draw({ix, cy, iw, ih});
            cy += ih;
            if (sep) {
                c.FillRect({ix, cy, iw, 1}, kBorder);
                cy += 1;
            }
        }
    }

    /// The mockup's "chev" icon turned by `t` x 90 degrees (details / disclosure).
    void DrawChevron(Rect r, Color col, float t) {
        if (t <= 0.001f) {
            C().Icon(IconPath("chev"), r, col);
            return;
        }
        // rotate around the box centre: the chevron's path rotated by 90 degrees is a downward one; between the two
        // the frames of the 0.12 s transition are interpolated as a rotation of the points
        const float a = t * 1.5707963f, cs = std::cos(a), sn = std::sin(a);
        const auto rot = [&](float px, float py) {
            const float dx = px - 12, dy = py - 12;
            return std::pair<float, float>{12 + dx * cs - dy * sn, 12 + dx * sn + dy * cs};
        };
        const auto [x0, y0] = rot(9, 5.5f);
        const auto [x1, y1] = rot(15.5f, 12);
        const auto [x2, y2] = rot(9, 18.5f);
        C().Icon(fmt::format("M{:.3f} {:.3f}L{:.3f} {:.3f}L{:.3f} {:.3f}", x0, y0, x1, y1, x2, y2), r, col);
    }

    // ------------------------------------------------------------------ drawing: the window

    void DrawTitlebar() {
        Canvas& c = C();
        Ui& ui = *u;
        const Rect bar{b, b, W - 2 * b, kTitleH - 1};
        c.FillRect(bar, Rgb(0x0b0c0e));
        c.FillRect({b, b + kTitleH - 1, W - 2 * b, 1}, kBorder);
        c.Icon(IconPath("mark"), {b + 12, b + (kTitleH - 1 - 18) / 2, 18, 18}, WithAlpha(kAccent, 0.95f), 0, true);
        const float lh = LineOf(13);
        c.Text(std::string(SecondSon::App::kProduct.name), {b + 12 + 18 + 10, b + (kTitleH - 1 - lh) / 2, 200, lh}, F(13, 600, 0.01f), kText, lh,
               Align::Left, false);
        const std::string_view icons[3] = {"win-min", "win-max", "win-close"};
        for (int i = 0; i < 3; ++i) {
            const Rect r{W - b - 46 * (3 - i), b - 0.5f, 46, kTitleH};
            const Ui::Hit h = ui.Interact(Id("win", static_cast<u64>(i)), r, true, true, Cursor::Arrow);
            Color fg = kMuted;
            if (h.hover) {
                c.FillRect(r, i == 2 ? Rgb(0xc42b2b) : kSurface3);
                fg = i == 2 ? kWhite : kText;
            }
            c.Icon(IconPath(icons[i]), {r.x + 17, r.y + 12, 12, 12}, fg, 1.2f, false, 12, true);
            if (h.focus_ring) {
                ui.FocusRing(r, 0, -2, kAccent);
            }
            if (h.clicked) {
                request = i == 0 ? WindowRequest::Minimize : i == 1 ? WindowRequest::Maximize : WindowRequest::Close;
            }
        }
    }

    void DrawNav() {
        Canvas& c = C();
        Ui& ui = *u;
        const Rect nav{b, b + kTitleH, kNavW, H - 2 * b - kTitleH};
        c.FillRect(nav, kSurface);
        c.FillRect({nav.R() - 1, nav.y, 1, nav.h}, kBorder);
        const float x = nav.x + 10, w = kNavW - 1 - 20;
        float y = nav.y + 14;
        // .nav-brand
        const float l11 = 11 * 1.15f, l16 = 16 * 1.15f;
        c.Text("inFAMOUS", {x + 10, y + 4, w, l11}, Fd(11, 400, 0.04f), kMuted, l11, Align::Left, false);
        c.Text(std::string(SecondSon::App::kProduct.brand), {x + 10, y + 4 + l11, w, l16}, Fd(16, 800, 0.06f), kText, l16, Align::Left, false);
        c.Text("PC EDITION", {x + 10, y + 4 + l11 + l16, w, l11}, Fd(11, 600, 0.08f), kAccent, l11, Align::Left,
               false);
        y += 4 + l11 + l16 + l11 + 14;
        const float ih = 9 + kLabelLh + 9;
        for (int s = 0; s < kSecCount; ++s) {
            if (s == 1) {
                y += 2 + 8;
                c.FillRect({x + 10, y, w - 20, 1}, kBorder);
                y += 1 + 8;
            } else if (s > 1) {
                y += 2;
            }
            const Rect r{x, y, w, ih};
            const Ui::Hit h = ui.Interact(Id("nav", static_cast<u64>(s)), r);
            const bool cur = s == current;
            if (cur) {
                c.FillRect(r, kAccentWeak, kRadius);
                c.FillRect({r.x, r.y + 9, 3, r.h - 18}, kAccent, 2);
            } else if (h.hover) {
                c.FillRect(r, kSurface3, kRadius);
            }
            const Color fg = cur || h.hover ? kText : kMuted;
            c.Icon(IconPath(kSecs[s].icon), {r.x + 12, r.y + (ih - 18) / 2, 18, 18}, cur ? kAccent : fg);
            c.Text(kSecs[s].label, {r.x + 12 + 18 + 12, r.y + 9, r.w - 60, kLabelLh}, F(14), fg, kLabelLh,
                   Align::Left, false);
            if (s != kHome && Dirty(s)) {
                c.FillCircle(r.R() - 12 - 3.5f, r.y + ih / 2, 3.5f, kAmber);
            }
            if (h.focus_ring) {
                ui.FocusRing(r, kRadius, -2, kAccent);
            }
            if (h.clicked) {
                Go(s);
            }
            y += ih;
        }
        // .nav-foot
        const float fl = 12 * 1.4f;
        c.Text("Те же настройки есть в игре:", {x + 10, nav.B() - 12 - 2 * fl, w - 20, fl}, F(12), kDim, fl,
               Align::Left, false);
        c.Text("Esc → «Настройки».", {x + 10, nav.B() - 12 - fl, w - 20, fl}, F(12), kDim, fl, Align::Left, false);
    }

    void DrawSection(Rect area) {
        Canvas& c = C();
        Ui& ui = *u;
        const Rect view{area.x, area.y, area.w, area.h - kActionsH};
        ScrollState& s = scroll[current];
        if (opt.scroll > 0 && s.content > 0) {
            // a screenshot's --launcher-scroll, once the section's height is known
            s.offset = s.target = std::clamp(opt.scroll, 0.0f, std::max(0.0f, s.content - view.h));
            opt.scroll = 0;
        }
        if (!modal) {
            ScrollInput(ui, s, view);
        }
        c.PushClip(view);
        ui.PushHitClip(view);
        const float x = view.x + 32, w = view.w - kGutter - 64;
        const float top = view.y - s.offset;
        float y = top + 26;
        // .page-head
        const float h1 = 24 * 1.2f;
        c.Text(kSecs[current].label, {x, y, w, h1}, Fd(24, 600, 0.005f), kText, h1, Align::Left, false);
        y += h1 + 6;
        const float lw = std::min(640.0f, w);
        const TextBox lead = c.Measure(kSecs[current].lead, kBody, kLabelLh, lw);
        c.Text(kSecs[current].lead, {x, y, lw + 1, lead.h}, kBody, kMuted, kLabelLh);
        y += lead.h;
        const float gw = std::min(kGroupMax, w);
        if (current == kControls) {
            // .tabs
            y += 18;
            const std::string_view labels[2] = {"Клавиатура и мышь", "Геймпад"};
            const std::string_view icons[2] = {"keyboard", "gamepad"};
            float tx = x;
            const float th = 9 + kLabelLh + 11;
            for (int i = 0; i < 2; ++i) {
                const float tw = 14 + 17 + 8 + TW(labels[i], kBody) + 14;
                const Rect r{tx, y, tw, th};
                const Ui::Hit h = ui.Interact(Id("tab", static_cast<u64>(i)), r);
                const bool sel = i == tab;
                if (h.hover) {
                    c.FillRect4(r, kSurface2, kRadius, kRadius, 0, 0);
                }
                const Color fg = sel || h.hover ? kText : kMuted;
                c.Icon(IconPath(icons[i]), {r.x + 14, r.y + (th - 17) / 2, 17, 17}, fg);
                c.Text(labels[i], {r.x + 14 + 17 + 8, r.y + 9, tw, kLabelLh}, kBody, fg, kLabelLh, Align::Left, false);
                if (h.focus_ring) {
                    ui.FocusRing(r, kRadius, -2, kAccent);
                }
                if (h.clicked && !sel) {
                    tab = i;
                    CancelCapture();
                    s = {};
                }
                if (h.focused && (ui.TakeKey(VK_RIGHT) || ui.TakeKey(VK_LEFT))) {
                    tab = 1 - tab;
                    ui.SetFocus(Id("tab", static_cast<u64>(tab)), true);
                    CancelCapture();
                }
                tx += tw + 4;
            }
            c.FillRect({x, y + th, gw, 1}, kBorder);
            // the selected tab's line over the border
            float sx = x;
            for (int i = 0; i < tab; ++i) {
                sx += 14 + 17 + 8 + TW(labels[i], kBody) + 14 + 4;
            }
            const float stw = 14 + 17 + 8 + TW(labels[tab], kBody) + 14;
            c.FillRect({sx + 10, y + th - 1, stw - 20, 2}, kAccent, 2);
            y += th + 1;
        }
        auto groups = Groups(current);
        for (auto& g : groups) {
            y += 18;
            DrawGroup(g, x, y, gw);
            y += GroupHeight(g, gw);
        }
        y += 28;
        ui.PopHitClip();
        c.PopClip();
        ScrollBar(ui, Id("scrollbar", static_cast<u64>(current)), s, view, y - top, kGutter);
        DrawActions({area.x, area.B() - kActionsH, area.w, kActionsH});
    }

    void DrawActions(Rect r) {
        Canvas& c = C();
        c.FillRect(r, kSurface);
        c.FillRect({r.x, r.y, r.w, 1}, kBorder);
        const bool dirty = Dirty(current);
        if (dirty) {
            c.FillRect({r.x, r.y + 1, r.w, 2}, kAmber);
        }
        const float lh = LineOf(13), cy = r.y + 1 + 12 + 17;
        c.FillCircle(r.x + 32 + 4, cy, 4, dirty ? kAmber : kDim);
        std::string state_text = dirty ? "Есть несохранённые изменения" : "Изменений нет";
        if (game_running) {
            state_text += " · игра запущена: сохранение после её закрытия";
        }
        c.Text(state_text, {r.x + 32 + 8 + 8, cy - lh / 2, r.w - 400, lh}, F(13), dirty ? kAmber : kMuted, lh,
               Align::Left, false);
        ButtonStyle save;
        save.kind = ButtonKind::Primary;
        ButtonStyle reset;
        reset.kind = ButtonKind::Ghost;
        const float sw = ButtonWidth(*u, "Сохранить", "save", save), rw = ButtonWidth(*u, "По умолчанию", "reset", reset);
        const float bx = r.R() - 32 - sw, by = r.y + 1 + 12;
        if (Button(*u, Id("reset-section"), {bx - 10 - rw, by, rw, 34}, "По умолчанию", "reset", reset,
                   !IsDefault(current))) {
            ResetSection(current);
        }
        if (Button(*u, Id("save-section"), {bx, by, sw, 34}, "Сохранить", "save", save,
                   dirty && !game_running && !opt.read_only)) {
            SaveSection(current);
        }
    }

    // ------------------------------------------------------------------ Home

    void DrawHome(Rect a) {
        Canvas& c = C();
        Ui& ui = *u;
        // .home-art: the game's own art (sce_sys/pic0.png in the game's folder), the gradients, the static rain
        c.PushClip(a);
        c.FillRect(a, Rgb(0x101114));
        if (art) {
            c.ImageCover(*art, a, 0.70f, 0.30f);
        }
        const Stop amber[] = {{0, Rgb(0xf0a73a, 0.1f)}, {0.7f, Rgb(0xf0a73a, 0)}};
        c.RadialGradient(a, 0.6f, 0.4f, 0.08f, 0.06f, amber);
        const Stop accent[] = {{0, Rgb(0xe5336c, 0.34f)}, {0.6f, Rgb(0xe5336c, 0)}};
        c.RadialGradient(a, 1.2f, 0.7f, 0.85f, 1.0f, accent);
        const Stop up[] = {{0, Rgb(0x0a0a0c, 0.96f)}, {0.42f, Rgb(0x0a0a0c, 0.2f)}, {1, Rgb(0x0a0a0c, 0.45f)}};
        c.LinearGradient(a, 0, up);
        const Stop side[] = {{0, Rgb(0x0c0c0f, 0.97f)}, {0.34f, Rgb(0x0c0c0f, 0.86f)}, {0.66f, Rgb(0x0c0c0f, 0.35f)},
                             {1, Rgb(0x0c0c0f, 0.55f)}};
        c.LinearGradient(a, 90, side);
        c.PushOpacity(0.22f);
        c.Hatch(a, 104, 61, 22, 1, Rgb(0xdce1eb, 0.16f), true);
        c.Hatch(a, 101, 89, 37, 1, Rgb(0xdce1eb, 0.10f), true);
        c.PopOpacity();

        // .home-bottom: the PC, "close after launch"
        const float avail = a.w - 88;
        const bool one_line = avail >= 420 + 12 + 300;
        const float hw_w = one_line ? avail - 312 : avail, al_w = std::min(300.0f, avail);
        struct HwItem {
            std::string_view icon, key;
            std::string value;
        };
        const HwItem items[] = {{"gpu", "Видеокарта", sys.gpu.empty() ? "—" : sys.gpu},
                                {"cpu", "Процессор", sys.cpu.empty() ? "—" : sys.cpu},
                                {"ram", "Память", sys.ram.empty() ? "—" : sys.ram}};
        const Font hk = F(11), hv = F(13), ht = Upper(11, 400, 0.06f);
        const float item_h = 11 * 1.2f + LineOf(13);
        const float title_w = TW("КОНФИГУРАЦИЯ", ht);
        const float items_w = hw_w - 2 - 32 - title_w - 18;
        // .hw-items: flex-wrap, gap 18
        std::vector<std::pair<float, float>> pos; // x, y in the items' box
        float ix = 0, iy = 0;
        for (const auto& it : items) {
            const float w = 18 + 8 + std::max(TW(it.key, hk), TW(it.value, hv));
            if (ix > 0 && ix + w > items_w) {
                ix = 0;
                iy += item_h + 18;
            }
            pos.push_back({ix, iy});
            ix += w + 18;
        }
        const float hw_content = std::max(LineOf(11), iy + item_h);
        const float al_content = std::max(LineOf(13) + LineOf(11.5f), 24.0f);
        const float box_h = 12 + std::max(hw_content, one_line ? al_content : 0.0f) + 12 + 2;
        const float al_h = one_line ? box_h : 12 + al_content + 12 + 2;
        const float bottom_h = one_line ? box_h : box_h + 12 + al_h;
        const float by = a.B() - 24 - bottom_h;
        {
            const Rect hw{a.x + 44, by, hw_w, box_h};
            c.FillRect(hw, Rgb(0x15161a, 0.82f), kRadiusL);
            c.Border(hw, kBorder, 1, kRadiusL);
            const float cy = hw.y + 1 + 12, ch = box_h - 26;
            c.Text("КОНФИГУРАЦИЯ", {hw.x + 17, cy + (ch - LineOf(11)) / 2, title_w + 4, LineOf(11)}, ht, kMuted,
                   LineOf(11), Align::Left, false);
            const float bx = hw.x + 17 + title_w + 18, boy = cy + (ch - (iy + item_h)) / 2;
            for (size_t i = 0; i < std::size(items); ++i) {
                const float x = bx + pos[i].first, y = boy + pos[i].second;
                c.Icon(IconPath(items[i].icon), {x, y + (item_h - 18) / 2, 18, 18}, kMuted);
                c.Text(items[i].key, {x + 26, y, 400, 11 * 1.2f}, hk, kMuted, 11 * 1.2f, Align::Left, false);
                c.Text(items[i].value, {x + 26, y + 11 * 1.2f, 400, LineOf(13)}, hv, kText, LineOf(13), Align::Left,
                       false);
            }
        }
        {
            const Rect al{one_line ? a.x + 44 + hw_w + 12 : a.x + 44, one_line ? by : by + box_h + 12, al_w, al_h};
            c.FillRect(al, Rgb(0x15161a, 0.82f), kRadiusL);
            c.Border(al, kBorder, 1, kRadiusL);
            const float th = LineOf(13) + LineOf(11.5f), ty = al.y + (al.h - th) / 2;
            c.Text("Закрывать лаунчер после запуска", {al.x + 17, ty, al.w - 34 - 42 - 14, LineOf(13)}, F(13), kText,
                   LineOf(13), Align::Left, false);
            c.Text("Сохраняется сразу", {al.x + 17, ty + LineOf(13), al.w - 34 - 42, LineOf(11.5f)}, F(11.5f), kMuted,
                   LineOf(11.5f), Align::Left, false);
            if (Switch(ui, Id("close-after"), {al.R() - 17 - 42, al.y + (al.h - 24) / 2, 42, 24},
                       state.close_after_launch)) {
                state.close_after_launch = !state.close_after_launch;
                SaveState();
            }
        }

        // .home-main: the logo, the status, Play, the profile (centred in the space above)
        const float h1m = 28 * 0.67f, logo_h = 17 + 4 + 58 + 10 + 13;
        const std::string_view help = kProfileHelp[static_cast<int>(state.profile)];
        const float help_h = std::max(36.0f, c.Measure(help, kHelp, kHelpLh, 380).h);
        const float content_h = h1m + logo_h + h1m + 26 + 32.85f + 18 + 52 + 22 + LineOf(12) + 6 + 34 + 6 + help_h;
        const float main_h = by - a.y;
        const float x = a.x + 44;
        float y = a.y + 34 + std::max(0.0f, (main_h - 34 - 20 - content_h) / 2);
        y += h1m;
        {
            const Rich l1 = ParseRich("<b>in</b>FAMOUS", 400);
            c.Text(l1.text, {x, y, 400, 17}, Fd(17, 700, 0.02f), Rgb(0xe7e7ea), 17, Align::Left, false, l1.spans);
            c.Text("SECOND SON", {x, y + 17 + 4, 600, 58}, Fd(58, 900, 0.035f), kWhite, 58, Align::Left, false);
            const float l3 = y + 17 + 4 + 58 + 10;
            c.FillRect({x, l3 + 13.0f / 2 - 1, 26, 2}, kAccent, 1);
            c.Text("PC EDITION", {x + 36, l3, 400, 13}, Fd(13, 700, 0.28f), kAccentHover, 13, Align::Left, false);
        }
        y += logo_h + h1m + 26;
        {
            // .status
            std::string text = "Готово к запуску";
            Color dot = kOk;
            if (game_running) {
                text = "Игра запущена";
                dot = kAmber;
            } else if (!config_error.empty()) {
                text = "SecondSonPC.toml: " + config_error;
                dot = kDanger;
            } else if (!game_ok_.empty()) {
                text = game_ok_;
                dot = kDanger;
            }
            const float tw = std::min(TW(text, F(13)), a.w - 88 - 60);
            const Rect st{x, y, 1 + 10 + 8 + 10 + tw + 12 + 1, 32.85f};
            c.FillRect(st, Rgb(0x15161a, 0.78f), st.h / 2);
            c.Border(st, kBorderStrong, 1, st.h / 2);
            c.FillCircle(st.x + 1 + 10 + 4, st.y + st.h / 2, 4 + 3, WithAlpha(dot, 0.18f));
            c.FillCircle(st.x + 1 + 10 + 4, st.y + st.h / 2, 4, dot);
            c.Text(text, {st.x + 1 + 10 + 8 + 10, st.y + (st.h - LineOf(13)) / 2, tw + 1, LineOf(13)}, F(13), kText,
                   LineOf(13), Align::Left, false);
        }
        y += 32.85f + 18;
        {
            ButtonStyle play;
            play.kind = ButtonKind::Primary;
            play.height = 52;
            play.pad_l = 28;
            play.pad_r = 34;
            play.font_size = 18;
            play.weight = 700;
            play.tracking = 0.02f;
            play.radius = 10;
            play.icon = 20;
            play.shadow_alpha = 0.32f;
            play.shadow_hover_alpha = 0.42f;
            play.shadow_blur = 22;
            play.shadow_hover_blur = 26;
            const float pw = ButtonWidth(ui, "Играть", "play", play);
            if (Button(ui, Id("play"), {x, y, pw, 52}, "Играть", "play", play, !game_running)) {
                StartLaunch();
            }
            ButtonStyle tall;
            tall.height = 52;
            tall.pad_l = tall.pad_r = 20;
            tall.radius = 10;
            tall.bg = Rgb(0x1b1c21, 0.85f);
            const float tw = ButtonWidth(ui, "Настройки", "gear", tall);
            if (Button(ui, Id("open-settings"), {x + pw + 12, y, tw, 52}, "Настройки", "gear", tall)) {
                Go(kDisplay);
            }
        }
        y += 52 + 22;
        {
            c.Text("Профиль запуска", {x, y, 380, LineOf(12)}, F(12, 500), kMuted, LineOf(12), Align::Left, false);
            y += LineOf(12) + 6;
            const SelectOption opts[] = {{std::string(kProfileLabel[0])}, {std::string(kProfileLabel[1])}};
            if (const int ch = Select(ui, Id("profile"), {x, y, 380, 34}, opts, static_cast<int>(state.profile), true,
                                      Rgb(0x1b1c21, 0.9f));
                ch >= 0) {
                SetProfile(static_cast<Profile>(ch));
            }
            y += 34 + 6;
            c.Text(help, {x, y, 381, help_h}, kHelp,
                   state.profile == Profile::Experimental ? Rgb(0xf6c98a) : kMuted, kHelpLh);
        }
        c.PopClip();
    }
    std::string game_ok_; // why the game cannot start ("" : it can), checked when the launcher opens
    std::filesystem::path art_path_; // the game's sce_sys\pic0.png ("" : none, or it did not load)

    // ------------------------------------------------------------------ the launch

    void StartLaunch() {
        if (game_running || modal) {
            return;
        }
        Modal m;
        m.title = "Запуск игры";
        m.launch = true;
        m.actions = {{"Отмена", ButtonKind::Normal, "cancel"}};
        m.initial = "cancel";
        m.done = [this](const std::string&) { launch = {}; };
        OpenModal(std::move(m));
        launch = {};
        launch.next = u->now + 0.35;
    }
    void LaunchFail(std::string error) {
        launch.error = std::move(error);
        launch.finished = true;
        modal->actions = {{"Понятно", ButtonKind::Primary, "ok"}};
        modal->initial = "ok";
        modal->fresh = true;
    }
    /// One step of the launch modal, each a moment after the last (app.js launchDemo: the same three steps, real).
    void LaunchTick() {
        if (!modal || !modal->launch || launch.finished || u->now < launch.next) {
            if (modal && modal->launch && launch.finished && launch.close_at > 0 && u->now >= launch.close_at) {
                request = WindowRequest::Close;
            }
            return;
        }
        std::string error;
        switch (launch.step) {
        case 0:
            error = CheckGame(cfg.game_dir);
            if (error.empty() && !opt.read_only) {
                error = BackupSaves(cfg.user_data_dir);
            }
            break;
        case 1:
            if (!svc->Flush(3000) && !svc->LastSaveError().empty()) {
                error = "settings.toml не записан: " + svc->LastSaveError();
            }
            break;
        case 2:
            if (opt.read_only) {
                error = "Снимок экрана: игра не запускается.";
                break;
            }
            ApplyProfileEnv(state.profile);
            error = StartGame(opt.game_args, game);
            if (error.empty()) {
                game_running = true;
            }
            break;
        default:
            break;
        }
        if (!error.empty()) {
            LaunchFail(std::move(error));
            return;
        }
        ++launch.step;
        launch.next = u->now + 0.35;
        if (launch.step == 3) {
            launch.finished = true;
            modal->actions = {{"Понятно", ButtonKind::Primary, "ok"}};
            modal->initial = "ok";
            modal->fresh = true;
            if (state.close_after_launch) {
                launch.close_at = u->now + 0.6;
            }
        }
    }

    // ------------------------------------------------------------------ dialogs, notices

    float ParasHeight(const std::vector<Rich>& paras, const std::vector<std::string>& notes, float w) {
        float h = 0;
        for (const auto& p : paras) {
            h += C().Measure(p.text, kBody, kLabelLh, w, true, p.spans).h + 10;
        }
        for (const auto& n : notes) {
            h += C().Measure(n, F(12), LineOf(12), w).h + 10;
        }
        return h;
    }

    void DrawModal() {
        if (!modal) {
            return;
        }
        Canvas& c = C();
        Ui& ui = *u;
        ui.SetLayer(1);
        c.FillRect({0, 0, W, H}, Rgb(0x050507, 0.66f));
        const float mw = std::min(460.0f, W - 32), iw = mw - 44;
        std::vector<Rich> paras;
        for (const auto& p : modal->paras) {
            paras.push_back(ParseRich(p, 600, kText));
        }
        if (modal->launch) {
            paras = {ParseRich(fmt::format("Профиль: <b>{}</b>", kProfileLabel[static_cast<int>(state.profile)]), 600,
                               kText)};
        }
        const float title_h = c.Measure(modal->title, Fd(18, 600), 18 * 1.3f, iw).h;
        float body_h = 20 + title_h + 8 + ParasHeight(paras, modal->notes, iw);
        const float steps_h = 3 * LineOf(13.5f) + 2 * 8;
        std::string result;
        if (modal->launch) {
            body_h += steps_h + 6 + 12 + 4 + 4;
            if (launch.finished) {
                result = !launch.error.empty() ? launch.error
                         : state.close_after_launch
                             ? "Игра запускается — лаунчер сейчас закроется."
                             : "Игра запускается — её окно откроется через несколько секунд.";
                body_h += 12 + 2 + 20 + c.Measure(result, F(14, 500), kLabelLh, iw - 26).h;
            }
            body_h += 6;
        } else {
            body_h += 6 - 10 + 10; // (the last p's margin stays inside the padding)
        }
        const float actions_h = 14 + 34 + 18;
        const float mh = body_h + actions_h;
        const Rect box{(W - mw) / 2, std::max(16.0f, (H - mh) / 2), mw, mh};
        c.Shadow(box, kRadiusL, 0, 20, 50, 0, Rgb(0, 0.6f));
        c.FillRect(box, kSurface, kRadiusL);
        c.Border(box, kBorderStrong, 1, kRadiusL);
        float y = box.y + 1 + 20;
        const float x = box.x + 1 + 22 - 1;
        c.Text(modal->title, {x, y, iw + 1, title_h}, Fd(18, 600), kText, 18 * 1.3f);
        y += title_h + 8;
        for (const auto& p : paras) {
            const float ph = c.Measure(p.text, kBody, kLabelLh, iw, true, p.spans).h;
            c.Text(p.text, {x, y, iw + 1, ph}, kBody, kMuted, kLabelLh, Align::Left, true, p.spans);
            y += ph + 10;
        }
        for (const auto& n : modal->notes) {
            const float nh = c.Measure(n, F(12), LineOf(12), iw).h;
            c.Text(n, {x, y, iw + 1, nh}, F(12), kDim, LineOf(12));
            y += nh + 10;
        }
        if (modal->launch) {
            // .launch-steps
            for (int i = 0; i < 3; ++i) {
                const bool done = i < launch.step, active = i == launch.step && launch.error.empty();
                const bool failed = i == launch.step && !launch.error.empty();
                const float ly = y + i * (LineOf(13.5f) + 8);
                const Rect ic{x, ly + (LineOf(13.5f) - 18) / 2, 18, 18};
                if (done) {
                    c.Icon(IconPath("check"), ic.Inset(1), kOk);
                } else if (active) {
                    c.FillCircle(ic.x + 9, ic.y + 9, 4, kAccent);
                } else if (failed) {
                    c.Icon(IconPath("warn"), ic.Inset(1), kAmber);
                }
                c.Text(kLaunchSteps[i], {x + 28, ly, iw - 28, LineOf(13.5f)}, F(13.5f),
                       active || failed ? kText : done ? kMuted : kDim, LineOf(13.5f), Align::Left, false);
            }
            y += steps_h + 6 + 12 - 10 + 10;
            // .launch-bar
            const float pct = ui.Anim(Id("launch-bar"), static_cast<float>(launch.step) / 3.0f, 0.5f);
            c.FillRect({x, y, iw, 4}, Rgb(0x3a3d46), 2);
            if (pct > 0) {
                c.FillRect({x, y, iw * pct, 4}, kAccent, 2);
            }
            y += 4 + 4;
            if (!result.empty()) {
                y += 12 - 4 + 4;
                const float rh = c.Measure(result, F(14, 500), kLabelLh, iw - 26).h;
                const Rect rb{x, y, iw, rh + 22};
                const bool bad = !launch.error.empty();
                c.FillRect(rb, bad ? kAmberWeak : kAccentWeak, kRadius);
                c.Border(rb, bad ? Rgb(0xf0a73a, 0.5f) : kAccentLine, 1, kRadius);
                c.Text(result, {rb.x + 13, rb.y + 11, iw - 26 + 1, rh}, F(14, 500), kText, kLabelLh);
            }
            ui.KeepAnimating();
        }
        // .modal-actions
        const float ay = box.B() - 1 - 18 - 34;
        float ax = box.R() - 1 - 22;
        std::string clicked;
        for (size_t i = modal->actions.size(); i-- > 0;) {
            const auto& a = modal->actions[i];
            ButtonStyle s;
            s.kind = a.kind;
            const float bw = ButtonWidth(ui, a.label, {}, s);
            ax -= bw;
            const u64 id = Id(a.value, 0x6d6f64616cull);
            if (modal->fresh && a.value == modal->initial) {
                ui.SetFocus(id, false);
            }
            if (Button(ui, id, {ax, ay, bw, 34}, a.label, {}, s)) {
                clicked = a.value;
            }
            ax -= 10;
        }
        modal->fresh = false;
        if (clicked.empty() && !modal->dismiss.empty() && ui.TakeKey(VK_ESCAPE)) {
            clicked = launch.finished && modal->launch ? "ok" : modal->dismiss;
        }
        if (!clicked.empty()) {
            CloseModal(clicked);
        }
        ui.SetLayer(0);
    }

    void DrawToasts() {
        Canvas& c = C();
        const double now = u->now;
        std::erase_if(toasts, [&](const Toast& t) { return t.t0 >= 0 && now - t.t0 > 3.6; });
        float y = H - 28;
        for (size_t i = toasts.size(); i-- > 0;) {
            Toast& t = toasts[i];
            if (t.t0 < 0) {
                t.t0 = now;
            }
            const float tw = std::min(TW(t.text, F(13)), 520.0f - 28 - 2 - 26);
            const TextBox tb = c.Measure(t.text, F(13), LineOf(13), tw + 1);
            const float w = 1 + 14 + 16 + 10 + tw + 14 + 1, h = 1 + 10 + tb.h + 10 + 1;
            y -= h;
            const Rect r{(W - w) / 2, y, w, h};
            c.Shadow(r, kRadius, 0, 8, 24, 0, Rgb(0, 0.5f));
            c.FillRect(r, Rgb(0x24262d), kRadius);
            c.Border(r, kBorderStrong, 1, kRadius);
            c.Icon(IconPath(t.warn ? "warn" : "check"), {r.x + 15, r.y + (h - 16) / 2, 16, 16}, t.warn ? kAmber : kOk);
            c.Text(t.text, {r.x + 15 + 16 + 10, r.y + 11, tw + 1, tb.h}, F(13), kText, LineOf(13));
            y -= 8;
        }
        if (!toasts.empty()) {
            u->KeepAnimating();
        }
    }

    // ------------------------------------------------------------------ the frame

    void Frame(Ui& ui, float width, float height, bool maximized) {
        u = &ui;
        W = width;
        H = height;
        b = maximized ? 0.0f : 1.0f;
        const bool was_running = game_running;
        game_running = GameRunning(game);
        if (was_running && !game_running) {
            ReloadService(); // the game may have changed the settings in its own menu
        }
        ui.SetInputLayer(modal ? 1 : 0);
        ui.SetLayer(0);
        CaptureInput();
        LaunchTick();
        Canvas& c = C();
        c.FillRect({0, 0, W, H}, kBg);
        const Rect content{b + kNavW, b + kTitleH, W - 2 * b - kNavW, H - 2 * b - kTitleH};
        if (current == kHome) {
            DrawHome(content);
        } else {
            DrawSection(content);
        }
        DrawNav();
        DrawTitlebar();
        if (b > 0) {
            c.Border({0, 0, W, H}, kBorderStrong, 1);
        }
        DrawModal();
        DrawToasts();
        ui.DrawPopup({0, 0, W, H});
        ui.raw_keys = capture >= 0 && !modal; // (the next frame's keys: Tab bound like any key)
    }
};

LauncherApp::LauncherApp(AppOptions options) : d_(std::make_unique<Impl>()) {
    d_->opt = std::move(options);
}

LauncherApp::~LauncherApp() {
    if (d_->svc != nullptr) {
        d_->svc->Flush(3000);
    }
}

bool LauncherApp::Init(std::string& error) {
    Impl& d = *d_;
    d.settings_file = d.opt.settings_file.empty() ? S::DefaultUserFile() : d.opt.settings_file;
    if (d.settings_file.empty()) {
        error = "LOCALAPPDATA is not set: no place for the settings";
        return false;
    }
    d.sys = QuerySystem();
    LoadLauncherState(d.state);
    ApplyProfileEnv(d.state.profile);
    for (int s = 0; s < kSecCount; ++s) {
        for (const auto id : kSectionIds[s]) {
            d.ids[s].emplace_back(id);
        }
    }
    for (size_t a = 0; a < Input::kActionCount; ++a) {
        d.ids[kControls].push_back(BindingId(static_cast<Action>(a)));
    }
    for (const auto& e : S::Experiments()) {
        d.ids[kAdvanced].push_back(S::ExperimentId(e.env));
    }
    d.ReloadService(true);
    d.game_ok_ = CheckGame(d.cfg.game_dir);
    std::string art_error;
    std::error_code ec;
    const auto pic = d.cfg.game_dir / L"sce_sys" / L"pic0.png";
    d.art_path_ = !d.opt.no_art && std::filesystem::is_regular_file(pic, ec) ? pic : std::filesystem::path{};
    for (int s = 0; s < kSecCount; ++s) {
        if (kSecs[s].id == d.opt.page) {
            d.current = s;
        }
    }
    d.tab = d.opt.tab == "pad" ? 1 : 0;
    d.exp_open = d.opt.open_experiments;
    return true;
}

void LauncherApp::Frame(Ui& ui, float width, float height, bool maximized) {
    Impl& d = *d_;
    if (!d.art && !d.art_path_.empty()) {
        std::string error;
        d.art = ui.c.LoadPicture(d.art_path_.wstring(), error);
        if (!d.art) {
            d.art_path_.clear();
        }
    }
    d.Frame(ui, width, height, maximized);
}

WindowRequest LauncherApp::TakeRequest() {
    return std::exchange(d_->request, WindowRequest::None);
}

bool LauncherApp::CaptionAt(float x, float y, float width) const {
    return y < kTitleH && x < width - 3 * 46 && !d_->modal;
}

} // namespace SecondSon::Launcher
