// SecondSonPC R8-A1 - the command processor's profiler (cp_profile.h; docs/NATIVE_ENGINE_R8_CP_PROFILE.md).
#include <windows.h>

#include "runtime/gpu/r8/cp_profile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/frame_log.h"
#include "runtime/gpu/pm4.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::CpProfile {

namespace {

u8 ParseMode() {
    char v[16] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_R8_CP_PROFILE", v, sizeof(v));
    if (n == 0 || n >= sizeof(v)) {
        return 0;
    }
    return v[0] == '1' ? 1 : v[0] == '2' ? 2 : 0;
}

u32 ParseEvery() {
    char v[16] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_R8_CP_PROFILE_SAMPLE", v, sizeof(v));
    const u32 every = n > 0 && n < sizeof(v) ? static_cast<u32>(std::strtoul(v, nullptr, 10)) : 32u;
    return std::clamp<u32>(every, 1u, 4096u);
}

enum class Cls : u8 { Compute, Sync, Wait, Idle };
constexpr std::string_view kClsNames[] = {"compute", "sync", "wait", "idle"};

struct Info {
    std::string_view name;
    Cls cls;
};

/// kInfo[s]: the section's name in the report and its class - compute (the thread's own work), sync (thread 1 waits for
/// thread 2 to reach a point of the stream: a drain), wait (thread 1 waits for capacity or the GPU: the queue or
/// journal full, a read over a GPU write, Synchronize), idle (nothing to do: not part of the thread's busy time).
constexpr std::array<Info, kSections> MakeInfo() {
    std::array<Info, kSections> a{};
    const auto set = [&](Sec s, std::string_view n, Cls c) { a[static_cast<u32>(s)] = {n, c}; };
    set(Sec::Other, "other (the backend loop, unmarked)", Cls::Compute);
    set(Sec::LoopPre, "submissions taken (Backend::Apply)", Cls::Compute);
    set(Sec::FrontInvalidate, "frontend invalidations (ShaderFrontend::Invalidate)", Cls::Compute);
    set(Sec::CpuWrites, "CPU writes applied (DrainCpuWrites)", Cls::Compute);
    set(Sec::Packets, "PM4 packets (decode, dispatch, registers, small packets, draw packet + BuildDraw)", Cls::Compute);
    set(Sec::RoundEnd, "round end without drain (CP-9), completions", Cls::Compute);
    set(Sec::Poll, "poll after a drained round, completions", Cls::Compute);
    set(Sec::Flush, "flush (submit) from the loop", Cls::Compute);
    set(Sec::IdleBlocked, "idle: WAIT_REG_MEM blocked", Cls::Idle);
    set(Sec::IdleGpu, "idle: waiting for the GPU", Cls::Idle);
    set(Sec::IdleEmpty, "idle: nothing submitted", Cls::Idle);
    set(Sec::JoinRun, "JoinRun (run members; a refused one's CloseRun)", Cls::Compute);
    set(Sec::ParDraw, "draw record (BeginRecord, descriptor copy, RunLeaderOk, PushRecord)", Cls::Compute);
    set(Sec::PrepDraw, "ParallelCp::PrepareDraw own part", Cls::Compute);
    set(Sec::GeoFast, "PrepareGeometryOnly (GEOFAST, refusals included)", Cls::Compute);
    set(Sec::Redecode, "RedecodeStage (Phase 6 / templates)", Cls::Compute);
    set(Sec::FrontUntagged, "frontend PrepareDraw: untagged draws", Cls::Compute);
    set(Sec::FrontMesh, "frontend PrepareDraw: mesh 0x1a1960 (Phase 5)", Cls::Compute);
    set(Sec::FrontShadow, "frontend PrepareDraw: shadow 0x43d480 (Phase 5)", Cls::Compute);
    set(Sec::FrontGBuffer, "frontend PrepareDraw: G-buffer 0x1a1770 (Phase 6)", Cls::Compute);
    set(Sec::CopyStage, "CopyStage (stage views into records)", Cls::Compute);
    set(Sec::ParDispatch, "dispatch record", Cls::Compute);
    set(Sec::FrontDispatch, "frontend PrepareDispatch + copy", Cls::Compute);
    set(Sec::FrontSync, "frontend read: idle-queue apply / GPU-write check", Cls::Wait);
    set(Sec::CbDma, "DMA / WRITE_DATA report (ParallelCp::Dma)", Cls::Compute);
    set(Sec::CbCache, "cache action", Cls::Compute);
    set(Sec::CbFence, "fence", Cls::Compute);
    set(Sec::CbAcquire, "met memory wait (MemoryAcquired)", Cls::Compute);
    set(Sec::CbWaitMet, "wait met by a held fence", Cls::Compute);
    set(Sec::CbCopy, "timeline copy", Cls::Compute);
    set(Sec::CbWrite, "timeline write (WRITE_DATA / fill)", Cls::Compute);
    set(Sec::CbZpass, "occlusion record (ZpassOnTimeline)", Cls::Compute);
    set(Sec::CbGds, "GDS DMA on the timeline", Cls::Compute);
    set(Sec::CbSync, "Synchronize (waits for the GPU)", Cls::Wait);
    set(Sec::CbFlip, "flip on thread 1 (Executor::Flip)", Cls::Compute);
    set(Sec::MemRead, "processor memory read (Memory::Read)", Cls::Compute);
    set(Sec::MemSync, "processor read over a GPU write (SyncFromMemoryRead)", Cls::Wait);
    set(Sec::PollRead, "WAIT_REG_MEM memory read", Cls::Compute);
    set(Sec::ApplyPending, "ApplyPending (queue idle)", Cls::Compute);
    set(Sec::Journal, "register journal replay (ApplyJournal)", Cls::Compute);
    set(Sec::WaitFull, "wait: queue full", Cls::Wait);
    set(Sec::WaitJournal, "wait: journal full", Cls::Wait);
    constexpr std::string_view kDrains[16] = {
        "drain: round end",     "drain: cache action", "drain: fence",          "drain: dma report",
        "drain: flip",          "drain: timeline write (WRITE_DATA / occlusion)", "drain: timeline copy",
        "drain: timeline gds",  "drain: memory wait",  "drain: wait met",       "drain: synchronize",
        "drain: memory read",   "drain: memory write", "drain: frontend read",  "drain: stop",
        "drain: ?"};
    for (u32 i = 0; i < 16; ++i) {
        set(Drain(i), kDrains[i], Cls::Sync);
    }
    set(Sec::T2Idle, "T2 idle: waiting for records", Cls::Idle);
    set(Sec::T2Draw, "T2 draw records (DrawPrepared / batch / a run's first)", Cls::Compute);
    set(Sec::T2Dispatch, "T2 dispatch records", Cls::Compute);
    set(Sec::T2RoundEnd, "T2 round end (flush + poll)", Cls::Compute);
    set(Sec::T2Flip, "T2 queued flip", Cls::Compute);
    set(Sec::T2Other, "T2 other records", Cls::Compute);
    set(Sec::T2Run, "T2 run members (DrawRunMembers)", Cls::Compute);
    set(Sec::T2Bind, "T2 BindStage", Cls::Compute);
    return a;
}
constexpr std::array<Info, kSections> kInfo = MakeInfo();

constexpr std::string_view kLightNames[kLights] = {"BuildDraw", "RegisterFile writes (SetRegs)",
                                                   "register journal writes (OnWrite)"};
constexpr std::string_view kPacketNames[kPacketClasses] = {
    "decode (Next, Pm4::Decode)", "SET_SH_REG", "SET_CONTEXT_REG", "SET_UCONFIG/CONFIG_REG", "NOP",
    "draw packets (own: registers, BuildDraw, tags)", "dispatch packets (own)", "DMA_DATA (own)",
    "WRITE_DATA (own)", "WAIT_REG_MEM (own)", "EVENT_WRITE (own)", "EOP/EOS/RELEASE_MEM (own)", "ACQUIRE_MEM (own)",
    "INDEX_TYPE/NUM_INSTANCES/INDEX_BASE/SET_BASE", "INDIRECT_BUFFER (own)", "other packets", "?"};

u32 ClassOf(u8 op) {
    namespace O = Pm4::Op;
    switch (op) {
    case O::SetShReg:
        return 1;
    case O::SetContextReg:
        return 2;
    case O::SetUconfigReg:
    case O::SetConfigReg:
        return 3;
    case O::Nop:
        return 4;
    case O::DrawIndex2:
    case O::DrawIndexOffset2:
    case O::DrawIndexAuto:
    case O::DrawIndexIndirect:
    case O::DrawIndirect:
        return 5;
    case O::DispatchDirect:
    case O::DispatchIndirect:
        return 6;
    case O::DmaData:
        return 7;
    case O::WriteData:
        return 8;
    case O::WaitRegMem:
        return 9;
    case O::EventWrite:
        return 10;
    case O::EventWriteEop:
    case O::EventWriteEos:
    case O::ReleaseMem:
        return 11;
    case O::AcquireMem:
        return 12;
    case O::IndexType:
    case O::NumInstances:
    case O::IndexBase:
    case O::IndexBufferSize:
    case O::SetBase:
        return 13;
    case O::IndirectBuffer:
        return 14;
    default:
        return 15;
    }
}

/// SSPC_R8_CP_PROFILE=2: the repetition census of a frame (thread 1 only).
struct Census {
    /// draws the frontend prepared, by class (0 both stages repeated, 1 PS only, 2 VS only, 3 none) and engine tag
    std::array<std::array<u64, 4>, 4> draw_ticks{};
    std::array<std::array<u32, 4>, 4> draws{};
    /// per stage (0 VS, 1 PS): prepared (ready), same result as the program's last, same registers, same result with
    /// other registers (a new table at a new address with the same bytes), same registers with another result
    std::array<std::array<u32, 5>, 2> stage{};
    u32 copies = 0, copies_unchanged = 0;
    u64 copy_ticks = 0, copy_unchanged_ticks = 0;
};

struct Row {
    u64 tsc;      ///< the frame's end (TSC)
    u32 frame;    ///< the processor's frame number (rows are a ring)
    u32 valid;
    u64 self[kSections];
    u32 calls[kSections];
    u64 light_ticks[kLights];
    u32 light_n[kLights];
    u64 packet_ticks[kPacketClasses];
    u32 packet_n[kPacketClasses];
    u64 overflow;
    Census census;
};
constexpr u32 kMaxFrames = 8192;

struct Store {
    std::unique_ptr<Row[]> rows[2]; ///< [0] the processor's thread, [1] the recording thread
    Row total[2]{};                 ///< the whole run (also past the ring)
    std::array<u64, kSections> incl[2]{};
    std::once_flag once;
};
Store g_store;
std::atomic<u32> g_cp_frame{0};
double g_pair_ticks = 0; ///< a marker pair's cost (Enter + Leave), calibrated at the processor thread's start
u32 g_every = 32;
Census g_census;         ///< the frame's census so far (thread 1)

struct LastPrep {
    u64 result = 0, regs = 0;
};
std::unordered_map<u64, LastPrep> g_last; ///< (stage, program address) -> its last preparation (thread 1)

inline u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 33);
}

