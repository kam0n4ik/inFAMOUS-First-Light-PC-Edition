// SecondSonPC — GPU command statistics (P4.1.2). See pm4_stats.h.
//
// Threads: the live tables belong to the command processor's thread (OnPacket/OnSpan run only there: the bridge
// creates its command tasks suspended and resumes them on its GpuCommandProcessor thread). Other threads see them
// through a published copy: a reader asks (want_snapshot), the command processor copies at its next frame end or
// when it goes idle. Frame rows are written once and published by a release store of their count. Submissions and
// the renderer's counters are atomics.
#include "runtime/gpu/pm4_stats.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <intrin.h>

#include <fmt/format.h>

#include "base/lifecycle.h"
#include "base/log.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::Pm4Stats {

std::atomic<bool> g_enabled{false};

namespace {

using Pm4::Category;
using Pm4::Engine;
using Pm4::NopMarker;
using Pm4::RegSpace;

constexpr size_t kEngines = static_cast<size_t>(Engine::Count);
constexpr size_t kSpaces = static_cast<size_t>(RegSpace::Count);
constexpr size_t kCategories = static_cast<size_t>(Category::Count);
constexpr size_t kSpans = static_cast<size_t>(Span::Count);
constexpr size_t kCounters = static_cast<size_t>(Counter::Count);
constexpr size_t kMarkers = static_cast<size_t>(NopMarker::Count);
constexpr size_t kUnknownKinds = 32;
constexpr size_t kUnknownWords = 8;
constexpr size_t kMaxSnapshots = 720;

struct Slot {
    u64 count = 0;
    u64 dwords = 0;
    u64 ticks = 0;
    Category category = Category::Unknown;
};

struct Unknown {
    u8 engine = 0;
    u8 type = 0;
    u8 opcode = 0;
    u8 error = 0; ///< Pm4::DecodeError
    u32 words_kept = 0;
    u64 count = 0;
    u32 words[kUnknownWords] = {};
};

/// Cumulative totals a frame row is the difference of.
struct Totals {
    u64 packets = 0, dwords = 0, busy = 0, draws = 0, dispatch_dcb = 0, dispatch_asc = 0, ctx_regs = 0, sh_regs = 0,
        uconfig_regs = 0, ib = 0, waits = 0, eops = 0, dma_bytes = 0;
    u64 span[kSpans] = {};
    u64 category[kCategories] = {};
};

/// Everything the command processor's thread counts.
struct Live {
    std::array<std::array<Slot, 256>, kEngines> ops{};
    Slot loop{}; ///< between a span's end and the next packet (the command processor's own loop)
    u64 type0[kEngines] = {}, type2[kEngines] = {}, decode_errors[kEngines] = {}, predicated = 0;
    std::array<std::array<u64, Pm4::kRegSpaceSize>, kSpaces> regs{};
    u64 regs_beyond[kSpaces] = {};
    u64 nop[kMarkers] = {};
    u64 event_write[64] = {}, eop_event[64] = {}, eos_event[64] = {}, release_event[64] = {};
    u64 eop_int[4] = {}, eop_data[8] = {};
    u64 dma[4][4] = {};
    u64 wait_function[8] = {}, wait_memory = 0, wait_register = 0;
    u64 ib_dwords[kEngines] = {};
    u64 draw_indices = 0, instanced = 0, dispatch_groups = 0, dispatch_disabled = 0;
    u64 span_count[kSpans] = {};
    std::array<Unknown, kUnknownKinds> unknown{};
    u32 unknown_kinds = 0;
    u64 unknown_dropped = 0;
    Totals totals{};
};

struct FrameRow {
    u64 t = 0;     ///< ticks since Enable at the flip marker
    u64 ticks = 0; ///< since the previous flip marker
    Totals d{};
    u64 submits_dcb = 0, submits_asc = 0;
    u64 counters[kCounters] = {};
};

struct SnapshotRow {
    u32 t_s = 0;
    u8 engine = 0;
    u8 opcode = 0;
    u64 count = 0, dwords = 0, ticks = 0;
};

struct State {
    Options options;
    u64 start_tsc = 0;
    bool test = false; ///< a self-test's capture: no command thread, the caller's thread publishes

    // command processor's thread
    Live live{};
    Slot* cur = nullptr;
    u64 cur_ts = 0;
    Span active = Span::Count;
    u64 span_start = 0;
    u64 last_flip = 0;
    Totals at_flip{};
    u64 submits_at_flip[2] = {};
    u64 counters_at_flip[kCounters] = {};
    u64 frames_seen = 0;
    const void* last_pipeline = nullptr;
    bool dirty = false;           ///< packets since the last publication
    std::atomic<bool> idle{false}; ///< the command thread waits for work and its tables are published

    // frame rows: written by the command processor, count published
    std::vector<FrameRow> frames;
    std::atomic<u64> frames_written{0};
    std::atomic<u64> frames_dropped{0};

