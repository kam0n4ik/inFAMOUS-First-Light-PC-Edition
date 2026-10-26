// SecondSonPC — the launcher's drawing surface (canvas.h). Own code.
#include "launcher/canvas.h"

#include <windows.h>

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite_1.h>
#include <wincodec.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <numbers>
#include <tuple>
#include <unordered_map>

namespace SecondSon::Launcher {

namespace {

template <class T>
void Release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

std::wstring Wide(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

/// UTF-16 index of UTF-8 byte offset `byte` in `s`.
u32 Utf16Index(std::string_view s, u32 byte) {
    u32 i = 0, w = 0;
    while (i < byte && i < s.size()) {
        const auto c = static_cast<unsigned char>(s[i]);
        const u32 len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        w += len == 4 ? 2 : 1;
        i += len;
    }
    return w;
}

D2D1_COLOR_F ToD2D(Color c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
}
D2D1_RECT_F ToD2D(Rect r) {
    return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h);
}

/// The face Chrome picks for a CSS weight among Segoe UI's (300, 350, 400, 600, 700, 900): CSS Fonts 4 §5.2.
int SegoeWeight(int css) {
    static constexpr int kFaces[] = {300, 350, 400, 600, 700, 900};
    for (const int f : kFaces) {
        if (f == css) {
            return f;
        }
    }
    if (css >= 400 && css <= 500) {
        for (const int f : kFaces) { // up to 500 ascending
            if (f > css && f <= 500) {
                return f;
            }
        }
    }
    if (css <= 500) { // then below, descending
        for (int i = 5; i >= 0; --i) {
            if (kFaces[i] < css) {
                return kFaces[i];
            }
        }
        return kFaces[0];
    }
    for (const int f : kFaces) { // > 500: ascending from the weight
        if (f > css) {
            return f;
        }
    }
    return 900;
}

// ---- SVG path data -> a Direct2D geometry (M L H V C S Q T A Z, absolute and relative)
class PathParser {
public:
    explicit PathParser(std::string_view s) : s_(s) {}

