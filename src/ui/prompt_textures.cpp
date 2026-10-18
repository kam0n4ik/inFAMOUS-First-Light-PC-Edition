// SecondSonPC — the game's button pictures drawn again for the keyboard (see prompt_textures.h).
#include "ui/prompt_textures.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>

#include "ui/prompt_icons.h"

namespace SecondSon::Ui::PromptTextures {

namespace {

constexpr Location kLocations[] = {
#include "ui/prompt_textures_table.inc"
};

struct KindInfo {
    u32 w, h;
    u64 fnv;    // FNV-1a 64 of the original top level
    u32 levels; // mip levels (each drawn again)
};

KindInfo Info(PictureKind k) {
    switch (k) {
    case PictureKind::kSideways256: return {256, 256, 0x4a8b828796c1653cull, 9};
    case PictureKind::kSideways128: return {128, 128, 0x8819943153427b7aull, 8};
    case PictureKind::kRattle1024: return {1024, 256, 0x3dfcd14efee62109ull, 11};
    case PictureKind::kRattle512: return {512, 128, 0xb4420a042a9be869ull, 10};
    case PictureKind::kTouchPrint256: return {256, 256, 0xf2e47d05c6d25ba6ull, 9};
    case PictureKind::kDpad256: return {256, 256, 0xd6349daad5d0602eull, 9};
    case PictureKind::kDpadKarma256: return {256, 256, 0x4cd4970d746fdd34ull, 9};
    case PictureKind::kDpad64: return {64, 64, 0x226c536031f7fbddull, 7};
    case PictureKind::kDiscTriangle128: return {128, 128, 0x11b1d5b54a980384ull, 8};
    case PictureKind::kDiscSquare128: return {128, 128, 0x606d90357f561093ull, 8};
    case PictureKind::kDpadRed64: return {64, 64, 0xadc3c2015a999d34ull, 7};
    }
    return {0, 0, 0, 0};
}

// ---- BC3 in the console layout: 4x4 blocks of 16 bytes, 8x8 blocks a micro tile in Morton order, tiles row by row

u32 Morton(u32 x, u32 y) {
    u32 m = 0;
    for (u32 i = 0; i < 3; ++i) {
        m |= ((x >> i) & 1) << (2 * i) | ((y >> i) & 1) << (2 * i + 1);
    }
    return m;
}

/// The block (bx, by)'s index in the tiled data (block rows of `bw` blocks, a multiple of 8).
u64 BlockIndex(u32 bx, u32 by, u32 bw) {
    const u32 tw = (bw + 7) / 8;
    return (u64{by / 8} * tw + bx / 8) * 64 + Morton(bx % 8, by % 8);
}

struct Rgb {
    int r, g, b;
};

Rgb From565(u16 c) {
    return {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
}

u16 To565(int r, int g, int b) {
    return static_cast<u16>(((std::clamp(r, 0, 255) * 31 + 127) / 255) << 11 |
                            ((std::clamp(g, 0, 255) * 63 + 127) / 255) << 5 | ((std::clamp(b, 0, 255) * 31 + 127) / 255));
}

void DecodeBlock(const u8* blk, u8 out[16][4]) {
    const int a0 = blk[0], a1 = blk[1];
    u64 abits = 0;
    for (int k = 0; k < 6; ++k) {
        abits |= u64{blk[2 + k]} << (8 * k);
    }
    int ap[8] = {a0, a1};
    for (int i = 1; i < 7; ++i) {
        ap[i + 1] = a0 > a1 ? ((7 - i) * a0 + i * a1) / 7 : (i < 5 ? ((5 - i) * a0 + i * a1) / 5 : (i == 5 ? 0 : 255));
    }
    u16 c0 = 0, c1 = 0;
    std::memcpy(&c0, blk + 8, 2);
    std::memcpy(&c1, blk + 10, 2);
    u32 cbits = 0;
    std::memcpy(&cbits, blk + 12, 4);
    const Rgb p0 = From565(c0), p1 = From565(c1);
    const Rgb pal[4] = {p0, p1, {(2 * p0.r + p1.r) / 3, (2 * p0.g + p1.g) / 3, (2 * p0.b + p1.b) / 3},
                        {(p0.r + 2 * p1.r) / 3, (p0.g + 2 * p1.g) / 3, (p0.b + 2 * p1.b) / 3}};
    for (int i = 0; i < 16; ++i) {
        const Rgb& c = pal[(cbits >> (2 * i)) & 3];
        out[i][0] = static_cast<u8>(c.r);
        out[i][1] = static_cast<u8>(c.g);
        out[i][2] = static_cast<u8>(c.b);
        out[i][3] = static_cast<u8>(ap[(abits >> (3 * i)) & 7]);
    }
}

void EncodeBlock(const u8 px[16][4], u8* blk) {
    // alpha: the block's extremes, 8 levels between them
    int amin = 255, amax = 0;
    for (int i = 0; i < 16; ++i) {
        amin = std::min<int>(amin, px[i][3]);
        amax = std::max<int>(amax, px[i][3]);
    }
    blk[0] = static_cast<u8>(amax);
    blk[1] = static_cast<u8>(amin);
    u64 abits = 0;
    for (int i = 0; i < 16; ++i) {
        int idx = 0;
        if (amax > amin) {
            const int k = static_cast<int>(std::lround(7.0 * (amax - px[i][3]) / (amax - amin))); // 0: a0 .. 7: a1
            idx = k == 0 ? 0 : k == 7 ? 1 : k + 1;
        }
        abits |= u64(idx) << (3 * i);
    }
    for (int k = 0; k < 6; ++k) {
        blk[2 + k] = static_cast<u8>(abits >> (8 * k));
    }
    // colour: the visible texels (all, when none is) along their principal axis, the line's ends refined by least
    // squares over the 4-colour palette; a transparent texel's colour is free
    int sel[16], n = 0;
    for (int i = 0; i < 16; ++i) {
        if (px[i][3] >= 16) {
            sel[n++] = i;
        }
    }
    if (n == 0) {
        for (int i = 0; i < 16; ++i) {
            sel[n++] = i;
        }
    }
    double mean[3] = {};
    for (int k = 0; k < n; ++k) {
        for (int c = 0; c < 3; ++c) {
            mean[c] += px[sel[k]][c];
        }
    }
    for (double& m : mean) {
        m /= n;
    }
    double cov[3][3] = {};
    for (int k = 0; k < n; ++k) {
        double d[3];
        for (int c = 0; c < 3; ++c) {
            d[c] = px[sel[k]][c] - mean[c];
        }
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                cov[a][b] += d[a] * d[b];
            }
        }
    }
    double axis[3] = {1, 1, 1};
    for (int it = 0; it < 8; ++it) { // power iteration
        double v[3] = {};
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                v[a] += cov[a][b] * axis[b];
            }
        }
        const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (l < 1e-9) {
            break;
        }
        for (int a = 0; a < 3; ++a) {
            axis[a] = v[a] / l;
        }
    }
    double tmin = 1e9, tmax = -1e9;
    for (int k = 0; k < n; ++k) {
        const double t = (px[sel[k]][0] - mean[0]) * axis[0] + (px[sel[k]][1] - mean[1]) * axis[1] +
                         (px[sel[k]][2] - mean[2]) * axis[2];
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }
    double e0[3], e1[3];
    for (int c = 0; c < 3; ++c) {
        e0[c] = mean[c] + axis[c] * tmax;
        e1[c] = mean[c] + axis[c] * tmin;
    }
    static constexpr double kW[4] = {1.0, 0.0, 2.0 / 3.0, 1.0 / 3.0}; // weight of c0 for index 0..3
    u32 cbits = 0;
    u16 c0 = 0, c1 = 0;
    for (int round = 0; round < 3; ++round) {
        c0 = To565(int(std::lround(e0[0])), int(std::lround(e0[1])), int(std::lround(e0[2])));
        c1 = To565(int(std::lround(e1[0])), int(std::lround(e1[1])), int(std::lround(e1[2])));
        const Rgb p0 = From565(c0), p1 = From565(c1);
        const Rgb pal[4] = {p0, p1, {(2 * p0.r + p1.r) / 3, (2 * p0.g + p1.g) / 3, (2 * p0.b + p1.b) / 3},
                            {(p0.r + 2 * p1.r) / 3, (p0.g + 2 * p1.g) / 3, (p0.b + 2 * p1.b) / 3}};
        cbits = 0;
        int chosen[16] = {};
        for (int i = 0; i < 16; ++i) {
            int best = 0, bestd = 1 << 30;
            for (int j = 0; j < 4; ++j) {
                const int dr = px[i][0] - pal[j].r, dg = px[i][1] - pal[j].g, db = px[i][2] - pal[j].b;
                const int dd = dr * dr + dg * dg + db * db;
                if (dd < bestd) {
                    bestd = dd;
                    best = j;
                }
            }
            chosen[i] = best;
            cbits |= u32(best) << (2 * i);
        }
        if (round == 2) {
            break;
        }
        // least squares: x_i = w_i e0 + (1 - w_i) e1 over the visible texels
        double aa = 0, ab = 0, bb = 0, ax[3] = {}, bx[3] = {};
        for (int k = 0; k < n; ++k) {
            const double w = kW[chosen[sel[k]]], v = 1.0 - w;
            aa += w * w;
            ab += w * v;
            bb += v * v;
            for (int c = 0; c < 3; ++c) {
                ax[c] += w * px[sel[k]][c];
                bx[c] += v * px[sel[k]][c];
            }
        }
        const double det = aa * bb - ab * ab;
        if (std::abs(det) < 1e-9) {
            break;
        }
        for (int c = 0; c < 3; ++c) {
            e0[c] = std::clamp((ax[c] * bb - bx[c] * ab) / det, 0.0, 255.0);
            e1[c] = std::clamp((bx[c] * aa - ax[c] * ab) / det, 0.0, 255.0);
        }
    }
    std::memcpy(blk + 8, &c0, 2);
    std::memcpy(blk + 10, &c1, 2);
    std::memcpy(blk + 12, &cbits, 4);
}

