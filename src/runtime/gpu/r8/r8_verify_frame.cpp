// SecondSonPC R8 - the deterministic mode for frame comparison (see r8_verify_frame.h).
#include "runtime/gpu/r8/r8_verify_frame.h"

#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>

#include <fmt/format.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "runtime/gpu/native_videoout.h"

namespace SecondSon::Gpu::R8::Verify::Frame {

namespace {

// the game's clock (docs/FRAMEBREAK.md FB-3; engine/game_clock_probe.h): entry `push rbp; mov rbp,rsp; push r15..rbx;
// push rax` (14 bytes, position-independent, nothing branches into them), then rdtsc; dt = (TSC - [rdi+0x1b0]) / the
// TSC frequency, min the clamp, negative -> 0; [rdi+0x1b0] = TSC at the end (0x12a53a)
constexpr Engine::ProbeSite kClockSite = {{0x12a160, 0x3f0, 0x1a5eedc1e99b3cebull}, 14};
constexpr u64 kLastTsc = 0x1b0;
constexpr u64 kWholeTicks[4] = {0x04, 0x14, 0x24, 0x34}; ///< s32 ticks of 1/1440 s
constexpr double kWorldFps = 30.0;                         ///< world frames per scripted second (the dt clamp)
constexpr s64 kNoWorld = -1;
constexpr u64 kWaitLimitTicks = 3'000'000'000ull; ///< ~1 s at 3 GHz: a frame never waits longer for the script

using Fn = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64);
Fn g_orig = nullptr;

struct Row {
    u64 frame, flip, wall_us, waited_us;
    float dt;
    s32 clocks[4];
    s64 world;
};

std::unique_ptr<Row[]> g_rows;
std::atomic<u64> g_frames{0};
std::atomic<s64> g_world{kNoWorld};
std::atomic<bool> g_armed{false};      ///< the script reached its anchor: the world start is looked for
std::atomic<s64> g_delivered{std::numeric_limits<s64>::max()}; ///< world frames the script has served
std::atomic<u32> g_pending{0};         ///< scripted events after the anchor not pushed yet
std::atomic<u64> g_wait_timeouts{0};
u32 g_clock_index = 0;
s32 g_armed_ticks = 0;
double g_anchor = -1.0;
std::chrono::steady_clock::time_point g_t0;

bool ReadEnabled() {
    char v[4] = {};
    return GetEnvironmentVariableA("SSPC_R8_VERIFY_DET", v, sizeof(v)) == 1 && v[0] == '1';
}

s32 Ticks(u64 clock, u32 i) {
    s32 t = 0;
    std::memcpy(&t, reinterpret_cast<const void*>(clock + kWholeTicks[i]), sizeof(t));
    return t;
}

// Guest thread (the main loop): no logging, no allocation, a small stack.
SSPC_SYSV_ABI u64 ClockProbe(u64 clock, u64 b, u64 c, u64 d, u64 e, u64 f) {
    *reinterpret_cast<u64*>(clock + kLastTsc) = 0; // the measured dt is over the clamp: dt = the clamp, exactly
    const u64 ret = g_orig(clock, b, c, d, e, f);
    const u64 frame = g_frames.fetch_add(1, std::memory_order_relaxed);
    s64 world = g_world.load(std::memory_order_relaxed);
    if (world != kNoWorld) {
        world = world + 1;
        g_world.store(world, std::memory_order_release);
    } else if (g_armed.load(std::memory_order_acquire)) {
        if (Ticks(clock, g_clock_index) != g_armed_ticks) {
            world = 0;
            g_world.store(0, std::memory_order_release);
        }
    } else {
        g_armed_ticks = Ticks(clock, g_clock_index);
    }
    u64 waited = 0;
    if (world != kNoWorld && g_pending.load(std::memory_order_acquire) != 0) {
        // the script pushes what is due by this world frame before the frame goes on
        const u64 t0 = __rdtsc();
        while (g_delivered.load(std::memory_order_acquire) < world && g_pending.load(std::memory_order_acquire) != 0) {
            _mm_pause();
            if (__rdtsc() - t0 > kWaitLimitTicks) {
                g_wait_timeouts.fetch_add(1, std::memory_order_relaxed);
                break;
            }
        }
        waited = __rdtsc() - t0;
    }
    if (frame < kMaxRows) {
        Row& r = g_rows[frame];
        r.frame = frame;
        r.flip = VideoOut::NextFlipId();
        r.wall_us = static_cast<u64>(
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count());
        r.waited_us = waited; // (ticks here; the writer converts)
        std::memcpy(&r.dt, reinterpret_cast<const void*>(clock), sizeof(r.dt));
        for (u32 i = 0; i < 4; ++i) {
            r.clocks[i] = Ticks(clock, i);
        }
        r.world = world;
    }
    return ret;
}

double TscPerUs() {
    LARGE_INTEGER f{}, a{}, b{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const u64 t0 = __rdtsc();
    Sleep(50);
    QueryPerformanceCounter(&b);
    const u64 t1 = __rdtsc();
    const double us = static_cast<double>(b.QuadPart - a.QuadPart) * 1e6 / static_cast<double>(f.QuadPart);
    return static_cast<double>(t1 - t0) / us;
}

void Writer(std::filesystem::path path) {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:R8DetFrames");
    const double per_us = TscPerUs();
    FILE* f = std::fopen(path.string().c_str(), "wb");
    if (f == nullptr) {
        SSPC_LOG(GPU, Error, "R8 deterministic mode: cannot write {}", path.string());
        return;
    }
    std::fprintf(f, "frame,flip,wall_ms,dt_ms,c0,c1,c2,c3,world,waited_ms\n");
    u64 written = 0, last_timeouts = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        // a row is complete once the next frame began (its clock call is the one after)
        const u64 n = std::min<u64>(g_frames.load(std::memory_order_acquire), kMaxRows);
        const u64 upto = n > 0 ? n - 1 : 0;
        for (; written < upto; ++written) {
            const Row& r = g_rows[written];
            std::fprintf(f, "%llu,%llu,%.3f,%.4f,%d,%d,%d,%d,%lld,%.3f\n", static_cast<unsigned long long>(r.frame),
                         static_cast<unsigned long long>(r.flip), static_cast<double>(r.wall_us) / 1000.0,
                         static_cast<double>(r.dt) * 1000.0, r.clocks[0], r.clocks[1], r.clocks[2], r.clocks[3],
                         static_cast<long long>(r.world), static_cast<double>(r.waited_us) / per_us / 1000.0);
        }
        std::fflush(f);
        if (const u64 t = g_wait_timeouts.load(std::memory_order_relaxed); t != last_timeouts) {
            SSPC_LOG(GPU, Warning, "R8 deterministic mode: {} frames went on without the script's input (1 s)", t);
            last_timeouts = t;
        }
        if (written + 1 >= kMaxRows) {
            std::fclose(f);
            SSPC_LOG(GPU, Info, "R8 deterministic mode: the frame log stops at {} rows", kMaxRows);
            return;
        }
    }
}

s64 DueWorldFrame(double seconds) {
    return static_cast<s64>(std::llround((seconds - g_anchor) * kWorldFps));
}

} // namespace