    bool Build(ID2D1GeometrySink* sink) {
        char cmd = 0;
        D2D1_POINT_2F cur{0, 0}, start{0, 0}, ctrl{0, 0};
        char last = 0;
        bool open = false;
        auto end_figure = [&](bool closed) {
            if (open) {
                sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
                open = false;
            }
        };
        auto begin = [&](D2D1_POINT_2F p) {
            end_figure(false);
            sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
            open = true;
            start = p;
        };
        for (;;) {
            Skip();
            if (i_ >= s_.size()) {
                break;
            }
            const char c = s_[i_];
            if (std::isalpha(static_cast<unsigned char>(c))) {
                cmd = c;
                ++i_;
            } else if (cmd == 0) {
                return false;
            }
            const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
            const char up = static_cast<char>(std::toupper(static_cast<unsigned char>(cmd)));
            const float ox = rel ? cur.x : 0.0f, oy = rel ? cur.y : 0.0f;
            if (up == 'M') {
                float x, y;
                if (!Num(x) || !Num(y)) {
                    return false;
                }
                cur = {ox + x, oy + y};
                begin(cur);
                cmd = rel ? 'l' : 'L'; // further pairs are lines
                last = 'M';
                continue;
            }
            if (up == 'Z') {
                end_figure(true);
                cur = start;
                last = 'Z';
                continue;
            }
            if (!open) {
                begin(cur);
            }
            switch (up) {
            case 'L': {
                float x, y;
                if (!Num(x) || !Num(y)) {
                    return false;
                }
                cur = {ox + x, oy + y};
                sink->AddLine(cur);
                break;
            }
            case 'H': {
                float x;
                if (!Num(x)) {
                    return false;
                }
                cur.x = ox + x;
                sink->AddLine(cur);
                break;
            }
            case 'V': {
                float y;
                if (!Num(y)) {
                    return false;
                }
                cur.y = oy + y;
                sink->AddLine(cur);
                break;
            }
            case 'C': {
                float x1, y1, x2, y2, x, y;
                if (!Num(x1) || !Num(y1) || !Num(x2) || !Num(y2) || !Num(x) || !Num(y)) {
                    return false;
                }
                sink->AddBezier({{ox + x1, oy + y1}, {ox + x2, oy + y2}, {ox + x, oy + y}});
                ctrl = {ox + x2, oy + y2};
                cur = {ox + x, oy + y};
                break;
            }
            case 'S': {
                float x2, y2, x, y;
                if (!Num(x2) || !Num(y2) || !Num(x) || !Num(y)) {
                    return false;
                }
                const D2D1_POINT_2F c1 =
                    last == 'C' || last == 'S' ? D2D1_POINT_2F{2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y} : cur;
                sink->AddBezier({c1, {ox + x2, oy + y2}, {ox + x, oy + y}});
                ctrl = {ox + x2, oy + y2};
                cur = {ox + x, oy + y};
                break;
            }
            case 'Q': {
                float x1, y1, x, y;
                if (!Num(x1) || !Num(y1) || !Num(x) || !Num(y)) {
                    return false;
                }
                sink->AddQuadraticBezier({{ox + x1, oy + y1}, {ox + x, oy + y}});
                ctrl = {ox + x1, oy + y1};
                cur = {ox + x, oy + y};
                break;
            }
            case 'T': {
                float x, y;
                if (!Num(x) || !Num(y)) {
                    return false;
                }
                const D2D1_POINT_2F c1 =
                    last == 'Q' || last == 'T' ? D2D1_POINT_2F{2 * cur.x - ctrl.x, 2 * cur.y - ctrl.y} : cur;
                sink->AddQuadraticBezier({c1, {ox + x, oy + y}});
                ctrl = c1;
                cur = {ox + x, oy + y};
                break;
            }
            case 'A': {
                float rx, ry, rot, x, y;
                bool large, sweep;
                if (!Num(rx) || !Num(ry) || !Num(rot) || !Flag(large) || !Flag(sweep) || !Num(x) || !Num(y)) {
                    return false;
                }
                const D2D1_POINT_2F to{ox + x, oy + y};
                if (rx == 0 || ry == 0) {
                    sink->AddLine(to);
                } else {
                    sink->AddArc({to, {std::abs(rx), std::abs(ry)}, rot,
                                  sweep ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                                  large ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
                }
                cur = to;
                break;
            }
            default:
                return false;
            }
            last = up;
        }
        end_figure(false);
        return true;
    }

private:
    void Skip() {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == ',' || s_[i_] == '\n' || s_[i_] == '\t' || s_[i_] == '\r')) {
            ++i_;
        }
    }
    bool Num(float& out) {
        Skip();
        const size_t b = i_;
        if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) {
            ++i_;
        }
        bool dot = false, digits = false;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c >= '0' && c <= '9') {
                digits = true;
            } else if (c == '.' && !dot) {
                dot = true;
            } else {
                break;
            }
            ++i_;
        }
        if (digits && i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E')) {
            ++i_;
            if (i_ < s_.size() && (s_[i_] == '-' || s_[i_] == '+')) {
                ++i_;
            }
            while (i_ < s_.size() && s_[i_] >= '0' && s_[i_] <= '9') {
                ++i_;
            }
        }
        if (!digits) {
            i_ = b;
            return false;
        }
        out = std::strtof(std::string(s_.substr(b, i_ - b)).c_str(), nullptr);
        return true;
    }
    bool Flag(bool& out) {
        Skip();
        if (i_ < s_.size() && (s_[i_] == '0' || s_[i_] == '1')) {
            out = s_[i_++] == '1';
            return true;
        }
        return false;
    }

    std::string_view s_;
    size_t i_ = 0;
};

struct LayoutKey {
    std::string text;
    float size, tracking, line_h, max_w;
    int weight;
    bool mono, tabular, italic, wrap, display;
    u8 align;
    std::string spans; // the spans as bytes
    bool operator<(const LayoutKey& o) const {
        return std::tie(text, size, tracking, line_h, max_w, weight, mono, tabular, italic, wrap, display, align,
                        spans) < std::tie(o.text, o.size, o.tracking, o.line_h, o.max_w, o.weight, o.mono, o.tabular,
                                          o.italic, o.wrap, o.display, o.align, o.spans);
    }
};

} // namespace

