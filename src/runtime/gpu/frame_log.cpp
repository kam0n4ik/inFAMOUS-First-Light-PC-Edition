// SecondSonPC — the native backend's frame log (P4.6). See frame_log.h.
#include "runtime/gpu/frame_log.h"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <memory>

#include <fmt/format.h>

#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::FrameLog {

namespace {

constexpr u32 kCap = 1u << 15; ///< frames (27 min at 20 FPS, 9 min at 60)

struct Row {
    u64 game_submit = 0, flip_enter = 0, game_flip = 0, done_enter = 0, done_exit = 0; ///< the game's threads (TSC)
    u32 submits = 0;
    u64 cp_flip = 0, gpu_value = 0, gpu_done = 0, last_unblock = 0; ///< the GPU thread (TSC)
    u64 present = 0;                                                ///< the present thread (TSC)
    u64 idle_wait_ns = 0, idle_waits = 0; ///< the game's running totals of submission lock waits, at the flip marker
    GpuThreadTotals totals;
};

struct State {
    u64 start = 0;
    std::unique_ptr<Row[]> rows;
    std::atomic<u32> game_n{0};    ///< flips the game requested
    std::atomic<u32> present_n{0}; ///< EOP flips presented
    std::atomic<u64> idle_wait_ns{0}, idle_waits{0};
    u32 cp_n = 0, done_n = 0; ///< the GPU thread's
};

State g_state;
std::atomic<bool> g_started{false};

Row* At(u32 i) {
    return i < kCap ? &g_state.rows[i] : nullptr;
}

bool On() {
    return g_started.load(std::memory_order_acquire);
}

} // namespace

void Start() {
    if (On()) {
        return;
    }
    g_state.rows = std::make_unique<Row[]>(kCap);
    g_state.start = Kernel::Clock::ReadTsc();
    g_started.store(true, std::memory_order_release);
}

void GameSubmit() {
    if (!On()) {
        return;
    }
    if (Row* r = At(g_state.game_n.load(std::memory_order_acquire)); r != nullptr) {
        if (r->game_submit == 0) {
            r->game_submit = Kernel::Clock::ReadTsc();
        }
        ++r->submits;
    }
}

void GameFlipEnter() {
    if (!On()) {
        return;
    }
    if (Row* r = At(g_state.game_n.load(std::memory_order_acquire)); r != nullptr) {
        r->flip_enter = Kernel::Clock::ReadTsc();
    }
}

void GameFlip() {
    if (!On()) {
        return;
    }
    const u32 n = g_state.game_n.load(std::memory_order_acquire);
    if (Row* r = At(n); r != nullptr) {
        r->game_flip = Kernel::Clock::ReadTsc();
    }
    g_state.game_n.store(n + 1, std::memory_order_release);
}

void GameSubmitDone(u64 enter_tsc, u64 exit_tsc) {
    if (!On()) {
        return;
    }
    // the frame flipped last (its SubmitDone follows its SubmitAndFlip)
    const u32 n = g_state.game_n.load(std::memory_order_acquire);
    if (Row* r = n > 0 ? At(n - 1) : nullptr; r != nullptr && r->done_enter == 0) {
        r->done_enter = enter_tsc;
        r->done_exit = exit_tsc;
    }
}

void GameIdleWait(u64 ns) {
    if (!On()) {
        return;
    }
    g_state.idle_wait_ns.fetch_add(ns, std::memory_order_relaxed);
    g_state.idle_waits.fetch_add(1, std::memory_order_relaxed);
}

void CpFlip(u64 gpu_value, const GpuThreadTotals& totals, u64 last_unblock_tsc) {
    if (!On()) {
        return;
    }
    if (Row* r = At(g_state.cp_n); r != nullptr) {
        r->cp_flip = Kernel::Clock::ReadTsc();
        r->gpu_value = gpu_value;
        r->last_unblock = last_unblock_tsc;
        r->totals = totals;
        r->idle_wait_ns = g_state.idle_wait_ns.load(std::memory_order_relaxed);
        r->idle_waits = g_state.idle_waits.load(std::memory_order_relaxed);
    }
    ++g_state.cp_n;
}

void GpuCompleted(u64 completed) {
    if (!On()) {
        return;
    }
    while (g_state.done_n < g_state.cp_n) {
        Row* r = At(g_state.done_n);
        if (r == nullptr) {
            g_state.done_n = g_state.cp_n;
            break;
        }
        if (r->gpu_value > completed) {
            break;
        }
        r->gpu_done = Kernel::Clock::ReadTsc();
        ++g_state.done_n;
    }
}

void Presented() {
    if (!On()) {
        return;
    }
    const u32 n = g_state.present_n.fetch_add(1, std::memory_order_acq_rel);
    if (Row* r = At(n); r != nullptr) {
        r->present = Kernel::Clock::ReadTsc();
    }
}

bool Write(const std::filesystem::path& file) {
    if (!On()) {
        return false;
    }
    const double freq = static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
    const auto ms = [&](u64 t) { return t > g_state.start ? static_cast<double>(t - g_state.start) * 1000.0 / freq : 0.0; };
    const auto dms = [](u64 a, u64 b) { return a >= b ? static_cast<double>(a - b) / 1e6 : 0.0; };
    const auto dn = [](u64 a, u64 b) { return a - std::min(a, b); };
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out << "n,game_submit_ms,flip_enter_ms,game_flip_ms,done_enter_ms,done_exit_ms,submits,cp_flip_ms,last_unblock_ms,"
           "gpu_done_ms,present_ms,game_idle_wait_ms,game_idle_waits,cp_ms,record_ms,sync_ms,blocked_label_ms,"
           "blocked_other_ms,empty_ms,gpu_busy_ms,draws,dispatches,batches,blocks\n";
    const u32 rows = std::min(kCap, std::max({g_state.game_n.load(), g_state.cp_n, g_state.present_n.load()}));
    const Row* prev = nullptr;
    for (u32 i = 0; i < rows; ++i) {
        const Row& r = g_state.rows[i];
        out << fmt::format("{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{:.3f},{:.3f},{:.3f},{:.3f}", i, ms(r.game_submit),
                           ms(r.flip_enter), ms(r.game_flip), ms(r.done_enter), ms(r.done_exit), r.submits,
                           ms(r.cp_flip), ms(r.last_unblock), ms(r.gpu_done), ms(r.present));
        if (r.cp_flip != 0) {
            const Row p = prev != nullptr ? *prev : Row{};
            const GpuThreadTotals& t = r.totals;
            const GpuThreadTotals& pt = p.totals;
            out << fmt::format(",{:.3f},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{}\n",
                               dms(r.idle_wait_ns, p.idle_wait_ns), dn(r.idle_waits, p.idle_waits),
                               dms(t.cp_ns, pt.cp_ns), dms(t.record_ns, pt.record_ns), dms(t.sync_ns, pt.sync_ns),
                               dms(t.blocked_label_ns, pt.blocked_label_ns),
                               dms(t.blocked_other_ns, pt.blocked_other_ns), dms(t.empty_ns, pt.empty_ns),
                               dms(t.gpu_busy_ns, pt.gpu_busy_ns), dn(t.draws, pt.draws),
                               dn(t.dispatches, pt.dispatches), dn(t.batches, pt.batches), dn(t.blocks, pt.blocks));
            prev = &r;
        } else {
            out << ",,,,,,,,,,,,,\n";
        }
    }
    return static_cast<bool>(out);
}

} // namespace SecondSon::Gpu::FrameLog