/// The result of a stage's preparation (what the executor binds), field by field (bindings carry padding bytes).
u64 ResultHash(const Gpu::Shader::StageView& s) {
    u64 h = Mix(0, reinterpret_cast<u64>(s.module));
    h = Mix(h, static_cast<u64>(s.status));
    const auto& p = s.plan;
    for (size_t i = 16; i < p.flat.size(); ++i) {
        h = Mix(h, p.flat[i]);
    }
    h = Mix(h, p.flat.size());
    for (const auto& b : p.buffers) {
        h = Mix(h, (u64{b.binding} << 32) | (static_cast<u64>(b.kind) << 8) | (b.written ? 2u : 0u) |
                       (b.formatted ? 1u : 0u));
        for (const u32 w : b.desc.raw) {
            h = Mix(h, w);
        }
        h = Mix(Mix(h, b.address), b.size);
    }
    for (const auto& im : p.images) {
        h = Mix(h, (u64{im.binding} << 32) | im.view_type);
        for (const u32 w : im.desc.raw) {
            h = Mix(h, w);
        }
    }
    for (const auto& sm : p.samplers) {
        h = Mix(h, sm.binding);
        for (const u32 w : sm.desc.raw) {
            h = Mix(h, w);
        }
    }
    for (const auto& v : p.vertices) {
        for (const u32 w : v.desc.raw) {
            h = Mix(h, w);
        }
        h = Mix(Mix(h, v.address), v.size);
    }
    return h;
}

