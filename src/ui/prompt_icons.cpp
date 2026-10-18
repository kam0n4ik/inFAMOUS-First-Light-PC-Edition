// SecondSonPC — the game's button icons for the device in use (see prompt_icons.h).
#include "ui/prompt_icons.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cctype>
#include <cstring>

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <d2d1.h>
#include <dwrite_3.h>

namespace SecondSon::Ui::PromptIcons {

namespace {

template <typename T>
void Release(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

// ------------------------------------------------------------------------------------------------ the atlas's layout

constexpr u32 kTilesPerRow = kAtlasSide / 8;

/// Element `i` (0..63) of an 8x8 micro tile: x in the even bits, y in the odd ones.
constexpr std::array<std::array<u8, 2>, 64> MakeMorton() {
    std::array<std::array<u8, 2>, 64> t{};
    for (u32 i = 0; i < 64; ++i) {
        u32 x = 0, y = 0;
        for (u32 b = 0; b < 3; ++b) {
            x |= ((i >> (2 * b)) & 1) << b;
            y |= ((i >> (2 * b + 1)) & 1) << b;
        }
        t[i] = {static_cast<u8>(x), static_cast<u8>(y)};
    }
    return t;
}
constexpr auto kMorton = MakeMorton();

template <bool kToTiled>
void Swizzle(const u8* src, u8* dst) {
    u64 t = 0;
    for (u32 ty = 0; ty < kTilesPerRow; ++ty) {
        for (u32 tx = 0; tx < kTilesPerRow; ++tx) {
            for (u32 i = 0; i < 64; ++i, t += 4) {
                const u32 x = tx * 8 + kMorton[i][0];
                const u32 row = kAtlasSide - 1 - (ty * 8 + kMorton[i][1]); // stored bottom-up
                const u64 l = (u64{row} * kAtlasSide + x) * 4;
                if constexpr (kToTiled) {
                    std::memcpy(dst + t, src + l, 4);
                } else {
                    std::memcpy(dst + l, src + t, 4);
                }
            }
        }
    }
}

// ------------------------------------------------------------------------------------------------ finding it

// The atlas's texture object: its id, and its T# words 1..4 (8_8_8_8 UNORM, 512x512, tiling index 13, pitch 512)
constexpr u8 kTextureId[8] = {0x29, 0xc1, 0x1c, 0x0c, 0xac, 0x5b, 0x95, 0xed};
constexpr u32 kDescriptor[4] = {0x00a00000, 0x007fc1ff, 0x94d00fac, 0x003fe000};

u32 U32(std::string_view f, u64 at) {
    u32 v = 0;
    std::memcpy(&v, f.data() + at, 4);
    return v;
}
u16 U16(std::string_view f, u64 at) {
    u16 v = 0;
    std::memcpy(&v, f.data() + at, 2);
    return v;
}

// ------------------------------------------------------------------------------------------------ what a cell shows

enum class Btn : u8 {
    Circle, Square, Triangle, Cross, L1, L2, R1, R2, Options, TouchPad, LeftStick, L3, RightStick, R3,
    Dpad, DpadRight, DpadUp, DpadLeft, DpadDown, None
};

Btn ButtonOf(u16 code) {
    switch (code) {
    case 0xF080: return Btn::Circle;
    case 0xF081: return Btn::Square;
    case 0xF082: return Btn::Triangle;
    case 0xF083: return Btn::Cross;
    case 0xF084: return Btn::L1;
    case 0xF085: return Btn::L2;
    case 0xF086: return Btn::R1;
    case 0xF087: return Btn::R2;
    case 0xF088: return Btn::Options;
    case 0xF089: return Btn::TouchPad;
    case 0xF08A: return Btn::LeftStick;
    case 0xF08B: return Btn::L3;
    case 0xF08C: return Btn::RightStick;
    case 0xF08D: return Btn::R3;
    case 0xF090: return Btn::Dpad;
    case 0xF091: return Btn::DpadRight;
    case 0xF092: return Btn::DpadUp;
    case 0xF093: return Btn::DpadLeft;
    case 0xF094: return Btn::DpadDown;
    default: return Btn::None;
    }
}

/// The keyboard/mouse action of a single DualShock 4 button.
std::optional<Input::Action> ActionOf(Btn b) {
    using A = Input::Action;
    switch (b) {
    case Btn::Circle: return A::Circle;
    case Btn::Square: return A::Square;
    case Btn::Triangle: return A::Triangle;
    case Btn::Cross: return A::Cross;
    case Btn::L1: return A::L1;
    case Btn::L2: return A::L2;
    case Btn::R1: return A::R1;
    case Btn::R2: return A::R2;
    case Btn::Options: return A::Options;
    case Btn::TouchPad: return A::TouchPad;
    case Btn::L3: return A::L3;
    case Btn::R3: return A::R3;
    case Btn::DpadRight: return A::DpadRight;
    case Btn::DpadUp: return A::DpadUp;
    case Btn::DpadLeft: return A::DpadLeft;
    case Btn::DpadDown: return A::DpadDown;
    default: return std::nullopt;
    }
}

enum class Arrow : u8 { None, Up, Down, Left, Right };

/// How a key is labelled: short (a square key), long (a wide key) or an arrow.
struct KeyLabel {
    std::string text;
    Arrow arrow = Arrow::None;
    bool wide = false;
};

KeyLabel LabelOf(u16 scancode) {
    switch (static_cast<SDL_Scancode>(scancode)) {
    case SDL_SCANCODE_UP: return {"", Arrow::Up};
    case SDL_SCANCODE_DOWN: return {"", Arrow::Down};
    case SDL_SCANCODE_LEFT: return {"", Arrow::Left};
    case SDL_SCANCODE_RIGHT: return {"", Arrow::Right};
    case SDL_SCANCODE_SPACE: return {"SPACE", Arrow::None, true};
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return {"SHIFT", Arrow::None, true};
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL: return {"CTRL", Arrow::None, true};
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: return {"ALT", Arrow::None, true};
    case SDL_SCANCODE_LGUI:
    case SDL_SCANCODE_RGUI: return {"WIN", Arrow::None, true};
    case SDL_SCANCODE_TAB: return {"TAB", Arrow::None, true};
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return {"ENTER", Arrow::None, true};
    case SDL_SCANCODE_ESCAPE: return {"ESC", Arrow::None, true};
    case SDL_SCANCODE_BACKSPACE: return {"BKSP", Arrow::None, true};
    case SDL_SCANCODE_CAPSLOCK: return {"CAPS", Arrow::None, true};
    case SDL_SCANCODE_DELETE: return {"DEL", Arrow::None, true};
    case SDL_SCANCODE_INSERT: return {"INS", Arrow::None, true};
    case SDL_SCANCODE_HOME: return {"HOME", Arrow::None, true};
    case SDL_SCANCODE_END: return {"END", Arrow::None, true};
    case SDL_SCANCODE_PAGEUP: return {"PGUP", Arrow::None, true};
    case SDL_SCANCODE_PAGEDOWN: return {"PGDN", Arrow::None, true};
    default: break;
    }
    const char* n = SDL_GetScancodeName(static_cast<SDL_Scancode>(scancode));
    std::string s = n != nullptr ? n : "";
    if (s.starts_with("Keypad ")) {
        s = "NUM" + s.substr(7);
    }
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (s.empty()) {
        s = "?";
    }
    const bool wide = s.size() > 2;
    return {s, Arrow::None, wide};
}

// ------------------------------------------------------------------------------------------------ drawing

/// Direct2D, DirectWrite and the bold condensed face, for one BuildAtlas.
struct Gfx {
    bool com = false;
    ID2D1Factory* d2d = nullptr;
    IWICImagingFactory* wic = nullptr;
    IDWriteFactory* dwrite = nullptr;
    IDWriteFontFace* face = nullptr;
    DWRITE_FONT_METRICS metrics{};
    std::string error;

    Gfx() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        com = SUCCEEDED(hr); // (RPC_E_CHANGED_MODE: the thread is an STA already - fine, nothing to undo)
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d)) ||
            FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_ISOLATED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(&dwrite)))) {
            error = "Direct2D / WIC / DirectWrite unavailable";
            return;
        }
        LoadFace();
        if (face == nullptr) {
            error = "no font for the icons (fonts\\RobotoCondensed[wght].ttf, Bahnschrift, Arial)";
        }
    }
    ~Gfx() {
        Release(face);
        Release(dwrite);
        Release(wic);
        Release(d2d);
        if (com) {
            CoUninitialize();
        }
    }
    bool Ok() const {
        return error.empty();
    }

    static IDWriteFontFace* Match(IDWriteFontCollection* c, const wchar_t* family, DWRITE_FONT_STRETCH stretch) {
        UINT32 index = 0;
        BOOL exists = FALSE;
        IDWriteFontFamily* fam = nullptr;
        IDWriteFont* font = nullptr;
        IDWriteFontFace* f = nullptr;
        if (c != nullptr && SUCCEEDED(c->FindFamilyName(family, &index, &exists)) && exists &&
            SUCCEEDED(c->GetFontFamily(index, &fam)) &&
            SUCCEEDED(fam->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_BOLD, stretch, DWRITE_FONT_STYLE_NORMAL, &font))) {
            font->CreateFontFace(&f);
        }
        Release(font);
        Release(fam);
        return f;
    }

    /// Roboto Condensed Bold (the face of the game's menus, <exe dir>\fonts), else Bahnschrift Condensed, else Arial.
    void LoadFace() {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe);
        dir = dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"fonts\\RobotoCondensed[wght].ttf";
        IDWriteFactory5* f5 = nullptr;
        IDWriteFontSetBuilder1* builder = nullptr;
        IDWriteFontFile* file = nullptr;
        IDWriteFontSet* set = nullptr;
        IDWriteFontCollection1* own = nullptr;
        if (SUCCEEDED(dwrite->QueryInterface(__uuidof(IDWriteFactory5), reinterpret_cast<void**>(&f5))) &&
            SUCCEEDED(f5->CreateFontSetBuilder(&builder)) &&
            SUCCEEDED(f5->CreateFontFileReference(dir.c_str(), nullptr, &file)) &&
            SUCCEEDED(builder->AddFontFile(file)) && SUCCEEDED(builder->CreateFontSet(&set)) &&
            SUCCEEDED(f5->CreateFontCollectionFromFontSet(set, &own))) {
            face = Match(own, L"Roboto Condensed", DWRITE_FONT_STRETCH_NORMAL);
        }
        Release(own);
        Release(set);
        Release(file);
        Release(builder);
        Release(f5);
        if (face == nullptr) {
            IDWriteFontCollection* sys = nullptr;
            if (SUCCEEDED(dwrite->GetSystemFontCollection(&sys, FALSE))) {
                face = Match(sys, L"Bahnschrift", DWRITE_FONT_STRETCH_CONDENSED);
                if (face == nullptr) {
                    face = Match(sys, L"Arial", DWRITE_FONT_STRETCH_NORMAL);
                }
            }
            Release(sys);
        }
        if (face != nullptr) {
            face->GetMetrics(&metrics);
        }
    }
};