Color Mix(Color a, Color b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

struct Image {
    IWICFormatConverter* source = nullptr; // 32bppPBGRA
    u32 w = 0, h = 0;
    mutable ID2D1Bitmap* bitmap = nullptr;
    mutable u64 generation = 0; // the render target's generation it was made for
    ~Image() {
        Release(bitmap);
        Release(source);
    }
};

struct Canvas::Impl {
    ID2D1Factory1* factory = nullptr;
    IDWriteFactory1* dwrite = nullptr;
    IWICImagingFactory* wic = nullptr;
    HWND hwnd = nullptr;
    ID2D1HwndRenderTarget* hwnd_rt = nullptr;
    IWICBitmap* offscreen = nullptr;
    ID2D1RenderTarget* rt = nullptr; // hwnd_rt or the offscreen one
    ID2D1DeviceContext* dc = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1StrokeStyle* round = nullptr;
    ID2D1Effect* shadow = nullptr;
    u64 generation = 1;
    float scale = 1.0f;
    u32 width = 0, height = 0;
    bool offscreen_mode = false;
    struct Face {
        std::wstring family;
        float asc = 0.0f, desc = 0.0f, gap = 0.0f; // em
        bool variable = false;                      // any weight (Segoe UI Variable), else Segoe UI's faces
    };
    Face text{L"Segoe UI", 2210.0f / 2048.0f, 514.0f / 2048.0f, 0.0f, false}; // (read from the fonts at Init)
    Face display_face = text;
    Face mono{L"Consolas", 0.0f, 0.0f, 0.0f, false};
    std::map<std::string, ID2D1PathGeometry*, std::less<>> icons;
    std::map<LayoutKey, IDWriteTextLayout*> layouts;
    std::map<std::tuple<int, int, bool>, IDWriteTextFormat*> formats; // (weight, face, italic) at 16 px
    IDWriteInlineObject* ellipsis = nullptr;
    IDWriteTypography* tabular = nullptr;
    std::vector<D2D1_MATRIX_3X2_F> transforms;
    std::vector<bool> layer_is_clip; // PushClip (true) / PushOpacity (false)

    ~Impl() {
        for (auto& [k, g] : icons) {
            Release(g);
        }
        for (auto& [k, l] : layouts) {
            Release(l);
        }
        for (auto& [k, f] : formats) {
            Release(f);
        }
        Release(ellipsis);
        Release(tabular);
        ReleaseTarget();
        Release(round);
        Release(offscreen);
        Release(wic);
        Release(dwrite);
        Release(factory);
    }

    void ReleaseTarget() {
        Release(shadow);
        Release(brush);
        Release(dc);
        Release(rt);
        hwnd_rt = nullptr;
        ++generation;
    }

    bool CreateTarget(std::string& error) {
        HRESULT hr;
        if (offscreen_mode) {
            const D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f * scale,
                96.0f * scale);
            hr = factory->CreateWicBitmapRenderTarget(offscreen, p, &rt);
        } else {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            width = static_cast<u32>(std::max<LONG>(1, rc.right - rc.left));
            height = static_cast<u32>(std::max<LONG>(1, rc.bottom - rc.top));
            const D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
                96.0f * scale, 96.0f * scale);
            hr = factory->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width, height)),
                                                 &hwnd_rt);
            rt = hwnd_rt;
        }
        if (FAILED(hr) || rt == nullptr) {
            error = "Direct2D: the render target could not be created";
            return false;
        }
        rt->QueryInterface(__uuidof(ID2D1DeviceContext), reinterpret_cast<void**>(&dc));
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &brush);
        if (dc != nullptr) {
            dc->CreateEffect(CLSID_D2D1Shadow, &shadow);
        }
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return brush != nullptr;
    }

    bool InitShared(std::string& error) {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                                     reinterpret_cast<void**>(&factory)))) {
            error = "Direct2D 1.1 is not available";
            return false;
        }
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory1),
                                       reinterpret_cast<IUnknown**>(&dwrite)))) {
            error = "DirectWrite is not available";
            return false;
        }
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
            error = "Windows Imaging Component is not available";
            return false;
        }
        const D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
            D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
        factory->CreateStrokeStyle(sp, nullptr, 0, &round);
        // the faces' metrics (CSS line boxes: the content area is ascent + descent) and the monospace face
        IDWriteFontCollection* sys = nullptr;
        if (SUCCEEDED(dwrite->GetSystemFontCollection(&sys, FALSE)) && sys != nullptr) {
            auto metrics = [&](const wchar_t* family, Face& out, bool variable) {
                UINT32 index = 0;
                BOOL exists = FALSE;
                if (FAILED(sys->FindFamilyName(family, &index, &exists)) || !exists) {
                    return false;
                }
                IDWriteFontFamily* fam = nullptr;
                IDWriteFont* font = nullptr;
                sys->GetFontFamily(index, &fam);
                if (fam != nullptr) {
                    fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                              DWRITE_FONT_STYLE_NORMAL, &font);
                }
                if (font != nullptr) {
                    DWRITE_FONT_METRICS m{};
                    font->GetMetrics(&m);
                    out.family = family;
                    out.asc = static_cast<float>(m.ascent) / m.designUnitsPerEm;
                    out.desc = static_cast<float>(m.descent) / m.designUnitsPerEm;
                    out.gap = static_cast<float>(m.lineGap) / m.designUnitsPerEm;
                    out.variable = variable;
                }
                Release(font);
                Release(fam);
                return font != nullptr;
            };
            // the mockup's stacks: --font "Segoe UI Variable Text", "Segoe UI"; --font-display "Segoe UI Variable
            // Display", "Segoe UI"; --mono "Cascadia Mono", Consolas
            if (!metrics(L"Segoe UI Variable Text", text, true)) {
                metrics(L"Segoe UI", text, false);
            }
            if (!metrics(L"Segoe UI Variable Display", display_face, true)) {
                display_face = text;
            }
            if (!metrics(L"Cascadia Mono", mono, false)) {
                metrics(L"Consolas", mono, false);
            }
            Release(sys);
        }
        dwrite->CreateTypography(&tabular);
        if (tabular != nullptr) {
            tabular->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
        }
        return true;
    }

    const Face& FaceOf(const Font& f) const {
        return f.mono ? mono : f.display ? display_face : text;
    }
    int WeightOf(const Face& face, int css) const {
        return face.variable ? css : &face == &mono ? 400 : SegoeWeight(css);
    }

    IDWriteTextFormat* Format(const Font& f) {
        const Face& face = FaceOf(f);
        const int w = WeightOf(face, f.weight);
        const auto key = std::make_tuple(w, f.mono ? 2 : f.display ? 1 : 0, f.italic);
        if (const auto it = formats.find(key); it != formats.end()) {
            return it->second;
        }
        IDWriteTextFormat* fmt = nullptr;
        dwrite->CreateTextFormat(face.family.c_str(), nullptr, static_cast<DWRITE_FONT_WEIGHT>(w),
                                 f.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"ru-ru", &fmt);
        if (fmt != nullptr && ellipsis == nullptr) {
            dwrite->CreateEllipsisTrimmingSign(fmt, &ellipsis);
        }
        formats.emplace(key, fmt);
        return fmt;
    }

    IDWriteTextLayout* Layout(std::string_view utf8, const Font& f, float line_h, float max_w, bool wrap, Align align,
                              std::span<const Span> spans) {
        LayoutKey key{std::string(utf8), f.size, f.tracking, line_h, max_w, f.weight, f.mono, f.tabular, f.italic,
                      wrap, f.display, static_cast<u8>(align), {}};
        if (!spans.empty()) {
            key.spans.assign(reinterpret_cast<const char*>(spans.data()), spans.size_bytes());
        }
        if (const auto it = layouts.find(key); it != layouts.end()) {
            return it->second;
        }
        if (layouts.size() > 4000) {
            for (auto& [k, l] : layouts) {
                Release(l);
            }
            layouts.clear();
        }
        IDWriteTextFormat* fmt = Format(f);
        if (fmt == nullptr) {
            return nullptr;
        }
        const std::wstring w = Wide(utf8);
        IDWriteTextLayout* layout = nullptr;
        dwrite->CreateTextLayout(w.c_str(), static_cast<UINT32>(w.size()), fmt, std::max(1.0f, max_w), 100000.0f,
                                 &layout);
        if (layout == nullptr) {
            return nullptr;
        }
        const DWRITE_TEXT_RANGE all{0, static_cast<UINT32>(w.size())};
        layout->SetFontSize(f.size, all);
        layout->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        layout->SetTextAlignment(align == Align::Center  ? DWRITE_TEXT_ALIGNMENT_CENTER
                                 : align == Align::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                                         : DWRITE_TEXT_ALIGNMENT_LEADING);
        const Face& face = FaceOf(f);
        const float baseline = (line_h - (face.asc + face.desc) * f.size) / 2.0f + face.asc * f.size;
        layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, line_h, baseline);
        if (!wrap && ellipsis != nullptr) {
            const DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            layout->SetTrimming(&trim, ellipsis);
        }
        if (f.tabular && tabular != nullptr) {
            layout->SetTypography(tabular, all);
        }
        if (f.tracking != 0.0f) {
            IDWriteTextLayout1* l1 = nullptr;
            if (SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1), reinterpret_cast<void**>(&l1)))) {
                l1->SetCharacterSpacing(0.0f, f.tracking * f.size, 0.0f, all);
                l1->Release();
            }
        }
        for (const Span& s : spans) {
            const u32 b = Utf16Index(utf8, s.begin), e = Utf16Index(utf8, s.end);
            const DWRITE_TEXT_RANGE r{b, e - b};
            if (s.weight != 0) {
                layout->SetFontWeight(static_cast<DWRITE_FONT_WEIGHT>(WeightOf(face, s.weight)), r);
            }
            if (s.italic) {
                layout->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
            }
        }
        layouts.emplace(std::move(key), layout);
        return layout;
    }

    ID2D1PathGeometry* Path(std::string_view data) {
        if (const auto it = icons.find(data); it != icons.end()) {
            return it->second;
        }
        ID2D1PathGeometry* g = nullptr;
        factory->CreatePathGeometry(&g);
        if (g != nullptr) {
            ID2D1GeometrySink* sink = nullptr;
            g->Open(&sink);
            if (sink != nullptr) {
                sink->SetFillMode(D2D1_FILL_MODE_WINDING);
                PathParser(data).Build(sink);
                sink->Close();
                sink->Release();
            }
        }
        icons.emplace(std::string(data), g);
        return g;
    }

    ID2D1Geometry* RoundedGeometry(Rect r, float tl, float tr, float br, float bl) {
        ID2D1PathGeometry* g = nullptr;
        factory->CreatePathGeometry(&g);
        if (g == nullptr) {
            return nullptr;
        }
        ID2D1GeometrySink* s = nullptr;
        g->Open(&s);
        if (s == nullptr) {
            g->Release();
            return nullptr;
        }
        const auto arc = [&](float x, float y, float rad) {
            s->AddArc({{x, y}, {rad, rad}, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL});
        };
        s->BeginFigure({r.x + tl, r.y}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine({r.R() - tr, r.y});
        if (tr > 0) {
            arc(r.R(), r.y + tr, tr);
        }
        s->AddLine({r.R(), r.B() - br});
        if (br > 0) {
            arc(r.R() - br, r.B(), br);
        }
        s->AddLine({r.x + bl, r.B()});
        if (bl > 0) {
            arc(r.x, r.B() - bl, bl);
        }
        s->AddLine({r.x, r.y + tl});
        if (tl > 0) {
            arc(r.x + tl, r.y, tl);
        }
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        s->Release();
        return g;
    }

    void SetColor(Color c) {
        brush->SetColor(ToD2D(c));
    }

    ID2D1GradientStopCollection* Stops(std::span<const Stop> stops) {
        std::vector<D2D1_GRADIENT_STOP> v;
        v.reserve(stops.size());
        for (const Stop& s : stops) {
            v.push_back({s.at, ToD2D(s.color)});
        }
        ID2D1GradientStopCollection* c = nullptr;
        rt->CreateGradientStopCollection(v.data(), static_cast<UINT32>(v.size()), D2D1_GAMMA_2_2,
                                         D2D1_EXTEND_MODE_CLAMP, &c);
        return c;
    }
};

