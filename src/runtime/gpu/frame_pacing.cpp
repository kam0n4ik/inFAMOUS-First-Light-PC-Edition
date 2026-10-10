// SecondSonPC — the flip timeline (P4.2, SSPC-015). See frame_pacing.h.
#include "runtime/gpu/frame_pacing.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include <intrin.h>
#include <windows.h>

#include <fmt/format.h>

#include "base/lifecycle.h"
#include "base/log.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::FramePacing {

std::atomic<bool> g_enabled{false};

namespace {

constexpr u32 kCap = 1u << 16; ///< samples per event (18 min of vblank ticks at 60 Hz)
constexpr size_t kEvents = static_cast<size_t>(Event::Count);

constexpr const char* kNames[] = {"submit_enter", "submit_unblocked", "submitted",  "submit_done",
                                  "cp_flip",      "cp_label_wait",    "cp_label_wait_end", "flip_queued",
                                  "vblank",       "flip_taken",       "presented"};
static_assert(std::size(kNames) == kEvents);

struct Track {
    std::unique_ptr<u64[]> t = std::make_unique<u64[]>(kCap);
    std::atomic<u32> n{0};
};

struct State {
    std::filesystem::path dir;
    u64 start = 0;
    std::array<Track, kEvents> tracks;
    std::unique_ptr<u64[]> cp_cycles = std::make_unique<u64[]>(kCap);
    std::unique_ptr<u16[]> cp_core = std::make_unique<u16[]>(kCap);
};

State* g_state = nullptr;

u32 Count(const State& s, Event e) {
    return std::min(s.tracks[static_cast<size_t>(e)].n.load(std::memory_order_acquire), kCap);
}

double Pct(std::vector<double> v, double q) {
    if (v.empty()) {
        return 0.0;
    }
    std::ranges::sort(v);
    return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5))];
}

double Mean(const std::vector<double>& v) {
    double sum = 0;
    for (const double x : v) {
        sum += x;
    }
    return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
}