// ---- drawing (RGBA top-down)

struct Image {
    u32 w = 0, h = 0;
    std::vector<u8> px; // RGBA
    u8* At(u32 x, u32 y) { return px.data() + (size_t{y} * w + x) * 4; }
};

void FlipRows(Image& im) {
    for (u32 y = 0; y < im.h / 2; ++y) {
        std::swap_ranges(im.At(0, y), im.At(0, y) + size_t{im.w} * 4, im.At(0, im.h - 1 - y));
    }
}

std::optional<Input::InputCode> Bound(const Input::Settings& layout, Input::Action a) {
    const auto& b = layout.bindings[static_cast<size_t>(a)];
    if (b.empty()) {
        return std::nullopt;
    }
    return b.front();
}

/// `mask` (side x side) composited at (x0, y0) in colour `c`.
void Put(Image& im, const std::vector<u8>& mask, u32 side, int x0, int y0, Rgb c) {
    for (u32 y = 0; y < side; ++y) {
        for (u32 x = 0; x < side; ++x) {
            const int X = x0 + static_cast<int>(x), Y = y0 + static_cast<int>(y);
            if (X < 0 || Y < 0 || X >= static_cast<int>(im.w) || Y >= static_cast<int>(im.h)) {
                continue;
            }
            const int a = mask[size_t{y} * side + x];
            if (a == 0) {
                continue;
            }
            u8* p = im.At(X, Y);
            p[0] = static_cast<u8>((c.r * a + p[0] * (255 - a)) / 255);
            p[1] = static_cast<u8>((c.g * a + p[1] * (255 - a)) / 255);
            p[2] = static_cast<u8>((c.b * a + p[2] * (255 - a)) / 255);
            p[3] = static_cast<u8>(std::max<int>(p[3], a));
        }
    }
}