    // any thread
    std::atomic<u64> submits[kEngines] = {};
    std::atomic<u64> submit_dwords[kEngines] = {};
    std::atomic<u64> submit_ccb_dwords{0};
    std::atomic<u64> counters[kCounters] = {};

    // published copy
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> want_snapshot{false};
    u64 published_serial = 0;
    u64 published_tsc = 0;
    std::unique_ptr<Live> published = std::make_unique<Live>();
    std::vector<SnapshotRow> snapshots;
    u32 snapshot_count = 0;

    std::mutex write_mutex;
    std::jthread reporter;
};

State* g_state = nullptr;

bool WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(f);
}

void RecordUnknown(Live& l, Engine engine, u8 type, u8 opcode, Pm4::DecodeError error, const u32* stream,
                   size_t available) {
    for (u32 i = 0; i < l.unknown_kinds; ++i) {
        auto& u = l.unknown[i];
        if (u.engine == static_cast<u8>(engine) && u.type == type && u.opcode == opcode &&
            u.error == static_cast<u8>(error)) {
            ++u.count;
            return;
        }
    }
    if (l.unknown_kinds == kUnknownKinds) {
        ++l.unknown_dropped;
        return;
    }
    auto& u = l.unknown[l.unknown_kinds++];
    u.engine = static_cast<u8>(engine);
    u.type = type;
    u.opcode = opcode;
    u.error = static_cast<u8>(error);
    u.count = 1;
    u.words_kept = static_cast<u32>(std::min(available, kUnknownWords));
    for (u32 i = 0; i < u.words_kept; ++i) {
        u.words[i] = stream[i];
    }
}

void Publish(State& s, u64 now) {
    std::scoped_lock lock{s.mutex};
    *s.published = s.live;
    s.published_tsc = now;
    ++s.published_serial;
    s.want_snapshot.store(false, std::memory_order_relaxed);
    s.cv.notify_all();
}

void CloseFrame(State& s, u64 now) {
    ++s.frames_seen;
    if (s.last_flip != 0) {
        const u64 n = s.frames_written.load(std::memory_order_relaxed);
        if (n < s.frames.size()) {
            auto& r = s.frames[n];
            const auto& t = s.live.totals;
            const auto& p = s.at_flip;
            r.t = now - s.start_tsc;
            r.ticks = now - s.last_flip;
            r.d.packets = t.packets - p.packets;
            r.d.dwords = t.dwords - p.dwords;
            r.d.busy = t.busy - p.busy;
            r.d.draws = t.draws - p.draws;
            r.d.dispatch_dcb = t.dispatch_dcb - p.dispatch_dcb;
            r.d.dispatch_asc = t.dispatch_asc - p.dispatch_asc;
            r.d.ctx_regs = t.ctx_regs - p.ctx_regs;
            r.d.sh_regs = t.sh_regs - p.sh_regs;
            r.d.uconfig_regs = t.uconfig_regs - p.uconfig_regs;
            r.d.ib = t.ib - p.ib;
            r.d.waits = t.waits - p.waits;
            r.d.eops = t.eops - p.eops;
            r.d.dma_bytes = t.dma_bytes - p.dma_bytes;
            for (size_t i = 0; i < kSpans; ++i) {
                r.d.span[i] = t.span[i] - p.span[i];
            }
            for (size_t i = 0; i < kCategories; ++i) {
                r.d.category[i] = t.category[i] - p.category[i];
            }
            const u64 sd = s.submits[static_cast<size_t>(Engine::Draw)].load(std::memory_order_relaxed);
            const u64 sa = s.submits[static_cast<size_t>(Engine::Compute)].load(std::memory_order_relaxed);
            r.submits_dcb = sd - s.submits_at_flip[0];
            r.submits_asc = sa - s.submits_at_flip[1];
            for (size_t i = 0; i < kCounters; ++i) {
                const u64 c = s.counters[i].load(std::memory_order_relaxed);
                r.counters[i] = c - s.counters_at_flip[i];
            }
            s.frames_written.store(n + 1, std::memory_order_release);
        } else {
            s.frames_dropped.fetch_add(1, std::memory_order_relaxed);
        }
    }
    s.last_flip = now;
    s.at_flip = s.live.totals;
    s.submits_at_flip[0] = s.submits[static_cast<size_t>(Engine::Draw)].load(std::memory_order_relaxed);
    s.submits_at_flip[1] = s.submits[static_cast<size_t>(Engine::Compute)].load(std::memory_order_relaxed);
    for (size_t i = 0; i < kCounters; ++i) {
        s.counters_at_flip[i] = s.counters[i].load(std::memory_order_relaxed);
    }
    if (s.want_snapshot.load(std::memory_order_relaxed)) {
        Publish(s, now);
    }
}

