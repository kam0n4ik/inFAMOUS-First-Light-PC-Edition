// SecondSonPC — Native Engine Reconstruction in the running game: the environment switches, installation in the
// loader's window and the reporter thread (see native_function_registry.h and native_measure.h).
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_hooks.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_engine_verify.h"
#include "native_engine/native_function_registry.h"
#include "native_engine/native_measure.h"
#include "native_engine/reconstructed/frame_scratch.h"
#include "runtime/cpu/fs_patcher.h"
#include "runtime/gpu/native_videoout.h"

namespace SecondSon::NativeEngine {

namespace {

std::string Env(const char* name) {
    char v[512] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::stringstream in(s);
    std::string item;
    while (std::getline(in, item, sep)) {
        if (!item.empty()) {
            out.push_back(item);
        }
    }
    return out;
}

inline u64 Rdtsc() {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<u64>(hi) << 32) | lo;
}

double TscPerNs() {
    static const double value = [] {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&a);
        const u64 t0 = Rdtsc();
        Sleep(100);
        QueryPerformanceCounter(&b);
        const u64 t1 = Rdtsc();
        const double ns = static_cast<double>(b.QuadPart - a.QuadPart) * 1e9 / static_cast<double>(f.QuadPart);
        return static_cast<double>(t1 - t0) / ns;
    }();
    return value;
}

struct Installed {
    u32 index;
    Mode mode;
};
std::vector<Installed> g_installed;
std::filesystem::path g_dir;

std::string FunctionLine(u32 i, Mode mode, double seconds, double frames, const u64* prev) {
    const FunctionSpec& f = Function(i);
    const Slot& s = *f.slot;
    const u64 native = Total(i, kCallsNative), original = Total(i, kCallsOriginal), checked = Total(i, kCallsChecked);
    const u64 calls = native + original + (mode == Mode::Verify ? checked : 0);
    const u64 dcalls = calls - prev[0];
    std::string line = fmt::format(
        "{} [{} -> now {}]: {:.0f} calls/s ({:.0f}/frame) native {} original {} checked {}; verified {} mismatches {} "
        "live-diff {} skipped {} fallbacks {}",
        f.key, ModeName(mode), ModeName(static_cast<Mode>(s.mode.load())), static_cast<double>(dcalls) / seconds,
        frames > 0 ? static_cast<double>(dcalls) / frames : 0.0, native, original, checked, s.verified.load(),
        s.mismatches.load(), s.live_diff.load(), s.skipped.load(), s.fallbacks.load());
    if (const u64 n = s.bench_samples.load(); n != 0) {
        const double k = 1.0 / (TscPerNs() * static_cast<double>(n));
        line += fmt::format("; bench {} samples: original {:.1f} ns, native {:.1f} ns, timer {:.1f} ns per call",
                            n, static_cast<double>(s.bench_original_ticks.load()) * k,
                            static_cast<double>(s.bench_native_ticks.load()) * k,
                            static_cast<double>(s.bench_empty_ticks.load()) * k);
    }
    return line;
}

void Reporter() {
    std::vector<u64> prev(FunctionCount(), 0);
    std::vector<u64> prev_measure(Measure::SiteCount() * 3, 0);
    u64 last_flip = Gpu::VideoOut::NextFlipId();
    auto last = std::chrono::steady_clock::now();
    const std::string dir = g_dir.string();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - last).count();
        const u64 flip = Gpu::VideoOut::NextFlipId();
        const double frames = static_cast<double>(flip - last_flip);
        std::string summary = fmt::format("native engine: {:.1f} s, {:.0f} frames, cases recorded {} dropped {}\n",
                                          seconds, frames, CasesRecorded(), CasesDropped());
        for (const Installed& in : g_installed) {
            const std::string line = FunctionLine(in.index, in.mode, seconds, frames, &prev[in.index]);
            SSPC_LOG(Engine, Info, "native engine/10s: {}", line);
            summary += line + "\n";
            prev[in.index] = Total(in.index, kCallsNative) + Total(in.index, kCallsOriginal) +
                             (in.mode == Mode::Verify ? Total(in.index, kCallsChecked) : 0);
        }
        const u32 sites = Measure::SiteCount();
        if (sites > 1) {
            prev_measure.resize(sites * 3, 0);
            const double per_ns = 1.0 / TscPerNs();
            std::string line;
            for (u32 i = 1; i < sites; ++i) { // site 0 is the calibration
                const Measure::SiteTotals t = Measure::Totals(i);
                const u64 dc = t.calls - prev_measure[i * 3], di = t.inclusive_ticks - prev_measure[i * 3 + 1],
                          de = t.exclusive_ticks - prev_measure[i * 3 + 2];
                prev_measure[i * 3] = t.calls;
                prev_measure[i * 3 + 1] = t.inclusive_ticks;
                prev_measure[i * 3 + 2] = t.exclusive_ticks;
                const double f = frames > 0 ? frames : 1;
                line += fmt::format(
                    "\n  {:#x} {}: {:.0f} calls/frame, inclusive {:.1f} us/frame ({:.0f} ns/call), exclusive {:.1f} "
                    "us/frame ({:.0f} ns/call)",
                    t.address - Engine::GuestImageBase(), t.name, static_cast<double>(dc) / f,
                    static_cast<double>(di) * per_ns / 1000.0 / f, dc ? static_cast<double>(di) * per_ns / dc : 0.0,
                    static_cast<double>(de) * per_ns / 1000.0 / f, dc ? static_cast<double>(de) * per_ns / dc : 0.0);
                summary += fmt::format("measure {:#x} {} calls {} inclusive_ns {:.0f} exclusive_ns {:.0f}\n",
                                       t.address - Engine::GuestImageBase(), t.name, t.calls,
                                       static_cast<double>(t.inclusive_ticks) * per_ns,
                                       static_cast<double>(t.exclusive_ticks) * per_ns);
            }
            SSPC_LOG(Engine, Info, "native engine measure/10s ({:.0f} frames, stub {:.0f} ns per call):{}", frames,
                     Measure::StubOverheadTicks() * per_ns, line);
        }
        FlushCases(dir);
        if (FILE* f = std::fopen((g_dir / "summary.txt").string().c_str(), "wb")) {
            std::fwrite(summary.data(), 1, summary.size(), f);
            std::fclose(f);
        }
        last = now;
        last_flip = flip;
    }
}

} // namespace