bool DrawKeyAt(Image& im, Input::InputCode in, u32 side, int cx, int cy, Rgb c) {
    const std::vector<u8> mask = PromptIcons::DrawInputAlpha(in, side);
    if (mask.size() != size_t{side} * side) {
        return false;
    }
    Put(im, mask, side, cx - static_cast<int>(side / 2), cy - static_cast<int>(side / 2), c);
    return true;
}

/// The graffiti tutorial: of the original only its yellow paint stays (the hand and the controller go), and the key
/// of `action` is drawn on it - one square slot per frame, the rattle's frames each a little aside.
bool DrawGraffiti(Image& im, const Input::Settings& layout, Input::Action action, bool rattle) {
    const auto key = Bound(layout, action);
    if (!key) {
        return false;
    }
    for (u32 y = 0; y < im.h; ++y) {
        for (u32 x = 0; x < im.w; ++x) {
            u8* p = im.At(x, y);
            const bool paint = p[0] >= 120 && p[1] >= 50 && p[2] <= 120 && p[0] > p[1] + 20 && p[0] > p[2] + 80;
            if (!paint) {
                p[0] = p[1] = p[2] = p[3] = 0;
            }
        }
    }
    const u32 slot = im.h, slots = im.w / slot;
    static constexpr int kShake[4] = {-1, 1, 0, 0};
    for (u32 s = 0; s < slots; ++s) {
        // a slot with little paint is an empty frame
        u32 visible = 0, samples = 0;
        u32 x0 = ~0u, x1 = 0, y0 = ~0u, y1 = 0;
        long sum[3] = {};
        for (u32 y = 0; y < slot; ++y) {
            for (u32 x = s * slot; x < (s + 1) * slot; ++x) {
                const u8* p = im.At(x, y);
                ++samples;
                if (p[3] == 0) {
                    continue;
                }
                ++visible;
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
                sum[0] += p[0];
                sum[1] += p[1];
                sum[2] += p[2];
            }
        }
        if (visible * 20 < samples) { // under 5 %: (the strip's unused last frame)
            for (u32 y = 0; y < slot; ++y) {
                std::fill(im.At(s * slot, y), im.At(s * slot, y) + size_t{slot} * 4, u8{0});
            }
            continue;
        }
        // the holes the hand leaves in the paint, filled with its own colour (the ragged rim stays the game's)
        const u8 fill[3] = {static_cast<u8>(sum[0] / visible), static_cast<u8>(sum[1] / visible),
                            static_cast<u8>(sum[2] / visible)};
        const u32 inset_x = (x1 - x0) * 8 / 100, inset_y = (y1 - y0) * 8 / 100;
        for (u32 y = y0 + inset_y; y + inset_y <= y1; ++y) {
            for (u32 x = x0 + inset_x; x + inset_x <= x1; ++x) {
                u8* p = im.At(x, y);
                if (p[3] < 128) {
                    p[0] = fill[0];
                    p[1] = fill[1];
                    p[2] = fill[2];
                    p[3] = 255;
                }
            }
        }
        const u32 side = slot * 55 / 100;
        const int dx = rattle ? kShake[s % 4] * static_cast<int>(slot / 14) : 0;
        if (!DrawKeyAt(im, *key, side, static_cast<int>(s * slot + slot / 2) + dx, static_cast<int>(slot / 2),
                       {255, 255, 255})) {
            return false;
        }
    }
    return true;
}