/// One cell being drawn: three coverage layers, composed as max(fill * (1 - cut), top) - white shapes with the
/// symbol cut out (the game's style), and parts drawn again over a cut (a mouse wheel inside its gap).
class Canvas {
public:
    enum Layer : u8 { Fill, Cut, Top };

    Canvas(Gfx& g, u32 w, u32 h) : g_{g}, w_{w}, h_{h} {
        const auto props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        for (int i = 0; i < 3; ++i) {
            if (FAILED(g.wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand, &bmp_[i])) ||
                FAILED(g.d2d->CreateWicBitmapRenderTarget(bmp_[i], props, &rt_[i])) ||
                FAILED(rt_[i]->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush_[i]))) {
                ok_ = false;
                return;
            }
            rt_[i]->BeginDraw();
            rt_[i]->Clear(D2D1::ColorF(0, 0, 0, 0));
        }
    }
    ~Canvas() {
        for (int i = 0; i < 3; ++i) {
            Release(brush_[i]);
            Release(rt_[i]);
            Release(bmp_[i]);
        }
    }

    /// Ends drawing; the cell's alpha (w * h).
    bool Finish(std::vector<u8>& alpha) {
        if (!ok_) {
            return false;
        }
        std::array<std::vector<u8>, 3> cov;
        for (int i = 0; i < 3; ++i) {
            if (FAILED(rt_[i]->EndDraw())) {
                return false;
            }
            cov[i].resize(size_t{w_} * h_);
            std::vector<u8> px(size_t{w_} * h_ * 4);
            if (FAILED(bmp_[i]->CopyPixels(nullptr, w_ * 4, static_cast<UINT>(px.size()), px.data()))) {
                return false;
            }
            for (size_t p = 0; p < cov[i].size(); ++p) {
                cov[i][p] = px[p * 4 + 3];
            }
        }
        alpha.resize(size_t{w_} * h_);
        for (size_t p = 0; p < alpha.size(); ++p) {
            const u32 a = (u32{cov[Fill][p]} * (255u - cov[Cut][p]) + 127) / 255;
            alpha[p] = static_cast<u8>(std::max<u32>(a, cov[Top][p]));
        }
        return true;
    }

    void Opacity(float o) {
        for (auto* b : brush_) {
            if (b != nullptr) {
                b->SetOpacity(o);
            }
        }
    }

    void RoundRect(Layer l, float x, float y, float w, float h, float r) {
        rt_[l]->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r), brush_[l]);
    }
    void StrokeRoundRect(Layer l, float x, float y, float w, float h, float r, float width) {
        const float i = width / 2; // the stroke inside the box
        rt_[l]->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + i, y + i, x + w - i, y + h - i), r - i, r - i),
                                     brush_[l], width);
    }
    void Circle(Layer l, float cx, float cy, float r) {
        rt_[l]->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_[l]);
    }
    void StrokeCircle(Layer l, float cx, float cy, float r, float width) {
        rt_[l]->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush_[l], width);
    }
    void Line(Layer l, float x0, float y0, float x1, float y1, float width) {
        rt_[l]->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush_[l], width);
    }
    void Rect(Layer l, float x, float y, float w, float h) {
        rt_[l]->FillRectangle(D2D1::RectF(x, y, x + w, y + h), brush_[l]);
    }
    void StrokeRect(Layer l, float x, float y, float w, float h, float width) {
        const float i = width / 2;
        rt_[l]->DrawRectangle(D2D1::RectF(x + i, y + i, x + w - i, y + h - i), brush_[l], width);
    }

    /// An arrow head (filled triangle) pointing `dir`, centred at (cx, cy), `s` from its centre to its tip.
    void Triangle(Layer l, Arrow dir, float cx, float cy, float s) {
        float p[6];
        const float b = s * 0.95f; // half the base
        switch (dir) {
        case Arrow::Up: p[0] = cx, p[1] = cy - s, p[2] = cx + b, p[3] = cy + s * 0.6f, p[4] = cx - b, p[5] = cy + s * 0.6f; break;
        case Arrow::Down: p[0] = cx, p[1] = cy + s, p[2] = cx - b, p[3] = cy - s * 0.6f, p[4] = cx + b, p[5] = cy - s * 0.6f; break;
        case Arrow::Left: p[0] = cx - s, p[1] = cy, p[2] = cx + s * 0.6f, p[3] = cy - b, p[4] = cx + s * 0.6f, p[5] = cy + b; break;
        default: p[0] = cx + s, p[1] = cy, p[2] = cx - s * 0.6f, p[3] = cy + b, p[4] = cx - s * 0.6f, p[5] = cy - b; break;
        }
        ID2D1PathGeometry* geo = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(g_.d2d->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
            sink->BeginFigure(D2D1::Point2F(p[0], p[1]), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(p[2], p[3]));
            sink->AddLine(D2D1::Point2F(p[4], p[5]));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            rt_[l]->FillGeometry(geo, brush_[l]);
        }
        Release(sink);
        Release(geo);
    }

    /// A box with its own radius per corner (top-left, top-right, bottom-right, bottom-left).
    void Shape(Layer l, float x, float y, float w, float h, float tl, float tr, float br, float bl) {
        ID2D1PathGeometry* geo = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(g_.d2d->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink))) {
            const auto arc = [&](float ex, float ey, float r) {
                sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(ex, ey), D2D1::SizeF(r, r), 0,
                                              D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            };
            sink->BeginFigure(D2D1::Point2F(x + tl, y), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(D2D1::Point2F(x + w - tr, y));
            arc(x + w, y + tr, tr);
            sink->AddLine(D2D1::Point2F(x + w, y + h - br));
            arc(x + w - br, y + h, br);
            sink->AddLine(D2D1::Point2F(x + bl, y + h));
            arc(x, y + h - bl, bl);
            sink->AddLine(D2D1::Point2F(x, y + tl));
            arc(x + tl, y, tl);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            rt_[l]->FillGeometry(geo, brush_[l]);
        }
        Release(sink);
        Release(geo);
    }

    /// Fills the part of a rounded box inside a rectangle (a mouse button inside the mouse's body).
    void RoundRectPart(Layer l, float x, float y, float w, float h, float r, float px, float py, float pw, float ph) {
        ID2D1RoundedRectangleGeometry* body = nullptr;
        ID2D1RectangleGeometry* part = nullptr;
        ID2D1PathGeometry* out = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(g_.d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r),
                                                             &body)) &&
            SUCCEEDED(g_.d2d->CreateRectangleGeometry(D2D1::RectF(px, py, px + pw, py + ph), &part)) &&
            SUCCEEDED(g_.d2d->CreatePathGeometry(&out)) && SUCCEEDED(out->Open(&sink)) &&
            SUCCEEDED(body->CombineWithGeometry(part, D2D1_COMBINE_MODE_INTERSECT, nullptr, sink)) &&
            SUCCEEDED(sink->Close())) {
            rt_[l]->FillGeometry(out, brush_[l]);
        }
        Release(sink);
        Release(out);
        Release(part);
        Release(body);
    }

    /// Capital letters/digits: their cap height `cap` (shrunk to `max_w` wide), centred on (cx, cy) by their ink.
    void Text(Layer l, std::string_view text, float cx, float cy, float cap, float max_w) {
        if (g_.face == nullptr || text.empty() || g_.metrics.capHeight == 0) {
            return;
        }
        std::vector<UINT32> cps(text.begin(), text.end());
        std::vector<UINT16> idx(cps.size());
        std::vector<DWRITE_GLYPH_METRICS> gm(cps.size());
        if (FAILED(g_.face->GetGlyphIndices(cps.data(), static_cast<UINT32>(cps.size()), idx.data())) ||
            FAILED(g_.face->GetDesignGlyphMetrics(idx.data(), static_cast<UINT32>(idx.size()), gm.data(), FALSE))) {
            return;
        }
        const float upem = g_.metrics.designUnitsPerEm;
        float em = cap * upem / g_.metrics.capHeight;
        ID2D1PathGeometry* geo = Outline(idx, gm, em);
        if (geo == nullptr) {
            return;
        }
        D2D1_RECT_F b{};
        geo->GetBounds(nullptr, &b);
        if (b.right - b.left > max_w && max_w > 0) { // too wide: smaller
            em *= max_w / (b.right - b.left);
            cap *= max_w / (b.right - b.left);
            Release(geo);
            geo = Outline(idx, gm, em);
            if (geo == nullptr) {
                return;
            }
            geo->GetBounds(nullptr, &b);
        }
        // the baseline cap/2 below the centre; horizontally the ink's middle on cx
        rt_[l]->SetTransform(D2D1::Matrix3x2F::Translation(cx - (b.left + b.right) / 2, cy + cap / 2));
        rt_[l]->FillGeometry(geo, brush_[l]);
        rt_[l]->SetTransform(D2D1::Matrix3x2F::Identity());
        Release(geo);
    }

    u32 W() const { return w_; }
    u32 H() const { return h_; }