bool Enabled() {
    static const bool on = ReadEnabled();
    return on;
}

bool InstallFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir) {
    if (!Enabled() || eboot_base == 0) {
        return false;
    }
    char v[32] = {};
    if (GetEnvironmentVariableA("SSPC_R8_VERIFY_DET_CLOCK", v, sizeof(v)) > 0) {
        g_clock_index = std::min<u32>(static_cast<u32>(std::strtoul(v, nullptr, 10)), 3);
    }
    if (GetEnvironmentVariableA("SSPC_R8_VERIFY_DET_ANCHOR", v, sizeof(v)) > 0) {
        g_anchor = std::strtod(v, nullptr);
    }
    std::string error;
    if (!Engine::VerifyGuestFunction(eboot_base, kClockSite.function, error)) {
        SSPC_LOG(GPU, Error, "R8 deterministic mode not installed (the game's clock): {}", error);
        return false;
    }
    g_rows = std::make_unique<Row[]>(kMaxRows);
    g_t0 = std::chrono::steady_clock::now();
    void* original = Engine::InstallEntryProbe(eboot_base, kClockSite, reinterpret_cast<const void*>(&ClockProbe), error);
    if (original == nullptr) {
        SSPC_LOG(GPU, Error, "R8 deterministic mode not installed: {}", error);
        return false;
    }
    g_orig = reinterpret_cast<Fn>(original);
    const std::filesystem::path path = logs_dir / "r8_det_frames.csv";
    SSPC_LOG(GPU, Warning,
             "VERIFICATION: SSPC_R8_VERIFY_DET=1 - every frame's dt is the game clock's clamp (0x12a160); input after "
             "the anchor {} s by world frames (clock {}); frame log {}",
             g_anchor, g_clock_index, path.string());
    std::thread(Writer, path).detach();
    return true;
}

double Anchor() {
    return Enabled() && g_orig != nullptr ? g_anchor : -1.0;
}

void ScriptStart(u32 events_after_anchor) {
    if (Anchor() < 0 || events_after_anchor == 0) {
        return;
    }
    g_delivered.store(-1, std::memory_order_release);
    g_pending.store(events_after_anchor, std::memory_order_release);
}

void WaitWorldFrame(double seconds) {
    if (!g_armed.exchange(true, std::memory_order_acq_rel)) {
        SSPC_LOG(GPU, Info, "R8 deterministic mode: anchor reached at game frame {}; looking for the world start",
                 g_frames.load(std::memory_order_relaxed));
    }
    const s64 due = DueWorldFrame(seconds);
    const auto since = std::chrono::steady_clock::now();
    bool said = false;
    for (;;) {
        const s64 w = g_world.load(std::memory_order_acquire);
        if (w != kNoWorld && w >= due) {
            return;
        }
        if (!said && w == kNoWorld && std::chrono::steady_clock::now() - since > std::chrono::seconds(60)) {
            SSPC_LOG(GPU, Warning, "R8 deterministic mode: no world start 60 s after the anchor (clock {} did not "
                                   "advance); the script waits", g_clock_index);
            said = true;
        }
        // nothing is due before `due`: the frames until then need not wait for the script
        if (due - 1 > g_delivered.load(std::memory_order_relaxed)) {
            g_delivered.store(due - 1, std::memory_order_release);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void Delivered(double next_seconds) {
    const u32 left = g_pending.load(std::memory_order_relaxed);
    if (left != 0) {
        g_pending.store(left - 1, std::memory_order_release);
    }
    const s64 w = g_world.load(std::memory_order_acquire);
    if (next_seconds >= g_anchor && DueWorldFrame(next_seconds) <= w) {
        return; // the next event belongs to this world frame too: the frame stays held until it is pushed
    }
    if (w > g_delivered.load(std::memory_order_relaxed)) {
        g_delivered.store(w, std::memory_order_release);
    }
}

u64 GameFrames() {
    return g_frames.load(std::memory_order_relaxed);
}

s64 WorldFrame() {
    return g_world.load(std::memory_order_relaxed);
}

} // namespace SecondSon::Gpu::R8::Verify::Frame