void Charge(State& s, u64 now) {
    if (s.cur != nullptr && s.active == Span::Count) {
        const u64 d = now - s.cur_ts;
        s.cur->ticks += d;
        s.live.totals.busy += d;
        s.live.totals.category[static_cast<size_t>(s.cur->category)] += d;
    }
    s.cur_ts = now;
}

void Body(State& s, Engine engine, const Pm4::Packet& p, const u32* w) {
    Live& l = s.live;
    // w[0] is the header; the packet's body is w[1 .. p.dwords - 1] (Decode checked it is readable).
    const u32 body = p.dwords - 1;
    RegSpace space{};
    if (Pm4::SetRegSpace(p.opcode, space) && body >= 1) {
        const u32 offset = w[1] & 0xFFFF;
        const u32 n = body - 1;
        auto& regs = l.regs[static_cast<size_t>(space)];
        for (u32 i = 0; i < n; ++i) {
            if (offset + i < Pm4::kRegSpaceSize) {
                ++regs[offset + i];
            } else {
                ++l.regs_beyond[static_cast<size_t>(space)];
            }
        }
        if (space == RegSpace::Context) {
            l.totals.ctx_regs += n;
        } else if (space == RegSpace::Sh) {
            l.totals.sh_regs += n;
        } else if (space == RegSpace::Uconfig) {
            l.totals.uconfig_regs += n;
        }
        return;
    }
    switch (p.opcode) {
    case Pm4::Op::Nop: {
        const NopMarker m = Pm4::ClassifyNop({w, p.dwords});
        ++l.nop[static_cast<size_t>(m)];
        if (m == NopMarker::PatchedFlip && engine == Engine::Draw) {
            CloseFrame(s, s.cur_ts);
        }
        break;
    }
    case Pm4::Op::DrawIndex2:
        ++l.totals.draws;
        if (body >= 4) {
            l.draw_indices += w[4];
        }
        break;
    case Pm4::Op::DrawIndexOffset2:
        ++l.totals.draws;
        if (body >= 3) {
            l.draw_indices += w[3];
        }
        break;
    case Pm4::Op::DrawIndexAuto:
        ++l.totals.draws;
        if (body >= 1) {
            l.draw_indices += w[1];
        }
        break;
    case Pm4::Op::DrawIndirect:
    case Pm4::Op::DrawIndexIndirect:
    case Pm4::Op::DrawIndirectMulti:
    case Pm4::Op::DrawIndexIndirectMulti:
    case Pm4::Op::DrawIndexMultiAuto:
    case Pm4::Op::DrawIndexIndirectCountMulti:
        ++l.totals.draws;
        break;
    case Pm4::Op::NumInstances:
        if (body >= 1 && w[1] > 1) {
            ++l.instanced;
        }
        break;
    case Pm4::Op::DispatchDirect:
        if (engine == Engine::Compute) {
            ++l.totals.dispatch_asc;
        } else {
            ++l.totals.dispatch_dcb;
        }
        if (body >= 4) {
            const u64 groups = u64{w[1]} * w[2] * w[3];
            l.dispatch_groups += groups;
            if ((w[4] & 1) == 0) {
                ++l.dispatch_disabled;
            }
        }
        break;
    case Pm4::Op::DispatchIndirect:
        if (engine == Engine::Compute) {
            ++l.totals.dispatch_asc;
        } else {
            ++l.totals.dispatch_dcb;
        }
        break;
    case Pm4::Op::EventWrite:
        if (body >= 1) {
            ++l.event_write[Pm4::EventType(w[1])];
        }
        break;
    case Pm4::Op::EventWriteEop:
        ++l.totals.eops;
        if (body >= 3) {
            ++l.eop_event[Pm4::EventType(w[1])];
            ++l.eop_int[Pm4::EopIntSel(w[3])];
            ++l.eop_data[Pm4::EopDataSel(w[3])];
        }
        break;
    case Pm4::Op::EventWriteEos:
        if (body >= 1) {
            ++l.eos_event[Pm4::EventType(w[1])];
        }
        break;
    case Pm4::Op::ReleaseMem:
        ++l.totals.eops;
        if (body >= 1) {
            ++l.release_event[Pm4::EventType(w[1])];
        }
        break;
    case Pm4::Op::DmaData:
        if (body >= 6) {
            ++l.dma[Pm4::DmaSrcSel(w[1])][Pm4::DmaDstSel(w[1])];
            l.totals.dma_bytes += Pm4::DmaBytes(w[6]);
        }
        break;
    case Pm4::Op::WaitRegMem:
        ++l.totals.waits;
        if (body >= 1) {
            ++l.wait_function[Pm4::WaitFunction(w[1])];
            if (Pm4::WaitOnMemory(w[1])) {
                ++l.wait_memory;
            } else {
                ++l.wait_register;
            }
        }
        break;
    case Pm4::Op::IndirectBuffer:
    case Pm4::Op::IndirectBufferConst:
        ++l.totals.ib;
        if (body >= 3) {
            l.ib_dwords[static_cast<size_t>(engine)] += Pm4::IndirectSize(w[3]);
        }
        break;
    default:
        break;
    }
}