private:
    ID2D1PathGeometry* Outline(const std::vector<UINT16>& idx, const std::vector<DWRITE_GLYPH_METRICS>& gm, float em) {
        std::vector<FLOAT> adv(idx.size());
        for (size_t i = 0; i < idx.size(); ++i) {
            adv[i] = gm[i].advanceWidth * em / g_.metrics.designUnitsPerEm;
        }
        ID2D1PathGeometry* geo = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (SUCCEEDED(g_.d2d->CreatePathGeometry(&geo)) && SUCCEEDED(geo->Open(&sink)) &&
            SUCCEEDED(g_.face->GetGlyphRunOutline(em, idx.data(), adv.data(), nullptr, static_cast<UINT32>(idx.size()),
                                                  FALSE, FALSE, sink)) &&
            SUCCEEDED(sink->Close())) {
            Release(sink);
            return geo;
        }
        Release(sink);
        Release(geo);
        return nullptr;
    }

    Gfx& g_;
    u32 w_, h_;
    bool ok_ = true;
    IWICBitmap* bmp_[3] = {};
    ID2D1RenderTarget* rt_[3] = {};
    ID2D1SolidColorBrush* brush_[3] = {};
};

// ---- keyboard and mouse

/// A key: a white key cap with its label cut out - a square cap for one or two characters, a wide one for names.
void DrawKey(Canvas& c, const KeyLabel& k) {
    const float cx = c.W() / 2.0f, cy = c.H() / 2.0f;
    if (k.wide) {
        const float w = c.W() - 4.0f, h = 32.0f;
        c.RoundRect(Canvas::Fill, cx - w / 2, cy - h / 2, w, h, 4.0f);
        c.Text(Canvas::Cut, k.text, cx, cy, 17.0f, w - 6.0f);
        return;
    }
    const float s = 34.0f; // as the game's L1/R1 plates
    c.RoundRect(Canvas::Fill, cx - s / 2, cy - s / 2, s, s, 3.0f);
    if (k.arrow != Arrow::None) {
        c.Triangle(Canvas::Cut, k.arrow, cx, cy + (k.arrow == Arrow::Up ? 1.5f : k.arrow == Arrow::Down ? -1.5f : 0.0f),
                   9.0f);
    } else {
        c.Text(Canvas::Cut, k.text, cx, cy, 22.0f, s - 8.0f);
    }
}

