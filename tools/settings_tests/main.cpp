// SecondSonPC — standalone tests of the PC settings and screenshots of the menu, without the game and without the
// bridge (docs/PC_SETTINGS_IMPLEMENTATION.md §8). Builds from SecondSonPC's own sources only.
//   sspc_settings_tests [--out <dir>] [--no-shots] [--demo <seconds>]
//     checks        the same checks as --selftest group Settings/Input/Ui -> stdout, exit code = failures
//     screenshots   the menu rendered by the Win32 overlay's renderer at 720p..4K, Russian/English -> <out>\*.png
//     --demo <s>    a real window with the overlay over it for <s> seconds (keyboard/mouse drive the menu), then a
//                   screen capture of the window area -> <out>\demo_window.png (DWM composites the layered windows)
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <SDL3/SDL.h>
#include <windows.h>
#include <objbase.h>
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

constexpr u32 kAllCaps = Settings::Capability::Window | Settings::Capability::BridgePresenter |
                         Settings::Capability::LiveVolume | Settings::Capability::RealController;

int Shots(const fs::path& out) {
    Ui::Win32Overlay overlay;
    std::string error;
    if (!overlay.Init(error)) {
        std::printf("SHOTS FAIL: overlay init: %s\n", error.c_str());
        return 1;
    }
    int failures = 0;
    const auto shot = [&](const char* name, float w, float h, bool ru, Settings::Category cat, u32 caps,
                          Input::PromptStyle prompts, const char* focus, int action) {
        Settings::Service s;
        const auto c = TestConfig();
        s.Init(c, {}, c, {}, caps);
        Ui::SettingsMenu menu(s);
        Ui::MenuEnv env;
        env.width = w;
        env.height = h;
        env.ru = ru;
        env.prompts = prompts;
        env.device_label = prompts == Input::PromptStyle::PlayStation ? "PS4 Controller"
                           : ru                                      ? "Клавиатура и мышь"
                                                                     : "Keyboard and mouse";
        env.backend_label = (caps & Settings::Capability::BridgePresenter) != 0
                                ? (ru ? "мост shadPS4 (по умолчанию)" : "shadPS4 bridge (default)")
                                : (ru ? "собственный SecondSonPC (экспериментальный)" : "SecondSonPC's own (experimental)");
        env.displays = {"DELL U2720Q", "Generic PnP Monitor"};
        env.window_sizes = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}};
        env.shader_cache_size = "25 + 17 MiB (vulkan.bin) / 2048 MiB"; // (P4.14: as the host shows it)
        env.shader_cache_content = ru ? "шейдеров: 833, конвейеров: 585" : "833 shaders, 585 pipelines";
        env.shader_cache_run = ru ? "заранее 546 из 546 (0.9 с), из кэша: 793, новых: 39"
                                  : "prepared 546 of 546 (0.9 s), read 793, new 39";
        env.now_ns = 1'000'000'000;
        menu.SetEnv(env);
        menu.Open();
        menu.SelectCategory(cat);
        if (focus != nullptr) {
            menu.Focus(focus);
        }
        if (action == 1) {
            menu.OnNav(Ui::Nav::Accept); // capture / confirmation dialog
        } else if (action == 2) {
            menu.OnNav(Ui::Nav::Accept);
            menu.OnCaptureInput({Input::InputCode::Kind::Key, static_cast<u16>(SDL_SCANCODE_E)}); // a conflict
        }
        Ui::DrawList list;
        const auto t0 = std::chrono::steady_clock::now();
        menu.Build(overlay.Measure(), list);
        std::vector<std::uint32_t> px;
        unsigned sw = 0, sh = 0;
        const bool rendered = overlay.RenderToPixels(list, px, sw, sh);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        unsigned fw = 0, fh = 0;
        const auto full = Composite(list, px, sw, sh, fw, fh);
        const bool saved = rendered && SavePng(out / name, full, fw, fh);
        std::printf("%s  shot %-34s %4.0fx%-4.0f surface %ux%u, build+render %.1f ms\n", saved ? "PASS" : "FAIL", name, w,
                    h, sw, sh, ms);
        failures += saved ? 0 : 1;
    };
    using C = Settings::Category;
    using P = Input::PromptStyle;
    shot("ru_1080_display.png", 1920, 1080, true, C::Display, kAllCaps, P::Keyboard, nullptr, 0);
    shot("ru_1080_graphics.png", 1920, 1080, true, C::Graphics, kAllCaps, P::Keyboard, "graphics.fsr", 0);
    shot("ru_1080_graphics_sspc.png", 1920, 1080, true, C::Graphics,
         Settings::Capability::Window | Settings::Capability::LiveVolume | Settings::Capability::RealController,
         P::Keyboard, "graphics.fsr", 0);
    shot("ru_1080_audio.png", 1920, 1080, true, C::Audio, kAllCaps, P::Keyboard, nullptr, 0);
    shot("ru_1080_mouse.png", 1920, 1080, true, C::Controls, kAllCaps, P::Keyboard, "input.mouseSensitivity", 0);
    shot("ru_1080_keys.png", 1920, 1080, true, C::Keyboard, kAllCaps, P::Keyboard, "keyboard.touchTwoFingers", 0);
    shot("ru_1080_keys_capture.png", 1920, 1080, true, C::Keyboard, kAllCaps, P::Keyboard, "keyboard.cross", 1);
    shot("ru_1080_keys_conflict.png", 1920, 1080, true, C::Keyboard, kAllCaps, P::Keyboard, "keyboard.cross", 2);
    shot("ru_1080_gamepad_ps.png", 1920, 1080, true, C::Gamepad, kAllCaps, P::PlayStation, nullptr, 0);
    shot("ru_1080_interface_xbox.png", 1920, 1080, true, C::Interface, kAllCaps, P::Xbox, nullptr, 0);
    shot("ru_1080_game.png", 1920, 1080, true, C::Game, kAllCaps, P::Keyboard, nullptr, 0);
    shot("ru_1080_shaders.png", 1920, 1080, true, C::Shaders, kAllCaps, P::Keyboard, "cmd.clear_shaders", 0);
    shot("ru_1080_upscaling.png", 1920, 1080, true, C::Upscaling, kAllCaps | Settings::Capability::RenderScale | Settings::Capability::Dlss |
             Settings::Capability::FsrFrameGen, P::Keyboard,
         "upscaling.upscaler", 0);
    shot("ru_1080_confirm_display.png", 1920, 1080, true, C::Display, kAllCaps, P::Keyboard, "display.fullscreen", 1);
    shot("en_1080_display.png", 1920, 1080, false, C::Display, kAllCaps, P::Keyboard, nullptr, 0);
    shot("ru_720_display.png", 1280, 720, true, C::Display, kAllCaps, P::Keyboard, nullptr, 0);
    shot("ru_1440_mouse.png", 2560, 1440, true, C::Controls, kAllCaps, P::Keyboard, nullptr, 0);
    shot("ru_2160_keys.png", 3840, 2160, true, C::Keyboard, kAllCaps, P::PlayStation, nullptr, 0);
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
    s.Init(c, {}, c, {}, kAllCaps);
    Ui::SettingsMenu menu(s);
    menu.Open();
    const auto env = [&] {
        Ui::MenuEnv e;
        RECT cr{};
        GetClientRect(hwnd, &cr);
        e.width = static_cast<float>(cr.right);
        e.height = static_cast<float>(cr.bottom);
        e.device_label = "Клавиатура и мышь";
        e.backend_label = "демонстрация без игры";
        e.window_sizes = {{1280, 720}, {1600, 900}};
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

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    fs::path out = fs::current_path() / "settings_tests";
    bool shots = true;
    int demo = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--out" && i + 1 < argc) {
            out = fs::absolute(argv[++i]);
        } else if (a == "--no-shots") {
            shots = false;
        } else if (a == "--demo" && i + 1 < argc) {
            demo = std::atoi(argv[++i]);
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
    std::printf("RESULT %s (%d failure(s))\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures;
}