/// The drain prompt's print on the touch pad: the touch pad's key instead.
bool DrawTouchPrint(Image& im, const Input::Settings& layout) {
    const auto key = Bound(layout, Input::Action::TouchPad);
    if (!key) {
        return false;
    }
    std::fill(im.px.begin(), im.px.end(), u8{0});
    return DrawKeyAt(im, *key, im.w * 70 / 100, static_cast<int>(im.w / 2), static_cast<int>(im.h / 2),
                     {255, 255, 255});
}

/// A d-pad wheel: its frame and dark disc stay; the cross becomes the four keys of the d-pad, each in the colour its
/// arm had (a highlighted direction stays highlighted).
bool DrawDpad(Image& im, const Input::Settings& layout) {
    using A = Input::Action;
    const auto up = Bound(layout, A::DpadUp), down = Bound(layout, A::DpadDown), left = Bound(layout, A::DpadLeft),
               right = Bound(layout, A::DpadRight);
    if (!up || !down || !left || !right) {
        return false;
    }
    const float W = static_cast<float>(im.w), cx = W / 2, cy = static_cast<float>(im.h) / 2;
    const auto average = [&](float x, float y, float r) {
        long s[3] = {}, n = 0;
        for (int yy = static_cast<int>(y - r); yy <= static_cast<int>(y + r); ++yy) {
            for (int xx = static_cast<int>(x - r); xx <= static_cast<int>(x + r); ++xx) {
                if (xx < 0 || yy < 0 || xx >= static_cast<int>(im.w) || yy >= static_cast<int>(im.h)) {
                    continue;
                }
                const u8* p = im.At(xx, yy);
                if (p[3] < 128) {
                    continue;
                }
                s[0] += p[0];
                s[1] += p[1];
                s[2] += p[2];
                ++n;
            }
        }
        return n == 0 ? Rgb{0, 0, 0} : Rgb{int(s[0] / n), int(s[1] / n), int(s[2] / n)};
    };
    const float d = 0.19f * W, r = std::max(1.0f, 0.035f * W);
    struct Arm {
        Input::InputCode key;
        float x, y;
        Rgb colour;
    };
    std::array<Arm, 4> arms{{{*up, cx, cy - d, {}}, {*down, cx, cy + d, {}}, {*left, cx - d, cy, {}},
                             {*right, cx + d, cy, {}}}};
    for (Arm& a : arms) {
        // the arm's highlight: its saturated texels (a fifth of the arm's area or more), else the plain grey key
        const float ar = 0.08f * W;
        long s[3] = {}, hits = 0, all = 0;
        for (int yy = static_cast<int>(a.y - ar); yy <= static_cast<int>(a.y + ar); ++yy) {
            for (int xx = static_cast<int>(a.x - ar); xx <= static_cast<int>(a.x + ar); ++xx) {
                if (xx < 0 || yy < 0 || xx >= static_cast<int>(im.w) || yy >= static_cast<int>(im.h)) {
                    continue;
                }
                const u8* p = im.At(xx, yy);
                ++all;
                if (p[3] >= 128 && std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}) > 60) {
                    s[0] += p[0];
                    s[1] += p[1];
                    s[2] += p[2];
                    ++hits;
                }
            }
        }
        a.colour = hits * 5 >= all && hits > 0
                       ? Rgb{std::min<int>(255, s[0] / hits * 5 / 4), std::min<int>(255, s[1] / hits * 5 / 4),
                             std::min<int>(255, s[2] / hits * 5 / 4)}
                       : Rgb{200, 200, 200};
        // a white arm (the highlight of a coloured wheel) stays white
        const Rgb mid = average(a.x, a.y, std::max(1.0f, 0.03f * W));
        if (std::min({mid.r, mid.g, mid.b}) > 200) {
            a.colour = {255, 255, 255};
        }
    }
    // the disc's own colour between the arms
    const float q = 0.17f * W;
    const Rgb c1 = average(cx - q, cy - q, r), c2 = average(cx + q, cy + q, r);
    const Rgb disc{(c1.r + c2.r) / 2, (c1.g + c2.g) / 2, (c1.b + c2.b) / 2};
    for (Arm& a : arms) { // (an arm the disc's own colour - a coloured wheel's plain arm - is a light grey key)
        const int d = std::abs(a.colour.r - disc.r * 5 / 4) + std::abs(a.colour.g - disc.g * 5 / 4) +
                      std::abs(a.colour.b - disc.b * 5 / 4);
        if (d < 120 && !(a.colour.r == 255 && a.colour.g == 255 && a.colour.b == 255)) {
            a.colour = {225, 225, 225};
        }
    }
    const float clear = 0.31f * W;
    for (u32 y = 0; y < im.h; ++y) {
        for (u32 x = 0; x < im.w; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= clear * clear) {
                u8* p = im.At(x, y);
                p[0] = static_cast<u8>(disc.r);
                p[1] = static_cast<u8>(disc.g);
                p[2] = static_cast<u8>(disc.b);
                p[3] = 255;
            }
        }
    }
    const u32 side = std::max<u32>(8, static_cast<u32>(0.24f * W));
    for (const Arm& a : arms) {
        if (!DrawKeyAt(im, a.key, side, static_cast<int>(a.x), static_cast<int>(a.y), a.colour)) {
            return false;
        }
    }
    return true;
}