void Flush(ThreadAcc& t, Row& row, Row& total, std::array<u64, kSections>& incl) {
    const u64 now = __rdtsc();
    t.self[t.stack[t.depth]] += now - t.last;
    t.last = now;
    row.tsc = now;
    row.valid = 1;
    std::memcpy(row.self, t.self, sizeof(row.self));
    std::memcpy(row.calls, t.calls, sizeof(row.calls));
    std::memcpy(row.light_ticks, t.light_ticks, sizeof(row.light_ticks));
    std::memcpy(row.light_n, t.light_n, sizeof(row.light_n));
    std::memcpy(row.packet_ticks, t.packet_ticks, sizeof(row.packet_ticks));
    std::memcpy(row.packet_n, t.packet_n, sizeof(row.packet_n));
    row.overflow = t.overflow;
    for (u32 s = 0; s < kSections; ++s) {
        total.self[s] += t.self[s];
        total.calls[s] += t.calls[s];
        incl[s] += t.incl[s];
    }
    for (u32 i = 0; i < kLights; ++i) {
        total.light_ticks[i] += t.light_ticks[i];
        total.light_n[i] += t.light_n[i];
    }
    for (u32 i = 0; i < kPacketClasses; ++i) {
        total.packet_ticks[i] += t.packet_ticks[i];
        total.packet_n[i] += t.packet_n[i];
    }
    total.overflow += t.overflow;
    std::memset(t.self, 0, sizeof(t.self));
    std::memset(t.incl, 0, sizeof(t.incl));
    std::memset(t.calls, 0, sizeof(t.calls));
    std::memset(t.light_ticks, 0, sizeof(t.light_ticks));
    std::memset(t.light_n, 0, sizeof(t.light_n));
    std::memset(t.packet_ticks, 0, sizeof(t.packet_ticks));
    std::memset(t.packet_n, 0, sizeof(t.packet_n));
    t.overflow = 0;
    ++t.flushes; // (a sampled packet across this flush is dropped)
}