namespace Measure {

namespace {
struct Candidate {
    Engine::GuestFunction function;
    const char* name;
};
// The candidates of the pilot's choice (docs/NATIVE_ENGINE_RECONSTRUCTION.md §3): bodies and hashes from Ghidra
// (query: tools/native-reconstruction/ghidra/queries/candidates.py), nothing branches into their first 14 bytes.
constexpr Candidate kCandidates[] = {
    {{0x1a2030, 0x1651, 0xd401384d1f123cc8ull}, "mesh pass loop"},
    {{0x1a1960, 0x225, 0xea1442f922a0a0ecull}, "mesh draw"},
    {{0x1a1770, 0x1e4, 0x5bf111e570dcaaedull}, "G-buffer instanced draw"},
    {{0x43d590, 0xdb9, 0xc098a21924b37b67ull}, "shadow pass loop"},
    {{0x43d480, 0x106, 0xaffc4c64777b1e37ull}, "shadow instanced draw"},
    {{0x448640, 0x15f, 0x22c6fc74651c7374ull}, "material bind"},
    {{0x11c890, 0x507, 0xa3cfda22c0d0ac59ull}, "object constants update"},
    {{0x11cde0, 0x459, 0x7195e831ed1fc41cull}, "object constants update (2)"},
    {{0x120280, 0x9d, 0xad95be1456114d2aull}, "draw constants"},
    {{0x61e260, 0xce, 0x82958d25b30182e7ull}, "Gnm initAsRegularBuffer (V#)"},
    {{0x1f0870, 0x73, 0xaf5edf11446f3a57ull}, "frame scratch alloc"},
    {{0x16aa0, 0xc6, 0xf6c333198de2deedull}, "CUE set vertex buffers"},
    {{0x16b70, 0xc6, 0x713c9f7b4b870c4dull}, "CUE set buffer"},
    {{0x14ad0, 0x1184, 0x8bb6e7985e565584ull}, "CUE commit"},
    {{0x623c60, 0x7a, 0x90df1869fca20135ull}, "Gnm drawIndex"},
};
} // namespace

bool InstallFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir) {
    (void)logs_dir;
    SetNearCodeAllocator(&Cpu::AllocateModuleCode);
    const std::string list = Env("SSPC_NATIVE_ENGINE_MEASURE");
    std::vector<u64> only;
    if (list != "1") {
        for (const std::string& a : Split(list, ',')) {
            only.push_back(std::strtoull(a.c_str(), nullptr, 16));
        }
    }
    u32 installed = 0;
    for (const Candidate& c : kCandidates) {
        if (!only.empty() && std::find(only.begin(), only.end(), c.function.rva) == only.end()) {
            continue;
        }
        std::string error;
        if (!Engine::VerifyGuestFunction(eboot_base, c.function, error)) {
            SSPC_LOG(Engine, Warning, "native engine measure: {:#x} {} skipped: {}", c.function.rva, c.name, error);
            continue;
        }
        if (InstallSite(eboot_base + c.function.rva, c.function.size, c.name, error) < 0) {
            SSPC_LOG(Engine, Error, "native engine measure: {:#x} {}: {}", c.function.rva, c.name, error);
            continue;
        }
        ++installed;
    }
    SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_NATIVE_ENGINE_MEASURE - timing stubs on {} engine functions (stub "
                              "{:.0f} ticks per call)",
             installed, StubOverheadTicks());
    return installed != 0;
}

} // namespace Measure

