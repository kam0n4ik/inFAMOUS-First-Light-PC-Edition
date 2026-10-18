// SecondSonPC — the game's button icons follow the input device (see prompt_service.h).
#include "ui/prompt_service.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "base/log.h"
#include "input/keyboard_mouse.h"
#include "runtime/filesystem/content_patch.h"
#include "runtime/filesystem/fs_host.h"
#include "settings/settings_service.h"
#include "ui/prompt_icons.h"
#include "ui/prompt_textures.h"

namespace SecondSon::Ui::PromptService {

namespace {

namespace P = PromptIcons;
namespace ContentPatch = Filesystem::ContentPatch;
using Input::PromptStyle;

/// Where the game put (part of) the watched span of the table: span bytes [at, at + size) at guest address `address`.
struct Copy {
    u64 at = 0, address = 0, size = 0;
};

/// A hint that names the controller in words ("Hold Wireless Controller Sideways", "the Touch Pad Button"): with
/// the keyboard its text names the keys bound now. The new text is written where the game's text starts (the key
/// name and the game's pointers stay), inside the text and its zero padding (`room` bytes, the last zero kept).
struct Text {
    const char* key = "";
    u64 offset = 0; // of the text in the file
    u32 room = 0;
    std::string original; // `room` bytes as in the file
};

struct State {
    std::mutex mutex;
    bool found = false;               // a language table with the icon atlas was opened
    std::vector<u8> original;         // its atlas (tiled, 1 MiB)
    std::vector<P::Cell> cells;
    u64 data_offset = 0;              // in the table opened last
    std::string table;                // its guest path
    bool russian = false;             // its language (the hint texts: Russian or English tables only)
    std::vector<Text> texts;          // its hints naming the controller
    std::vector<std::string> applied_texts; // their bytes now in the game's memory (`room` each)
    u64 span_lo = 0, span_hi = 0;     // the watched file range: the hint texts and the atlas
    std::vector<Copy> copies;         // where the game keeps it
    PromptStyle applied = PromptStyle::PlayStation; // what the game read / what is written there now
    u64 applied_layout = ~0ull;       // the keyboard layout generation of `applied` (keyboard only)
    std::vector<u8> applied_bytes;    // the atlas now in the game's memory
    std::vector<u8> xbox;             // built once
    std::vector<u8> keyboard;         // built for keyboard_layout
    u64 keyboard_layout = ~0ull;
    u64 last_apply_ns = 0;
    u32 rewrites = 0;
    bool reported_failure = false;
};

/// A pack with button pictures (ui/prompt_textures.h): its pictures' places, original bytes and what the game read.
struct Picture {
    const PromptTextures::Location* at = nullptr;
    std::vector<u8> original, applied;
};
struct Pack {
    std::vector<Picture> pictures;
    u64 span_lo = 0, span_hi = 0;
    std::vector<Copy> copies;
};
std::map<std::string, Pack, std::less<>> g_packs; // by guest path (State::mutex)
std::map<std::pair<int, u64>, std::vector<u8>> g_built; // (kind, style << 32 | layout generation) -> bytes

State& S() {
    static State s;
    return s;
}

std::atomic<PromptStyle> g_wanted{PromptStyle::PlayStation};
std::atomic<u64> g_wanted_layout{0};

std::string_view StyleName(PromptStyle s) {
    switch (s) {
    case PromptStyle::Keyboard: return "keyboard and mouse";
    case PromptStyle::Xbox: return "Xbox";
    case PromptStyle::PlayStation: return "PlayStation (the game's own)";
    }
    return "?";
}

// SSPC_DIAG_PROMPTS=<s>:<style>[,<s>:<style>...] (diagnostic, automated runs without a controller): from <s> seconds
// after Install the style is forced (keyboard | xbox | playstation) - the rewrite in the game's memory is exercised
struct Forced {
    u64 after_ns;
    PromptStyle style;
};
std::vector<Forced> g_forced;
u64 g_install_ns = 0;

u64 SteadyNs() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return static_cast<u64>(static_cast<double>(c.QuadPart) * 1e9 / static_cast<double>(f.QuadPart));
}

void ParseForced() {
    char buf[256] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_DIAG_PROMPTS", buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) {
        return;
    }
    std::string_view rest(buf, n);
    while (!rest.empty()) {
        const auto comma = rest.find(',');
        const std::string_view item = rest.substr(0, comma);
        rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
        const auto colon = item.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const u64 secs = std::strtoull(std::string(item.substr(0, colon)).c_str(), nullptr, 10);
        const std::string_view name = item.substr(colon + 1);
        const PromptStyle st = name == "xbox"          ? PromptStyle::Xbox
                               : name == "playstation" ? PromptStyle::PlayStation
                                                       : PromptStyle::Keyboard;
        g_forced.push_back({secs * 1'000'000'000ull, st});
    }
    SSPC_LOG(App, Info, "button icons: SSPC_DIAG_PROMPTS - {} forced style change(s)", g_forced.size());
}

PromptStyle WantedNow() {
    const auto cfg = Settings::Instance().Current();
    PromptStyle style = Input::ResolvePrompts(cfg.pc.prompts, Input::Devices().Active());
    const u64 since = SteadyNs() - g_install_ns;
    for (const auto& f : g_forced) {
        if (since >= f.after_ns) {
            style = f.style;
        }
    }
    return style;
}

/// A key's name in a hint: the SDL name for keys ("Tab", "G", "Left Shift"), the mouse as players write it.
std::string KeyLabel(Input::Action action, bool russian) {
    const Input::Settings layout = Input::Player1().Layout();
    const auto& b = layout.bindings[static_cast<size_t>(action)];
    if (b.empty()) {
        return {};
    }
    const Input::InputCode c = b.front();
    if (c.kind == Input::InputCode::Kind::MouseButton) {
        switch (c.code) {
        case 1: return russian ? "ЛКМ" : "LMB";
        case 2: return russian ? "СКМ" : "MMB";
        case 3: return russian ? "ПКМ" : "RMB";
        default: break;
        }
    }
    return Input::InputName(c);
}

void ReplaceAll(std::string& s, std::string_view what, std::string_view with) {
    for (size_t at = s.find(what); at != std::string::npos; at = s.find(what, at + with.size())) {
        s.replace(at, what.size(), with);
    }
}

constexpr const char* kHintKeys[] = {
    "u_brief_tilt_controller",      "u_hint_three_powers",          "u_hint_two_powers",
    "u_hint_vor_door",              "u_interact_press_for_details", "u_powerk_desc_concretedrain",
    "u_powerk_desc_concretedrain_alt", "u_powerk_desc_laserdrain",  "u_powerk_desc_laserdrain_alt",
    "u_powerk_desc_smokedrain",     "u_powerk_desc_smokedrain_alt", "u_powerk_desc_tvdrain",
    "u_powerk_desc_tvdrain_alt",    "u_protip_ftp_12",              "u_stencil_shake_controller",
    "u_stencil_sideways_controller", "u_trainer_smi_push_car",      "u_trainer_smi_push_car_alt",
};

/// The keyboard text of a hint (empty: none - the game's stays): the game's own sentence with the controller's
/// part replaced by the keys bound now (touch pad, two-finger click, swipe, sideways, rattle, spray).
std::string KeyboardText(const Text& t, bool russian) {
    using A = Input::Action;
    const std::string tp = KeyLabel(A::TouchPad, russian), two = KeyLabel(A::TouchTwoFingers, russian),
                      drag = KeyLabel(A::TouchDrag, russian), side = KeyLabel(A::MotionMode, russian),
                      shake = KeyLabel(A::Shake, russian), spray = KeyLabel(A::R2, russian);
    const std::string_view key = t.key;
    std::string s(t.original.c_str()); // (the text up to its zero)
    if (key == "u_stencil_sideways_controller") {
        return side.empty() ? "" : russian ? "Нажмите " + side + ", чтобы держать боком" : "Press " + side + " to hold sideways";
    }
    if (key == "u_stencil_shake_controller") {
        return shake.empty() ? "" : russian ? "Встряхните: держите " + shake : "Hold " + shake + " to rattle";
    }
    if (key == "u_brief_tilt_controller") {
        return spray.empty() ? "" : russian ? "Распыляйте " + spray + ", наводите мышью"
                                            : "Spray with " + spray + ", aim with the mouse";
    }
    if (tp.empty() || two.empty() || drag.empty()) {
        return {};
    }
    if (russian) {
        ReplaceAll(s, "кнопку сенсорной панели двумя пальцами", "клавишу " + two);
        ReplaceAll(s, "кнопку сенсорной панели", "клавишу " + tp);
        ReplaceAll(s, "Проведите пальцем по сенсорной панели", "Удерживайте " + drag + " и ведите мышью");
    } else {
        ReplaceAll(s, "the Touch Pad Button with both thumbs", two);
        ReplaceAll(s, "the Touch Pad Button", tp);
        ReplaceAll(s, "Touch Pad Button", tp);
        ReplaceAll(s, "Swipe the Touch Pad", "Hold " + drag + " + mouse");
    }
    return s == t.original.c_str() ? "" : s;
}

/// A hint's `room` bytes for `style`: the game's own, or the keyboard text with zeros (if it fits).
std::string TextBytes(const Text& t, PromptStyle style, bool russian) {
    if (style != PromptStyle::Keyboard) {
        return t.original;
    }
    const std::string s = KeyboardText(t, russian);
    if (s.empty() || s.size() + 1 > t.room) {
        return t.original;
    }
    std::string out(t.room, '\0');
    std::memcpy(out.data(), s.data(), s.size());
    return out;
}

/// The hints of a language table: "key\0text\0" entries found by their key names.
std::vector<Text> FindTexts(const std::string& bytes) {
    std::vector<Text> out;
    for (const char* key : kHintKeys) {
        const std::string pat = std::string(1, '\0') + key + std::string(1, '\0');
        const size_t at = bytes.find(pat);
        if (at == std::string::npos || bytes.find(pat, at + 1) != std::string::npos) {
            continue; // (absent, or not one entry)
        }
        const size_t text = at + pat.size();
        const size_t end = bytes.find('\0', text);
        if (end == std::string::npos) {
            continue;
        }
        size_t pad = end;
        while (pad < bytes.size() && bytes[pad] == '\0') {
            ++pad;
        }
        Text t;
        t.key = key;
        t.offset = text;
        t.room = static_cast<u32>(pad - 1 - text); // (the last zero before the next entry kept)
        t.original = bytes.substr(text, t.room);
        out.push_back(std::move(t));
    }
    return out;
}

/// A picture for `style` (cached by kind, style and layout). Mutex held; drawn on a host thread (Direct2D).
const std::vector<u8>& PictureFor(const Picture& p, PromptStyle style, u64 layout_gen) {
    if (style != PromptStyle::Keyboard) {
        return p.original;
    }
    const auto key = std::make_pair(static_cast<int>(p.at->kind), (u64{static_cast<u32>(style)} << 32) | layout_gen);
    auto it = g_built.find(key);
    if (it == g_built.end()) {
        const Input::Settings layout = Input::Player1().Layout();
        std::vector<u8> built;
        std::thread worker([&] { built = PromptTextures::Build(p.at->kind, p.original.data(), style, layout); });
        worker.join();
        it = g_built.emplace(key, std::move(built)).first;
    }
    return it->second;
}

ContentPatch::FilePatch OnPackOpen(std::string_view guest, const std::filesystem::path& host) {
    const auto slash = guest.find_last_of('/');
    const std::string_view name = slash == std::string_view::npos ? guest : guest.substr(slash + 1);
    const auto places = PromptTextures::InPack(name);
    if (places.empty()) {
        return {};
    }
    Pack pack;
    std::ifstream in(host, std::ios::binary);
    for (const auto* l : places) {
        Picture p;
        p.at = l;
        p.original.resize(PromptTextures::Bytes(l->kind));
        in.seekg(static_cast<std::streamoff>(l->offset));
        if (!in.read(reinterpret_cast<char*>(p.original.data()), static_cast<std::streamsize>(p.original.size())) ||
            !PromptTextures::IsOriginal(l->kind, p.original.data())) {
            in.clear();
            SSPC_LOG(App, Warning, "button pictures: {} @{:#x} - other bytes than the game's; left as it is", guest,
                     l->offset);
            continue;
        }
        pack.pictures.push_back(std::move(p));
    }
    if (pack.pictures.empty()) {
        return {};
    }
    auto& st = S();
    std::scoped_lock lock{st.mutex};
    const PromptStyle style = g_wanted.load();
    const u64 gen = g_wanted_layout.load();
    ContentPatch::FilePatch patch;
    pack.span_lo = ~0ull;
    u32 changed = 0;
    for (Picture& p : pack.pictures) {
        p.applied = PictureFor(p, style, gen);
        pack.span_lo = std::min(pack.span_lo, p.at->offset);
        pack.span_hi = std::max<u64>(pack.span_hi, p.at->offset + p.original.size());
        if (p.applied != p.original) {
            patch.edits.push_back({p.at->offset, std::string(p.applied.begin(), p.applied.end())});
            ++changed;
        }
    }
    std::sort(patch.edits.begin(), patch.edits.end(),
              [](const ContentPatch::Edit& a, const ContentPatch::Edit& b) { return a.offset < b.offset; });
    patch.watch_offset = pack.span_lo;
    patch.watch_size = pack.span_hi - pack.span_lo;
    SSPC_LOG(App, Info, "button pictures: {} - {} picture(s), {} read for the {} (the file is unchanged)", guest,
             pack.pictures.size(), changed, StyleName(style));
    g_packs.insert_or_assign(std::string(guest), std::move(pack));
    return patch;
}

/// The atlas for `style` (cached). Drawing runs on a thread of its own: the caller may be a guest thread (Direct2D,
/// COM and their stack depth belong on a host thread). Mutex held.
const std::vector<u8>* AtlasFor(State& st, PromptStyle style, u64 layout_gen) {
    if (style == PromptStyle::PlayStation) {
        return &st.original;
    }
    std::vector<u8>& slot = style == PromptStyle::Xbox ? st.xbox : st.keyboard;
    if (!slot.empty() && (style == PromptStyle::Xbox || st.keyboard_layout == layout_gen)) {
        return &slot;
    }
    const Input::Settings layout = Input::Player1().Layout();
    u32 drawn = 0;
    std::string error;
    std::vector<u8> built;
    std::thread worker([&] { built = P::BuildAtlas(st.original.data(), st.cells, style, layout, &drawn, &error); });
    worker.join();
    if (built.size() != P::kAtlasBytes) {
        if (!st.reported_failure) {
            st.reported_failure = true;
            SSPC_LOG(App, Error, "button icons: cannot draw the {} icons ({}); the game's own stay", StyleName(style),
                     error);
        }
        return nullptr;
    }
    SSPC_LOG(App, Info, "button icons: {} atlas drawn ({} icons)", StyleName(style), drawn);
    slot = std::move(built);
    if (style == PromptStyle::Keyboard) {
        st.keyboard_layout = layout_gen;
    }
    return &slot;
}

ContentPatch::FilePatch OnOpen(std::string_view guest, const std::string& bytes) {
    std::string why;
    const auto info = P::Locate(bytes, &why);
    if (!info) {
        SSPC_LOG(App, Warning, "button icons: {} - {}; the game's own icons stay", guest, why);
        return {};
    }
    auto& st = S();
    std::scoped_lock lock{st.mutex};
    st.original.assign(bytes.begin() + static_cast<std::ptrdiff_t>(info->data_offset),
                       bytes.begin() + static_cast<std::ptrdiff_t>(info->data_offset + P::kAtlasBytes));
    st.cells = info->cells;
    st.data_offset = info->data_offset;
    st.table = std::string(guest);
    st.copies.clear(); // (another language: the previous table's copies are not ours any more)
    st.found = true;
    const bool ru = guest.find("lang_russian_text") != std::string_view::npos;
    const bool en = guest.find("lang_english_text") != std::string_view::npos;
    st.russian = ru;
    st.texts = ru || en ? FindTexts(bytes) : std::vector<Text>{};
    st.span_lo = info->data_offset;
    st.span_hi = info->data_offset + P::kAtlasBytes;
    for (const Text& t : st.texts) {
        st.span_lo = std::min(st.span_lo, t.offset);
        st.span_hi = std::max<u64>(st.span_hi, t.offset + t.room);
    }
    PromptStyle style = g_wanted.load();
    const u64 gen = g_wanted_layout.load();
    const std::vector<u8>* atlas = AtlasFor(st, style, gen);
    if (atlas == nullptr) {
        style = PromptStyle::PlayStation;
        atlas = &st.original;
    }
    st.applied = style;
    st.applied_layout = gen;
    st.applied_bytes = *atlas;
    ContentPatch::FilePatch patch;
    patch.watch_offset = st.span_lo;
    patch.watch_size = st.span_hi - st.span_lo;
    if (style != PromptStyle::PlayStation) {
        ContentPatch::Edit e;
        e.offset = info->data_offset;
        e.bytes.assign(reinterpret_cast<const char*>(atlas->data()), atlas->size());
        patch.edits.push_back(std::move(e));
    }
    st.applied_texts.clear();
    u32 replaced = 0;
    for (const Text& t : st.texts) {
        st.applied_texts.push_back(TextBytes(t, style, ru));
        if (st.applied_texts.back() != t.original) {
            patch.edits.push_back({t.offset, st.applied_texts.back()});
            ++replaced;
        }
    }
    std::sort(patch.edits.begin(), patch.edits.end(),
              [](const ContentPatch::Edit& a, const ContentPatch::Edit& b) { return a.offset < b.offset; });
    SSPC_LOG(App, Info, "button icons: {} - {} hints naming the controller found, {} read with the keys", guest,
             st.texts.size(), replaced);
    SSPC_LOG(App, Info,
             "button icons: {} - icon atlas @{:#x}, {} glyphs; the game reads the {} icons (the file is unchanged)",
             guest, info->data_offset, info->cells.size(), StyleName(style));
    return patch;
}

void OnRead(std::string_view guest, u64 offset, u64 address, u64 size) {
    auto& st = S();
    std::scoped_lock lock{st.mutex};
    if (const auto it = g_packs.find(guest); it != g_packs.end()) {
        if (offset >= it->second.span_lo) {
            it->second.copies.push_back({offset - it->second.span_lo, address, size});
        }
        return;
    }
    if (!st.found || guest != st.table || offset < st.span_lo) {
        return;
    }
    st.copies.push_back({offset - st.span_lo, address, size});
    SSPC_LOG(App, Info, "button icons: table bytes {:#x}+{:#x} (icon atlas, hints) read to {:#x}", offset, size,
             address);
}

/// [address, address + size) is committed memory of this process (the game did not unmap it).
bool Mapped(u64 address, u64 size) {
    u64 a = address;
    while (a < address + size) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<const void*>(a), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT) {
            return false;
        }
        a = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

} // namespace

void Install() {
    g_install_ns = SteadyNs();
    ParseForced();
    g_wanted = WantedNow();
    g_wanted_layout = Input::Player1().LayoutGeneration();
    ContentPatch::SetLanguageTableHook({.open = &OnOpen, .read = &OnRead, .pack_open = &OnPackOpen});
    SSPC_LOG(App, Info, "button icons: following the input device ([Interface] prompts = \"{}\"; now {})",
             Settings::Instance().Current().pc.prompts, StyleName(g_wanted.load()));
}

void Update(u64 now_ns) {
    const PromptStyle style = WantedNow();
    const u64 gen = Input::Player1().LayoutGeneration();
    g_wanted = style;
    g_wanted_layout = gen;
    auto& st = S();
    std::scoped_lock lock{st.mutex};
    if (!st.found || (style == st.applied && (style != PromptStyle::Keyboard || gen == st.applied_layout))) {
        return;
    }
    if (st.last_apply_ns != 0 && now_ns >= st.last_apply_ns && now_ns - st.last_apply_ns < kMinGapNs) {
        return; // the next call (the poll) applies it
    }
    st.last_apply_ns = now_ns;
    const std::vector<u8>* atlas = AtlasFor(st, style, gen);
    if (atlas == nullptr) {
        st.applied = style; // (not drawable: not retried every poll; the game keeps what it has)
        st.applied_layout = gen;
        return;
    }
    // a region of the span ([at, at + n) span bytes): written in every copy that holds all of it, only where those
    // bytes are still exactly what was written there (the game may have freed or reused the memory)
    const auto rewrite = [&](u64 at, u64 n, const u8* expected, const u8* bytes, bool gpu) {
        u32 done = 0;
        for (const Copy& c : st.copies) {
            if (at < c.at || at + n > c.at + c.size) {
                continue;
            }
            const u64 a = c.address + (at - c.at);
            if (!Mapped(a, n) || std::memcmp(reinterpret_cast<const void*>(a), expected, n) != 0) {
                SSPC_LOG(App, Warning, "button icons: the game no longer keeps {} at {:#x} (freed or reused) - not "
                                       "written there", gpu ? "the atlas" : "a hint", a);
                continue;
            }
            if (gpu) {
                Filesystem::NotifyHostWrite(a, n); // (the GPU side gives the pages back first)
            }
            std::memcpy(reinterpret_cast<void*>(a), bytes, n);
            if (gpu) {
                Filesystem::NotifyHostWrite(a, n); // its textures there are uploaded again
            }
            ++done;
        }
        return done;
    };
    const u32 written = rewrite(st.data_offset - st.span_lo, P::kAtlasBytes, st.applied_bytes.data(), atlas->data(),
                                true);
    u32 texts = 0;
    for (size_t i = 0; i < st.texts.size() && i < st.applied_texts.size(); ++i) {
        const Text& t = st.texts[i];
        std::string next = TextBytes(t, style, st.russian);
        if (next != st.applied_texts[i] &&
            rewrite(t.offset - st.span_lo, t.room, reinterpret_cast<const u8*>(st.applied_texts[i].data()),
                    reinterpret_cast<const u8*>(next.data()), false) != 0) {
            st.applied_texts[i] = std::move(next);
            ++texts;
        }
    }
    SSPC_LOG(App, Info, "button icons: {} hint texts rewritten", texts);
    u32 pictures = 0;
    for (auto& [guest, pack] : g_packs) {
        for (Picture& p : pack.pictures) {
            const std::vector<u8>& next = PictureFor(p, style, gen);
            if (next == p.applied) {
                continue;
            }
            const u64 at = p.at->offset - pack.span_lo, n = p.original.size();
            u32 done = 0;
            for (const Copy& c : pack.copies) {
                if (at < c.at || at + n > c.at + c.size) {
                    continue;
                }
                const u64 a = c.address + (at - c.at);
                if (!Mapped(a, n) || std::memcmp(reinterpret_cast<const void*>(a), p.applied.data(), n) != 0) {
                    continue; // (freed or reused by the game: not ours any more)
                }
                Filesystem::NotifyHostWrite(a, n);
                std::memcpy(reinterpret_cast<void*>(a), next.data(), n);
                Filesystem::NotifyHostWrite(a, n);
                ++done;
            }
            if (done != 0) {
                p.applied = next;
                ++pictures;
            }
        }
    }
    SSPC_LOG(App, Info, "button pictures: {} rewritten in the game's memory", pictures);
    SSPC_LOG(App, Info, "button icons: {} -> {} ({} cop{} in the game's memory rewritten)", StyleName(st.applied),
             StyleName(style), written, written == 1 ? "y" : "ies");
    st.applied = style;
    st.applied_layout = gen;
    st.applied_bytes = *atlas;
    ++st.rewrites;
}

PromptStyle Applied() {
    std::scoped_lock lock{S().mutex};
    return S().applied;
}

u32 Rewrites() {
    std::scoped_lock lock{S().mutex};
    return S().rewrites;
}

} // namespace SecondSon::Ui::PromptService