void AddCensus(Census& to, const Census& c) {
    for (u32 k = 0; k < 4; ++k) {
        for (u32 e = 0; e < 4; ++e) {
            to.draw_ticks[k][e] += c.draw_ticks[k][e];
            to.draws[k][e] += c.draws[k][e];
        }
    }
    for (u32 s = 0; s < 2; ++s) {
        for (u32 i = 0; i < 5; ++i) {
            to.stage[s][i] += c.stage[s][i];
        }
    }
    to.copies += c.copies;
    to.copies_unchanged += c.copies_unchanged;
    to.copy_ticks += c.copy_ticks;
    to.copy_unchanged_ticks += c.copy_unchanged_ticks;
}

std::pair<double, double> Window() {
    char v[32] = {};
    double from = 70.0, to = 148.0;
    if (const DWORD n = GetEnvironmentVariableA("SSPC_R8_CP_PROFILE_WINDOW", v, sizeof(v)); n > 0 && n < sizeof(v)) {
        char* end = nullptr;
        from = std::strtod(v, &end);
        to = end != nullptr && *end == ':' ? std::strtod(end + 1, nullptr) : 1e9;
    }
    return {from, to};
}

double Freq() {
    return static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1));
}

/// The frame's start of row `f` (seconds of the frame log; the previous row's end, or the row's own end).
double RowSeconds(const Row& r) {
    const u64 t0 = FrameLog::StartTsc();
    return r.tsc > t0 ? static_cast<double>(r.tsc - t0) / Freq() : -1.0;
}

} // namespace

const u8 g_mode = ParseMode();
thread_local constinit ThreadAcc t_acc{};

void RegisterThread(Role role) {
    std::call_once(g_store.once, [] {
        g_every = ParseEvery();
        for (auto& r : g_store.rows) {
            r = std::make_unique<Row[]>(kMaxFrames); // (value-initialized: zero)
        }
        SSPC_LOG(GPU, Warning, "DIAGNOSTICS: SSPC_R8_CP_PROFILE={} - the command processor's sections per frame "
                               "(gpu_native.txt \"R8 CP profile\"; packets sampled 1 of {}{})",
                 g_mode, g_every, g_mode == 2 ? "; the repetition census" : "");
    });
    ThreadAcc& t = t_acc;
    std::memset(&t, 0, sizeof(t));
    t.role = static_cast<u8>(role);
    t.on = true;
    t.last = __rdtsc();
    if (role == Role::Cp) {
        // a marker pair's cost on this thread (its counts dropped again): the report's overhead estimate
        constexpr u32 kPairs = 4096;
        const u64 c0 = __rdtsc();
        for (u32 i = 0; i < kPairs; ++i) {
            Enter(Sec::Other);
            Leave();
        }
        g_pair_ticks = static_cast<double>(__rdtsc() - c0) / kPairs;
        std::memset(&t, 0, sizeof(t));
        t.role = static_cast<u8>(role);
        t.on = true;
        t.last = __rdtsc();
    } else {
        t.frame = g_cp_frame.load(std::memory_order_acquire);
    }
}

void FrameEnd() {
    ThreadAcc& t = t_acc;
    if (!t.on || t.role != static_cast<u8>(Role::Cp)) {
        return;
    }
    const u32 f = t.frame;
    Row& row = g_store.rows[0][f % kMaxFrames];
    Flush(t, row, g_store.total[0], g_store.incl[0]);
    row.frame = f;
    row.census = g_census;
    AddCensus(g_store.total[0].census, g_census);
    g_census = {};
    t.frame = f + 1;
    g_cp_frame.store(f + 1, std::memory_order_release);
}

