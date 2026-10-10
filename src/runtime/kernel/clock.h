// SecondSonPC — the process clock (Phase 3, P3.5.3; docs/PHASE3_P35_KERNEL_HLE.md §4).
//
// The guest's time base is the host's TSC: the game reads it with rdtsc and scales it with sceKernelGetTscFrequency,
// VideoOut stamps flips and vblanks with it (flip_status.tsc) and with the process time (microseconds since the
// process started: flip/vblank process_time, AudioOut's pacing, input timestamps, CLOCK_PROCTIME).
//
// Before P3.5 two clocks measured this TSC independently: SecondSonPC's (sceKernelGetTscFrequency, in the
// background at start-up) and the bridge's Common::NativeClock (its own 100 ms measurement, its own epoch) — the
// frequencies may differ by one 100 kHz step, and the process time was the bridge's. This module owns one
// measurement and one epoch. By default the bridge keeps its NativeClock (the game's TSC frequency is this one's, as
// since P2.2); with --native clock (EXPERIMENTAL) the bridge reads this clock through patch 0024
// (Libraries::Kernel::UseExternalClock): its TSC frequency, process time and process time counter.
//
// Units: ticks of the host TSC (invariant, the same on every core on the CPUs this runtime accepts; rdtsc is fenced
// as the oracle's) and exact integer conversions (no fixed-point rounding, no overflow for any tick count). The TSC
// frequency is the oracle's measurement (100 ms against the system time, rounded to 100 kHz): the value the game has
// seen since P2.2. Monotonic: a later read on any thread never gives an earlier time (the TSC is synchronised; a read
// before the epoch on a core whose TSC lags gives 0, never a wrapped value).
#pragma once

#include <atomic>
#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Kernel::Clock {

/// Exact floor(ticks * units_per_second / frequency) without overflow for any u64 ticks (frequency > 0).
u64 TicksTo(u64 ticks, u64 units_per_second, u64 frequency);

/// A TSC-based clock: an epoch (a TSC value) and a frequency. The process has one (below); self-tests make their own.
class ProcessClock {
public:
    ProcessClock(u64 epoch, u64 frequency) : epoch_{epoch}, frequency_{frequency} {}
    u64 Frequency() const {
        return frequency_;
    }
    u64 Epoch() const {
        return epoch_.load(std::memory_order_relaxed);
    }
    /// TSC ticks since the epoch at `tsc` (0 before it).
    u64 Ticks(u64 tsc) const {
        const u64 epoch = Epoch();
        return tsc > epoch ? tsc - epoch : 0;
    }
    u64 Microseconds(u64 tsc) const {
        return TicksTo(Ticks(tsc), 1'000'000, frequency_);
    }
    u64 Nanoseconds(u64 tsc) const {
        return TicksTo(Ticks(tsc), 1'000'000'000, frequency_);
    }
    /// The bridge's debugger pause: the paused time does not count.
    void AdvanceEpoch(u64 ticks) {
        epoch_.fetch_add(ticks, std::memory_order_relaxed);
    }

private:
    std::atomic<u64> epoch_;
    u64 frequency_;
};

/// rdtsc between load fences (the oracle's FencedRDTSC).
u64 ReadTsc();

/// The epoch (now) and the frequency measurement (100 ms, in the background). Idempotent: the first call counts.
void Start();
bool Started();

/// The TSC frequency (waits for the measurement the first time; starts the clock if nobody did).
u64 TscFrequency();
/// The process time since the epoch: microseconds (sceKernelGetProcessTime), TSC ticks (...Counter).
u64 ProcessTimeUs();
u64 ProcessTimeCounter();
/// The bridge's debugger pause (patch 0024's adapter): the epoch moves forward by the paused ticks.
void AdvanceEpoch(u64 ticks);

/// --native clock: this clock is the process clock of the bridge too (the caller has pointed it here before the
/// bridge's clock would have started).
void Enable();
bool Enabled();

/// The drift of the process time against QueryPerformanceCounter since the epoch (a check of the TSC frequency
/// over the whole session), and the TSC's properties.
struct Report {
    u64 tsc_frequency = 0;
    u64 process_us = 0;    ///< this clock since the epoch
    u64 reference_us = 0;  ///< QueryPerformanceCounter since the epoch
    double drift_ppm = 0;  ///< (process - reference) / reference
    bool invariant_tsc = false;
};
Report MeasureDrift();
/// One line in the log ("process clock at exit: ...").
void LogSummary(const char* when, u64 bridge_tsc_frequency);

/// Group "clock" (experimental): sceKernelGetProcessTime.
std::span<const Runtime::Export> Exports();

} // namespace SecondSon::Kernel::Clock
