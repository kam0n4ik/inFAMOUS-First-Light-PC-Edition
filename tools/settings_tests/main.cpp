// SecondSonPC — standalone tests of the PC settings and screenshots of the menu, without the game and without the
// bridge (docs/PC_SETTINGS_IMPLEMENTATION.md §8). Builds from SecondSonPC's own sources only.
//   sspc_settings_tests [--out <dir>] [--no-shots] [--demo <seconds>] [--stress <cycles>]
//     checks        the same checks as --selftest group Settings/Input/Ui -> stdout, exit code = failures
//     screenshots   the window rendered by the Win32 overlay's renderer: every page, 720p..4K, 21:9, Russian/English,
//                   the three hint styles, lists, dialogs -> <out>\*.png; build+render times (1080p, 4K)
//     --demo <s>    a real window with the overlay over it for <s> seconds (keyboard/mouse drive the menu), then a
//                   screen capture of the window area -> <out>\demo_window.png (DWM composites the layered windows)
//     --stress <n>  a real window: the settings window opened, drawn and closed n times; GDI/USER objects, handles
//                   and private memory before and after (leaks)
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <windows.h>
#include <objbase.h>
#include <psapi.h>
#include <wincodec.h>

#include "app/config.h"
#include "settings/settings_checks.h"
#include "settings/settings_service.h"
#include "ui/overlay_win32.h"
#include "ui/settings_menu.h"

namespace fs = std::filesystem;
using namespace SecondSon;

namespace {

/// 32-bit BGRA (straight alpha after compositing) -> PNG with WIC.
bool SavePng(const fs::path& path, const std::vector<std::uint32_t>& px, unsigned w, unsigned h) {
    IWICImagingFactory* f = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        return false;
    }
    IWICStream* s = nullptr;
    IWICBitmapEncoder* e = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) &&
              SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h));
    if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
             SUCCEEDED(fr->WritePixels(h, w * 4, w * h * 4, reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(px.data())))) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
    }
    if (fr) fr->Release();
    if (e) e->Release();
    if (s) s->Release();
    f->Release();
    return ok;
}