// The mouse: a body outline, its buttons split by lines; the button meant filled.
struct MouseBody {
    float x, y, w = 24.0f, h = 38.0f, r = 12.0f;
    float split() const { return y + 15.0f; }
};

void DrawMouseOutline(Canvas& c, const MouseBody& m) {
    c.StrokeRoundRect(Canvas::Fill, m.x, m.y, m.w, m.h, m.r, 3.0f);
    c.Line(Canvas::Fill, m.x + 1.5f, m.split(), m.x + m.w - 1.5f, m.split(), 2.5f);
    c.Line(Canvas::Fill, m.x + m.w / 2, m.y + 1.5f, m.x + m.w / 2, m.split(), 2.5f);
}

/// The wheel: drawn over a gap cut in the body's lines.
void DrawWheel(Canvas& c, const MouseBody& m, bool filled) {
    const float cx = m.x + m.w / 2;
    c.RoundRect(Canvas::Cut, cx - 4.5f, m.y + 4.0f, 9.0f, 14.0f, 4.5f);
    if (filled) {
        c.RoundRect(Canvas::Top, cx - 2.5f, m.y + 6.0f, 5.0f, 10.0f, 2.5f);
    } else {
        c.StrokeRoundRect(Canvas::Top, cx - 2.5f, m.y + 6.0f, 5.0f, 10.0f, 2.5f, 1.5f);
    }
}