Canvas::Canvas() : d_(std::make_unique<Impl>()) {}
Canvas::~Canvas() = default;

bool Canvas::Init(HWND__* hwnd, std::string& error) {
    d_->hwnd = hwnd;
    return d_->InitShared(error) && d_->CreateTarget(error);
}

bool Canvas::InitOffscreen(u32 width_px, u32 height_px, float scale, std::string& error) {
    if (!d_->InitShared(error)) {
        return false;
    }
    d_->offscreen_mode = true;
    d_->scale = scale;
    d_->width = width_px;
    d_->height = height_px;
    if (FAILED(d_->wic->CreateBitmap(width_px, height_px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                     &d_->offscreen))) {
        error = "WIC: the offscreen bitmap could not be created";
        return false;
    }
    return d_->CreateTarget(error);
}

void Canvas::Resize(u32 width_px, u32 height_px) {
    d_->width = std::max(1u, width_px);
    d_->height = std::max(1u, height_px);
    if (d_->hwnd_rt != nullptr) {
        d_->hwnd_rt->Resize(D2D1::SizeU(d_->width, d_->height));
    }
}

void Canvas::SetScale(float scale) {
    d_->scale = scale;
    if (d_->rt != nullptr) {
        d_->rt->SetDpi(96.0f * scale, 96.0f * scale);
    }
}