void FramePollSlow() {
    ThreadAcc& t = t_acc;
    const u32 g = g_cp_frame.load(std::memory_order_acquire);
    if (g == t.frame || t.role != static_cast<u8>(Role::Record)) {
        return;
    }
    Row& row = g_store.rows[1][t.frame % kMaxFrames];
    Flush(t, row, g_store.total[1], g_store.incl[1]);
    row.frame = t.frame;
    t.frame = g;
}

void PacketSample::Begin() {
    ThreadAcc& t = t_acc;
    if (!t.on || t.sampling || t.stack[t.depth] != static_cast<u8>(Sec::Packets) || ++t.sample_count < g_every) {
        return;
    }
    t.sample_count = 0;
    t.sampling = true;
    flushes_ = t.flushes;
    // (the Packets section's ticks as of now: charged + pending - the packet's own time is what it gains)
    a_ = t.self[static_cast<u32>(Sec::Packets)] + (__rdtsc() - t.last);
    active_ = true;
}

void PacketSample::OnDecoded(u8 opcode) {
    ThreadAcc& t = t_acc;
    if (t.flushes != flushes_ || t.stack[t.depth] != static_cast<u8>(Sec::Packets)) {
        active_ = false;
        t.sampling = false;
        return;
    }
    b_ = t.self[static_cast<u32>(Sec::Packets)] + (__rdtsc() - t.last);
    t.packet_ticks[0] += b_ - a_;
    ++t.packet_n[0];
    cls_ = static_cast<u8>(ClassOf(opcode));
    decoded_ = true;
}

void PacketSample::End() {
    ThreadAcc& t = t_acc;
    t.sampling = false;
    if (!decoded_ || t.flushes != flushes_ || t.stack[t.depth] != static_cast<u8>(Sec::Packets)) {
        return; // (no packet, or a flip marker's flush inside it: dropped)
    }
    const u64 c = t.self[static_cast<u32>(Sec::Packets)] + (__rdtsc() - t.last);
    t.packet_ticks[cls_] += c - b_;
    ++t.packet_n[cls_];
}

void NotePreparedSlow(const Cp::DrawDescriptor& draw, const Gpu::Shader::StageView& vs, const Gpu::Shader::StageView& ps) {
    ThreadAcc& t = t_acc;
    if (!t.on || t.role != static_cast<u8>(Role::Cp)) {
        return;
    }
    const u64 ticks = t.last_incl; // (the frontend's PrepareDraw: the section just left)
    bool same[2] = {false, false};
    const Gpu::Shader::StageView* views[2] = {&vs, &ps};
    for (u32 s = 0; s < 2; ++s) {
        const Gpu::Shader::StageView& v = *views[s];
        if (v.status != Gpu::Shader::StageStatus::Ready || v.program == nullptr || v.plan.flat.size() < 16) {
            continue;
        }
        const u64 result = ResultHash(v);
        u64 regs = 0x6a09e667f3bcc909ull;
        for (u32 i = 0; i < 16; ++i) {
            regs = Mix(regs, v.plan.flat[i]);
        }
        auto& c = g_census.stage[s];
        ++c[0];
        LastPrep& last = g_last[(u64{s} << 62) ^ v.address];
        if (last.result != 0 || last.regs != 0) {
            same[s] = last.result == result;
            c[1] += same[s] ? 1 : 0;
            c[2] += last.regs == regs ? 1 : 0;
            c[3] += same[s] && last.regs != regs ? 1 : 0;
            c[4] += !same[s] && last.regs == regs ? 1 : 0;
        }
        last = {result, regs};
    }
    const u32 cls = same[0] && same[1] ? 0 : same[1] ? 1 : same[0] ? 2 : 3;
    const u32 tag = draw.engine_run & 3;
    g_census.draw_ticks[cls][tag] += ticks;
    ++g_census.draws[cls][tag];
}

void NoteCopySlow(bool unchanged) {
    ThreadAcc& t = t_acc;
    if (!t.on || t.role != static_cast<u8>(Role::Cp)) {
        return;
    }
    ++g_census.copies;
    g_census.copy_ticks += t.last_incl;
    if (unchanged) {
        ++g_census.copies_unchanged;
        g_census.copy_unchanged_ticks += t.last_incl;
    }
}