/// A button on a disc over a character (the subdue / melee prompts): the disc and its ring stay; the button's shape
/// becomes the key bound to it, in the shape's own colour.
bool DrawDisc(Image& im, const Input::Settings& layout, Input::Action action) {
    const auto key = Bound(layout, action);
    if (!key) {
        return false;
    }
    const float W = static_cast<float>(im.w), cx = W / 2, cy = static_cast<float>(im.h) / 2;
    const float inner = 0.30f * W;
    long hi[3] = {}, nhi = 0, lo[3] = {}, nlo = 0;
    for (u32 y = 0; y < im.h; ++y) {
        for (u32 x = 0; x < im.w; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            const u8* p = im.At(x, y);
            if (p[3] < 128 || dx * dx + dy * dy > inner * inner) {
                continue;
            }
            const bool sat = std::max({p[0], p[1], p[2]}) - std::min({p[0], p[1], p[2]}) > 60;
            long* acc = sat ? hi : lo;
            acc[0] += p[0];
            acc[1] += p[1];
            acc[2] += p[2];
            ++(sat ? nhi : nlo);
        }
    }
    if (nhi == 0 || nlo == 0) {
        return false;
    }
    const Rgb colour{std::min<int>(255, int(hi[0] / nhi) * 5 / 4), std::min<int>(255, int(hi[1] / nhi) * 5 / 4),
                     std::min<int>(255, int(hi[2] / nhi) * 5 / 4)};
    const Rgb disc{int(lo[0] / nlo), int(lo[1] / nlo), int(lo[2] / nlo)};
    for (u32 y = 0; y < im.h; ++y) {
        for (u32 x = 0; x < im.w; ++x) {
            const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
            if (dx * dx + dy * dy <= inner * inner) {
                u8* p = im.At(x, y);
                p[0] = static_cast<u8>(disc.r);
                p[1] = static_cast<u8>(disc.g);
                p[2] = static_cast<u8>(disc.b);
                p[3] = 255;
            }
        }
    }
    return DrawKeyAt(im, *key, static_cast<u32>(0.46f * W), static_cast<int>(cx), static_cast<int>(cy), colour);
}