float Canvas::Scale() const {
    return d_->scale;
}

bool Canvas::Begin(Color clear) {
    if (d_->rt == nullptr) {
        std::string error;
        if (!d_->CreateTarget(error)) {
            return false;
        }
    }
    d_->transforms.clear();
    d_->layer_is_clip.clear();
    d_->rt->BeginDraw();
    d_->rt->SetTransform(D2D1::Matrix3x2F::Identity());
    d_->rt->Clear(ToD2D(clear));
    return true;
}

bool Canvas::End() {
    while (!d_->layer_is_clip.empty()) {
        if (d_->layer_is_clip.back()) {
            PopClip();
        } else {
            PopOpacity();
        }
    }
    const HRESULT hr = d_->rt->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        d_->ReleaseTarget();
        return false;
    }
    return SUCCEEDED(hr);
}

bool Canvas::SavePng(const std::wstring& path, std::string& error) {
    if (d_->offscreen == nullptr) {
        error = "not an offscreen canvas";
        return false;
    }
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(d_->wic->CreateStream(&stream)) &&
              SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(d_->wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
              SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr));
    if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        frame->SetSize(d_->width, d_->height);
        frame->SetPixelFormat(&fmt);
        ok = SUCCEEDED(frame->WriteSource(d_->offscreen, nullptr)) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(enc->Commit());
    }
    Release(frame);
    Release(enc);
    Release(stream);
    if (!ok) {
        error = "the PNG could not be written";
    }
    return ok;
}

