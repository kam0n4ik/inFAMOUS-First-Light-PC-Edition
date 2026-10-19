// SecondSonPC — the menu overlay over the game window (see overlay_win32.h).
#include "ui/overlay_win32.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>

#include <windows.h>
#include <d2d1.h>
#include <dwrite_3.h>

namespace SecondSon::Ui {

namespace {

template <typename T>
void Release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

std::wstring Wide(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}

D2D1_COLOR_F ToD2D(Color c) {
    return D2D1::ColorF(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
}

D2D1_RECT_F ToD2D(Rect r) {
    return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
}

constexpr wchar_t kClassName[] = L"SecondSonPCMenuOverlay";

} // namespace

struct Win32Overlay::Impl final : TextMeasure {
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dwrite = nullptr;
    bool have_bahnschrift = false;
    IDWriteFontCollection* game_fonts = nullptr; // assets/fonts next to the exe (Roboto Slab, Roboto Condensed)
    ID2D1DCRenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    std::map<std::tuple<int, int, bool>, IDWriteTextFormat*> formats; // (size * 4, weight, wrap)
    std::unordered_map<std::string, float> widths; // measured texts (a text layout each is the costly part of a build)
    bool ok = false;

    HWND owner = nullptr;
    HWND dim = nullptr;
    HWND panel = nullptr;
    bool visible = false;
    u8 fade = 255;      // the window's opacity while it fades in after being shown (0..255)
    u8 dim_alpha = 0;   // the backdrop's opacity the list asked for
    HDC mem_dc = nullptr;
    HBITMAP dib = nullptr;
    void* bits = nullptr;
    int dib_w = 0, dib_h = 0;

    void (*timer_cb)(void*) = nullptr;
    void* timer_ctx = nullptr;
    unsigned timer_ms = 50;
    Stats stats;

    ~Impl() override {
        DestroyWindows();
        FreeDib();
        for (auto& [k, f] : formats) {
            Release(f);
        }
        Release(brush);
        Release(rt);
        Release(game_fonts);
        Release(dwrite);
        Release(d2d);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_NCHITTEST:
            return HTTRANSPARENT; // the game window gets the mouse
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_TIMER: {
            auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            if (self != nullptr && wp == kFadeTimer) {
                self->FadeStep();
            } else if (self != nullptr && self->timer_cb != nullptr) {
                self->timer_cb(self->timer_ctx);
            }
            return 0;
        }
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
        }
    }

    static constexpr UINT_PTR kFadeTimer = 2;

    /// One step of the fade-in: the same pixels, a higher constant opacity (no new drawing).
    void FadeStep() {
        fade = static_cast<u8>(std::min(255, fade + 64));
        if (panel != nullptr) {
            BLENDFUNCTION blend{AC_SRC_OVER, 0, fade, AC_SRC_ALPHA};
            UpdateLayeredWindow(panel, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &blend, ULW_ALPHA);
        }
        if (dim != nullptr) {
            SetLayeredWindowAttributes(dim, 0, static_cast<BYTE>(dim_alpha * fade / 255), LWA_ALPHA);
        }
        if (fade == 255 && panel != nullptr) {
            KillTimer(panel, kFadeTimer);
        }
    }