void DrawMouseInput(Canvas& c, Input::InputCode in) {
    const float cy = c.H() / 2.0f;
    MouseBody m{.x = 0, .y = cy - 19.0f};
    if (in.kind == Input::InputCode::Kind::MouseWheel) {
        m.x = c.W() / 2.0f - 6.0f - m.w / 2; // room for the arrow on the right
        DrawMouseOutline(c, m);
        DrawWheel(c, m, true);
        const Arrow dir = in.code == 0 ? Arrow::Up : in.code == 1 ? Arrow::Down : in.code == 2 ? Arrow::Left : Arrow::Right;
        c.Triangle(Canvas::Fill, dir, m.x + m.w + 9.0f, cy, 6.0f);
        return;
    }
    const bool side = in.code == SDL_BUTTON_X1 || in.code == SDL_BUTTON_X2;
    m.x = c.W() / 2.0f - m.w / 2 + (side ? 4.0f : 0.0f);
    DrawMouseOutline(c, m);
    switch (in.code) {
    case SDL_BUTTON_LEFT:
        c.RoundRectPart(Canvas::Fill, m.x, m.y, m.w, m.h, m.r, m.x, m.y, m.w / 2, m.split() - m.y);
        break;
    case SDL_BUTTON_RIGHT:
        c.RoundRectPart(Canvas::Fill, m.x, m.y, m.w, m.h, m.r, m.x + m.w / 2, m.y, m.w / 2, m.split() - m.y);
        break;
    case SDL_BUTTON_MIDDLE:
        DrawWheel(c, m, true);
        break;
    default: { // side buttons: X2 (forward) above X1 (back) on the left side; the one meant solid
        const bool x2 = in.code == SDL_BUTTON_X2;
        c.Opacity(x2 ? 1.0f : 0.45f);
        c.RoundRect(Canvas::Fill, m.x - 7.0f, cy - 6.0f, 7.0f, 7.0f, 2.0f);
        c.Opacity(x2 ? 0.45f : 1.0f);
        c.RoundRect(Canvas::Fill, m.x - 7.0f, cy + 3.0f, 7.0f, 7.0f, 2.0f);
        c.Opacity(1.0f);
        break;
    }
    }
}

void DrawInput(Canvas& c, Input::InputCode in) {
    if (in.kind == Input::InputCode::Kind::Key) {
        DrawKey(c, LabelOf(in.code));
    } else {
        DrawMouseInput(c, in);
    }
}

/// A short label of any input for the small keys of a cluster.
KeyLabel SmallLabel(Input::InputCode in) {
    if (in.kind == Input::InputCode::Kind::Key) {
        return LabelOf(in.code);
    }
    if (in.kind == Input::InputCode::Kind::MouseWheel) {
        return {in.code == 0 ? "WU" : in.code == 1 ? "WD" : in.code == 2 ? "WL" : "WR"};
    }
    switch (in.code) {
    case SDL_BUTTON_LEFT: return {"LMB"};
    case SDL_BUTTON_RIGHT: return {"RMB"};
    case SDL_BUTTON_MIDDLE: return {"MMB"};
    default: return {"M" + std::to_string(in.code)};
    }
}

