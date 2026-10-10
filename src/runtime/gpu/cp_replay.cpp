// SecondSonPC — the command processor on a recorded trace (P4.2). See cp_replay.h.
#include "runtime/gpu/cp_replay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <span>
#include <intrin.h>

#include <fmt/format.h>

#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/cp_validation.h"
#include "runtime/gpu/pm4.h"

// Options and records are filled naming only the fields that matter.
#pragma clang diagnostic ignored "-Wmissing-designated-field-initializers"

namespace SecondSon::Gpu::Cp {

namespace {

using Clock = std::chrono::steady_clock;

template <typename T>
T Get(std::span<const u8> p, size_t& off) {
    T v{};
    if (off + sizeof(T) <= p.size()) {
        std::memcpy(&v, p.data() + off, sizeof(T));
    }
    off += sizeof(T);
    return v;
}

class TraceMemory final : public CommandMemory {
public:
    /// The latest read of an address wins: the game rewrites the same buffers every frame, and the trace records a
    /// read just before the packets that use it.
    void Add(u64 addr, std::span<const u8> data) {
        chunks_[addr].assign(data.begin(), data.end());
    }
    bool Read(u64 addr, void* dst, u64 bytes) override {
        // the nearest chunk starting at or before addr may be a short read inside a longer one: look further back
        auto it = chunks_.upper_bound(addr);
        for (u32 tries = 0; it != chunks_.begin() && tries < 64; ++tries) {
            --it;
            const u64 off = addr - it->first;
            if (off <= it->second.size() && bytes <= it->second.size() - off) {
                std::memcpy(dst, it->second.data() + off, bytes);
                return true;
            }
        }
        return false; // not in the trace
    }
    bool Write(u64, const void*, u64) override {
        return false;
    }
    const std::map<u64, std::vector<u8>>& Chunks() const {
        return chunks_;
    }

private:
    std::map<u64, std::vector<u8>> chunks_;
};

class CountingWork final : public GpuWork {
public:
    void Draw(const DrawDescriptor&) override {
        ++draws;
    }
    void Dispatch(const DispatchDescriptor&) override {
        ++dispatches;
    }
    u64 draws = 0, dispatches = 0;
};

struct Sub {
    u32 queue = 0;
    u64 addr = 0;
    std::vector<u32> data;
};

/// What a trace gives to start from and to feed.
struct Inputs {
    bool have_state = false;
    std::vector<u32> flat;
    std::array<u32, 8> cb{};
    u32 db = 0;
    u64 indirect = 0;
    std::vector<std::pair<u32, std::array<u32, kComputeCount>>> compute;
    std::vector<Sub> subs;
    TraceMemory memory;
    u64 frames = 0;
};

void Collect(const TraceReader& trace, Inputs& in) {
    for (const auto& item : trace.Items()) {
        size_t o = 0;
        const auto p = item.payload;
        switch (item.type) {
        case TraceRecord::State: {
            Get<u32>(p, o); // the bridge's INDEX_BUFFER_SIZE (compared at checkpoints)
            in.indirect = Get<u64>(p, o);
            for (auto& c : in.cb) {
                c = Get<u32>(p, o);
            }
            in.db = Get<u32>(p, o);
            if (p.size() >= o + kNumRegs * 4) {
                in.flat.resize(kNumRegs);
                std::memcpy(in.flat.data(), p.data() + o, kNumRegs * 4);
                in.have_state = true;
            }
            break;
        }
        case TraceRecord::Compute: {
            const u32 q = Get<u32>(p, o);
            std::array<u32, kComputeCount> cs{};
            if (p.size() >= o + sizeof(cs)) {
                std::memcpy(cs.data(), p.data() + o, sizeof(cs));
                in.compute.emplace_back(q, cs);
            }
            break;
        }
        case TraceRecord::Submission: {
            Sub s;
            s.queue = Get<u32>(p, o);
            Get<u32>(p, o);
            s.addr = Get<u64>(p, o);
            const u32 dw = Get<u32>(p, o);
            if (p.size() >= o + size_t{dw} * 4) {
                s.data.resize(dw);
                std::memcpy(s.data.data(), p.data() + o, size_t{dw} * 4);
                in.subs.push_back(std::move(s));
            }
            break;
        }
        case TraceRecord::Memory: {
            const u64 addr = Get<u64>(p, o);
            const u32 bytes = Get<u32>(p, o);
            if (p.size() >= o + bytes) {
                in.memory.Add(addr, p.subspan(o, bytes));
            }
            break;
        }
        case TraceRecord::Flip:
            ++in.frames;
            break;
        default:
            break;
        }
    }
}

TraceCheckpoint ParseCheckpoint(std::span<const u8> p) {
    size_t o = 0;
    TraceCheckpoint c;
    c.seq = Get<u32>(p, o);
    c.queue = Get<u32>(p, o);
    c.kind = Get<u32>(p, o);
    c.addr = Get<u64>(p, o);
    c.regs_hash = Get<u64>(p, o);
    c.cs_hash = Get<u64>(p, o);
    c.index_buffer_size = Get<u32>(p, o);
    c.reg_dma_size = Get<u32>(p, o);
    c.indirect_args = Get<u64>(p, o);
    for (auto& e : c.cb_extent) {
        e = Get<u32>(p, o);
    }
    c.db_extent = Get<u32>(p, o);
    return c;
}

u64 Percentile(std::vector<u32>& v, double q) {
    if (v.empty()) {
        return 0;
    }
    const size_t k = std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size() - 1) + 0.5));
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
    return v[k];
}

} // namespace