/// Bytes of a level's blocks: padded to 8x8 blocks (the micro tile).
u64 LevelBytes(u32 w, u32 h, u32 level) {
    const u32 bw = ((std::max(1u, w >> level) + 3) / 4 + 7) / 8 * 8;
    const u32 bh = ((std::max(1u, h >> level) + 3) / 4 + 7) / 8 * 8;
    return u64{bw} * bh * 16;
}

/// The next level: 2x2 boxes averaged with premultiplied alpha.
Image Half(const Image& a) {
    Image b;
    b.w = std::max(1u, a.w / 2);
    b.h = std::max(1u, a.h / 2);
    b.px.assign(size_t{b.w} * b.h * 4, 0);
    for (u32 y = 0; y < b.h; ++y) {
        for (u32 x = 0; x < b.w; ++x) {
            u32 s[4] = {}, n = 0;
            for (u32 dy = 0; dy < 2; ++dy) {
                for (u32 dx = 0; dx < 2; ++dx) {
                    const u32 X = std::min(a.w - 1, x * 2 + dx), Y = std::min(a.h - 1, y * 2 + dy);
                    const u8* p = a.px.data() + (size_t{Y} * a.w + X) * 4;
                    for (int c = 0; c < 3; ++c) {
                        s[c] += u32{p[c]} * p[3];
                    }
                    s[3] += p[3];
                    ++n;
                }
            }
            u8* q = b.px.data() + (size_t{y} * b.w + x) * 4;
            for (int c = 0; c < 3; ++c) {
                q[c] = static_cast<u8>(s[3] == 0 ? 0 : s[c] / s[3]);
            }
            q[3] = static_cast<u8>(s[3] / n);
        }
    }
    return b;
}

u64 Fnv(const u8* p, u64 n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < n; ++i) {
        h = (h ^ p[i]) * 0x100000001b3ull;
    }
    return h;
}

} // namespace

std::vector<const Location*> InPack(std::string_view pack) {
    std::vector<const Location*> out;
    for (const Location& l : kLocations) {
        if (pack == l.pack) {
            out.push_back(&l);
        }
    }
    return out;
}