double Ms(u64 ticks, u64 freq) {
    return freq == 0 ? 0.0 : static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq);
}

bool RequestSnapshot(State& s, std::chrono::milliseconds timeout) {
    if (s.test) {
        Publish(s, __rdtsc()); // the self-test's thread is the command thread
        return true;
    }
    std::unique_lock lock{s.mutex};
    if (s.idle.load(std::memory_order_acquire)) {
        return true; // published when it went idle, nothing counted since
    }
    const u64 serial = s.published_serial;
    s.want_snapshot.store(true, std::memory_order_relaxed);
    return s.cv.wait_for(lock, timeout, [&] { return s.published_serial != serial; });
}

void TakeSnapshotRows(State& s, u32 t_s) {
    std::scoped_lock lock{s.mutex};
    if (s.snapshot_count >= kMaxSnapshots) {
        return;
    }
    ++s.snapshot_count;
    for (size_t e = 0; e < kEngines; ++e) {
        for (size_t op = 0; op < 256; ++op) {
            const Slot& slot = s.published->ops[e][op];
            if (slot.count != 0) {
                s.snapshots.push_back(SnapshotRow{t_s, static_cast<u8>(e), static_cast<u8>(op), slot.count,
                                                  slot.dwords, slot.ticks});
            }
        }
    }
}

void ReportLoop(std::stop_token stop) {
    State& s = *g_state;
    const auto t0 = std::chrono::steady_clock::now();
    u64 next_report = s.options.report_seconds;
    u64 next_snapshot = s.options.snapshot_seconds;
    u64 frames_before = 0;
    Totals before{};
    u64 tsc_before = __rdtsc();
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const u64 elapsed =
            static_cast<u64>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - t0)
                                 .count());
        const bool report = s.options.report_seconds != 0 && elapsed >= next_report;
        const bool snapshot = s.options.snapshot_seconds != 0 && elapsed >= next_snapshot;
        if (!report && !snapshot) {
            continue;
        }
        const bool fresh = RequestSnapshot(s, std::chrono::milliseconds(1000));
        if (snapshot) {
            next_snapshot += s.options.snapshot_seconds;
            if (fresh) {
                TakeSnapshotRows(s, static_cast<u32>(elapsed));
            }
        }
        if (!report) {
            continue;
        }
        next_report += s.options.report_seconds;
        Totals now{};
        u64 tsc_now = 0;
        {
            std::scoped_lock lock{s.mutex};
            now = s.published->totals;
            tsc_now = s.published_tsc;
        }
        const u64 frames = s.frames_written.load(std::memory_order_acquire) + s.frames_dropped.load();
        const u64 df = frames - frames_before;
        const u64 freq = Kernel::Clock::TscFrequency();
        const double window = tsc_now > tsc_before ? Ms(tsc_now - tsc_before, freq) : 0.0;
        const auto pct = [&](u64 ticks) { return window > 0 ? 100.0 * Ms(ticks, freq) / window : 0.0; };
        const auto per = [&](u64 a, u64 b) { return df != 0 ? static_cast<double>(a - b) / df : 0.0; };
        const u64 draw_ticks = now.category[static_cast<size_t>(Category::Draw)] -
                               before.category[static_cast<size_t>(Category::Draw)];
        SSPC_LOG(GPU, Info,
                 "gpu stats {} s: {} frames ({:.1f}/s){}, per frame {:.0f} packets, {:.0f} draws, {:.1f}+{:.1f} "
                 "dispatches (dcb+asc), {:.0f} context + {:.0f} sh registers; command thread busy {:.0f}% (draw "
                 "packets {:.0f}%, {:.1f} us/draw), idle {:.0f}%, VideoOut wait {:.0f}%, flush {:.0f}%, queued "
                 "commands {:.0f}%",
                 elapsed, df, window > 0 ? df * 1000.0 / window : 0.0, fresh ? "" : " (stale)",
                 per(now.packets, before.packets), per(now.draws, before.draws),
                 per(now.dispatch_dcb, before.dispatch_dcb), per(now.dispatch_asc, before.dispatch_asc),
                 per(now.ctx_regs, before.ctx_regs), per(now.sh_regs, before.sh_regs), pct(now.busy - before.busy),
                 pct(draw_ticks),
                 now.draws > before.draws ? Ms(draw_ticks, freq) * 1000.0 / static_cast<double>(now.draws - before.draws)
                                          : 0.0,
                 pct(now.span[0] - before.span[0]), pct(now.span[1] - before.span[1]),
                 pct(now.span[2] - before.span[2]), pct(now.span[3] - before.span[3]));
        frames_before = frames;
        before = now;
        tsc_before = tsc_now;
    }
}