/// Four keys as on a keyboard: one above three (up; left, down, right) - WASD, the arrow keys.
void DrawCluster(Canvas& c, const std::array<Input::InputCode, 4>& keys) {
    constexpr float gap = 2.0f;
    const float k = std::floor(std::min((c.W() - 2.0f - 2 * gap) / 3.0f, (c.H() - 6.0f - gap) / 2.0f));
    const float x0 = (c.W() - (3 * k + 2 * gap)) / 2.0f, y0 = (c.H() - (2 * k + gap)) / 2.0f;
    const float pos[4][2] = {{x0 + k + gap, y0}, {x0, y0 + k + gap}, {x0 + k + gap, y0 + k + gap},
                             {x0 + 2 * (k + gap), y0 + k + gap}};
    for (int i = 0; i < 4; ++i) {
        const float x = pos[i][0], y = pos[i][1];
        c.RoundRect(Canvas::Fill, x, y, k, k, 2.5f);
        const KeyLabel l = SmallLabel(keys[i]);
        if (l.arrow != Arrow::None) {
            c.Triangle(Canvas::Cut, l.arrow, x + k / 2, y + k / 2, k * 0.27f);
        } else {
            c.Text(Canvas::Cut, l.text, x + k / 2, y + k / 2, std::round(k * 0.62f), k - 4.0f);
        }
    }
}

/// Looking around with the mouse: the mouse, arrows to its sides.
void DrawMouseLook(Canvas& c) {
    const float cx = c.W() / 2.0f, cy = c.H() / 2.0f;
    const MouseBody m{.x = cx - 12.0f, .y = cy - 19.0f};
    DrawMouseOutline(c, m);
    c.Triangle(Canvas::Fill, Arrow::Left, cx - 20.0f, cy + 2.0f, 5.5f);
    c.Triangle(Canvas::Fill, Arrow::Right, cx + 20.0f, cy + 2.0f, 5.5f);
}

bool DrawKeyboard(Canvas& c, Btn b, const Input::Settings& layout) {
    const auto first = [&](Input::Action a) -> std::optional<Input::InputCode> {
        const auto& v = layout.bindings[static_cast<size_t>(a)];
        if (v.empty()) {
            return std::nullopt;
        }
        return v.front();
    };
    const auto cluster = [&](Input::Action u, Input::Action l, Input::Action d, Input::Action r) {
        const auto a = first(u), bb = first(l), cc = first(d), dd = first(r);
        if (!a || !bb || !cc || !dd) {
            return false; // an unbound direction: the game's own icon
        }
        DrawCluster(c, {*a, *bb, *cc, *dd});
        return true;
    };
    using A = Input::Action;
    switch (b) {
    case Btn::LeftStick:
        return cluster(A::MoveForward, A::MoveLeft, A::MoveBack, A::MoveRight);
    case Btn::Dpad:
        return cluster(A::DpadUp, A::DpadLeft, A::DpadDown, A::DpadRight);
    case Btn::RightStick:
        DrawMouseLook(c);
        return true;
    default:
        break;
    }
    const auto action = ActionOf(b);
    if (!action) {
        return false;
    }
    const auto in = first(*action);
    if (!in) {
        return false; // unbound: the DualShock 4 icon stays
    }
    DrawInput(c, *in);
    return true;
}

// ---- Xbox

bool DrawXbox(Canvas& c, Btn b) {
    const float cx = c.W() / 2.0f, cy = c.H() / 2.0f;
    const auto face = [&](std::string_view letter) {
        c.Circle(Canvas::Fill, cx, cy, 19.0f); // as the game's face buttons
        c.Text(Canvas::Cut, letter, cx, cy, 20.0f, 30.0f);
    };
    const auto bumper = [&](std::string_view t, bool left) {
        const float w = 44.0f, h = 24.0f, x = cx - w / 2, y = cy - h / 2;
        c.Shape(Canvas::Fill, x, y, w, h, left ? 11.0f : 3.0f, left ? 3.0f : 11.0f, 3.0f, 3.0f);
        c.Text(Canvas::Cut, t, cx, cy, 17.0f, w - 10.0f);
    };
    const auto trigger = [&](std::string_view t) {
        const float w = 32.0f, h = 36.0f, x = cx - w / 2, y = cy - h / 2;
        c.Shape(Canvas::Fill, x, y, w, h, 13.0f, 13.0f, 3.0f, 3.0f);
        c.Text(Canvas::Cut, t, cx, cy + 3.0f, 18.0f, w - 6.0f);
    };
    const auto stick = [&](std::string_view letter) {
        c.Circle(Canvas::Fill, cx, cy, 15.0f);
        c.Text(Canvas::Cut, letter, cx, cy, 15.0f, 20.0f);
        c.Triangle(Canvas::Fill, Arrow::Left, cx - 22.0f, cy, 4.5f);
        c.Triangle(Canvas::Fill, Arrow::Right, cx + 22.0f, cy, 4.5f);
        c.Triangle(Canvas::Fill, Arrow::Up, cx, cy - 20.5f, 4.0f);
        c.Triangle(Canvas::Fill, Arrow::Down, cx, cy + 20.5f, 4.0f);
    };
    const auto click = [&](std::string_view t) { // a stick pressed: its cap, a ring inside
        c.Circle(Canvas::Fill, cx, cy, 19.0f);
        c.StrokeCircle(Canvas::Cut, cx, cy, 16.0f, 1.6f);
        c.Text(Canvas::Cut, t, cx, cy, 15.0f, 24.0f);
    };
    switch (b) {
    case Btn::Cross: face("A"); return true;
    case Btn::Circle: face("B"); return true;
    case Btn::Square: face("X"); return true;
    case Btn::Triangle: face("Y"); return true;
    case Btn::L1: bumper("LB", true); return true;
    case Btn::R1: bumper("RB", false); return true;
    case Btn::L2: trigger("LT"); return true;
    case Btn::R2: trigger("RT"); return true;
    case Btn::LeftStick: stick("L"); return true;
    case Btn::RightStick: stick("R"); return true;
    case Btn::L3: click("LS"); return true;
    case Btn::R3: click("RS"); return true;
    case Btn::Options: // Menu: three lines
        c.Circle(Canvas::Fill, cx, cy, 18.0f);
        for (int i = -1; i <= 1; ++i) {
            c.Rect(Canvas::Cut, cx - 8.0f, cy + i * 5.5f - 1.4f, 16.0f, 2.8f);
        }
        return true;
    case Btn::TouchPad: // View (the bridge's touch pad on Xbox controllers): two overlapping windows
        c.Circle(Canvas::Fill, cx, cy, 18.0f);
        c.StrokeRect(Canvas::Cut, cx - 8.5f, cy - 7.5f, 11.0f, 9.0f, 2.2f);
        c.Rect(Canvas::Cut, cx - 2.5f, cy - 1.5f, 11.0f, 9.0f);
        return true;
    default:
        return false; // the d-pad: the game's cross is the Xbox one's too
    }
}

} // namespace