bool Replay(const TraceReader& trace, ReplayResult& out) {
    TraceMemory memory;
    CountingWork work;
    CommandProcessor cp(Options{.mode = Mode::Shadow, .memory = &memory, .work = &work, .resume_after_fault = true});
    BridgeMirror mirror(cp);
    std::deque<std::vector<u32>> owned; // submissions' dwords, alive for the whole replay
    const auto detail = [&out](std::string s) {
        if (out.details.size() < 64) {
            out.details.push_back(std::move(s));
        }
    };
    const auto advance = [&](u32 q, u64 addr) {
        for (u64 guard = 0; guard < 100'000'000; ++guard) {
            u64 a = 0;
            std::span<const u32> pk;
            if (!cp.Peek(q, a, pk)) {
                return false;
            }
            if (a == addr) {
                return true;
            }
            const StepStatus st = cp.Step(q);
            if (st == StepStatus::Faulted) {
                ++out.faults;
                detail("fault: " + cp.LastError().Text());
            } else if (st != StepStatus::Packet) {
                return false;
            }
        }
        return false;
    };
    bool have_state = false;
    for (const auto& item : trace.Items()) {
        size_t o = 0;
        const auto p = item.payload;
        switch (item.type) {
        case TraceRecord::State: {
            Get<u32>(p, o);
            const u64 indirect = Get<u64>(p, o);
            std::array<u32, 8> cb{};
            for (auto& c : cb) {
                c = Get<u32>(p, o);
            }
            const u32 db = Get<u32>(p, o);
            if (p.size() < o + kNumRegs * 4) {
                out.error = "the state record is too short";
                return false;
            }
            std::vector<u32> flat(kNumRegs);
            std::memcpy(flat.data(), p.data() + o, kNumRegs * 4);
            mirror.Load(flat.data());
            cp.SetExtents(cb, db);
            cp.SetDrawIndirectBase(indirect);
            have_state = true;
            break;
        }
        case TraceRecord::Compute: {
            const u32 q = Get<u32>(p, o);
            if (ComputeRegisters* cr = cp.MutableCompute(q); cr != nullptr && p.size() >= o + kComputeCount * 4) {
                std::memcpy(cr->values.data(), p.data() + o, kComputeCount * 4);
            }
            break;
        }
        case TraceRecord::Submission: {
            const u32 q = Get<u32>(p, o);
            Get<u32>(p, o);
            const u64 addr = Get<u64>(p, o);
            const u32 dw = Get<u32>(p, o);
            if (p.size() < o + size_t{dw} * 4) {
                out.error = "a submission record is too short";
                return false;
            }
            auto& data = owned.emplace_back(dw);
            std::memcpy(data.data(), p.data() + o, size_t{dw} * 4);
            const auto submit = [&] {
                return q == 0 ? cp.SubmitGraphics(addr, dw, data.data()) : cp.SubmitCompute(q, addr, dw, data.data());
            };
            bool ok = submit();
            if (!ok && cp.LastError().kind == CpErrorKind::QueueFull) {
                // many spans without a checkpoint between them: process the queued ones first (a checkpoint this
                // passes is reported as not reached, never skipped silently)
                for (StepStatus st = cp.Step(q); st == StepStatus::Packet; st = cp.Step(q)) {
                }
                ok = submit();
            }
            if (!ok) {
                ++out.faults;
                detail("submission refused: " + cp.LastError().Text());
            }
            break;
        }
        case TraceRecord::Memory: {
            const u64 addr = Get<u64>(p, o);
            const u32 bytes = Get<u32>(p, o);
            if (p.size() >= o + bytes) {
                memory.Add(addr, p.subspan(o, bytes));
            }
            break;
        }
        case TraceRecord::Checkpoint: {
            if (!have_state) {
                out.error = "a checkpoint before the start state";
                return false;
            }
            const TraceCheckpoint c = ParseCheckpoint(p);
            ++out.checkpoints;
            if (!advance(c.queue, c.addr)) {
                ++out.unreached;
                detail(fmt::format("checkpoint {} (queue {}, {:#x}) not reached", c.seq, c.queue, c.addr));
                break;
            }
            if (c.kind == 0) {
                if (HashFlatRegs(mirror.Flat(), kDrawRanges) != c.regs_hash) {
                    ++out.hash_mismatches;
                    detail(fmt::format("checkpoint {} (queue {}, draw at {:#x}): registers differ from the bridge's",
                                       c.seq, c.queue, c.addr));
                }
                const u32 ours = cp.Registers().Get(Reg::VgtDmaSize);
                if (ours != c.index_buffer_size && ours != c.reg_dma_size) {
                    ++out.extra_mismatches;
                    detail(fmt::format("checkpoint {}: VGT_DMA_SIZE {:#x}, bridge {:#x}/{:#x}", c.seq, ours,
                                       c.index_buffer_size, c.reg_dma_size));
                }
                if (cp.DrawIndirectBase() != c.indirect_args) {
                    ++out.extra_mismatches;
                    detail(fmt::format("checkpoint {}: draw-indirect base {:#x}, bridge {:#x}", c.seq,
                                       cp.DrawIndirectBase(), c.indirect_args));
                    cp.SetDrawIndirectBase(c.indirect_args);
                }
                if (cp.CbExtents() != c.cb_extent || cp.DbExtent() != c.db_extent) {
                    ++out.extra_mismatches;
                    detail(fmt::format("checkpoint {}: target size hints differ (cb0 {:#x}/{:#x}, db {:#x}/{:#x})",
                                       c.seq, cp.CbExtents()[0], c.cb_extent[0], cp.DbExtent(), c.db_extent));
                    cp.SetExtents(c.cb_extent, c.db_extent);
                }
            } else {
                const ComputeRegisters* cr = cp.Compute(c.queue);
                if (cr == nullptr || HashCompute(cr->values.data()) != c.cs_hash) {
                    ++out.compute_mismatches;
                    detail(fmt::format("checkpoint {} (queue {}, dispatch at {:#x}): compute registers differ",
                                       c.seq, c.queue, c.addr));
                }
            }
            break;
        }
        case TraceRecord::Full: {
            const u32 seq = Get<u32>(p, o);
            if (p.size() < o + kNumRegs * 4) {
                break;
            }
            std::vector<u32> flat(kNumRegs);
            std::memcpy(flat.data(), p.data() + o, kNumRegs * 4);
            std::vector<RegDiff> diffs;
            const u32 n = DiffFlat(mirror, flat.data(), kAllRanges, diffs, 16);
            ++out.full_checks;
            if (n != 0) {
                out.full_diff_regs += n;
                for (const auto& d : diffs) {
                    detail(fmt::format("full check at checkpoint {}: register {:#06x} ({}) ours {:#010x}, bridge "
                                       "{:#010x}",
                                       seq, d.reg, GroupName(GroupOf(d.reg)), d.ours, d.bridge));
                }
                mirror.Load(flat.data()); // resynchronised: the next difference is a new one
            }
            break;
        }
        case TraceRecord::Flip:
            ++out.frames;
            break;
        default:
            break;
        }
    }
    for (u32 q = 0; q < kNumQueues; ++q) { // the rest after the last checkpoint
        for (StepStatus st = cp.Step(q); st != StepStatus::Idle; st = cp.Step(q)) {
            if (st == StepStatus::Faulted) {
                ++out.faults;
                detail("fault: " + cp.LastError().Text());
                if (cp.PendingSubmissions(q) == 0) {
                    break;
                }
            } else if (st != StepStatus::Packet) {
                break;
            }
        }
    }
    const Counters& s = cp.Stats();
    for (u32 op = 0; op < 256; ++op) {
        out.packets += s.graphics_packets[op] + s.compute_packets[op];
    }
    out.packets += s.type2;
    out.submissions = s.submissions;
    out.draws = s.draws;
    out.dispatches = s.dispatches;
    if (!have_state) {
        out.error = "no start state in the trace";
        return false;
    }
    return true;
}