void ExitStep() {
    WriteNow();
}

void Init(const Options& options) {
    auto* s = new State();
    s->options = options;
    s->start_tsc = __rdtsc();
    s->frames.resize(options.max_frames);
    for (size_t e = 0; e < kEngines; ++e) {
        for (size_t op = 0; op < 256; ++op) {
            s->live.ops[e][op].category = Pm4::Classify(static_cast<u8>(op));
        }
    }
    s->live.loop.category = Category::Control;
    g_state = s;
}

std::string RegLine(const Live& l, RegSpace space, size_t top) {
    struct Entry {
        u32 reg;
        u64 count;
    };
    std::vector<Entry> v;
    const auto& regs = l.regs[static_cast<size_t>(space)];
    for (u32 i = 0; i < Pm4::kRegSpaceSize; ++i) {
        if (regs[i] != 0) {
            v.push_back({i, regs[i]});
        }
    }
    std::ranges::sort(v, [](const Entry& a, const Entry& b) { return a.count > b.count; });
    std::string out = fmt::format("  {:8} {} distinct registers written, beyond the first {}: {} writes; top:",
                                  Pm4::RegSpaceName(space), v.size(), Pm4::kRegSpaceSize,
                                  l.regs_beyond[static_cast<size_t>(space)]);
    for (size_t i = 0; i < std::min(top, v.size()); ++i) {
        out += fmt::format("{} {:#06x}={}", i % 8 == 0 ? "\n   " : "",
                           Pm4::kRegBase[static_cast<size_t>(space)] + v[i].reg, v[i].count);
    }
    return out + "\n";
}

std::string Histogram(const char* title, const u64* v, size_t n) {
    std::string out = fmt::format("  {}:", title);
    bool any = false;
    for (size_t i = 0; i < n; ++i) {
        if (v[i] != 0) {
            out += fmt::format(" {}={}", i, v[i]);
            any = true;
        }
    }
    return out + (any ? "\n" : " none\n");
}

} // namespace

void Enable(const Options& options) {
    if (g_state != nullptr) {
        return;
    }
    Init(options);
    g_state->reporter = std::jthread(&ReportLoop);
    g_enabled.store(true, std::memory_order_release); // after the state exists: the command thread may run already
    if (options.exit_step) {
        Lifecycle::AddExitStep(Lifecycle::Stage::Reports, "gpu command statistics", &ExitStep);
    }
    SSPC_LOG(GPU, Info,
             "--gpu-stats: PM4 statistics on (diagnostic capture: every packet decoded and timed; not for FPS "
             "comparisons); files {}\\gpu_stats.txt, gpu_frames.csv (<= {} frames), gpu_stats_windows.csv",
             options.dir.string(), options.max_frames);
}

void OnPacket(Engine engine, const u32* stream, size_t available) {
    State& s = *g_state;
    const u64 now = __rdtsc();
    Charge(s, now);
    s.dirty = true;
    Live& l = s.live;
    const size_t e = static_cast<size_t>(engine);
    Pm4::Packet p;
    Pm4::DecodeError error{};
    if (stream == nullptr || !Pm4::Decode({stream, available}, p, error)) {
        ++l.decode_errors[e];
        RecordUnknown(l, engine, stream != nullptr && available != 0 ? static_cast<u8>(stream[0] >> 30) : 0, 0,
                      error, stream, stream != nullptr ? available : 0);
        s.cur = &l.loop;
        return;
    }
    ++l.totals.packets;
    l.totals.dwords += p.dwords;
    if (p.type == 2) {
        ++l.type2[e];
        s.cur = &l.loop;
        return;
    }
    if (p.type == 0) {
        ++l.type0[e];
        RecordUnknown(l, engine, 0, 0, error, stream, p.dwords);
        s.cur = &l.loop;
        return;
    }
    Slot& slot = l.ops[e][p.opcode];
    ++slot.count;
    slot.dwords += p.dwords;
    s.cur = &slot;
    if (p.predicate) {
        ++l.predicated;
    }
    if (slot.category == Category::Unknown) {
        RecordUnknown(l, engine, 3, p.opcode, error, stream, p.dwords);
        return;
    }
    Body(s, engine, p, stream);
}

void OnSpan(Span span, bool begin) {
    State& s = *g_state;
    const u64 now = __rdtsc();
    const size_t i = static_cast<size_t>(span);
    if (begin) {
        Charge(s, now);
        if (s.active != Span::Count) { // not nested in practice; close the open one
            s.live.totals.span[static_cast<size_t>(s.active)] += now - s.span_start;
        }
        s.active = span;
        s.span_start = now;
        ++s.live.span_count[i];
        if (span == Span::Idle) {
            // a reader asking while the thread sleeps gets this publication (RequestSnapshot checks `idle`)
            if (s.dirty || s.want_snapshot.load(std::memory_order_relaxed)) {
                Publish(s, now);
                s.dirty = false;
            }
            s.idle.store(true, std::memory_order_release);
        }
        return;
    }
    if (span == Span::Idle) {
        s.idle.store(false, std::memory_order_release);
    }
    if (s.active == span) {
        s.live.totals.span[i] += now - s.span_start;
        s.active = Span::Count;
    }
    s.cur_ts = now;
    if (span == Span::Idle || span == Span::Flush) {
        s.cur = &s.live.loop;
    }
}