void Write() {
    State& s = *g_state;
    const double freq = static_cast<double>(Kernel::Clock::TscFrequency());
    if (freq <= 0) {
        return;
    }
    const auto ms = [&](u64 t) { return t > s.start ? static_cast<double>(t - s.start) * 1000.0 / freq : 0.0; };
    const auto at = [&](Event e, u32 i) { return s.tracks[static_cast<size_t>(e)].t[i]; };
    const Event per_frame[] = {Event::SubmitEnter, Event::SubmitUnblocked, Event::Submitted, Event::SubmitDone,
                               Event::CpFlip,      Event::FlipQueued,      Event::FlipTaken, Event::Presented};
    u32 rows = 0;
    for (const Event e : per_frame) {
        rows = std::max(rows, Count(s, e));
    }
    std::ofstream csv(s.dir / "frame_pacing.csv", std::ios::binary | std::ios::trunc);
    csv << "n";
    for (const Event e : per_frame) {
        csv << ',' << kNames[static_cast<size_t>(e)] << "_ms";
        if (e == Event::CpFlip) {
            csv << ",cp_thread_cpu_ms,cp_core";
        }
    }
    csv << '\n';
    for (u32 i = 0; i < rows; ++i) {
        csv << i;
        for (const Event e : per_frame) {
            csv << ',';
            if (i < Count(s, e)) {
                csv << fmt::format("{:.3f}", ms(at(e, i)));
            }
            if (e == Event::CpFlip) {
                csv << ',';
                if (i < Count(s, e)) {
                    csv << fmt::format("{:.3f},{}", static_cast<double>(s.cp_cycles[i]) * 1000.0 / freq, s.cp_core[i]);
                } else {
                    csv << ',';
                }
            }
        }
        csv << '\n';
    }

    // what the frames waited for
    // a slot taken but not yet written at the exit holds 0: skipped (a thread may still be inside Mark)
    const auto intervals = [&](Event e) {
        std::vector<double> v;
        const u32 n = Count(s, e);
        for (u32 i = 1; i < n; ++i) {
            if (at(e, i) != 0 && at(e, i - 1) != 0) {
                v.push_back(ms(at(e, i)) - ms(at(e, i - 1)));
            }
        }
        return v;
    };
    const auto present = intervals(Event::FlipTaken);
    const auto work = intervals(Event::CpFlip);
    const auto ticks = intervals(Event::Vblank);
    u32 bins[6] = {};
    for (const double d : present) {
        bins[d < 12 ? 0 : d < 20 ? 1 : d < 28 ? 2 : d < 37 ? 3 : d < 45 ? 4 : 5]++;
    }
    std::vector<double> busy, queue_lat, blocked;
    const u32 cp = Count(s, Event::CpFlip);
    const auto both = [&](Event a, Event b, u32 i) { return at(a, i) != 0 && at(b, i) != 0; };
    for (u32 i = 1; i < cp; ++i) {
        if (at(Event::CpFlip, i) == 0 || at(Event::CpFlip, i - 1) == 0) {
            continue;
        }
        const double wall = static_cast<double>(at(Event::CpFlip, i) - at(Event::CpFlip, i - 1));
        if (wall > 0) {
            busy.push_back(static_cast<double>(s.cp_cycles[i] - s.cp_cycles[i - 1]) / wall);
        }
    }
    // pairs by index: the n-th prepared frame is the n-th taken one, the n-th submit enters and leaves (one game thread)
    for (u32 i = 0; i < std::min(Count(s, Event::FlipQueued), Count(s, Event::FlipTaken)); ++i) {
        if (both(Event::FlipQueued, Event::FlipTaken, i)) {
            queue_lat.push_back(ms(at(Event::FlipTaken, i)) - ms(at(Event::FlipQueued, i)));
        }
    }
    for (u32 i = 0; i < std::min(Count(s, Event::SubmitEnter), Count(s, Event::SubmitUnblocked)); ++i) {
        if (both(Event::SubmitEnter, Event::SubmitUnblocked, i)) {
            blocked.push_back(ms(at(Event::SubmitUnblocked, i)) - ms(at(Event::SubmitEnter, i)));
        }
    }
    double label_wait = 0;
    for (u32 i = 0; i < std::min(Count(s, Event::CpLabelWait), Count(s, Event::CpLabelWaitEnd)); ++i) {
        if (both(Event::CpLabelWait, Event::CpLabelWaitEnd, i)) {
            label_wait += ms(at(Event::CpLabelWaitEnd, i)) - ms(at(Event::CpLabelWait, i));
        }
    }
    std::set<u16> cores;
    u32 moves = 0;
    for (u32 i = 0; i < cp; ++i) {
        cores.insert(s.cp_core[i]);
        moves += i > 0 && s.cp_core[i] != s.cp_core[i - 1] ? 1 : 0;
    }
    u32 late = 0, early = 0;
    for (const double d : ticks) {
        late += d > 20.0 ? 1 : 0;
        early += d < 13.0 ? 1 : 0;
    }
    u32 slow_busy = 0;
    for (const double b : busy) {
        slow_busy += b < 0.85 ? 1 : 0;
    }
    const double frames = static_cast<double>(std::max<size_t>(present.size(), 1));
    std::string txt = fmt::format(
        "SecondSonPC flip timeline (--frame-pacing, P4.2 / SSPC-015): {} frames presented, {} vblank ticks ({} without a "
        "frame)\n"
        "Present intervals: mean {:.2f} ms ({:.1f} FPS), P50 {:.2f}, P95 {:.2f}; bins <12 {} | 12-20 {} | 20-28 {} | "
        "28-37 {} | 37-45 {} | >=45 {}; share of ~33 ms frames {:.1f}%\n"
        "GPU thread work per frame (flip marker to flip marker): mean {:.2f} ms, P50 {:.2f}, P95 {:.2f}\n"
        "GPU thread CPU time / wall time between flip markers: mean {:.3f}, P5 {:.3f}; frames below 0.85 (the thread "
        "was off the CPU) {}; cores it ran on at the markers {}, core changes {}\n"
        "The game's submit blocked by the previous frame: mean {:.2f} ms, P95 {:.2f}\n"
        "VideoOut label waits of the GPU thread: {:.2f} ms per frame\n"
        "Flip queue (prepared -> taken by a tick): mean {:.2f} ms, P95 {:.2f}\n"
        "Vblank ticks: mean {:.3f} ms, min {:.3f}, max {:.3f}; late (>20 ms) {}, catch-up (<13 ms) {}\n",
        present.size() + (Count(s, Event::FlipTaken) > 0 ? 1 : 0), Count(s, Event::Vblank),
        Count(s, Event::Vblank) - std::min(Count(s, Event::Vblank), Count(s, Event::FlipTaken)), Mean(present),
        Mean(present) > 0 ? 1000.0 / Mean(present) : 0.0, Pct(present, 0.5), Pct(present, 0.95), bins[0], bins[1],
        bins[2], bins[3], bins[4], bins[5], 100.0 * (bins[3] + bins[4]) / frames, Mean(work), Pct(work, 0.5),
        Pct(work, 0.95), Mean(busy), Pct(busy, 0.05), slow_busy, cores.size(), moves, Mean(blocked), Pct(blocked, 0.95),
        label_wait / frames, Mean(queue_lat), Pct(queue_lat, 0.95), Mean(ticks),
        ticks.empty() ? 0.0 : *std::ranges::min_element(ticks), ticks.empty() ? 0.0 : *std::ranges::max_element(ticks),
        late, early);
    std::ofstream(s.dir / "frame_pacing.txt", std::ios::binary | std::ios::trunc) << txt;
}

void ExitStep() {
    Write();
}

} // namespace

void Enable(const std::filesystem::path& dir) {
    if (g_state != nullptr) {
        return;
    }
    auto* s = new State();
    s->dir = dir;
    s->start = __rdtsc();
    g_state = s;
    g_enabled.store(true, std::memory_order_release);
    Lifecycle::AddExitStep(Lifecycle::Stage::Reports, "frame pacing", &ExitStep);
    SSPC_LOG(GPU, Info, "--frame-pacing: flip timeline -> {}\\frame_pacing.csv, frame_pacing.txt", dir.string());
}

void Mark(Event event) {
    State& s = *g_state;
    const size_t e = static_cast<size_t>(event);
    const u32 i = s.tracks[e].n.fetch_add(1, std::memory_order_relaxed);
    if (i >= kCap) {
        return;
    }
    s.tracks[e].t[i] = __rdtsc();
    if (event == Event::CpFlip) {
        ULONG64 cycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &cycles);
        s.cp_cycles[i] = cycles;
        s.cp_core[i] = static_cast<u16>(GetCurrentProcessorNumber());
    }
}

} // namespace SecondSon::Gpu::FramePacing
