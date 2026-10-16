// SecondSonPC — FrameBreak FB-3: the game's clock probe. See game_clock_probe.h.
#include "engine/game_clock_probe.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

#include <windows.h>

#include <fmt/format.h>

#include "base/log.h"

namespace SecondSon::Engine::ClockProbe {

namespace {

constexpr u64 kClock = 0xe16290; ///< the clock object (eboot data)

float ReadFloat(u64 at) {
    float v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(at), sizeof(v));
    return v;
}

} // namespace

void StartFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir) {
    char v[4] = {};
    if (GetEnvironmentVariableA("SSPC_FB_CLOCK_PROBE", v, sizeof(v)) != 1 || v[0] != '1' || eboot_base == 0) {
        return;
    }
    const std::filesystem::path path = logs_dir / "game_clock.csv";
    SSPC_LOG(Loader, Warning, "DIAGNOSTIC: SSPC_FB_CLOCK_PROBE - the game's clock (eboot+{:#x}) every 100 ms -> {}",
             kClock, path.string());
    std::thread([base = eboot_base + kClock, path] {
        SetThreadDescription(GetCurrentThread(), L"SecondSonPC:ClockProbe");
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "t_s,dt_ms,c0,c1,c2,c3\n";
        const auto t0 = std::chrono::steady_clock::now();
        for (u64 i = 1;; ++i) {
            std::this_thread::sleep_until(t0 + std::chrono::milliseconds(100 * i));
            const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            // game seconds per clock: whole ticks of 1/1440 s (an s32) + the remainder in seconds (a float)
            const auto clock = [&](u64 whole, u64 frac) {
                s32 ticks = 0;
                std::memcpy(&ticks, reinterpret_cast<const void*>(base + whole), sizeof(ticks));
                return static_cast<double>(ticks) / 1440.0 + static_cast<double>(ReadFloat(base + frac));
            };
            out << fmt::format("{:.4f},{:.4f},{:.6f},{:.6f},{:.6f},{:.6f}\n", t, ReadFloat(base) * 1000.0f,
                               clock(0x04, 0x0c), clock(0x14, 0x1c), clock(0x24, 0x2c), clock(0x34, 0x3c));
            if (i % 10 == 0) {
                out.flush();
            }
        }
    }).detach();
}

} // namespace SecondSon::Engine::ClockProbe
