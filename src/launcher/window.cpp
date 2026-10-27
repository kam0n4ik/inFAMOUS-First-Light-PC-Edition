// SecondSonPC — the launcher's window (app.h Run): a frameless Win32 window with the system's resize borders, snap
// and shadow, drawn by the Canvas each time something changed; or one frame offscreen into a PNG. Own code.
#include "app/product.h"
#include "launcher/app.h"

#include <windows.h>

#include <dwmapi.h>
#include <objbase.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace SecondSon::Launcher {

namespace {

constexpr float kWidth = 1100, kHeight = 720; // the mockup's .window
constexpr float kMinWidth = 900, kMinHeight = 600;
constexpr float kResizeBorder = 6;

double NowSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

void ShowError(const std::string& message) {
    MessageBoxW(nullptr, Wide(message).c_str(), std::wstring(App::kProduct.wide_name).c_str(), MB_ICONERROR | MB_OK);
}

struct Window {
    HWND hwnd = nullptr;
    Canvas canvas;
    Ui ui{canvas};
    LauncherApp* app = nullptr;
    float scale = 1;
    bool tracking = false;
    bool dirty = true;
    bool closed = false;

    float Dips(int px) const {
        return static_cast<float>(px) / scale;
    }
    bool Maximized() const {
        return IsZoomed(hwnd) != FALSE;
    }
    void Size(float& w, float& h) const {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        w = Dips(rc.right - rc.left);
        h = Dips(rc.bottom - rc.top);
    }

