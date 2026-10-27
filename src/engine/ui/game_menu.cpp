// SecondSonPC — the game's own menus as SecondSonPC sees them (docs/PC_EDITION_UI.md).
#include "engine/ui/game_menu.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"

namespace SecondSon::Engine::GameMenu {

namespace {

void Snapshot(u64 base, u64 size, const std::string& dir, unsigned second) {
    const std::string stem = fmt::format("{}\\snap_{}", dir, second);
    FILE* bin = std::fopen((stem + ".bin").c_str(), "wb");
    FILE* map = std::fopen((stem + ".map").c_str(), "w");
    if (bin == nullptr || map == nullptr) {
        SSPC_LOG(Engine, Error, "memory snapshot {}: cannot write {}", second, stem);
        if (bin) std::fclose(bin);
        if (map) std::fclose(map);
        return;
    }
    u64 written = 0;
    for (u64 at = base; at < base + size;) {
        MEMORY_BASIC_INFORMATION mi{};
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mi, sizeof(mi)) == 0) {
            break;
        }
        const u64 end = std::min<u64>(reinterpret_cast<u64>(mi.BaseAddress) + mi.RegionSize, base + size);
        constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (mi.State == MEM_COMMIT && (mi.Protect & kWritable) != 0 && (mi.Protect & PAGE_GUARD) == 0) {
            std::fwrite(reinterpret_cast<const void*>(at), 1, end - at, bin);
            std::fprintf(map, "%llx %llx\n", static_cast<unsigned long long>(at - base),
                         static_cast<unsigned long long>(end - at));
            written += end - at;
        }
        at = end;
    }
    std::fclose(bin);
    std::fclose(map);
    SSPC_LOG(Engine, Info, "memory snapshot at {} s: {} MiB of the game's writable image -> {}.bin", second,
             written >> 20, stem);
}


// ---- the game's pausing screens

#if defined(SSPC_PRODUCT_FIRST_LIGHT)
// First Light's same two functions (research/fl_sites.md §1); the screen's fields are 0x2c further. The close
// function pushes r14 and rbx (Second Son's rbx and rax): its prologue is 17 bytes as well.
constexpr ProbeSite kOpenedSite{{0x2e75a0, 0xe2, 0x2a2ccf44d65abfaeull}, 17};
constexpr ProbeSite kClosedSite{{0x2e7ad0, 0xfa, 0x59fc0e5d56670225ull}, 17};
constexpr u64 kScreenActive = 0x647;
constexpr u64 kScreenKind = 0x64c; // SOUND_PAUSE_KIND in the game's reflection table
constexpr int kSettingsKind = 7;   // the constructor's default, as Second Son's; not measured on First Light yet
constexpr u32 kDumpFrom = 0x5c0, kDumpTo = 0x680;
#else
constexpr ProbeSite kOpenedSite{{0x27db10, 0xd0, 0x8109e3344cdd9cf3ull}, 17};
constexpr ProbeSite kClosedSite{{0x27e210, 0xdf, 0x33ff129a2ada5846ull}, 16};
constexpr u64 kScreenActive = 0x61b; // u8: the screen counts as open (both functions test and flip it)
constexpr u64 kScreenKind = 0x620;   // u32
constexpr int kSettingsKind = 7;     // the settings screen Esc / Options opens during play (measured 2026-10-15)
constexpr u32 kDumpFrom = 0x600, kDumpTo = 0x640;
#endif

using ScreenFn = SSPC_SYSV_ABI void (*)(u64 screen);
ScreenFn g_opened = nullptr;
ScreenFn g_closed = nullptr;
std::atomic<bool> g_installed{false};
bool g_diag = false;

struct Screen {
    u64 object;
    int kind;
};
std::mutex g_mutex;
std::vector<Screen> g_open; // in opening order
u64 g_changes = 0;

void DumpScreen(const char* what, u64 screen) {
    if (!g_diag) {
        return;
    }
    const auto* p = reinterpret_cast<const u8*>(screen);
    std::string head, tail;
    for (u32 i = 0; i < 0x80; ++i) {
        head += fmt::format("{:02x}", p[i]);
    }
    for (u32 i = kDumpFrom; i < kDumpTo; ++i) {
        tail += fmt::format("{:02x}", p[i]);
    }
    SSPC_LOG(Engine, Info, "game menu {} {:#x}: [0..80] {} [{:x}..{:x}] {}", what, screen, head, kDumpFrom, kDumpTo,
             tail);
}