    bool CreateRenderTarget() {
        Release(brush);
        Release(rt);
        // Software rasterization: the overlay never touches a GPU device (the game's renderer owns the GPU).
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (FAILED(d2d->CreateDCRenderTarget(&props, &rt))) {
            return false;
        }
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // ClearType needs an opaque target
        return SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush));
    }

    IDWriteTextFormat* Format(float size, FontWeight weight, bool wrap) {
        const auto key = std::make_tuple(static_cast<int>(std::lround(size * 4)), static_cast<int>(weight), wrap);
        if (const auto it = formats.find(key); it != formats.end()) {
            return it->second;
        }
        IDWriteTextFormat* f = nullptr;
        if ((weight == FontWeight::GameSlab || weight == FontWeight::GameItem) && game_fonts != nullptr) {
            if (FAILED(dwrite->CreateTextFormat(weight == FontWeight::GameSlab ? L"Roboto Slab" : L"Roboto Condensed",
                                                game_fonts, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                                DWRITE_FONT_STRETCH_NORMAL, std::max(1.0f, size), L"", &f))) {
                return nullptr;
            }
        } else if (weight == FontWeight::GameSlab || weight == FontWeight::GameItem) {
            weight = weight == FontWeight::GameSlab ? FontWeight::Title : FontWeight::Regular; // no font files
        }
        // Bahnschrift (Windows 10+) narrowed: close to the game's condensed menu faces; Segoe UI where it is missing
        const auto w = weight == FontWeight::Title      ? DWRITE_FONT_WEIGHT_BOLD
                       : weight == FontWeight::Semibold ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                                        : DWRITE_FONT_WEIGHT_NORMAL;
        const auto stretch =
            weight == FontWeight::Title ? DWRITE_FONT_STRETCH_CONDENSED : DWRITE_FONT_STRETCH_SEMI_CONDENSED;
        if (f == nullptr &&
            FAILED(dwrite->CreateTextFormat(have_bahnschrift ? L"Bahnschrift" : L"Segoe UI", nullptr, w,
                                            DWRITE_FONT_STYLE_NORMAL, have_bahnschrift ? stretch
                                                                                       : DWRITE_FONT_STRETCH_NORMAL,
                                            std::max(1.0f, size), L"", &f))) {
            return nullptr;
        }
        if (wrap) {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        } else {
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            IDWriteInlineObject* sign = nullptr;
            if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(f, &sign))) {
                const DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
                f->SetTrimming(&trimming, sign);
                sign->Release(); // the format keeps its own reference
            }
        }
        formats.emplace(key, f);
        return f;
    }

    /// The game's menu faces from <exe dir>\\fonts (copied from assets/fonts at build time; Windows 10 1703+).
    void LoadGameFonts() {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"fonts\\";
        IDWriteFactory5* f5 = nullptr;
        IDWriteFontSetBuilder1* builder = nullptr;
        IDWriteFontSet* set = nullptr;
        IDWriteFontCollection1* collection = nullptr;
        int added = 0;
        if (SUCCEEDED(dwrite->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(&f5))) &&
            SUCCEEDED(f5->CreateFontSetBuilder(&builder))) {
            for (const wchar_t* name : {L"RobotoSlab[wght].ttf", L"RobotoCondensed[wght].ttf"}) {
                IDWriteFontFile* file = nullptr;
                if (SUCCEEDED(f5->CreateFontFileReference((dir + name).c_str(), nullptr, &file)) &&
                    SUCCEEDED(builder->AddFontFile(file))) {
                    ++added;
                }
                Release(file);
            }
            if (added == 2 && SUCCEEDED(builder->CreateFontSet(&set)) &&
                SUCCEEDED(f5->CreateFontCollectionFromFontSet(set, &collection))) {
                game_fonts = collection;
                collection = nullptr;
            }
        }
        Release(collection);
        Release(set);
        Release(builder);
        Release(f5);
    }

    float Width(std::string_view utf8, float size, FontWeight weight) override {
        if (utf8.empty()) {
            return 0.0f;
        }
        std::string key = std::to_string(std::lround(size * 4)) + '/' + std::to_string(static_cast<int>(weight)) + '/';
        key += utf8;
        if (const auto it = widths.find(key); it != widths.end()) {
            return it->second;
        }
        IDWriteTextFormat* f = Format(size, weight, true);
        if (f == nullptr) {
            return 0.0f;
        }
        const auto text = Wide(utf8);
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), f, 100000.0f, 1000.0f,
                                            &layout))) {
            return 0.0f;
        }
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        const float w = m.widthIncludingTrailingWhitespace * (game_fonts != nullptr ? FontStretch(weight) : 1.0f);
        if (widths.size() >= 4096) {
            widths.clear(); // (texts with numbers that change: kept bounded)
        }
        widths.emplace(std::move(key), w);
        return w;
    }

    void Triangle(const float* p, bool fill, float width) {
        ID2D1PathGeometry* geo = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(d2d->CreatePathGeometry(&geo)) || FAILED(geo->Open(&sink))) {
            Release(geo);
            return;
        }
        sink->BeginFigure(D2D1::Point2F(p[0], p[1]), fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
        sink->AddLine(D2D1::Point2F(p[2], p[3]));
        sink->AddLine(D2D1::Point2F(p[4], p[5]));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close();
        sink->Release();
        if (fill) {
            rt->FillGeometry(geo, brush);
        } else {
            rt->DrawGeometry(geo, brush, width);
        }
        geo->Release();
    }

    /// Draws the list's surface into `dc` (w x h). false: Direct2D failed (the target is recreated next time).
    bool Draw(const DrawList& list, HDC dc, int w, int h) {
        if (rt == nullptr && !CreateRenderTarget()) {
            return false;
        }
        const RECT bounds{0, 0, w, h};
        if (FAILED(rt->BindDC(dc, &bounds))) {
            return false;
        }
        const Rect surface = list.Surface();
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        rt->SetTransform(D2D1::Matrix3x2F::Translation(-surface.x, -surface.y));
        int clips = 0;
        for (const auto& c : list.Commands()) {
            brush->SetColor(ToD2D(c.color));
            switch (c.kind) {
            case DrawCmd::Kind::FillRect:
                if (c.radius > 0) {
                    rt->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(c.rect), c.radius, c.radius), brush);
                } else {
                    rt->FillRectangle(ToD2D(c.rect), brush);
                }
                break;
            case DrawCmd::Kind::StrokeRect: {
                // inset by half the stroke so the outline stays inside the rectangle
                const float i = c.width / 2;
                const Rect r{c.rect.x + i, c.rect.y + i, c.rect.w - c.width, c.rect.h - c.width};
                if (c.radius > 0) {
                    rt->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(r), c.radius, c.radius), brush, c.width);
                } else {
                    rt->DrawRectangle(ToD2D(r), brush, c.width);
                }
                break;
            }
            case DrawCmd::Kind::Line:
                rt->DrawLine(D2D1::Point2F(c.p[0], c.p[1]), D2D1::Point2F(c.p[2], c.p[3]), brush, c.width);
                break;
            case DrawCmd::Kind::FillCircle:
            case DrawCmd::Kind::StrokeCircle: {
                const auto e = D2D1::Ellipse(D2D1::Point2F(c.rect.x + c.rect.w / 2, c.rect.y + c.rect.h / 2),
                                             c.rect.w / 2, c.rect.h / 2);
                if (c.kind == DrawCmd::Kind::FillCircle) {
                    rt->FillEllipse(e, brush);
                } else {
                    rt->DrawEllipse(e, brush, c.width);
                }
                break;
            }
            case DrawCmd::Kind::FillTriangle:
            case DrawCmd::Kind::StrokeTriangle:
                Triangle(c.p, c.kind == DrawCmd::Kind::FillTriangle, c.width);
                break;
            case DrawCmd::Kind::Text: {
                IDWriteTextFormat* f = Format(c.size, c.weight, c.wrap);
                if (f == nullptr || c.text.empty()) {
                    break;
                }
                f->SetTextAlignment(c.align == Align::Left    ? DWRITE_TEXT_ALIGNMENT_LEADING
                                    : c.align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                              : DWRITE_TEXT_ALIGNMENT_CENTER);
                const auto text = Wide(c.text);
                const float sx = game_fonts != nullptr ? FontStretch(c.weight) : 1.0f;
                if (sx != 1.0f) {
                    // narrowed: drawn in a box sx times wider under a horizontal scale
                    D2D1_MATRIX_3X2_F old{};
                    rt->GetTransform(&old);
                    rt->SetTransform(D2D1::Matrix3x2F::Scale(sx, 1.0f) * old);
                    const Rect r{c.rect.x / sx, c.rect.y, c.rect.w / sx, c.rect.h};
                    rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), f, ToD2D(r), brush,
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    rt->SetTransform(old);
                } else {
                    rt->DrawText(text.c_str(), static_cast<UINT32>(text.size()), f, ToD2D(c.rect), brush,
                                 D2D1_DRAW_TEXT_OPTIONS_CLIP);
                }
                break;
            }
            case DrawCmd::Kind::PushClip:
                rt->PushAxisAlignedClip(ToD2D(c.rect), D2D1_ANTIALIAS_MODE_ALIASED);
                ++clips;
                break;
            case DrawCmd::Kind::PopClip:
                if (clips > 0) {
                    rt->PopAxisAlignedClip();
                    --clips;
                }
                break;
            }
        }
        while (clips-- > 0) {
            rt->PopAxisAlignedClip();
        }
        const HRESULT hr = rt->EndDraw();
        if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
            Release(brush);
            Release(rt);
            return false;
        }
        return SUCCEEDED(hr);
    }

    bool EnsureDib(int w, int h) {
        if (dib != nullptr && w == dib_w && h == dib_h) {
            return true;
        }
        FreeDib();
        HDC screen = GetDC(nullptr);
        mem_dc = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = w;
        bmi.bmiHeader.biHeight = -h; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib == nullptr) {
            FreeDib();
            return false;
        }
        SelectObject(mem_dc, dib);
        dib_w = w;
        dib_h = h;
        return true;
    }

    void FreeDib() {
        if (mem_dc != nullptr) {
            DeleteDC(mem_dc);
            mem_dc = nullptr;
        }
        if (dib != nullptr) {
            DeleteObject(dib);
            dib = nullptr;
        }
        bits = nullptr;
        dib_w = dib_h = 0;
    }

    void DestroyWindows() {
        if (panel != nullptr) {
            KillTimer(panel, 1);
            KillTimer(panel, kFadeTimer);
            DestroyWindow(panel);
            panel = nullptr;
        }
        if (dim != nullptr) {
            DestroyWindow(dim);
            dim = nullptr;
        }
        owner = nullptr;
        visible = false;
    }

    bool EnsureWindows(HWND new_owner) {
        if (owner == new_owner && panel != nullptr && dim != nullptr) {
            return true;
        }
        DestroyWindows();
        static const ATOM atom = [] {
            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.lpfnWndProc = &Impl::Proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            wc.lpszClassName = kClassName;
            return RegisterClassExW(&wc);
        }();
        if (atom == 0) {
            return false;
        }
        constexpr DWORD kEx = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
        dim = CreateWindowExW(kEx, kClassName, L"SecondSonPC menu backdrop", WS_POPUP, 0, 0, 1, 1, new_owner,
                              nullptr, GetModuleHandleW(nullptr), nullptr);
        panel = CreateWindowExW(kEx, kClassName, L"SecondSonPC menu", WS_POPUP, 0, 0, 1, 1, new_owner, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        if (dim == nullptr || panel == nullptr) {
            DestroyWindows();
            return false;
        }
        SetWindowLongPtrW(panel, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        owner = new_owner;
        return true;
    }
};

Win32Overlay::Win32Overlay() : impl_{std::make_unique<Impl>()} {}
Win32Overlay::~Win32Overlay() = default;

bool Win32Overlay::Init(std::string& error) {
    auto& m = *impl_;
    if (m.ok) {
        return true;
    }
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &m.d2d))) {
        error = "Direct2D factory";
        return false;
    }
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&m.dwrite)))) {
        error = "DirectWrite factory";
        return false;
    }
    if (IDWriteFontCollection* fonts = nullptr; SUCCEEDED(m.dwrite->GetSystemFontCollection(&fonts, FALSE))) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        m.have_bahnschrift = SUCCEEDED(fonts->FindFamilyName(L"Bahnschrift", &index, &exists)) && exists;
        fonts->Release();
    }
    m.LoadGameFonts();
    if (!m.CreateRenderTarget()) {
        error = "Direct2D software render target";
        return false;
    }
    m.ok = true;
    return true;
}