    void Draw() {
        if (closed || IsIconic(hwnd)) {
            return;
        }
        float w = 0, h = 0;
        Size(w, h);
        ui.BeginFrame(NowSeconds());
        if (!canvas.Begin(Theme::kDesk)) {
            ui.EndFrame();
            return;
        }
        app->Frame(ui, w, h, Maximized());
        ui.EndFrame();
        if (!canvas.End()) {
            dirty = true; // the device was lost: everything is made again for the next frame
        }
        switch (app->TakeRequest()) {
        case WindowRequest::Minimize:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case WindowRequest::Maximize:
            ShowWindow(hwnd, Maximized() ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case WindowRequest::Close:
            closed = true;
            DestroyWindow(hwnd);
            break;
        case WindowRequest::None:
            break;
        }
    }

    void Key(WPARAM vk, LPARAM lp) {
        KeyEvent k;
        k.vk = static_cast<u32>(vk);
        k.scan = static_cast<u32>((lp >> 16) & 0xFF) | ((lp & (1 << 24)) != 0 ? 0xE000u : 0u);
        k.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        k.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        k.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        ui.in.keys.push_back(k);
        dirty = true;
    }

    LRESULT Proc(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_NCCALCSIZE:
            if (wp == TRUE) {
                // the whole window is client area; maximized, the part past the monitor's edge is cut off
                if (Maximized()) {
                    auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                    const UINT dpi = GetDpiForWindow(hwnd);
                    const int pad = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                    const int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + pad;
                    const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + pad;
                    p->rgrc[0].left += fx;
                    p->rgrc[0].right -= fx;
                    p->rgrc[0].top += fy;
                    p->rgrc[0].bottom -= fy;
                }
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            return DefWindowProcW(hwnd, msg, wp, -1); // (no frame of its own is painted)
        case WM_NCHITTEST: {
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &pt);
            float w = 0, h = 0;
            Size(w, h);
            const float x = Dips(pt.x), y = Dips(pt.y);
            if (!Maximized()) {
                const bool l = x < kResizeBorder, r = x >= w - kResizeBorder;
                const bool t = y < kResizeBorder, b = y >= h - kResizeBorder;
                if (t && l) {
                    return HTTOPLEFT;
                }
                if (t && r) {
                    return HTTOPRIGHT;
                }
                if (b && l) {
                    return HTBOTTOMLEFT;
                }
                if (b && r) {
                    return HTBOTTOMRIGHT;
                }
                if (l || r) {
                    return l ? HTLEFT : HTRIGHT;
                }
                if (t || b) {
                    return t ? HTTOP : HTBOTTOM;
                }
            }
            return app->CaptionAt(x, y, w) ? HTCAPTION : HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize.x = static_cast<LONG>(kMinWidth * scale);
            mm->ptMinTrackSize.y = static_cast<LONG>(kMinHeight * scale);
            return 0;
        }
        case WM_DPICHANGED: {
            scale = static_cast<float>(HIWORD(wp)) / 96.0f;
            canvas.SetScale(scale);
            const RECT* r = reinterpret_cast<const RECT*>(lp);
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            dirty = true;
            return 0;
        }
        case WM_SIZE:
            canvas.Resize(LOWORD(lp), HIWORD(lp));
            dirty = true;
            Draw(); // (while the system's resize loop runs, the main loop does not)
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            Draw();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT) {
                const Cursor c = ui.CursorShape();
                SetCursor(LoadCursorW(nullptr, c == Cursor::Hand         ? MAKEINTRESOURCEW(32649)
                                               : c == Cursor::NotAllowed ? MAKEINTRESOURCEW(32648)
                                                                         : MAKEINTRESOURCEW(32512)));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE:
            ui.in.mx = Dips(GET_X_LPARAM(lp));
            ui.in.my = Dips(GET_Y_LPARAM(lp));
            if (!tracking) {
                TRACKMOUSEEVENT t{.cbSize = sizeof(t), .dwFlags = TME_LEAVE, .hwndTrack = hwnd};
                tracking = TrackMouseEvent(&t) != FALSE;
            }
            dirty = true;
            return 0;
        case WM_MOUSELEAVE:
            tracking = false;
            if (!ui.in.down) {
                ui.in.mx = ui.in.my = -1e6f;
            }
            dirty = true;
            return 0;
        case WM_NCMOUSEMOVE:
            ui.in.mx = ui.in.my = -1e6f; // over the caption or a border: no widget is hovered there
            dirty = true;
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            SetCapture(hwnd);
            ui.in.mx = Dips(GET_X_LPARAM(lp));
            ui.in.my = Dips(GET_Y_LPARAM(lp));
            ui.in.down = ui.in.pressed = true;
            dirty = true;
            return 0;
        case WM_LBUTTONUP:
            ui.in.mx = Dips(GET_X_LPARAM(lp));
            ui.in.my = Dips(GET_Y_LPARAM(lp));
            ui.in.down = false;
            ui.in.released = true;
            ReleaseCapture();
            dirty = true;
            return 0;
        case WM_RBUTTONDOWN:
            ui.in.other_pressed |= 1;
            dirty = true;
            return 0;
        case WM_MBUTTONDOWN:
            ui.in.other_pressed |= 2;
            dirty = true;
            return 0;
        case WM_XBUTTONDOWN:
            ui.in.other_pressed |= GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? 4u : 8u;
            dirty = true;
            return TRUE;
        case WM_MOUSEWHEEL:
            ui.in.wheel += static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / WHEEL_DELTA;
            dirty = true;
            return 0;
        case WM_CAPTURECHANGED:
            if (ui.in.down) {
                ui.in.down = false;
                ui.in.released = true;
                dirty = true;
            }
            break;
        case WM_KEYDOWN:
            Key(wp, lp);
            return 0;
        case WM_SYSKEYDOWN:
            Key(wp, lp);
            if (wp == VK_F10) {
                return 0; // (no menu bar to activate)
            }
            break; // Alt+F4 and the other system keys as Windows has them
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_KEYMENU) {
                return 0; // Alt alone: no system menu (the title bar is the window's own)
            }
            break;
        case WM_ACTIVATE:
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
            dirty = true;
            break;
        case WM_TIMER:
            dirty = true; // the game's process, the notices' timeouts
            return 0;
        case WM_DESTROY:
            closed = true;
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    static LRESULT CALLBACK Static(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            auto* self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->hwnd = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        return self != nullptr && self->app != nullptr ? self->Proc(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
    }
};

int RunWindow(LauncherApp& app) {
    const HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{.cbSize = sizeof(wc),
                   .style = CS_DBLCLKS,
                   .lpfnWndProc = &Window::Static,
                   .hInstance = inst,
                   .hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)),
                   .hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
                   .lpszClassName = L"SecondSonPC.Launcher"};
    RegisterClassExW(&wc);
    Window w;
    // centred on the work area of the monitor under the pointer, at that monitor's scale
    POINT cursor{};
    GetCursorPos(&cursor);
    const HMONITOR mon = MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{.cbSize = sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi_x = 96, dpi_y = 96;
    using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        if (auto fn = reinterpret_cast<GetDpiForMonitorFn>(GetProcAddress(shcore, "GetDpiForMonitor"))) {
            fn(mon, 0, &dpi_x, &dpi_y); // (MDT_EFFECTIVE_DPI)
        }
    }
    w.scale = static_cast<float>(dpi_x) / 96.0f;
    const RECT& work = mi.rcWork;
    const int ww = std::min(static_cast<int>(kWidth * w.scale), static_cast<int>(work.right - work.left));
    const int wh = std::min(static_cast<int>(kHeight * w.scale), static_cast<int>(work.bottom - work.top));
    const int x = work.left + (work.right - work.left - ww) / 2, y = work.top + (work.bottom - work.top - wh) / 2;
    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, wc.lpszClassName, std::wstring(App::kProduct.wide_name).c_str(),
                                WS_POPUP | WS_THICKFRAME | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX,
                                x, y, ww, wh, nullptr, nullptr, inst, &w);
    if (hwnd == nullptr) {
        ShowError("Окно лаунчера не создано (ошибка " + std::to_string(GetLastError()) + ").");
        return 1;
    }
    w.scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;
    std::string error;
    if (!w.canvas.Init(hwnd, error)) {
        DestroyWindow(hwnd);
        ShowError("Лаунчер не может рисовать (Direct2D): " + error);
        return 1;
    }
    w.canvas.SetScale(w.scale);
    w.app = &app;
    const MARGINS margins{1, 1, 1, 1}; // (the system's shadow around the frameless window)
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetTimer(hwnd, 1, 500, nullptr);
    MSG msg{};
    while (!w.closed) {
        // nothing to draw: wait for a message; something moving: the next frame at once (the present waits for the
        // display's refresh)
        if (!w.dirty && !w.ui.Animating()) {
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (!w.closed && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                w.closed = true;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (w.closed) {
            break;
        }
        w.dirty = false;
        w.Draw();
    }
    return 0;
}

int RunShot(LauncherApp& app, const std::wstring& png) {
    Canvas canvas;
    std::string error;
    if (!canvas.InitOffscreen(static_cast<u32>(kWidth), static_cast<u32>(kHeight), 1.0f, error)) {
        std::fprintf(stderr, "SecondSonPC launcher: %s\n", error.c_str());
        return 1;
    }
    Ui ui{canvas};
    // a few frames a second apart: the art loaded, every transition at its end
    double t = 1000;
    for (int i = 0; i < 4; ++i) {
        ui.BeginFrame(t);
        canvas.Begin(Theme::kDesk);
        app.Frame(ui, kWidth, kHeight, false);
        ui.EndFrame();
        canvas.End();
        t += 1.0;
    }
    if (!canvas.SavePng(png, error)) {
        std::fprintf(stderr, "SecondSonPC launcher: %s\n", error.c_str());
        return 1;
    }
    return 0;
}

} // namespace

int Run(int argc, char* argv[]) {
    AppOptions opt;
    std::wstring shot;
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
        const bool has_value = i + 1 < argc;
        if (i == 1 && a == "--launcher") {
            for (int j = 2; j < argc; ++j) {
                opt.game_args.emplace_back(argv[j]); // the rest goes to the game
            }
            break;
        }
        if (a == "--launcher-shot" && has_value) {
            shot = Wide(argv[++i]);
            opt.read_only = true;
        } else if (a == "--launcher-page" && has_value) {
            opt.page = argv[++i];
        } else if (a == "--launcher-tab" && has_value) {
            opt.tab = argv[++i];
        } else if (a == "--launcher-experiments") {
            opt.open_experiments = true;
        } else if (a == "--launcher-scroll" && has_value) {
            opt.scroll = std::strtof(argv[++i], nullptr);
        } else if (a == "--launcher-no-art") {
            opt.no_art = true;
        } else if (a == "--settings" && has_value) {
            opt.settings_file = std::filesystem::absolute(argv[++i]);
        }
    }
    if (shot.empty()) {
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LauncherApp app(std::move(opt));
    std::string error;
    if (!app.Init(error)) {
        if (shot.empty()) {
            ShowError(error);
        } else {
            std::fprintf(stderr, "SecondSonPC launcher: %s\n", error.c_str());
        }
        return 1;
    }
    return shot.empty() ? RunWindow(app) : RunShot(app, shot);
}

} // namespace SecondSon::Launcher