void Canvas::FillRect(Rect r, Color c, float radius) {
    if (c.a <= 0.0f || r.w <= 0 || r.h <= 0) {
        return;
    }
    d_->SetColor(c);
    if (radius <= 0.0f) {
        d_->rt->FillRectangle(ToD2D(r), d_->brush);
    } else {
        radius = std::min({radius, r.w / 2, r.h / 2});
        d_->rt->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(r), radius, radius), d_->brush);
    }
}

void Canvas::FillRect4(Rect r, Color c, float tl, float tr, float br, float bl) {
    if (c.a <= 0.0f) {
        return;
    }
    ID2D1Geometry* g = d_->RoundedGeometry(r, tl, tr, br, bl);
    if (g != nullptr) {
        d_->SetColor(c);
        d_->rt->FillGeometry(g, d_->brush);
        g->Release();
    }
}

void Canvas::Border(Rect r, Color c, float width, float radius) {
    if (c.a <= 0.0f || width <= 0) {
        return;
    }
    d_->SetColor(c);
    const Rect in = r.Inset(width / 2);
    if (radius <= 0.0f) {
        d_->rt->DrawRectangle(ToD2D(in), d_->brush, width);
    } else {
        const float rad = std::max(0.0f, std::min({radius - width / 2, in.w / 2, in.h / 2}));
        d_->rt->DrawRoundedRectangle(D2D1::RoundedRect(ToD2D(in), rad, rad), d_->brush, width);
    }
}

void Canvas::Line(float x0, float y0, float x1, float y1, Color c, float width) {
    d_->SetColor(c);
    d_->rt->DrawLine({x0, y0}, {x1, y1}, d_->brush, width);
}

void Canvas::FillCircle(float cx, float cy, float radius, Color c) {
    d_->SetColor(c);
    d_->rt->FillEllipse(D2D1::Ellipse({cx, cy}, radius, radius), d_->brush);
}

void Canvas::StrokeCircle(float cx, float cy, float radius, Color c, float width) {
    d_->SetColor(c);
    d_->rt->DrawEllipse(D2D1::Ellipse({cx, cy}, radius, radius), d_->brush, width);
}

void Canvas::LinearGradient(Rect r, float angle_deg, std::span<const Stop> stops, float radius) {
    ID2D1GradientStopCollection* sc = d_->Stops(stops);
    if (sc == nullptr) {
        return;
    }
    // the CSS gradient line: through the centre at the angle, as long as the box's projection on it
    const float a = angle_deg * std::numbers::pi_v<float> / 180.0f;
    const float dx = std::sin(a), dy = -std::cos(a);
    const float len = std::abs(r.w * dx) + std::abs(r.h * dy);
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    ID2D1LinearGradientBrush* b = nullptr;
    d_->rt->CreateLinearGradientBrush(
        {{cx - dx * len / 2, cy - dy * len / 2}, {cx + dx * len / 2, cy + dy * len / 2}}, sc, &b);
    if (b != nullptr) {
        if (radius > 0) {
            d_->rt->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(r), radius, radius), b);
        } else {
            d_->rt->FillRectangle(ToD2D(r), b);
        }
        b->Release();
    }
    sc->Release();
}

void Canvas::RadialGradient(Rect r, float rx, float ry, float cx, float cy, std::span<const Stop> stops) {
    ID2D1GradientStopCollection* sc = d_->Stops(stops);
    if (sc == nullptr) {
        return;
    }
    ID2D1RadialGradientBrush* b = nullptr;
    d_->rt->CreateRadialGradientBrush({{r.x + r.w * cx, r.y + r.h * cy}, {0, 0}, r.w * rx, r.h * ry}, sc, &b);
    if (b != nullptr) {
        d_->rt->FillRectangle(ToD2D(r), b);
        b->Release();
    }
    sc->Release();
}

