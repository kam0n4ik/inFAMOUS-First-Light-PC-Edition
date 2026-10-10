// SecondSonPC — the process clock (P3.5.3). See clock.h. The TSC measurement is the one time.cpp has made since P2.2
// (the oracle's common/rdtsc.cpp @ 0fe263a: 100 ms against the precise system time, rounded to 100 kHz), moved here
// unchanged; QueryPerformanceCounter is only the reference of the drift report.
#include "runtime/kernel/clock.h"

#include <windows.h>

#include <intrin.h>

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::Kernel::Clock {

namespace {

std::once_flag g_start;
std::atomic<bool> g_started{false};
std::atomic<bool> g_enabled{false};
std::atomic<u64> g_epoch{0};     // TSC at Start()
std::atomic<u64> g_frequency{0}; // the measurement, once it is known
std::shared_future<u64> g_measurement;
LARGE_INTEGER g_qpc_epoch{};
LARGE_INTEGER g_qpc_frequency{};

u64 WallClockNs() {
    constexpr u64 kWindowsEpochToUnixEpoch = 0x19DB1DED53E8000LL;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    const u64 t = (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return 100 * (t - kWindowsEpochToUnixEpoch);
}

u64 EstimateTscFrequency() {
    ReadTsc(); // the first measurement is discarded
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    ReadTsc();
    const u64 start_time = WallClockNs();
    const u64 tsc_start = ReadTsc();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const u64 end_time = WallClockNs();
    const u64 tsc_end = ReadTsc();
    const auto freq = static_cast<u64>(static_cast<unsigned __int128>(tsc_end - tsc_start) * 1'000'000'000ULL /
                                       (end_time - start_time));
    constexpr u64 kNearest = 100'000;
    const u64 mod = freq % kNearest;
    return mod >= kNearest / 2 ? freq - mod + kNearest : freq - mod;
}

u64 SinceEpoch(u64 tsc) {
    const u64 epoch = g_epoch.load(std::memory_order_relaxed);
    return tsc > epoch ? tsc - epoch : 0;
}

bool InvariantTsc() {
    int regs[4] = {};
    __cpuid(regs, static_cast<int>(0x80000000));
    if (static_cast<u32>(regs[0]) < 0x80000007u) {
        return false;
    }
    __cpuid(regs, static_cast<int>(0x80000007));
    return (regs[3] & (1 << 8)) != 0;
}

/// libkernel `u64 sceKernelGetProcessTime(void)`.
SSPC_SYSV_ABI u64 GetProcessTime() {
    return ProcessTimeUs();
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    // Imported by libc.prx.
    {"4J2sUJmuHZQ", "libkernel", "libkernel", "sceKernelGetProcessTime", Fn(&GetProcessTime), nullptr, "clock", false,
     true},
};

} // namespace

u64 TicksTo(u64 ticks, u64 units_per_second, u64 frequency) {
    const u64 seconds = ticks / frequency;
    const u64 rest = ticks % frequency; // < frequency
    // rest * units fits in 64 bits for any TSC below 18 GHz (ns) — else the exact 128-bit product.
    const u64 part = rest <= ~0ull / units_per_second
                         ? rest * units_per_second / frequency
                         : static_cast<u64>(static_cast<unsigned __int128>(rest) * units_per_second / frequency);
    return seconds * units_per_second + part;
}

u64 ReadTsc() {
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 t = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return t;
}

void Start() {
    std::call_once(g_start, [] {
        QueryPerformanceFrequency(&g_qpc_frequency);
        g_epoch.store(ReadTsc(), std::memory_order_relaxed);
        QueryPerformanceCounter(&g_qpc_epoch);
        g_measurement = std::async(std::launch::async, EstimateTscFrequency).share();
        g_started.store(true, std::memory_order_release);
    });
}

bool Started() {
    return g_started.load(std::memory_order_acquire);
}

u64 TscFrequency() {
    u64 f = g_frequency.load(std::memory_order_acquire);
    if (f != 0) {
        return f;
    }
    Start();
    f = g_measurement.get(); // const: concurrent callers may wait on it together
    g_frequency.store(f, std::memory_order_release);
    return f;
}

u64 ProcessTimeCounter() {
    Start();
    return SinceEpoch(ReadTsc());
}

u64 ProcessTimeUs() {
    const u64 f = TscFrequency(); // first: the first call may wait for the measurement
    return TicksTo(SinceEpoch(ReadTsc()), 1'000'000, f);
}

void AdvanceEpoch(u64 ticks) {
    g_epoch.fetch_add(ticks, std::memory_order_relaxed);
}

void Enable() {
    g_enabled.store(true, std::memory_order_release);
}

bool Enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

Report MeasureDrift() {
    Report r;
    r.tsc_frequency = TscFrequency();
    r.invariant_tsc = InvariantTsc();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    const u64 tsc = ReadTsc();
    r.reference_us = TicksTo(static_cast<u64>(now.QuadPart - g_qpc_epoch.QuadPart), 1'000'000,
                             static_cast<u64>(g_qpc_frequency.QuadPart));
    r.process_us = TicksTo(SinceEpoch(tsc), 1'000'000, r.tsc_frequency);
    r.drift_ppm = r.reference_us == 0 ? 0.0
                                      : (static_cast<double>(r.process_us) - static_cast<double>(r.reference_us)) * 1e6 /
                                            static_cast<double>(r.reference_us);
    return r;
}

void LogSummary(const char* when, u64 bridge_tsc_frequency) {
    if (!Started()) {
        return;
    }
    const Report r = MeasureDrift();
    SSPC_LOG(Kernel, Info,
             "process clock {}: owner {}; TSC {} Hz ({}), invariant TSC {}; {:.3f} s since the epoch, "
             "QueryPerformanceCounter {:.3f} s: drift {:+.1f} ppm",
             when, Enabled() ? "SecondSonPC (--native clock)" : "the bridge (SecondSonPC measures the game's TSC frequency)",
             r.tsc_frequency,
             bridge_tsc_frequency == r.tsc_frequency
                 ? std::string("the bridge's: the same")
                 : fmt::format("the bridge's own measurement {} Hz, {:+} Hz", bridge_tsc_frequency,
                               static_cast<s64>(bridge_tsc_frequency) - static_cast<s64>(r.tsc_frequency)),
             r.invariant_tsc ? "yes" : "NO", r.process_us / 1e6, r.reference_us / 1e6, r.drift_ppm);
}

std::span<const Runtime::Export> Exports() {
    return kExports;
}

} // namespace SecondSon::Kernel::Clock