SSPC_SYSV_ABI void OpenedProbe(u64 screen) {
    const bool was_open = *reinterpret_cast<const u8*>(screen + kScreenActive) != 0;
    g_opened(screen);
    if (!was_open) {
        const int kind = static_cast<int>(*reinterpret_cast<const u32*>(screen + kScreenKind));
        {
            std::scoped_lock lock{g_mutex};
            g_open.push_back({screen, kind});
            ++g_changes;
        }
        SSPC_LOG(Engine, Info, "game menu: pausing screen {:#x} opened (kind {})", screen, kind);
        DumpScreen("opened", screen);
    }
}

SSPC_SYSV_ABI void ClosedProbe(u64 screen) {
    const bool was_open = *reinterpret_cast<const u8*>(screen + kScreenActive) != 0;
    g_closed(screen);
    if (was_open) {
        {
            std::scoped_lock lock{g_mutex};
            std::erase_if(g_open, [screen](const Screen& s) { return s.object == screen; });
            ++g_changes;
        }
        SSPC_LOG(Engine, Info, "game menu: pausing screen {:#x} closed", screen);
    }
}

} // namespace

bool Install(u64 image_base) {
    char d[4] = {};
    g_diag = GetEnvironmentVariableA("SSPC_DIAG_GAME_MENU", d, sizeof(d)) == 1 && d[0] == '1';
    std::string error;
    void* opened = InstallEntryProbe(image_base, kOpenedSite, reinterpret_cast<const void*>(&OpenedProbe), error);
    if (opened == nullptr) {
        SSPC_LOG(Engine, Error, "game menu: screen probes not installed ({}); the PC settings open with F1 only", error);
        return false;
    }
    g_opened = reinterpret_cast<ScreenFn>(opened);
    void* closed = InstallEntryProbe(image_base, kClosedSite, reinterpret_cast<const void*>(&ClosedProbe), error);
    if (closed == nullptr) {
        // the open probe stays (it only observes); without closes the state would never clear: not used
        SSPC_LOG(Engine, Error, "game menu: screen close probe not installed ({}); the PC settings open with F1 only",
                 error);
        return false;
    }
    g_closed = reinterpret_cast<ScreenFn>(closed);
    g_installed.store(true, std::memory_order_release);
    SSPC_LOG(Engine, Info, "game menu: the game's pausing screens are observed (eboot {:#x} / {:#x})",
             kOpenedSite.function.rva, kClosedSite.function.rva);
    return true;
}

bool Installed() {
    return g_installed.load(std::memory_order_acquire);
}

State Current() {
    State s;
    std::scoped_lock lock{g_mutex};
    s.open = static_cast<int>(g_open.size());
    if (!g_open.empty()) {
        s.top_kind = g_open.back().kind;
        s.top_screen = g_open.back().object;
    }
    s.changes = g_changes;
    return s;
}

bool SettingsScreenOnTop() {
    const State s = Current();
    return s.open > 0 && s.top_kind == kSettingsKind;
}

void StartMemorySnapshotsFromEnvironment(u64 image_base, u64 image_size) {
    char spec[256] = {};
    char dir[MAX_PATH] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_MEMSNAP", spec, sizeof(spec)) == 0) {
        return;
    }
    if (GetEnvironmentVariableA("SSPC_DIAG_MEMSNAP_DIR", dir, sizeof(dir)) == 0) {
        std::snprintf(dir, sizeof(dir), ".");
    }
    std::vector<unsigned> seconds;
    for (const char* p = spec; *p;) {
        char* end = nullptr;
        const unsigned long v = std::strtoul(p, &end, 10);
        if (end == p) {
            break;
        }
        seconds.push_back(static_cast<unsigned>(v));
        p = *end == ',' ? end + 1 : end;
    }
    SSPC_LOG(Engine, Warning, "SSPC_DIAG_MEMSNAP: {} snapshots of the game's writable image -> {}", seconds.size(),
             dir);
    std::thread([image_base, image_size, seconds, d = std::string(dir)] {
        const auto start = std::chrono::steady_clock::now();
        for (const unsigned s : seconds) {
            std::this_thread::sleep_until(start + std::chrono::seconds(s));
            Snapshot(image_base, image_size, d, s);
        }
    }).detach();
}

} // namespace SecondSon::Engine::GameMenu