void Canvas::Hatch(Rect r, float angle_deg, float period, float offset, float line, Color c, bool fade) {
    // the mask as a layer's opacity brush
    ID2D1LinearGradientBrush* mask = nullptr;
    ID2D1GradientStopCollection* msc = nullptr;
    ID2D1Layer* layer = nullptr;
    bool pushed = false;
    if (fade) {
        const Stop ms[] = {{0.0f, Rgb(0, 1.0f)}, {0.6f, Rgb(0, 0.5f)}, {1.0f, Rgb(0, 0.0f)}};
        msc = d_->Stops(ms);
        if (msc != nullptr) {
            d_->rt->CreateLinearGradientBrush({{r.x, r.y}, {r.x, r.B()}}, msc, &mask);
        }
        d_->rt->CreateLayer(nullptr, &layer);
        if (layer != nullptr && mask != nullptr) {
            d_->rt->PushLayer(D2D1::LayerParameters(ToD2D(r), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                    D2D1::IdentityMatrix(), 1.0f, mask),
                              layer);
            pushed = true;
        }
    }
    if (!pushed) {
        d_->rt->PushAxisAlignedClip(ToD2D(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }
    const float a = angle_deg * std::numbers::pi_v<float> / 180.0f;
    const float dx = std::sin(a), dy = -std::cos(a); // the gradient's direction
    const float len = std::abs(r.w * dx) + std::abs(r.h * dy);
    const float cx = r.x + r.w / 2, cy = r.y + r.h / 2;
    const float sx = cx - dx * len / 2, sy = cy - dy * len / 2; // its start
    const float px = -dy, py = dx;                             // along a line
    d_->SetColor(c);
    for (float t = offset + line / 2; t < len; t += period) {
        const float lx = sx + dx * t, ly = sy + dy * t;
        d_->rt->DrawLine({lx - px * len, ly - py * len}, {lx + px * len, ly + py * len}, d_->brush, line);
    }
    if (pushed) {
        d_->rt->PopLayer();
    } else {
        d_->rt->PopAxisAlignedClip();
    }
    Release(layer);
    Release(mask);
    Release(msc);
}

void Canvas::Shadow(Rect r, float radius, float dx, float dy, float blur, float spread, Color c) {
    if (c.a <= 0.0f) {
        return;
    }
    const Rect s{r.x + dx - spread, r.y + dy - spread, r.w + 2 * spread, r.h + 2 * spread};
    const float rad = std::max(0.0f, radius + spread);
    if (blur <= 0.0f || d_->dc == nullptr || d_->shadow == nullptr) {
        FillRect(s, c, rad);
        return;
    }
    ID2D1CommandList* list = nullptr;
    if (FAILED(d_->dc->CreateCommandList(&list))) {
        FillRect(s, c, rad);
        return;
    }
    ID2D1Image* target = nullptr;
    d_->dc->GetTarget(&target);
    D2D1_MATRIX_3X2_F m;
    d_->dc->GetTransform(&m);
    d_->dc->SetTarget(list);
    d_->dc->SetTransform(D2D1::Matrix3x2F::Identity());
    d_->SetColor(Rgb(0));
    d_->dc->FillRoundedRectangle(D2D1::RoundedRect(ToD2D(s), rad, rad), d_->brush);
    list->Close();
    d_->dc->SetTarget(target);
    d_->dc->SetTransform(m);
    d_->shadow->SetInput(0, list);
    d_->shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, blur / 2.0f);
    d_->shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(c.r, c.g, c.b, c.a));
    d_->dc->DrawImage(d_->shadow);
    d_->shadow->SetInput(0, nullptr);
    Release(target);
    list->Release();
}

void Canvas::Icon(std::string_view path_data, Rect box, Color c, float stroke, bool fill, float view, bool butt) {
    if (path_data.empty()) {
        return;
    }
    ID2D1PathGeometry* g = d_->Path(path_data);
    if (g == nullptr) {
        return;
    }
    D2D1_MATRIX_3X2_F old;
    d_->rt->GetTransform(&old);
    d_->rt->SetTransform(D2D1::Matrix3x2F::Scale(box.w / view, box.h / view) *
                         D2D1::Matrix3x2F::Translation(box.x, box.y) * old);
    d_->SetColor(c);
    if (fill) {
        d_->rt->FillGeometry(g, d_->brush);
    }
    if (stroke > 0.0f) {
        d_->rt->DrawGeometry(g, d_->brush, stroke, butt ? nullptr : d_->round);
    }
    d_->rt->SetTransform(old);
}

std::shared_ptr<Image> Canvas::LoadPicture(const std::wstring& path, std::string& error) {
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    auto img = std::make_shared<Image>();
    if (FAILED(d_->wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                                  &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(d_->wic->CreateFormatConverter(&img->source)) ||
        FAILED(img->source->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeMedianCut))) {
        Release(frame);
        Release(dec);
        error = "the image could not be read";
        return nullptr;
    }
    UINT w = 0, h = 0;
    img->source->GetSize(&w, &h);
    img->w = w;
    img->h = h;
    Release(frame);
    Release(dec);
    return img;
}