namespace {

std::string Bench(const TraceReader& trace, u32 passes) {
    Inputs in;
    Collect(trace, in);
    if (!in.have_state || in.subs.empty()) {
        return "Benchmark: the trace has no state or no submissions\n";
    }
    CountingWork work;
    CommandProcessor cp(
        Options{.mode = Mode::Shadow, .memory = &in.memory, .work = &work, .resume_after_fault = true});
    const auto load = [&] {
        cp.Reset();
        cp.MutableRegisters().Load(std::span<const u32, kNumRegs>(in.flat.data(), kNumRegs));
        for (const auto& [q, cs] : in.compute) {
            if (ComputeRegisters* cr = cp.MutableCompute(q)) {
                cr->values = cs;
            }
        }
        cp.SetExtents(in.cb, in.db);
        cp.SetDrawIndirectBase(in.indirect);
    };
    const auto run = [&] {
        for (const auto& s : in.subs) {
            const u32 dw = static_cast<u32>(s.data.size());
            if (s.queue == 0) {
                cp.SubmitGraphics(s.addr, dw, s.data.data());
            } else {
                cp.SubmitCompute(s.queue, s.addr, dw, s.data.data());
            }
            cp.Run(s.queue);
        }
    };
    const auto packets_now = [&] {
        u64 n = cp.Stats().type2;
        for (u32 op = 0; op < 256; ++op) {
            n += cp.Stats().graphics_packets[op] + cp.Stats().compute_packets[op];
        }
        return n;
    };

    // warm-up: the processor's buffers grow here
    u64 g0 = cp.Stats().buffer_growths;
    load();
    const u64 p0 = packets_now(), d0 = cp.Stats().draws, k0 = cp.Stats().pipeline_key_rebuilds;
    const u64 w0 = cp.Registers().Writes(), c0r = cp.Registers().Changes();
    run();
    const u64 packets = packets_now() - p0, draws = cp.Stats().draws - d0;
    const u64 rebuilds = cp.Stats().pipeline_key_rebuilds - k0;
    const u64 warm_growths = cp.Stats().buffer_growths - g0;
    const u64 reg_writes = cp.Registers().Writes() - w0, reg_changes = cp.Registers().Changes() - c0r;

    // timed passes
    std::vector<double> ms;
    u64 cycles = 0;
    double ns_total = 0;
    g0 = cp.Stats().buffer_growths;
    for (u32 i = 0; i < passes; ++i) {
        load();
        const u64 c0 = __rdtsc();
        const auto t0 = Clock::now();
        run();
        const double dt = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        cycles += __rdtsc() - c0;
        ns_total += dt * 1e6;
        ms.push_back(dt);
    }
    const u64 steady_growths = cp.Stats().buffer_growths - g0;
    const double cyc_per_ns = ns_total > 0 ? static_cast<double>(cycles) / ns_total : 1.0;
    std::ranges::sort(ms);

    // one pass timed packet by packet
    std::vector<std::vector<u32>> samples(512); // [compute * 256 + opcode]
    load();
    for (const auto& s : in.subs) {
        const u32 dw = static_cast<u32>(s.data.size());
        if (s.queue == 0) {
            cp.SubmitGraphics(s.addr, dw, s.data.data());
        } else {
            cp.SubmitCompute(s.queue, s.addr, dw, s.data.data());
        }
        for (;;) {
            u64 a = 0;
            std::span<const u32> pk;
            if (!cp.Peek(s.queue, a, pk)) {
                break;
            }
            const u32 op = (pk[0] >> 30) == 3 ? (pk[0] >> 8) & 0xFF : 0;
            const u64 t0 = __rdtsc();
            const StepStatus st = cp.Step(s.queue);
            const u64 t1 = __rdtsc();
            samples[(s.queue != 0 ? 256 : 0) + op].push_back(static_cast<u32>(std::min<u64>(t1 - t0, 0xFFFFFFFF)));
            if (st != StepStatus::Packet) {
                break;
            }
        }
    }

    // the decoder alone over the same dwords (submissions and indirect buffers)
    std::vector<std::vector<u32>> ib_dwords;
    for (const auto& [addr, bytes] : in.memory.Chunks()) {
        auto& d = ib_dwords.emplace_back(bytes.size() / 4);
        std::memcpy(d.data(), bytes.data(), d.size() * 4);
    }
    u64 decoded = 0;
    const auto walk = [&decoded](std::span<const u32> s) {
        Pm4::Packet pk;
        Pm4::DecodeError e{};
        while (!s.empty() && Pm4::Decode(s, pk, e)) {
            ++decoded;
            s = s.subspan(pk.dwords);
        }
    };
    const auto t_dec = Clock::now();
    for (u32 rep = 0; rep < passes; ++rep) {
        for (const auto& s : in.subs) {
            walk(s.data);
        }
        for (const auto& d : ib_dwords) {
            walk(d);
        }
    }
    const double dec_ns = std::chrono::duration<double, std::nano>(Clock::now() - t_dec).count();

    // register writes: with the change tracking (RegisterFile) and as a plain copy, the same SET_*_REG payloads
    struct SetRegs {
        Space space;
        u32 offset;
        const u32* values;
        u32 count;
    };
    std::vector<SetRegs> sets;
    for (const auto& s : in.subs) {
        std::span<const u32> rest(s.data);
        Pm4::Packet pk;
        Pm4::DecodeError e{};
        while (!rest.empty() && Pm4::Decode(rest, pk, e)) {
            Pm4::RegSpace sp{};
            if (pk.type == 3 && pk.dwords >= 2 && Pm4::SetRegSpace(pk.opcode, sp)) {
                const u32 off = rest[1] & 0xFFFF, cnt = pk.dwords - 2;
                const auto space = static_cast<Space>(static_cast<u32>(sp));
                const bool compute = space == Space::Sh && off >= 0x200 && off < 0x250;
                if (!compute && off + cnt <= kSpaceCount[static_cast<u32>(space)]) {
                    sets.push_back({space, off, rest.data() + 2, cnt});
                }
            }
            rest = rest.subspan(pk.dwords);
        }
    }
    u64 set_regs = 0;
    for (const auto& s : sets) {
        set_regs += s.count;
    }
    RegisterFile rf;
    std::vector<u32> plain(kNumRegs);
    double tracked_ns = 1e300, plain_ns = 1e300;
    for (u32 rep = 0; rep < std::max(passes, 3u); ++rep) {
        auto t0 = Clock::now();
        for (const auto& s : sets) {
            rf.Write(s.space, s.offset, s.values, s.count);
        }
        tracked_ns = std::min(tracked_ns, std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
        t0 = Clock::now();
        for (const auto& s : sets) {
            std::memcpy(plain.data() + kSpaceFirst[static_cast<u32>(s.space)] + s.offset, s.values,
                        size_t{s.count} * 4);
        }
        plain_ns = std::min(plain_ns, std::chrono::duration<double, std::nano>(Clock::now() - t0).count());
    }

    const double frames = static_cast<double>(std::max<u64>(in.frames, 1));
    const double best = ms.front(), median = ms[ms.size() / 2];
    std::string out = fmt::format(
        "\nBenchmark ({} passes of the trace's {} submissions, {} frames; Mode::Shadow, a GpuWork that counts):\n"
        "  per pass: best {:.3f} ms, median {:.3f} ms -> {:.3f} ms per frame (best), {:.1f} ns per packet, "
        "{:.0f} ns per draw incl. its state packets ({:.3f} ms per 1000 draws)\n"
        "  {} packets, {} draws per pass; pipeline key rebuilt {} times ({:.1f}% of draws); register writes {} "
        "changing a value {} ({:.1f}%)\n"
        "  allocations of the command processor (buffer growths): warm-up {}, the {} timed passes {} ({:.2f} per "
        "frame)\n"
        "  decoder alone: {:.1f} ns per packet ({} packets per pass)\n"
        "  register writes ({} SET_*_REG registers per pass): with change tracking {:.2f} ns, plain copy {:.2f} ns per "
        "register -> tracking costs {:.2f} ns per register, {:.3f} ms per frame\n",
        passes, in.subs.size(), in.frames, best, median, best / frames,
        packets != 0 ? best * 1e6 / static_cast<double>(packets) : 0.0,
        draws != 0 ? best * 1e6 / static_cast<double>(draws) : 0.0,
        draws != 0 ? best * 1000.0 / static_cast<double>(draws) : 0.0, packets, draws, rebuilds,
        draws != 0 ? 100.0 * static_cast<double>(rebuilds) / static_cast<double>(draws) : 0.0, reg_writes, reg_changes,
        reg_writes != 0 ? 100.0 * static_cast<double>(reg_changes) / static_cast<double>(reg_writes) : 0.0,
        warm_growths, passes, steady_growths, static_cast<double>(steady_growths) / (frames * passes),
        decoded != 0 ? dec_ns / static_cast<double>(decoded) : 0.0, decoded / std::max(passes, 1u), set_regs,
        set_regs != 0 ? tracked_ns / static_cast<double>(set_regs) : 0.0,
        set_regs != 0 ? plain_ns / static_cast<double>(set_regs) : 0.0,
        set_regs != 0 ? (tracked_ns - plain_ns) / static_cast<double>(set_regs) : 0.0,
        (tracked_ns - plain_ns) / 1e6 / frames);
    out += "  per opcode, one pass timed packet by packet (Step of one packet; ns):\n"
           "    queue  opcode                          count      mean       P50       P95       P99\n";
    for (u32 i = 0; i < samples.size(); ++i) {
        auto& v = samples[i];
        if (v.empty()) {
            continue;
        }
        double sum = 0;
        for (const u32 x : v) {
            sum += x;
        }
        const double mean = sum / static_cast<double>(v.size()) / cyc_per_ns;
        const double p50 = static_cast<double>(Percentile(v, 0.50)) / cyc_per_ns;
        const double p95 = static_cast<double>(Percentile(v, 0.95)) / cyc_per_ns;
        const double p99 = static_cast<double>(Percentile(v, 0.99)) / cyc_per_ns;
        out += fmt::format("    {:5}  {:#04x} {:26} {:>9} {:>9.1f} {:>9.1f} {:>9.1f} {:>9.1f}\n",
                           i >= 256 ? "asc" : "dcb", i & 0xFF, Pm4::OpcodeName(static_cast<u8>(i & 0xFF)), v.size(),
                           mean, p50, p95, p99);
    }
    return out;
}

} // namespace

int RunReplay(const std::filesystem::path& trace_path, const std::filesystem::path& logs_dir, u32 bench_passes) {
    std::string report;
    TraceReader trace;
    std::string error;
    int rc = 0;
    if (!trace.Load(trace_path, error)) {
        report = fmt::format("--gpu-replay {}: {}\n", trace_path.string(), error);
        rc = 2;
    } else {
        ReplayResult r;
        const auto t0 = Clock::now();
        Replay(trace, r);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        report = fmt::format(
            "SecondSonPC command processor replay (--gpu-replay, P4.2): {}\n{} records, {} frames, {} submissions, {} "
            "packets, {} draws, {} dispatches; {:.1f} ms\n"
            "Checkpoints {}: register hash mismatches {}, compute mismatches {}, other mismatches (index buffer "
            "size, draw-indirect base, size hints) {}, not reached {}; full checks {} with {} registers differing; "
            "faults {}\nResult: {}\n",
            trace_path.string(), trace.Items().size(), r.frames, r.submissions, r.packets, r.draws, r.dispatches, ms,
            r.checkpoints, r.hash_mismatches, r.compute_mismatches, r.extra_mismatches, r.unreached, r.full_checks,
            r.full_diff_regs, r.faults,
            !r.error.empty() ? "ERROR: " + r.error
            : r.Matched() ? std::string("MATCH: the command processor's state equals the bridge's at every checkpoint")
                          : std::string("DIFFERENCES (below)"));
        for (const auto& d : r.details) {
            report += "  " + d + "\n";
        }
        if (bench_passes != 0) {
            report += Bench(trace, bench_passes);
        }
        rc = r.Matched() ? 0 : 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(logs_dir, ec);
    std::ofstream(logs_dir / "gpu_replay.txt", std::ios::binary | std::ios::trunc) << report;
    std::fwrite(report.data(), 1, report.size(), stdout);
    return rc;
}

} // namespace SecondSon::Gpu::Cp