/// The menu over a stand-in for the game picture: a dark gradient dimmed like the overlay's backdrop.
std::vector<std::uint32_t> Composite(const Ui::DrawList& list, const std::vector<std::uint32_t>& surface, unsigned sw,
                                     unsigned sh, unsigned& w, unsigned& h) {
    w = static_cast<unsigned>(list.Width());
    h = static_cast<unsigned>(list.Height());
    std::vector<std::uint32_t> out(static_cast<size_t>(w) * h);
    const float dim = 1.0f - list.Dim() / 255.0f;
    const auto sx = static_cast<int>(list.Surface().x), sy = static_cast<int>(list.Surface().y);
    for (unsigned y = 0; y < h; ++y) {
        for (unsigned x = 0; x < w; ++x) {
            float r = (60 + 80.0f * x / w) * dim, g = (70 + 60.0f * y / h) * dim, b = (90 + 40.0f * x / w) * dim;
            const int lx = static_cast<int>(x) - sx, ly = static_cast<int>(y) - sy;
            if (lx >= 0 && ly >= 0 && lx < static_cast<int>(sw) && ly < static_cast<int>(sh)) {
                const std::uint32_t p = surface[static_cast<size_t>(ly) * sw + static_cast<size_t>(lx)];
                const float a = (p >> 24) / 255.0f; // premultiplied source over the background
                b = (p & 0xFF) + b * (1 - a);
                g = ((p >> 8) & 0xFF) + g * (1 - a);
                r = ((p >> 16) & 0xFF) + r * (1 - a);
            }
            out[static_cast<size_t>(y) * w + x] = 0xFF000000u | (static_cast<std::uint32_t>(r) << 16) |
                                                   (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
        }
    }
    return out;
}

App::Config TestConfig() {
    App::Config c;
    c.language_locale = "ru-RU";
    c.language_id = 8;
    return c;
}

// the native backend (the default) on a GPU with DLSS and FSR frame generation; the bridge's presenter for comparison
constexpr u32 kNative = Settings::Capability::Window | Settings::Capability::LiveVolume |
                        Settings::Capability::RealController | Settings::Capability::RenderScale |
                        Settings::Capability::FrameLimiter | Settings::Capability::FpsCounter |
                        Settings::Capability::Dlss | Settings::Capability::FsrFrameGen | Settings::Capability::Reflex;
constexpr u32 kNoRtx = kNative & ~(Settings::Capability::Dlss | Settings::Capability::Reflex);
constexpr u32 kBridge = Settings::Capability::Window | Settings::Capability::BridgePresenter |
                        Settings::Capability::LiveVolume | Settings::Capability::RealController |
                        Settings::Capability::FpsCounter;

Ui::MenuEnv ShotEnv(float w, float h, bool ru, u32 caps, Input::PromptStyle prompts) {
    Ui::MenuEnv env;
    env.width = w;
    env.height = h;
    env.ru = ru;
    env.prompts = prompts;
    env.device_label = prompts == Input::PromptStyle::PlayStation ? "Wireless Controller (DualShock 4)"
                       : prompts == Input::PromptStyle::Xbox      ? "Xbox Wireless Controller"
                       : ru                                       ? "Клавиатура и мышь"
                                                                  : "Keyboard and mouse";
    env.backend_label = (caps & Settings::Capability::BridgePresenter) != 0
                            ? (ru ? "мост shadPS4 (--gpu-backend bridge)" : "shadPS4 bridge (--gpu-backend bridge)")
                            : (ru ? "собственный Vulkan SecondSonPC" : "SecondSonPC's own Vulkan");
    env.displays = {"DELL U2720Q", "Generic PnP Monitor"};
    env.audio_devices = {"Динамики (Realtek High Definition Audio)", "Наушники (HyperX Cloud II)"};
    env.window_sizes = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
    env.shader_cache_size = "25 + 17 MiB (vulkan.bin) / 2048 MiB"; // (P4.14: as the host shows it)
    env.shader_cache_content = ru ? "шейдеров: 833, конвейеров: 585" : "833 shaders, 585 pipelines";
    env.shader_cache_run = ru ? "заранее 546 из 546 (0,9 с), из кэша: 793, новых: 39"
                              : "prepared 546 of 546 (0.9 s), read 793, new 39";
    env.settings_file = "C:\\Users\\player\\AppData\\Local\\SecondSonPC\\settings.toml";
    env.render_label = ru ? "1920 × 1080 (без масштабирования)" : "1920 × 1080 (native)";
    if ((caps & Settings::Capability::Dlss) == 0) {
        env.dlss_why = "no NGX device";
    }
    env.now_ns = 1'000'000'000;
    return env;
}

int Shots(const fs::path& out) {
    Ui::Win32Overlay overlay;
    std::string error;
    if (!overlay.Init(error)) {
        std::printf("SHOTS FAIL: overlay init: %s\n", error.c_str());
        return 1;
    }
    int failures = 0;
    enum Action { None, Accept, Conflict, ListDown, Restart };
    const auto shot = [&](const char* name, float w, float h, bool ru, Ui::Page page, u32 caps,
                          Input::PromptStyle prompts, const char* focus, Action action, float dpi = 1.0f) {
        Settings::Service s;
        const auto c = TestConfig();
        s.Init(c, {}, c, {}, caps);
        Ui::SettingsMenu menu(s);
        auto env = ShotEnv(w, h, ru, caps, prompts);
        env.dpi_scale = dpi;
        menu.SetEnv(env);
        menu.Open();
        menu.SelectPage(page);
        if (action == Restart) {
            menu.Focus("game.enterButton");
            menu.OnNav(Ui::Nav::Right); // a restart setting changed (the header's notice, the row's badge)
            menu.Focus("game.language");
            menu.OnNav(Ui::Nav::Right);
        }
        if (focus != nullptr) {
            menu.Focus(focus);
        }
        if (action == Accept || action == ListDown) {
            menu.OnNav(Ui::Nav::Accept); // a key capture, a dialog or a drop-down list
            if (action == ListDown) {
                menu.OnNav(Ui::Nav::Down);
            }
        } else if (action == Conflict) {
            menu.OnNav(Ui::Nav::Accept);
            menu.OnCaptureInput({Input::InputCode::Kind::Key, static_cast<u16>(SDL_SCANCODE_E)}); // a conflict
        }
        Ui::DrawList list;
        menu.Build(overlay.Measure(), list); // (layout: the second build has the scroll in place)
        const auto t0 = std::chrono::steady_clock::now();
        menu.Build(overlay.Measure(), list);
        std::vector<std::uint32_t> px;
        unsigned sw = 0, sh = 0;
        const bool rendered = overlay.RenderToPixels(list, px, sw, sh);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        unsigned fw = 0, fh = 0;
        const auto full = Composite(list, px, sw, sh, fw, fh);
        const bool saved = rendered && SavePng(out / name, full, fw, fh);
        std::printf("%s  shot %-36s %4.0fx%-4.0f surface %ux%u, %zu commands, build+render %.1f ms\n",
                    saved ? "PASS" : "FAIL", name, w, h, sw, sh, list.Commands().size(), ms);
        failures += saved ? 0 : 1;
    };
    using G = Ui::Page;
    using P = Input::PromptStyle;
    // every page at 1080p (Russian, keyboard hints), with its typical focus
    shot("ru_1080_graphics.png", 1920, 1080, true, G::Graphics, kNative, P::Keyboard, "upscaling.quality", None);
    shot("ru_1080_graphics_list.png", 1920, 1080, true, G::Graphics, kNoRtx, P::Keyboard, "upscaling.upscaler", ListDown);
    shot("ru_1080_graphics_bridge.png", 1920, 1080, true, G::Graphics, kBridge, P::Keyboard, "graphics.fsr", None);
    shot("ru_1080_display.png", 1920, 1080, true, G::Display, kNative, P::Keyboard, "display.frameRateLimit", None);
    shot("ru_1080_display_confirm.png", 1920, 1080, true, G::Display, kNative, P::Keyboard, "display.fullscreen", Accept);
    shot("ru_1080_display_sizes.png", 1920, 1080, true, G::Display, kNative, P::Keyboard, "window.size", Accept);
    shot("ru_1080_controls.png", 1920, 1080, true, G::Controls, kNative, P::Keyboard, "input.mouseSensitivity", None);
    shot("ru_1080_controls_ps.png", 1920, 1080, true, G::Controls, kNative, P::PlayStation, "gamepad.vibration", None);
    shot("ru_1080_keys.png", 1920, 1080, true, G::Keys, kNative, P::Keyboard, "keyboard.cross", None);
    shot("ru_1080_keys_capture.png", 1920, 1080, true, G::Keys, kNative, P::Keyboard, "keyboard.cross", Accept);
    shot("ru_1080_keys_conflict.png", 1920, 1080, true, G::Keys, kNative, P::Keyboard, "keyboard.cross", Conflict);
    shot("ru_1080_audio.png", 1920, 1080, true, G::Audio, kNative, P::Keyboard, "audio.volume", None);
    shot("ru_1080_game.png", 1920, 1080, true, G::Game, kNative, P::Xbox, "interface.prompts", Restart);
    shot("ru_1080_advanced.png", 1920, 1080, true, G::Advanced, kNative, P::Keyboard, "display.showFps", None);
    shot("ru_1080_advanced_cache.png", 1920, 1080, true, G::Advanced, kNative, P::Keyboard, "cmd.clear_shaders", None);
    shot("ru_1080_reset.png", 1920, 1080, true, G::Audio, kNative, P::Keyboard, "reset.page", Accept);
    // English, other sizes and proportions, display scale
    shot("en_1080_graphics.png", 1920, 1080, false, G::Graphics, kNative, P::Keyboard, "upscaling.frameGeneration", None);
    shot("en_1080_keys.png", 1920, 1080, false, G::Keys, kNative, P::Xbox, "keyboard.l3", None);
    shot("ru_720_graphics.png", 1280, 720, true, G::Graphics, kNative, P::Keyboard, nullptr, None);
    shot("ru_720_controls.png", 1280, 720, true, G::Controls, kNative, P::PlayStation, "gamepad.menuButton", None);
    shot("ru_1440_display.png", 2560, 1440, true, G::Display, kNative, P::Keyboard, nullptr, None);
    shot("ru_2160_keys.png", 3840, 2160, true, G::Keys, kNative, P::PlayStation, "keyboard.touchTwoFingers", None);
    shot("ru_2160_graphics_dpi150.png", 3840, 2160, true, G::Graphics, kNative, P::Keyboard, nullptr, None, 1.5f);
    shot("ru_2560x1080_display.png", 2560, 1080, true, G::Display, kNative, P::Keyboard, nullptr, None);
    shot("ru_1024x768_advanced.png", 1024, 768, true, G::Advanced, kNative, P::Keyboard, nullptr, None);
    shot("ru_800x600_graphics.png", 800, 600, true, G::Graphics, kNative, P::Keyboard, nullptr, None);

    // cost: a build + a render of one picture (what the host does per input / timer step), 1080p and 4K
    for (const auto& [w, h] : {std::pair{1920.0f, 1080.0f}, std::pair{3840.0f, 2160.0f}}) {
        Settings::Service s;
        const auto c = TestConfig();
        s.Init(c, {}, c, {}, kNative);
        Ui::SettingsMenu menu(s);
        menu.SetEnv(ShotEnv(w, h, true, kNative, P::Keyboard));
        menu.Open();
        Ui::DrawList list;
        std::vector<std::uint32_t> px;
        unsigned sw = 0, sh = 0;
        double total = 0, worst = 0, build_total = 0;
        constexpr int kRuns = 120;
        for (int i = 0; i < kRuns; ++i) {
            menu.SelectPage(static_cast<Ui::Page>(i % static_cast<int>(Ui::kPageCount)));
            menu.OnNav(Ui::Nav::Down);
            const auto t0 = std::chrono::steady_clock::now();
            menu.Build(overlay.Measure(), list);
            const auto t1 = std::chrono::steady_clock::now();
            overlay.RenderToPixels(list, px, sw, sh);
            const auto t2 = std::chrono::steady_clock::now();
            const double ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
            build_total += std::chrono::duration<double, std::milli>(t1 - t0).count();
            total += ms;
            worst = std::max(worst, ms);
        }
        std::printf("PASS  cost %4.0fx%-4.0f: %d pictures, build %.2f ms + render, average %.2f ms, slowest %.2f ms\n",
                    w, h, kRuns, build_total / kRuns, total / kRuns, worst);
    }
    return failures;
}

int Demo(const fs::path& out, int seconds) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("DEMO FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("SecondSonPC settings menu demo (no game)", 1280, 720, SDL_WINDOW_RESIZABLE);
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    Ui::Win32Overlay overlay;
    std::string error;
    if (window == nullptr || hwnd == nullptr || !overlay.Init(error)) {
        std::printf("DEMO FAIL: window/overlay %s\n", error.c_str());
        return 1;
    }
    Settings::Service s;
    const auto c = TestConfig();
    s.Init(c, {}, c, {}, kNative);
    Ui::SettingsMenu menu(s);
    menu.Open();
    const auto env = [&] {
        RECT cr{};
        GetClientRect(hwnd, &cr);
        Ui::MenuEnv e = ShotEnv(static_cast<float>(cr.right), static_cast<float>(cr.bottom), true, kNative,
                                Input::PromptStyle::Keyboard);
        e.backend_label = "демонстрация без игры";
        e.now_ns = SDL_GetTicksNS();
        return e;
    };
    const auto render = [&] {
        menu.SetEnv(env());
        Ui::DrawList list;
        menu.Build(overlay.Measure(), list);
        overlay.Present(hwnd, list);
    };
    render();
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < end && menu.IsOpen()) {
        SDL_Event e;
        while (SDL_WaitEventTimeout(&e, 50)) {
            menu.SetEnv(env());
            if (e.type == SDL_EVENT_QUIT) {
                menu.Close();
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                switch (e.key.scancode) {
                case SDL_SCANCODE_UP: menu.OnNav(Ui::Nav::Up); break;
                case SDL_SCANCODE_DOWN: menu.OnNav(Ui::Nav::Down); break;
                case SDL_SCANCODE_LEFT: menu.OnNav(Ui::Nav::Left); break;
                case SDL_SCANCODE_RIGHT: menu.OnNav(Ui::Nav::Right); break;
                case SDL_SCANCODE_RETURN: menu.OnNav(Ui::Nav::Accept); break;
                case SDL_SCANCODE_ESCAPE: menu.OnNav(Ui::Nav::Back); break;
                case SDL_SCANCODE_TAB: menu.OnNav(Ui::Nav::NextTab); break;
                case SDL_SCANCODE_DELETE: menu.OnNav(Ui::Nav::Reset); break;
                case SDL_SCANCODE_PAGEUP: menu.OnNav(Ui::Nav::PageUp); break;
                case SDL_SCANCODE_PAGEDOWN: menu.OnNav(Ui::Nav::PageDown); break;
                default:
                    if (menu.Capturing()) {
                        menu.OnCaptureInput({Input::InputCode::Kind::Key, static_cast<u16>(e.key.scancode)});
                    }
                    break;
                }
            } else if (e.type == SDL_EVENT_MOUSE_MOTION) {
                menu.OnMouseMove(e.motion.x, e.motion.y);
            } else if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN || e.type == SDL_EVENT_MOUSE_BUTTON_UP) {
                menu.OnMouseButton(e.button.x, e.button.y, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            } else if (e.type == SDL_EVENT_MOUSE_WHEEL) {
                menu.OnWheel(e.wheel.y);
            }
            if (menu.TakeCloseRequest()) {
                menu.Close();
            }
            render();
        }
        menu.Tick(SDL_GetTicksNS());
        render();
    }
    // the window area as the screen shows it (DWM composites the layered windows over it)
    POINT o{0, 0};
    ClientToScreen(hwnd, &o);
    RECT cr{};
    GetClientRect(hwnd, &cr);
    const int w = cr.right, h = cr.bottom;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    SelectObject(mem, dib);
    BitBlt(mem, 0, 0, w, h, screen, o.x, o.y, SRCCOPY | CAPTUREBLT);
    std::vector<std::uint32_t> px(static_cast<const std::uint32_t*>(bits),
                                  static_cast<const std::uint32_t*>(bits) + static_cast<size_t>(w) * h);
    for (auto& p : px) {
        p |= 0xFF000000u;
    }
    const bool saved = SavePng(out / "demo_window.png", px, static_cast<unsigned>(w), static_cast<unsigned>(h));
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    const auto st = overlay.GetStats();
    std::printf("%s  demo: %llu renders, last %.1f ms, slowest %.1f ms, capture %s\n", saved ? "PASS" : "FAIL",
                static_cast<unsigned long long>(st.frames), st.last_ms, st.max_ms, (out / "demo_window.png").string().c_str());
    overlay.Hide();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return saved ? 0 : 1;
}