void Detile(const u8* tiled, u8* rgba) {
    Swizzle<false>(tiled, rgba);
}

void Tile(const u8* rgba, u8* tiled) {
    Swizzle<true>(rgba, tiled);
}

u64 Fnv1a(const u8* data, u64 n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < n; ++i) {
        h = (h ^ data[i]) * 0x100000001b3ull;
    }
    return h;
}

std::optional<AtlasInfo> Locate(std::string_view f, std::string* why) {
    const auto fail = [&](std::string s) -> std::optional<AtlasInfo> {
        if (why != nullptr) {
            *why = std::move(s);
        }
        return std::nullopt;
    };
    if (f.size() < 0x200 || f.substr(0, 4) != "KCAP") {
        return fail("not a KCAP package");
    }
    const u64 header = U32(f, 0x28);
    AtlasInfo info;
    const std::string_view id(reinterpret_cast<const char*>(kTextureId), sizeof(kTextureId));
    bool found = false;
    for (size_t p = f.find(id); p != std::string_view::npos && !found; p = f.find(id, p + 1)) {
        if (p < 0x20) {
            continue;
        }
        const u64 t = p - 0x20; // the T# before the id
        bool same = true;
        for (int w = 0; w < 4; ++w) {
            same = same && U32(f, t + 4 + 4 * w) == kDescriptor[w];
        }
        if (!same) {
            continue;
        }
        const u64 data = header + U32(f, t);
        if (data + kAtlasBytes > f.size()) {
            return fail("the atlas's data lies outside the file");
        }
        if (Fnv1a(reinterpret_cast<const u8*>(f.data()) + data, kAtlasBytes) != kOriginalAtlasFnv) {
            return fail("the atlas is not the one of version 01.07 (another game version or a modified file)");
        }
        info.data_offset = data;
        found = true;
    }
    if (!found) {
        return fail("no icon atlas (texture 29c11c0cac5b95ed 512x512 RGBA8, tiling 13)");
    }
    // the glyph table: records {u16 code, x, y, w, h, s16 ox, oy, u16 advance, 16 bytes} from U+F080 (50x50) on
    for (size_t q = f.find("\x80\xF0"); q != std::string_view::npos; q = f.find("\x80\xF0", q + 1)) {
        if (q + 64 > f.size() || U16(f, q + 6) != 50 || U16(f, q + 8) != 50 || U16(f, q + 32) != 0xF081) {
            continue;
        }
        u16 last = 0;
        for (u64 r = q; r + 32 <= f.size(); r += 32) {
            const Cell c{U16(f, r), U16(f, r + 2), U16(f, r + 4), U16(f, r + 6), U16(f, r + 8)};
            if (c.code < 0xF000 || c.code >= 0xF200 || c.code <= last || c.w == 0 || c.h == 0 ||
                c.x + c.w > kAtlasSide || c.y + c.h > kAtlasSide) {
                break;
            }
            info.cells.push_back(c);
            last = c.code;
        }
        info.glyph_offset = q;
        break;
    }
    const auto has = [&](u16 code) {
        return std::ranges::any_of(info.cells, [&](const Cell& c) { return c.code == code; });
    };
    for (u16 code : {0xF080, 0xF083, 0xF084, 0xF087, 0xF08A, 0xF08D, 0xF090, 0xF094}) {
        if (!has(code)) {
            return fail("the icon font's glyph table was not found");
        }
    }
    return info;
}

std::string_view CodeName(u16 code) {
    constexpr std::string_view kNames[] = {"circle", "square", "triangle", "cross", "L1", "L2", "R1", "R2",
                                           "options", "touch pad", "left stick", "L3", "right stick", "R3",
                                           "d-pad", "d-pad right", "d-pad up", "d-pad left", "d-pad down", "-"};
    return kNames[static_cast<size_t>(ButtonOf(code))];
}

namespace {

CellArt DrawWith(Gfx& g, const Cell& cell, Input::PromptStyle style, const Input::Settings& layout) {
    CellArt art;
    const Btn b = ButtonOf(cell.code);
    if (b == Btn::None || style == Input::PromptStyle::PlayStation || !g.Ok()) {
        return art;
    }
    Canvas c(g, cell.w, cell.h);
    const bool drawn = style == Input::PromptStyle::Xbox ? DrawXbox(c, b) : DrawKeyboard(c, b, layout);
    std::vector<u8> alpha;
    if (c.Finish(alpha) && drawn) {
        art.drawn = true;
        art.alpha = std::move(alpha);
    }
    return art;
}

} // namespace

