// SecondSonPC — the game's button icons follow the input device (see prompt_service.h).
#include "ui/prompt_service.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
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

namespace SecondSon::Ui::PromptService {

namespace {

namespace P = PromptIcons;
namespace ContentPatch = Filesystem::ContentPatch;
using Input::PromptStyle;

/// Where the game put (part of) the atlas it read: atlas bytes [at, at + size) at guest address `address`.
struct Copy {
    u64 at = 0, address = 0, size = 0;
};

struct State {
    std::mutex mutex;
    bool found = false;               // a language table with the icon atlas was opened
    std::vector<u8> original;         // its atlas (tiled, 1 MiB)
    std::vector<P::Cell> cells;
    u64 data_offset = 0;              // in the table opened last
    std::string table;                // its guest path
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
    patch.watch_offset = info->data_offset;
    patch.watch_size = P::kAtlasBytes;
    if (style != PromptStyle::PlayStation) {
        ContentPatch::Edit e;
        e.offset = info->data_offset;
        e.bytes.assign(reinterpret_cast<const char*>(atlas->data()), atlas->size());
        patch.edits.push_back(std::move(e));
    }
    SSPC_LOG(App, Info,
             "button icons: {} - icon atlas @{:#x}, {} glyphs; the game reads the {} icons (the file is unchanged)",
             guest, info->data_offset, info->cells.size(), StyleName(style));
    return patch;
}

void OnRead(std::string_view guest, u64 offset, u64 address, u64 size) {
    auto& st = S();
    std::scoped_lock lock{st.mutex};
    if (!st.found || guest != st.table || offset < st.data_offset) {
        return;
    }
    st.copies.push_back({offset - st.data_offset, address, size});
    SSPC_LOG(App, Info, "button icons: atlas bytes {:#x}+{:#x} read to {:#x}", offset - st.data_offset, size, address);
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
    ContentPatch::SetLanguageTableHook({.open = &OnOpen, .read = &OnRead});
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
    u32 written = 0;
    for (auto it = st.copies.begin(); it != st.copies.end();) {
        const Copy c = *it;
        const bool ok = c.at + c.size <= P::kAtlasBytes && Mapped(c.address, c.size) &&
                        std::memcmp(reinterpret_cast<const void*>(c.address), st.applied_bytes.data() + c.at, c.size) == 0;
        if (!ok) {
            SSPC_LOG(App, Warning,
                     "button icons: the game no longer keeps the atlas at {:#x} (freed or reused) - not written there",
                     c.address);
            it = st.copies.erase(it);
            continue;
        }
        Filesystem::NotifyHostWrite(c.address, c.size); // (the GPU side gives the pages back first)
        std::memcpy(reinterpret_cast<void*>(c.address), atlas->data() + c.at, c.size);
        Filesystem::NotifyHostWrite(c.address, c.size); // its textures there are uploaded again
        ++written;
        ++it;
    }
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