/// Opens, draws and closes the settings window `cycles` times over a real window (as the host does), then reports what
/// the process holds before and after: GDI and USER objects, handles, private memory.
int Stress(int cycles) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::printf("STRESS FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("SecondSonPC settings stress (no game)", 1920, 1080, 0);
    const auto hwnd = static_cast<HWND>(
        SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    Ui::Win32Overlay overlay;
    std::string error;
    if (window == nullptr || hwnd == nullptr || !overlay.Init(error)) {
        std::printf("STRESS FAIL: window/overlay %s\n", error.c_str());
        return 1;
    }
    Settings::Service s;
    const auto c = TestConfig();
    s.Init(c, {}, c, {}, kNative);
    Ui::SettingsMenu menu(s);
    const auto usage = [] {
        PROCESS_MEMORY_COUNTERS_EX pm{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
        DWORD handles = 0;
        GetProcessHandleCount(GetCurrentProcess(), &handles);
        return std::array<double, 4>{static_cast<double>(GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)),
                                     static_cast<double>(GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS)),
                                     static_cast<double>(handles), static_cast<double>(pm.PrivateUsage) / 1048576.0};
    };
    const auto pump = [] {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    };
    const auto cycle = [&](int i) {
        auto e = ShotEnv(1920, 1080, true, kNative, Input::PromptStyle::Keyboard);
        e.now_ns = SDL_GetTicksNS();
        menu.SetEnv(e);
        menu.Open();
        menu.SelectPage(static_cast<Ui::Page>(i % static_cast<int>(Ui::kPageCount)));
        for (int k = 0; k < 3; ++k) {
            menu.OnNav(Ui::Nav::Down);
            Ui::DrawList list;
            menu.Build(overlay.Measure(), list);
            overlay.Present(hwnd, list);
        }
        pump();
        menu.Close();
        overlay.Hide();
        pump();
    };
    for (int i = 0; i < 20; ++i) {
        cycle(i); // warm-up: the fonts, the text formats, the windows
    }
    const auto before = usage();
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < cycles; ++i) {
        cycle(i);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const auto after = usage();
    const bool ok = after[0] <= before[0] + 2 && after[1] <= before[1] + 2 && after[2] <= before[2] + 4 &&
                    after[3] <= before[3] + 4.0;
    const auto st = overlay.GetStats();
    std::printf("%s  stress: %d open/draw x3/close cycles in %.0f ms (%.2f ms per cycle); GDI %g -> %g, USER %g -> %g, "
                "handles %g -> %g, private %.1f -> %.1f MiB; renders %llu, average %.2f ms, slowest %.2f ms\n",
                ok ? "PASS" : "FAIL", cycles, ms, ms / cycles, before[0], after[0], before[1], after[1], before[2],
                after[2], before[3], after[3], static_cast<unsigned long long>(st.frames),
                st.frames != 0 ? st.total_ms / static_cast<double>(st.frames) : 0.0, st.max_ms);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    fs::path out = fs::current_path() / "settings_tests";
    bool shots = true;
    int demo = 0;
    int stress = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            out = fs::absolute(argv[++i]);
        } else if (a == "--no-shots") {
            shots = false;
        } else if (a == "--demo" && i + 1 < argc) {
            demo = std::atoi(argv[++i]);
        } else if (a == "--stress" && i + 1 < argc) {
            stress = std::atoi(argv[++i]);
        }
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::error_code ec;
    fs::create_directories(out, ec);
    int checks = 0, failures = 0;
    Settings::RunChecks(
        [&](std::string_view group, std::string_view what, bool ok, const std::string& detail) {
            ++checks;
            failures += ok ? 0 : 1;
            std::printf("%s  %-22.*s %.*s%s%s\n", ok ? "PASS" : "FAIL", static_cast<int>(group.size()), group.data(),
                        static_cast<int>(what.size()), what.data(), detail.empty() ? "" : "  | ", detail.c_str());
        },
        out / "files");
    std::printf("checks: %d, failures: %d\n", checks, failures);
    if (shots) {
        failures += Shots(out);
    }
    if (demo > 0) {
        failures += Demo(out, demo);
    }
    if (stress > 0) {
        failures += Stress(stress);
    }
    std::printf("RESULT %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures;
}