bool Win32Overlay::Ok() const {
    return impl_->ok;
}

TextMeasure& Win32Overlay::Measure() {
    return *impl_;
}

bool Win32Overlay::Visible() const {
    return impl_->visible;
}

void Win32Overlay::SetTimer(void (*callback)(void*), void* context, unsigned period_ms) {
    impl_->timer_cb = callback;
    impl_->timer_ctx = context;
    impl_->timer_ms = period_ms;
    if (impl_->visible && impl_->panel != nullptr) {
        ::SetTimer(impl_->panel, 1, period_ms, nullptr);
    }
}

bool Win32Overlay::Present(void* owner_hwnd, const DrawList& list) {
    auto& m = *impl_;
    const auto owner = static_cast<HWND>(owner_hwnd);
    if (!m.ok || owner == nullptr || !m.EnsureWindows(owner)) {
        return false;
    }
    const auto t0 = std::chrono::steady_clock::now();
    POINT origin{0, 0};
    ClientToScreen(owner, &origin);
    RECT client{};
    GetClientRect(owner, &client);
    const int cw = client.right - client.left, ch = client.bottom - client.top;

    // shown again: it fades in over a few timer steps (the opacity of the same pixels, nothing drawn again)
    if (!m.visible) {
        m.fade = 0;
    }
    m.dim_alpha = list.Dim();
    // backdrop: one colour with a constant alpha (no pixels to upload)
    SetLayeredWindowAttributes(m.dim, 0, static_cast<BYTE>(m.dim_alpha * m.fade / 255), LWA_ALPHA);
    SetWindowPos(m.dim, HWND_TOP, origin.x, origin.y, cw, ch, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    const Rect s = list.Surface();
    const int sx = static_cast<int>(std::floor(s.x)), sy = static_cast<int>(std::floor(s.y));
    const int sw = std::max(1, static_cast<int>(std::ceil(s.w))), sh = std::max(1, static_cast<int>(std::ceil(s.h)));
    bool drawn = m.EnsureDib(sw, sh) && m.Draw(list, m.mem_dc, sw, sh);
    if (!drawn) {
        drawn = m.EnsureDib(sw, sh) && m.Draw(list, m.mem_dc, sw, sh); // a recreated render target, once
    }
    if (drawn) {
        POINT dst{origin.x + sx, origin.y + sy};
        SIZE size{sw, sh};
        POINT src{0, 0};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, m.fade, AC_SRC_ALPHA};
        HDC screen = GetDC(nullptr);
        drawn = UpdateLayeredWindow(m.panel, screen, &dst, &size, m.mem_dc, &src, 0, &blend, ULW_ALPHA) != FALSE;
        ReleaseDC(nullptr, screen);
        SetWindowPos(m.panel, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    if (!drawn) {
        Hide(); // never a dimmed screen without the menu on it
        return false;
    }
    if (!m.visible && m.timer_cb != nullptr) {
        ::SetTimer(m.panel, 1, m.timer_ms, nullptr);
    }
    if (!m.visible && m.fade < 255) {
        ::SetTimer(m.panel, Impl::kFadeTimer, 16, nullptr);
    }
    m.visible = true;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    ++m.stats.frames;
    m.stats.last_ms = ms;
    m.stats.max_ms = std::max(m.stats.max_ms, ms);
    m.stats.total_ms += ms;
    return true;
}

void Win32Overlay::Hide() {
    auto& m = *impl_;
    if (m.panel != nullptr) {
        KillTimer(m.panel, 1);
        KillTimer(m.panel, Impl::kFadeTimer);
        ShowWindow(m.panel, SW_HIDE);
    }
    if (m.dim != nullptr) {
        ShowWindow(m.dim, SW_HIDE);
    }
    m.visible = false;
    m.fade = 255;
}

bool Win32Overlay::RenderToPixels(const DrawList& list, std::vector<std::uint32_t>& pixels, unsigned& width,
                                  unsigned& height) {
    auto& m = *impl_;
    if (!m.ok) {
        return false;
    }
    const Rect s = list.Surface();
    const int sw = std::max(1, static_cast<int>(std::ceil(s.w))), sh = std::max(1, static_cast<int>(std::ceil(s.h)));
    if (!m.EnsureDib(sw, sh) || !m.Draw(list, m.mem_dc, sw, sh)) {
        return false;
    }
    GdiFlush();
    width = static_cast<unsigned>(sw);
    height = static_cast<unsigned>(sh);
    const auto* src = static_cast<const std::uint32_t*>(m.bits);
    pixels.assign(src, src + static_cast<size_t>(sw) * static_cast<size_t>(sh));
    return true;
}

Win32Overlay::Stats Win32Overlay::GetStats() const {
    return impl_->stats;
}

} // namespace SecondSon::Ui