namespace {

/// The rows of `role` for the frames in [from, to] s (by the processor thread's row of the same frame), summed.
u32 SumWindow(u32 role, double from, double to, Row& out) {
    std::memset(&out, 0, sizeof(out));
    u32 n = 0;
    const Row* cp = g_store.rows[0].get();
    const Row* rows = g_store.rows[role].get();
    for (u32 i = 0; i < kMaxFrames; ++i) {
        const Row& c = cp[i];
        if (c.valid == 0) {
            continue;
        }
        const double s = RowSeconds(c);
        if (s < from || s > to) {
            continue;
        }
        const Row& r = rows[i];
        if (r.valid == 0 || r.frame != c.frame) {
            if (role != 0) {
                ++n; // (thread 2 had no records that frame: an empty row)
            }
            continue;
        }
        ++n;
        for (u32 k = 0; k < kSections; ++k) {
            out.self[k] += r.self[k];
            out.calls[k] += r.calls[k];
        }
        for (u32 k = 0; k < kLights; ++k) {
            out.light_ticks[k] += r.light_ticks[k];
            out.light_n[k] += r.light_n[k];
        }
        for (u32 k = 0; k < kPacketClasses; ++k) {
            out.packet_ticks[k] += r.packet_ticks[k];
            out.packet_n[k] += r.packet_n[k];
        }
        out.overflow += r.overflow;
        AddCensus(out.census, r.census);
    }
    return n;
}

std::string Table(const Row& w, u32 frames, bool t2) {
    const double f = std::max<double>(frames, 1);
    const double ms_per_tick = 1000.0 / Freq();
    const auto ms = [&](u64 ticks) { return static_cast<double>(ticks) * ms_per_tick / f; };
    u64 wall = 0;
    std::array<u64, 4> by_cls{};
    for (u32 s = 0; s < kSections; ++s) {
        wall += w.self[s];
        by_cls[static_cast<u32>(kInfo[s].cls)] += w.self[s];
    }
    const u64 busy = wall - by_cls[static_cast<u32>(Cls::Idle)];
    u64 pairs = 0;
    for (u32 s = 0; s < kSections; ++s) {
        pairs += w.calls[s];
    }
    std::string out = fmt::format(
        "  {}: wall {:.2f} ms/frame = busy {:.2f} (compute {:.2f}, sync {:.2f}, wait {:.2f}) + idle {:.2f}; markers "
        "{:.0f}/frame (est. cost {:.2f} ms/frame at {:.0f} ticks a pair){}\n",
        t2 ? "thread 2 (SecondSonPC:GpuRecord)" : "thread 1 (SecondSonPC:GpuNative)", ms(wall), ms(busy),
        ms(by_cls[0]), ms(by_cls[1]), ms(by_cls[2]), ms(by_cls[3]), static_cast<double>(pairs) / f,
        ms(static_cast<u64>(static_cast<double>(pairs) * g_pair_ticks)), g_pair_ticks,
        w.overflow != 0 ? fmt::format(", stack overflows {}", w.overflow) : std::string{});
    std::vector<u32> order;
    for (u32 s = 0; s < kSections; ++s) {
        if (w.self[s] != 0 || w.calls[s] != 0) {
            order.push_back(s);
        }
    }
    std::ranges::sort(order, [&](u32 a, u32 b) { return w.self[a] > w.self[b]; });
    out += fmt::format("    {:<72} {:>8} {:>9} {:>7} {:>11} {:>9}\n", "section", "class", "ms/frame", "% busy",
                       "calls/frame", "ns/call");
    for (const u32 s : order) {
        out += fmt::format("    {:<72} {:>8} {:>9.3f} {:>7.2f} {:>11.1f} {:>9.0f}\n", kInfo[s].name,
                           kClsNames[static_cast<u32>(kInfo[s].cls)], ms(w.self[s]),
                           busy != 0 && kInfo[s].cls != Cls::Idle ? 100.0 * static_cast<double>(w.self[s]) /
                                                                        static_cast<double>(busy)
                                                                  : 0.0,
                           static_cast<double>(w.calls[s]) / f,
                           w.calls[s] != 0 ? static_cast<double>(w.self[s]) * ms_per_tick * 1e6 / w.calls[s] : 0.0);
    }
    if (!t2) {
        // the Packets section split by the sampled packets (scaled by N)
        u64 sampled = 0;
        for (u32 k = 0; k < kPacketClasses; ++k) {
            sampled += w.packet_ticks[k];
        }
        out += fmt::format("    sampled packets (1 of {}, scaled; their sum {:.3f} ms/frame against the Packets section "
                           "{:.3f}):\n", g_every, ms(sampled * g_every), ms(w.self[static_cast<u32>(Sec::Packets)]));
        for (u32 k = 0; k < kPacketClasses; ++k) {
            if (w.packet_n[k] == 0) {
                continue;
            }
            out += fmt::format("      {:<64} {:>9.3f} ms/frame {:>9.0f} packets/frame {:>7.0f} ns\n", kPacketNames[k],
                               ms(w.packet_ticks[k] * g_every),
                               static_cast<double>(w.packet_n[k]) * g_every / f,
                               static_cast<double>(w.packet_ticks[k]) * ms_per_tick * 1e6 / w.packet_n[k]);
        }
        for (u32 k = 0; k < kLights; ++k) {
            if (w.light_n[k] == 0) {
                continue;
            }
            out += fmt::format("      light, inclusive: {:<46} {:>9.3f} ms/frame {:>9.0f} calls/frame {:>7.0f} ns\n",
                               kLightNames[k], ms(w.light_ticks[k] * g_every),
                               static_cast<double>(w.light_n[k]) * g_every / f,
                               static_cast<double>(w.light_ticks[k]) * ms_per_tick * 1e6 / w.light_n[k]);
        }
    }
    return out;
}

std::string CensusText(const Census& c, u32 frames) {
    const double f = std::max<double>(frames, 1);
    const double ms_per_tick = 1000.0 / Freq();
    static constexpr std::string_view kCls[4] = {"both stages repeated", "PS only repeated", "VS only repeated",
                                                 "none repeated"};
    static constexpr std::string_view kTag[4] = {"untagged", "mesh", "shadow", "G-buffer"};
    std::string out = "  repetition census (SSPC_R8_CP_PROFILE=2; a stage repeated: its preparation's result - module, "
                      "walk output, descriptors - is its program's last one; the frontend's time per draw class):\n";
    for (u32 k = 0; k < 4; ++k) {
        u64 ticks = 0, n = 0;
        std::string tags;
        for (u32 e = 0; e < 4; ++e) {
            ticks += c.draw_ticks[k][e];
            n += c.draws[k][e];
            tags += fmt::format(" {} {:.0f}/{:.3f} ms", kTag[e], c.draws[k][e] / f,
                                static_cast<double>(c.draw_ticks[k][e]) * ms_per_tick / f);
        }
        out += fmt::format("    {:<22} {:>7.0f} draws/frame {:>8.3f} ms/frame |{}\n", kCls[k], n / f,
                           static_cast<double>(ticks) * ms_per_tick / f, tags);
    }
    static constexpr std::string_view kStage[2] = {"VS", "PS"};
    for (u32 s = 0; s < 2; ++s) {
        const auto& x = c.stage[s];
        const double n = std::max<double>(x[0], 1);
        out += fmt::format("    {} stages prepared {:.0f}/frame: same result {:.1f}%, same registers {:.1f}%, same "
                           "result with other registers {:.1f}%, same registers with another result {:.1f}%\n",
                           kStage[s], x[0] / f, 100.0 * x[1] / n, 100.0 * x[2] / n, 100.0 * x[3] / n, 100.0 * x[4] / n);
    }
    out += fmt::format("    stage copies {:.0f}/frame ({:.3f} ms), of them the view unchanged since the last copy "
                       "{:.0f}/frame ({:.3f} ms)\n",
                       c.copies / f, static_cast<double>(c.copy_ticks) * ms_per_tick / f, c.copies_unchanged / f,
                       static_cast<double>(c.copy_unchanged_ticks) * ms_per_tick / f);
    return out;
}

} // namespace

