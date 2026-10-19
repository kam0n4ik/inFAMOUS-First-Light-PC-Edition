// SecondSonPC — which of the engine's mesh draw functions record which render pass (CITY-OPT Phase 3). See
// draw_census.h.
#include "engine/render/draw_census.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "engine/render/render_layout.h"
#include "runtime/gpu/native_videoout.h"

namespace SecondSon::Engine::Render::DrawCensus {

namespace {

namespace L = Layout;

// the callers of drawIndex 0x623c60 (docs/native-reconstruction/NATIVE_RENDER_INTERFACE.md); prologues checked in
// Ghidra (queries/prologue.py: position-independent pushes and SUB RSP, nothing branches into them)
constexpr ProbeSite kSites[4] = {
    {{0x1a1960, 0x225, 0xea1442f922a0a0ecull}, 20}, // mesh draw (context, mesh instance)
    {{0x1a1770, 0x1e4, 0x5bf111e570dcaaedull}, 17}, // mesh draw with a per-draw buffer (context, ..., geometry)
    {{0x448b30, 0x1a1, 0xd03279f8ad801a10ull}, 15},
    {{0x43d480, 0x106, 0xaffc4c64777b1e37ull}, 17},
};
constexpr u32 kPasses = 42; ///< 0..40, then "not a pool context / unknown"

using Fn = SSPC_SYSV_ABI void (*)(u64, u64, u64, u64, u64, u64);
std::array<Fn, 4> g_orig{};
std::array<std::array<std::atomic<u64>, kPasses>, 4> g_calls{};
std::atomic<u64> g_overrides{0};    ///< 0x1a1960 draws whose instance replaces vertex streams (arg 3 + 0x20, & 7)
std::atomic<u64> g_pass14_index{0}; ///< the last mesh index buffer address 0x1a1960 drew in pass 14 (sample)

u32 PassOf(u64 r) {
    // a pool context's key (render_layout.h); a value outside 0..40 counts as unknown
    s32 pass = -1;
    std::memcpy(&pass, reinterpret_cast<const void*>(r + L::kSlotPass), sizeof(pass));
    return pass >= 0 && pass <= 40 ? static_cast<u32>(pass) : kPasses - 1;
}

template <u32 I>
SSPC_SYSV_ABI void Probe(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    // 0x1a1960 (param_1, R, instance), 0x1a1770 and 0x43d480 (param_1, R, ..., geometry): the context is the second
    // argument
    const u32 pass = (I <= 1 || I == 3) && b != 0 ? PassOf(b) : kPasses - 1;
    g_calls[I][pass].fetch_add(1, std::memory_order_relaxed);
    if constexpr (I == 0) {
        // the instance: +0x18 -> -> the mesh (+0x58 indices, +0x60 V# array, +0x6c streams), +0x20 its overrides
        const u64 over = *reinterpret_cast<const u64*>(c + 0x20);
        if (over != 0 && (*reinterpret_cast<const u8*>(over) & 7) != 0) {
            g_overrides.fetch_add(1, std::memory_order_relaxed);
        }
        if (pass == 14) {
            const u64 mesh = **reinterpret_cast<const u64* const*>(c + 0x18);
            g_pass14_index.store(*reinterpret_cast<const u64*>(mesh + 0x58), std::memory_order_relaxed);
        }
    }
    g_orig[I](a, b, c, d, e, f);
}

void Reporter() {
    std::array<std::array<u64, kPasses>, 4> last{};
    u64 last_flip = Gpu::VideoOut::NextFlipId(), last_over = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const u64 flip = Gpu::VideoOut::NextFlipId();
        const double frames = static_cast<double>(std::max<u64>(1, flip - last_flip));
        std::string line;
        for (u32 i = 0; i < 4; ++i) {
            for (u32 p = 0; p < kPasses; ++p) {
                const u64 n = g_calls[i][p].load(std::memory_order_relaxed);
                if (n != last[i][p]) {
                    line += fmt::format(" {:#x}/{}:{:.0f}", kSites[i].function.rva,
                                        p == kPasses - 1 ? -1 : static_cast<s32>(p), (n - last[i][p]) / frames);
                }
                last[i][p] = n;
            }
        }
        const u64 over = g_overrides.load(std::memory_order_relaxed);
        SSPC_LOG(Engine, Info, "draw census/10s ({:.0f} frames): calls per frame by function/pass:{}; 0x1a1960 with "
                               "vertex overrides {:.0f}/frame; a pass-14 mesh's indices at {:#x}",
                 frames, line, (over - last_over) / frames, g_pass14_index.load(std::memory_order_relaxed));
        last_flip = flip;
        last_over = over;
    }
}

} // namespace

bool InstallFromEnvironment(u64 eboot_base) {
    char v[4] = {};
    if (GetEnvironmentVariableA("SSPC_DIAG_DRAW_CENSUS", v, sizeof(v)) != 1 || v[0] != '1') {
        return true;
    }
    std::string error;
    for (const ProbeSite& s : kSites) {
        if (!VerifyGuestFunction(eboot_base, s.function, error)) {
            SSPC_LOG(Engine, Error, "draw census not installed: {}", error);
            return false;
        }
    }
    const std::array<const void*, 4> targets = {reinterpret_cast<const void*>(&Probe<0>),
                                                reinterpret_cast<const void*>(&Probe<1>),
                                                reinterpret_cast<const void*>(&Probe<2>),
                                                reinterpret_cast<const void*>(&Probe<3>)};
    for (u32 i = 0; i < 4; ++i) {
        void* original = InstallEntryProbe(eboot_base, kSites[i], targets[i], error);
        if (original == nullptr) {
            SSPC_LOG(Engine, Critical, "draw census: {}", error);
            std::abort();
        }
        g_orig[i] = reinterpret_cast<Fn>(original);
    }
    SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_DIAG_DRAW_CENSUS=1 - entry probes on the drawIndex callers 0x1a1960, "
                              "0x1a1770, 0x448b30, 0x43d480 (read-only)");
    std::thread(Reporter).detach();
    return true;
}

} // namespace SecondSon::Engine::Render::DrawCensus