bool InstallFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir) {
    // clones with outside references go into the eboot's trampoline area: the guest's reservation leaves no free
    // memory within +-2 GiB of the module (measured: eboot at 0xe2d0000, 20.10.2026)
    SetNearCodeAllocator(&Cpu::AllocateModuleCode);
    const std::string global = Env("SSPC_NATIVE_ENGINE");
    const std::string measure = Env("SSPC_NATIVE_ENGINE_MEASURE");
    Mode global_mode = Mode::Original;
    if (!global.empty() && !ParseMode(global, global_mode)) {
        SSPC_LOG(Engine, Error, "SSPC_NATIVE_ENGINE={}: want off|1|native|verify|guarded|bench; nothing installed",
                 global);
        return false;
    }
    if (global_mode == Mode::Original && measure.empty() && Env("SSPC_NATIVE_ENGINE_FUNCS").empty()) {
        return true; // the default: the game exactly as it is
    }
    g_dir = logs_dir / "native_engine";
    std::error_code ec;
    std::filesystem::create_directories(g_dir, ec);

    // which functions, in which mode: SSPC_NATIVE_ENGINE_FUNCS=key[:mode],... (default: all, the global mode)
    std::vector<Installed> wanted;
    if (const std::string list = Env("SSPC_NATIVE_ENGINE_FUNCS"); !list.empty()) {
        for (const std::string& item : Split(list, ',')) {
            const size_t colon = item.find(':');
            const std::string key = item.substr(0, colon);
            Mode mode = global_mode;
            if (colon != std::string::npos && !ParseMode(item.substr(colon + 1), mode)) {
                SSPC_LOG(Engine, Error, "SSPC_NATIVE_ENGINE_FUNCS: bad mode in '{}'", item);
                return false;
            }
            bool found = false;
            for (u32 i = 0; i < FunctionCount(); ++i) {
                if (key == Function(i).key) {
                    wanted.push_back({i, mode});
                    found = true;
                }
            }
            if (!found) {
                SSPC_LOG(Engine, Error, "SSPC_NATIVE_ENGINE_FUNCS: no reconstruction '{}'", key);
                return false;
            }
        }
    } else if (global_mode != Mode::Original) {
        for (u32 i = 0; i < FunctionCount(); ++i) {
            wanted.push_back({i, global_mode});
        }
    }
    u32 period = 0;
    if (const std::string p = Env("SSPC_NATIVE_ENGINE_SAMPLE"); !p.empty()) {
        period = static_cast<u32>(std::strtoul(p.c_str(), nullptr, 10));
    }

    // Static recompilation PoC (docs/NATIVE_ENGINE_RECONSTRUCTION.md §9): SSPC_NATIVE_ENGINE_RECOMP=<dll> - the
    // machine-translated FrameScratch::Alloc (tools/native-reconstruction/recomp) replaces the hand-written one as
    // frame_alloc's implementation; the install's self-check and the verify/guarded modes judge it the same way.
    if (const std::string dll = Env("SSPC_NATIVE_ENGINE_RECOMP"); !dll.empty()) {
        HMODULE m = LoadLibraryA(dll.c_str());
        auto* base = m ? reinterpret_cast<u64*>(GetProcAddress(m, "recomp_image_base")) : nullptr;
        auto* fn = m ? GetProcAddress(m, "recomp_1f0870") : nullptr;
        if (base == nullptr || fn == nullptr) {
            SSPC_LOG(Engine, Error, "SSPC_NATIVE_ENGINE_RECOMP={}: not a recompiled module (recomp_image_base, "
                     "recomp_1f0870); frame_alloc keeps the hand-written reconstruction", dll);
        } else {
            *base = eboot_base;
            Reconstructed::FrameScratch::UseImplementation(reinterpret_cast<Reconstructed::FrameScratch::Fn>(fn),
                                                           "static recompilation (P-code -> C++ -> clang)");
            SSPC_LOG(Engine, Warning, "STATIC RECOMPILATION PoC: frame_alloc's implementation is recomp_1f0870 from {}",
                     dll);
        }
    }

    bool all = true;
    for (const Installed& w : wanted) {
        if (w.mode == Mode::Original) {
            continue;
        }
        const FunctionSpec& spec = Function(w.index);
        spec.slot->sample_period = period != 0 ? period : (w.mode == Mode::Bench ? 64 : 256);
        std::string error;
        const auto t0 = std::chrono::steady_clock::now();
        if (!InstallFunction(eboot_base, spec, w.mode, error)) {
            SSPC_LOG(Engine, Error, "native engine: {} NOT installed, the game's own code runs: {}", spec.name, error);
            all = false;
            continue;
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        g_installed.push_back(w);
        SSPC_LOG(Engine, Warning,
                 "NATIVE ENGINE: {} (eboot+{:#x}, {} bytes) replaced by SecondSonPC's reconstruction, mode {} "
                 "(hashes, clone and self-check passed in {:.1f} ms; sample period {})",
                 spec.name, spec.function.rva, spec.function.size, ModeName(w.mode), ms, spec.slot->sample_period);
    }

    if (!measure.empty() && measure != "0") {
        Measure::InstallFromEnvironment(eboot_base, logs_dir);
    }
    if (!g_installed.empty() || Measure::SiteCount() > 1) {
        std::thread(Reporter).detach();
    }
    return all;
}

} // namespace SecondSon::NativeEngine