void Canvas::ImageCover(const Image& image, Rect r, float px, float py, float opacity) {
    if (image.bitmap == nullptr || image.generation != d_->generation) {
        Release(image.bitmap);
        d_->rt->CreateBitmapFromWicBitmap(image.source, nullptr, &image.bitmap);
        image.generation = d_->generation;
    }
    if (image.bitmap == nullptr || image.w == 0 || image.h == 0) {
        return;
    }
    const float k = std::max(r.w / static_cast<float>(image.w), r.h / static_cast<float>(image.h));
    const float dw = static_cast<float>(image.w) * k, dh = static_cast<float>(image.h) * k;
    const Rect dst{r.x + (r.w - dw) * px, r.y + (r.h - dh) * py, dw, dh};
    d_->rt->PushAxisAlignedClip(ToD2D(r), D2D1_ANTIALIAS_MODE_ALIASED);
    if (d_->dc != nullptr) {
        d_->dc->DrawBitmap(image.bitmap, ToD2D(dst), opacity, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr,
                           nullptr);
    } else {
        d_->rt->DrawBitmap(image.bitmap, ToD2D(dst), opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
    d_->rt->PopAxisAlignedClip();
}

void Canvas::Text(std::string_view utf8, Rect box, const Font& font, Color c, float line_h, Align align, bool wrap,
                  std::span<const Span> spans) {
    if (utf8.empty() || c.a <= 0.0f) {
        return;
    }
    IDWriteTextLayout* l = d_->Layout(utf8, font, line_h, box.w, wrap, align, spans);
    if (l == nullptr) {
        return;
    }
    // coloured spans: drawing effects (a brush per span)
    std::vector<ID2D1SolidColorBrush*> made;
    for (const Span& s : spans) {
        if (!s.color) {
            continue;
        }
        ID2D1SolidColorBrush* b = nullptr;
        d_->rt->CreateSolidColorBrush(ToD2D(s.rgb), &b);
        if (b != nullptr) {
            const u32 bi = Utf16Index(utf8, s.begin), ei = Utf16Index(utf8, s.end);
            l->SetDrawingEffect(b, {bi, ei - bi});
            made.push_back(b);
        }
    }
    d_->SetColor(c);
    d_->rt->DrawTextLayout({box.x, box.y}, l, d_->brush, D2D1_DRAW_TEXT_OPTIONS_NONE);
    if (!made.empty()) {
        l->SetDrawingEffect(nullptr, {0, 0xFFFFFFFFu});
        for (auto* b : made) {
            b->Release();
        }
    }
}

TextBox Canvas::Measure(std::string_view utf8, const Font& font, float line_h, float max_w, bool wrap,
                        std::span<const Span> spans) {
    TextBox out;
    if (utf8.empty()) {
        return out;
    }
    IDWriteTextLayout* l = d_->Layout(utf8, font, line_h, wrap ? max_w : 100000.0f, wrap, Align::Left, spans);
    if (l == nullptr) {
        return out;
    }
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    out.w = m.widthIncludingTrailingWhitespace;
    out.lines = m.lineCount;
    out.h = static_cast<float>(m.lineCount) * line_h;
    DWRITE_HIT_TEST_METRICS hm{};
    FLOAT x = 0, y = 0;
    l->HitTestTextPosition(static_cast<UINT32>(Wide(utf8).size()), FALSE, &x, &y, &hm);
    out.last_line_w = x;
    return out;
}

float Canvas::NormalLineHeight(const Font& font) const {
    const auto& face = d_->FaceOf(font);
    return (face.asc + face.desc + face.gap) * font.size;
}

void Canvas::PushClip(Rect r) {
    d_->rt->PushAxisAlignedClip(ToD2D(r), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d_->layer_is_clip.push_back(true);
}

void Canvas::PopClip() {
    d_->rt->PopAxisAlignedClip();
    d_->layer_is_clip.pop_back();
}

void Canvas::PushOpacity(float alpha) {
    d_->rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                            D2D1::IdentityMatrix(), alpha),
                      nullptr);
    d_->layer_is_clip.push_back(false);
}

void Canvas::PopOpacity() {
    d_->rt->PopLayer();
    d_->layer_is_clip.pop_back();
}

void Canvas::PushTranslate(float dx, float dy) {
    D2D1_MATRIX_3X2_F m;
    d_->rt->GetTransform(&m);
    d_->transforms.push_back(m);
    d_->rt->SetTransform(D2D1::Matrix3x2F::Translation(dx, dy) * m);
}

void Canvas::PopTranslate() {
    if (!d_->transforms.empty()) {
        d_->rt->SetTransform(d_->transforms.back());
        d_->transforms.pop_back();
    }
}

} // namespace SecondSon::Launcher