std::string Report() {
    if (g_mode == 0 || g_store.rows[0] == nullptr) {
        return {};
    }
    const auto [from, to] = Window();
    std::string out = fmt::format("R8 CP profile (SSPC_R8_CP_PROFILE={}; docs/NATIVE_ENGINE_R8_CP_PROFILE.md): self "
                                  "time per section (a marker inside another takes its time out of it)\n",
                                  g_mode);
    Row w{};
    const u32 n1 = SumWindow(0, from, to, w);
    out += fmt::format(" window {:.0f}..{:.0f} s: {} frames\n", from, to, n1);
    if (n1 != 0) {
        out += Table(w, n1, false);
        if (g_mode == 2) {
            out += CensusText(w.census, n1);
        }
        Row w2{};
        const u32 n2 = SumWindow(1, from, to, w2);
        out += Table(w2, std::max(n2, 1u), true);
    }
    const u32 frames = g_cp_frame.load(std::memory_order_acquire);
    out += fmt::format(" whole run: {} frames\n", frames);
    if (frames != 0) {
        out += Table(g_store.total[0], frames, false);
        if (g_mode == 2) {
            out += CensusText(g_store.total[0].census, frames);
        }
        out += Table(g_store.total[1], frames, true);
        // inclusive times of the outer sections (whole run)
        out += "  inclusive (whole run, ms/frame):";
        for (const Sec s : {Sec::Packets, Sec::JoinRun, Sec::ParDraw, Sec::PrepDraw, Sec::GeoFast, Sec::CbGds,
                            Sec::CbZpass, Sec::CbWrite, Sec::CbSync, Sec::CbFlip, Sec::MemRead, Sec::T2Draw}) {
            const u32 i = static_cast<u32>(s);
            const u32 role = i >= static_cast<u32>(Sec::T2Idle) ? 1 : 0;
            out += fmt::format(" {} {:.3f};", kInfo[i].name.substr(0, kInfo[i].name.find(' ')),
                               static_cast<double>(g_store.incl[role][i]) * 1000.0 / Freq() / frames);
        }
        out += "\n";
    }
    return out;
}