void OnSubmit(Engine engine, size_t dwords, size_t ccb_dwords) {
    State& s = *g_state;
    const size_t e = static_cast<size_t>(engine);
    s.submits[e].fetch_add(1, std::memory_order_relaxed);
    s.submit_dwords[e].fetch_add(dwords, std::memory_order_relaxed);
    s.submit_ccb_dwords.fetch_add(ccb_dwords, std::memory_order_relaxed);
}

void Count(Counter counter, u64 n) {
    g_state->counters[static_cast<size_t>(counter)].fetch_add(n, std::memory_order_relaxed);
}

void OnPipelineBind(const void* pipeline) {
    State& s = *g_state;
    Count(Counter::PipelineBinds);
    if (pipeline != s.last_pipeline) {
        s.last_pipeline = pipeline;
        Count(Counter::PipelineChanges);
    }
}

bool WriteNow() {
    State* sp = g_state;
    if (sp == nullptr) {
        return false;
    }
    State& s = *sp;
    std::scoped_lock write_lock{s.write_mutex};
    const bool fresh = RequestSnapshot(s, std::chrono::milliseconds(500));
    const u64 freq = Kernel::Clock::TscFrequency();
    Live l;
    u64 tsc = 0;
    std::vector<SnapshotRow> snapshots;
    {
        std::scoped_lock lock{s.mutex};
        l = *s.published;
        tsc = s.published_tsc;
        snapshots = s.snapshots;
    }
    const u64 frames = s.frames_written.load(std::memory_order_acquire);
    const double run_ms = Ms(tsc > s.start_tsc ? tsc - s.start_tsc : 0, freq);
    bool ok = true;

    // gpu_stats.txt
    std::string t;
    t += fmt::format("SecondSonPC GPU command statistics (--gpu-stats, P4.1.2): {:.1f} s captured, {} frames "
                     "(flip markers) recorded{}, {} frames beyond the row limit; snapshot {}\n",
                     run_ms / 1000.0, frames, frames >= s.frames.size() ? " (limit reached)" : "",
                     s.frames_dropped.load(), fresh ? "current" : "stale (the command thread did not answer)");
    t += fmt::format("TSC {} Hz. Times: the command processor's thread from one packet to the next (a draw packet "
                     "includes the renderer's work for it).\n\n",
                     freq);
    const auto& tot = l.totals;
    const double fr = frames != 0 ? static_cast<double>(frames) : 1.0;
    t += fmt::format("Totals: {} packets, {} dwords, {} draws, {}+{} dispatches (dcb+asc, {} groups, {} with the "
                     "initiator off), {} indices, {} instanced draw states, {} indirect buffers ({} / {} / {} dwords "
                     "dcb/ccb/asc), {} waits ({} memory, {} register), {} end-of-pipe/release writes, {} DMA bytes, "
                     "{} predicated packets\n",
                     tot.packets, tot.dwords, tot.draws, tot.dispatch_dcb, tot.dispatch_asc, l.dispatch_groups,
                     l.dispatch_disabled, l.draw_indices, l.instanced, tot.ib, l.ib_dwords[0], l.ib_dwords[1],
                     l.ib_dwords[2], tot.waits, l.wait_memory, l.wait_register, tot.eops, tot.dma_bytes, l.predicated);
    t += fmt::format("Per recorded frame (average over the whole capture): {:.0f} packets, {:.0f} draws, {:.1f} "
                     "dispatches, {:.0f} context / {:.0f} sh / {:.0f} uconfig register writes\n",
                     tot.packets / fr, tot.draws / fr, (tot.dispatch_dcb + tot.dispatch_asc) / fr, tot.ctx_regs / fr,
                     tot.sh_regs / fr, tot.uconfig_regs / fr);
    t += fmt::format("Command thread: packets {:.1f} ms, idle {:.1f} ms ({} waits), VideoOut label waits {:.1f} ms "
                     "({}), flush {:.1f} ms ({}), queued commands {:.1f} ms ({}), loop {:.1f} ms\n",
                     Ms(tot.busy, freq), Ms(tot.span[0], freq), l.span_count[0], Ms(tot.span[1], freq),
                     l.span_count[1], Ms(tot.span[2], freq), l.span_count[2], Ms(tot.span[3], freq),
                     l.span_count[3], Ms(l.loop.ticks, freq));
    t += "  by category:";
    for (size_t c = 0; c < kCategories; ++c) {
        if (tot.category[c] != 0) {
            t += fmt::format(" {} {:.1f} ms", Pm4::CategoryName(static_cast<Category>(c)), Ms(tot.category[c], freq));
        }
    }
    t += "\n\nSubmissions: ";
    for (size_t e = 0; e < kEngines; ++e) {
        t += fmt::format("{} {} ({} dwords); ", Pm4::EngineName(static_cast<Engine>(e)), s.submits[e].load(),
                         s.submit_dwords[e].load());
    }
    t += fmt::format("ccb with them {} dwords\n", s.submit_ccb_dwords.load());
    static constexpr const char* kCounterNames[] = {"renderer draws",      "renderer dispatches",
                                                    "pipeline binds",      "pipeline changes",
                                                    "pipelines created",   "programs translated",
                                                    "render pass begins",  "barriers"};
    static_assert(std::size(kCounterNames) == kCounters);
    t += "Renderer:";
    for (size_t i = 0; i < kCounters; ++i) {
        t += fmt::format(" {} {};", kCounterNames[i], s.counters[i].load());
    }
    t += "\n\nOpcodes (engine, opcode, name, category, packets, per frame, dwords, total ms, us per packet):\n";
    struct Row {
        size_t e;
        size_t op;
        const Slot* slot;
    };
    std::vector<Row> rows;
    for (size_t e = 0; e < kEngines; ++e) {
        for (size_t op = 0; op < 256; ++op) {
            if (l.ops[e][op].count != 0) {
                rows.push_back({e, op, &l.ops[e][op]});
            }
        }
    }
    std::ranges::sort(rows, [](const Row& a, const Row& b) { return a.slot->count > b.slot->count; });
    for (const auto& r : rows) {
        const auto name = Pm4::OpcodeName(static_cast<u8>(r.op));
        t += fmt::format("  {} {:#04x} {:32} {:10} {:>12} {:>10.1f} {:>14} {:>10.1f} {:>8.2f}\n",
                         Pm4::EngineName(static_cast<Engine>(r.e)), r.op, name.empty() ? "(unknown)" : name,
                         Pm4::CategoryName(r.slot->category), r.slot->count, r.slot->count / fr, r.slot->dwords,
                         Ms(r.slot->ticks, freq), Ms(r.slot->ticks, freq) * 1000.0 / r.slot->count);
    }
    t += fmt::format("  type-2 (padding): dcb {} ccb {} asc {}; type-0: dcb {} ccb {} asc {}; decode errors: dcb {} "
                     "ccb {} asc {}\n",
                     l.type2[0], l.type2[1], l.type2[2], l.type0[0], l.type0[1], l.type0[2], l.decode_errors[0],
                     l.decode_errors[1], l.decode_errors[2]);
    t += "\nNOP payloads:";
    for (size_t m = 0; m < kMarkers; ++m) {
        if (l.nop[m] != 0) {
            t += fmt::format(" {} {};", Pm4::NopMarkerName(static_cast<NopMarker>(m)), l.nop[m]);
        }
    }
    t += "\n\nRegisters written (register index = space base + offset):\n";
    for (size_t sp = 0; sp < kSpaces; ++sp) {
        t += RegLine(l, static_cast<RegSpace>(sp), 48);
    }
    t += "\nEvents (event type = VGT_EVENT_INITIATOR.EVENT_TYPE):\n";
    t += Histogram("EVENT_WRITE", l.event_write, 64);
    t += Histogram("EVENT_WRITE_EOP", l.eop_event, 64);
    t += Histogram("EVENT_WRITE_EOP int_sel", l.eop_int, 4);
    t += Histogram("EVENT_WRITE_EOP data_sel", l.eop_data, 8);
    t += Histogram("EVENT_WRITE_EOS", l.eos_event, 64);
    t += Histogram("RELEASE_MEM", l.release_event, 64);
    t += Histogram("WAIT_REG_MEM function", l.wait_function, 8);
    t += "  DMA_DATA src_sel x dst_sel (0 memory, 1 GDS, 2 data, 3 memory/L2):";
    for (u32 a = 0; a < 4; ++a) {
        for (u32 b = 0; b < 4; ++b) {
            if (l.dma[a][b] != 0) {
                t += fmt::format(" {}->{}={}", a, b, l.dma[a][b]);
            }
        }
    }
    t += fmt::format("\n\nPackets SecondSonPC cannot name, type-0 packets, decode errors: {} kinds{}\n",
                     l.unknown_kinds, l.unknown_dropped != 0 ? fmt::format(" (+{} not kept)", l.unknown_dropped) : "");
    for (u32 i = 0; i < l.unknown_kinds; ++i) {
        const auto& u = l.unknown[i];
        t += fmt::format("  {} type {} opcode {:#04x} error {} x{}: words", Pm4::EngineName(static_cast<Engine>(u.engine)),
                         u.type, u.opcode, u.error, u.count);
        for (u32 k = 0; k < u.words_kept; ++k) {
            t += fmt::format(" {:08x}", u.words[k]);
        }
        t += "\n";
    }
    ok &= WriteFile(s.options.dir / "gpu_stats.txt", t);

    // gpu_frames.csv
    std::string c = "frame,t_ms,frame_ms,busy_ms,draw_ms,dispatch_ms,register_ms,sync_ms,memory_ms,idle_ms,vowait_ms,"
                    "flush_ms,commands_ms,packets,dwords,draws,dispatch_dcb,dispatch_asc,ctx_regs,sh_regs,"
                    "uconfig_regs,indirect,waits,eops,dma_bytes,submits_dcb,submits_asc,r_draws,r_dispatches,"
                    "pipe_binds,pipe_changes,pipes_created,programs,render_passes,barriers\n";
    for (u64 i = 0; i < frames; ++i) {
        const auto& r = s.frames[i];
        const auto cat = [&](Category k) { return Ms(r.d.category[static_cast<size_t>(k)], freq); };
        c += fmt::format("{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},"
                         "{},{},{},{},{},{},{},{},{},{},{},{}",
                         i, Ms(r.t, freq), Ms(r.ticks, freq), Ms(r.d.busy, freq), cat(Category::Draw),
                         cat(Category::Dispatch), cat(Category::Register), cat(Category::Sync), cat(Category::Memory),
                         Ms(r.d.span[0], freq), Ms(r.d.span[1], freq), Ms(r.d.span[2], freq), Ms(r.d.span[3], freq),
                         r.d.packets, r.d.dwords, r.d.draws, r.d.dispatch_dcb, r.d.dispatch_asc, r.d.ctx_regs,
                         r.d.sh_regs, r.d.uconfig_regs, r.d.ib, r.d.waits, r.d.eops, r.d.dma_bytes, r.submits_dcb,
                         r.submits_asc);
        for (size_t k = 0; k < kCounters; ++k) {
            c += fmt::format(",{}", r.counters[k]);
        }
        c += "\n";
    }
    ok &= WriteFile(s.options.dir / "gpu_frames.csv", c);

    // gpu_stats_windows.csv
    std::string w = "t_s,engine,opcode,name,count,dwords,ms\n";
    for (const auto& r : snapshots) {
        w += fmt::format("{},{},{:#04x},{},{},{},{:.3f}\n", r.t_s, Pm4::EngineName(static_cast<Engine>(r.engine)),
                         r.opcode, Pm4::OpcodeName(r.opcode), r.count, r.dwords, Ms(r.ticks, freq));
    }
    ok &= WriteFile(s.options.dir / "gpu_stats_windows.csv", w);
    SSPC_LOG(GPU, Info, "gpu stats written: {} frames, {} packets, {} snapshots{}", frames, tot.packets,
             snapshots.empty() ? 0 : snapshots.back().t_s, ok ? "" : " (a file could not be written)");
    return ok;
}