u64 Bytes(PictureKind kind) {
    const KindInfo k = Info(kind);
    u64 n = 0;
    for (u32 l = 0; l < k.levels; ++l) {
        n += LevelBytes(k.w, k.h, l);
    }
    return n;
}

bool IsOriginal(PictureKind kind, const u8* data) {
    const KindInfo k = Info(kind);
    return Fnv(data, u64{k.w} * k.h) == k.fnv;
}

void DecodeBc3(const u8* tiled, u32 w, u32 h, u8* rgba) {
    const u32 bw = w / 4, bh = h / 4;
    u8 px[16][4];
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            DecodeBlock(tiled + BlockIndex(bx, by, bw) * 16, px);
            for (u32 i = 0; i < 16; ++i) {
                std::memcpy(rgba + ((size_t{by} * 4 + i / 4) * w + bx * 4 + i % 4) * 4, px[i], 4);
            }
        }
    }
}

void EncodeBc3(const u8* rgba, u32 w, u32 h, u8* tiled) {
    const u32 bw = w / 4, bh = h / 4;
    u8 px[16][4];
    for (u32 by = 0; by < bh; ++by) {
        for (u32 bx = 0; bx < bw; ++bx) {
            for (u32 i = 0; i < 16; ++i) {
                std::memcpy(px[i], rgba + ((size_t{by} * 4 + i / 4) * w + bx * 4 + i % 4) * 4, 4);
            }
            EncodeBlock(px, tiled + BlockIndex(bx, by, bw) * 16);
        }
    }
}

std::vector<u8> Build(PictureKind kind, const u8* original, Input::PromptStyle style, const Input::Settings& layout) {
    const KindInfo k = Info(kind);
    std::vector<u8> out(original, original + Bytes(kind));
    if (style != Input::PromptStyle::Keyboard) {
        return out; // a controller: the game's own picture
    }
    Image im;
    im.w = k.w;
    im.h = k.h;
    im.px.resize(size_t{k.w} * k.h * 4);
    DecodeBc3(original, k.w, k.h, im.px.data());
    FlipRows(im); // (stored bottom-up)
    bool drawn = false;
    switch (kind) {
    case PictureKind::kSideways256:
    case PictureKind::kSideways128:
        drawn = DrawGraffiti(im, layout, Input::Action::MotionMode, false);
        break;
    case PictureKind::kRattle1024:
    case PictureKind::kRattle512:
        drawn = DrawGraffiti(im, layout, Input::Action::Shake, true);
        break;
    case PictureKind::kTouchPrint256:
        drawn = DrawTouchPrint(im, layout);
        break;
    case PictureKind::kDpad256:
    case PictureKind::kDpadKarma256:
    case PictureKind::kDpad64:
    case PictureKind::kDpadRed64:
        drawn = DrawDpad(im, layout);
        break;
    case PictureKind::kDiscTriangle128:
        drawn = DrawDisc(im, layout, Input::Action::Triangle);
        break;
    case PictureKind::kDiscSquare128:
        drawn = DrawDisc(im, layout, Input::Action::Square);
        break;
    }
    if (!drawn) {
        return out;
    }
    // every mip level from the drawn top level (the game samples the small ones on screen), each stored bottom-up in
    // its padded block tiles
    u64 at = 0;
    Image level = std::move(im);
    for (u32 l = 0; l < k.levels; ++l) {
        if (l != 0) {
            level = Half(level);
        }
        const u32 pw = ((level.w + 3) / 4 + 7) / 8 * 8 * 4, ph = ((level.h + 3) / 4 + 7) / 8 * 8 * 4;
        std::vector<u8> padded(size_t{pw} * ph * 4, 0);
        for (u32 y = 0; y < level.h; ++y) {
            std::memcpy(padded.data() + size_t{y} * pw * 4, level.px.data() + size_t{level.h - 1 - y} * level.w * 4,
                        size_t{level.w} * 4);
        }
        EncodeBc3(padded.data(), pw, ph, out.data() + at);
        at += LevelBytes(k.w, k.h, l);
    }
    return out;
}

} // namespace SecondSon::Ui::PromptTextures