void WriteCsv(const std::filesystem::path& dir) {
    char v[8] = {};
    if (g_mode == 0 || g_store.rows[0] == nullptr || GetEnvironmentVariableA("SSPC_R8_CP_PROFILE_CSV", v, sizeof(v)) == 0 ||
        v[0] != '1') {
        return;
    }
    std::FILE* f = nullptr;
    if (_wfopen_s(&f, (dir / "r8_cp_profile.csv").c_str(), L"w") != 0 || f == nullptr) {
        return;
    }
    const double ms_per_tick = 1000.0 / Freq();
    std::string head = "frame,t_s";
    for (u32 r = 0; r < 2; ++r) {
        for (u32 s = 0; s < kSections; ++s) {
            if ((s >= static_cast<u32>(Sec::T2Idle)) == (r == 1)) {
                head += fmt::format(",{}ms_{},{}n_{}", r == 0 ? "t1" : "t2", s, r == 0 ? "t1" : "t2", s);
            }
        }
    }
    for (u32 k = 0; k < kPacketClasses; ++k) {
        head += fmt::format(",pkt_ms_{},pkt_n_{}", k, k);
    }
    for (u32 k = 0; k < kLights; ++k) {
        head += fmt::format(",light_ms_{},light_n_{}", k, k);
    }
    std::fprintf(f, "%s\n", head.c_str());
    // the sections' names (a comment row: "# <index> <name>")
    for (u32 s = 0; s < kSections; ++s) {
        std::fprintf(f, "# %u %s\n", s, std::string(kInfo[s].name).c_str());
    }
    const u32 frames = g_cp_frame.load(std::memory_order_acquire);
    const u32 first = frames > kMaxFrames ? frames - kMaxFrames : 0;
    for (u32 fr = first; fr < frames; ++fr) {
        const Row& c = g_store.rows[0][fr % kMaxFrames];
        if (c.valid == 0 || c.frame != fr) {
            continue;
        }
        const Row& t2 = g_store.rows[1][fr % kMaxFrames];
        const bool t2ok = t2.valid != 0 && t2.frame == fr;
        std::string line = fmt::format("{},{:.4f}", fr, RowSeconds(c));
        for (u32 r = 0; r < 2; ++r) {
            const Row& x = r == 0 ? c : t2;
            for (u32 s = 0; s < kSections; ++s) {
                if ((s >= static_cast<u32>(Sec::T2Idle)) == (r == 1)) {
                    const bool ok = r == 0 || t2ok;
                    line += fmt::format(",{:.4f},{}", ok ? static_cast<double>(x.self[s]) * ms_per_tick : 0.0,
                                        ok ? x.calls[s] : 0u);
                }
            }
        }
        for (u32 k = 0; k < kPacketClasses; ++k) {
            line += fmt::format(",{:.4f},{}", static_cast<double>(c.packet_ticks[k]) * g_every * ms_per_tick,
                                u64{c.packet_n[k]} * g_every);
        }
        for (u32 k = 0; k < kLights; ++k) {
            line += fmt::format(",{:.4f},{}", static_cast<double>(c.light_ticks[k]) * g_every * ms_per_tick,
                                u64{c.light_n[k]} * g_every);
        }
        std::fprintf(f, "%s\n", line.c_str());
    }
    std::fclose(f);
}

} // namespace SecondSon::Gpu::CpProfile