std::vector<u8> DrawInputAlpha(Input::InputCode in, u32 side) {
    Gfx g;
    if (!g.Ok() || side == 0) {
        return {};
    }
    // drawn at the font's cell size (the shapes' proportions are made for it), then scaled up: bilinear, and the
    // edge's ramp narrowed back to about a pixel so the larger shape stays crisp
    constexpr u32 kCell = 50;
    Canvas c(g, kCell, kCell);
    DrawInput(c, in);
    std::vector<u8> a;
    if (!c.Finish(a) || a.size() != size_t{kCell} * kCell) {
        return {};
    }
    std::vector<u8> out(size_t{side} * side);
    const float k = static_cast<float>(kCell) / static_cast<float>(side);
    const float ramp = std::min(0.5f, 1.5f * k); // the edge's width in source pixels -> about 1.5 output pixels
    for (u32 y = 0; y < side; ++y) {
        const float fy = std::clamp((y + 0.5f) * k - 0.5f, 0.0f, kCell - 1.0f);
        const u32 y0 = static_cast<u32>(fy), y1 = std::min(y0 + 1, kCell - 1);
        const float ty = fy - y0;
        for (u32 x = 0; x < side; ++x) {
            const float fx = std::clamp((x + 0.5f) * k - 0.5f, 0.0f, kCell - 1.0f);
            const u32 x0 = static_cast<u32>(fx), x1 = std::min(x0 + 1, kCell - 1);
            const float tx = fx - x0;
            const float v = ((a[y0 * kCell + x0] * (1 - tx) + a[y0 * kCell + x1] * tx) * (1 - ty) +
                             (a[y1 * kCell + x0] * (1 - tx) + a[y1 * kCell + x1] * tx) * ty) / 255.0f;
            const float s = std::clamp((v - 0.5f) / (2.0f * ramp) + 0.5f, 0.0f, 1.0f);
            out[size_t{y} * side + x] = static_cast<u8>(std::lround(s * 255.0f));
        }
    }
    return out;
}

CellArt Draw(const Cell& cell, Input::PromptStyle style, const Input::Settings& layout) {
    Gfx g;
    return DrawWith(g, cell, style, layout);
}

std::vector<u8> BuildAtlas(const u8* original_tiled, const std::vector<Cell>& cells, Input::PromptStyle style,
                           const Input::Settings& layout, u32* drawn, std::string* error) {
    if (drawn != nullptr) {
        *drawn = 0;
    }
    std::vector<u8> out(original_tiled, original_tiled + kAtlasBytes);
    if (style == Input::PromptStyle::PlayStation) {
        return out; // the game's own icons
    }
    Gfx g;
    if (!g.Ok()) {
        if (error != nullptr) {
            *error = g.error;
        }
        return {};
    }
    std::vector<u8> rgba(kAtlasBytes);
    Detile(original_tiled, rgba.data());
    for (const Cell& cell : cells) {
        const CellArt art = DrawWith(g, cell, style, layout);
        if (!art.drawn) {
            continue;
        }
        for (u32 y = 0; y < cell.h; ++y) {
            for (u32 x = 0; x < cell.w; ++x) {
                u8* p = rgba.data() + ((u64{cell.y} + y) * kAtlasSide + cell.x + x) * 4;
                p[0] = p[1] = p[2] = 255; // white, the shape in alpha (as the game's icons)
                p[3] = art.alpha[size_t{y} * cell.w + x];
            }
        }
        if (drawn != nullptr) {
            ++*drawn;
        }
    }
    Tile(rgba.data(), out.data());
    return out;
}

bool SavePng(const std::wstring& path, const u8* rgba, u32 w, u32 h) {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::vector<u8> bgra(size_t{w} * h * 4);
    for (size_t i = 0; i < size_t{w} * h; ++i) {
        bgra[i * 4 + 0] = rgba[i * 4 + 2];
        bgra[i * 4 + 1] = rgba[i * 4 + 1];
        bgra[i * 4 + 2] = rgba[i * 4 + 0];
        bgra[i * 4 + 3] = rgba[i * 4 + 3];
    }
    IWICImagingFactory* f = nullptr;
    IWICStream* s = nullptr;
    IWICBitmapEncoder* e = nullptr;
    IWICBitmapFrameEncode* fr = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) &&
              SUCCEEDED(f->CreateStream(&s)) && SUCCEEDED(s->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &e)) &&
              SUCCEEDED(e->Initialize(s, WICBitmapEncoderNoCache)) && SUCCEEDED(e->CreateNewFrame(&fr, nullptr)) &&
              SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(w, h));
    if (ok) {
        WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
        ok = SUCCEEDED(fr->SetPixelFormat(&fmt)) &&
             SUCCEEDED(fr->WritePixels(h, w * 4, static_cast<UINT>(bgra.size()), bgra.data())) &&
             SUCCEEDED(fr->Commit()) && SUCCEEDED(e->Commit());
    }
    Release(fr);
    Release(e);
    Release(s);
    Release(f);
    if (SUCCEEDED(co)) {
        CoUninitialize();
    }
    return ok;
}

} // namespace SecondSon::Ui::PromptIcons