namespace Test {

void EnableForTest(const Options& options, bool command_thread) {
    Reset();
    if (g_state != nullptr) {
        return; // a real capture runs
    }
    Init(options);
    g_state->test = !command_thread; // g_enabled stays false: the bridge's hooks never reach this capture
}

void Reset() {
    if (g_enabled.load(std::memory_order_acquire)) {
        return; // a real capture (--gpu-stats): its hooks may be running, it stays
    }
    if (g_state != nullptr) {
        if (g_state->reporter.joinable()) {
            g_state->reporter.request_stop();
            g_state->reporter.join();
        }
        delete g_state;
        g_state = nullptr;
    }
    g_enabled = false;
}

Summary Take() {
    Summary out;
    if (g_state == nullptr) {
        return out;
    }
    const Live& l = g_state->live;
    out.packets = l.totals.packets;
    out.dwords = l.totals.dwords;
    out.frames = g_state->frames_seen;
    out.draws = l.totals.draws;
    out.dispatches = l.totals.dispatch_dcb + l.totals.dispatch_asc;
    out.unknown_kinds = l.unknown_kinds;
    for (size_t e = 0; e < kEngines; ++e) {
        out.decode_errors += l.decode_errors[e];
    }
    out.context_regs = l.totals.ctx_regs;
    out.sh_regs = l.totals.sh_regs;
    for (size_t m = 0; m < kMarkers; ++m) {
        out.nop_markers[m] = l.nop[m];
    }
    return out;
}

u64 OpcodeCount(Engine engine, u8 opcode) {
    return g_state == nullptr ? 0 : g_state->live.ops[static_cast<size_t>(engine)][opcode].count;
}

} // namespace Test

} // namespace SecondSon::Gpu::Pm4Stats
