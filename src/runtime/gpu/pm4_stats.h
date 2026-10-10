// SecondSonPC — GPU command statistics (Phase 4, P4.1.2; docs/PHASE4_P41_GPU_FOUNDATION.md §3): what the game's
// command buffers carry and where the command processor's thread spends its time. A diagnostic capture: off unless
// --gpu-stats, then each hook is one load of a flag that never changes after start-up (the performance runs keep
// it off). With it on, the command processor (the bridge's Liverpool through patch 0027; SecondSonPC's own in P4.2)
// reports every packet it takes and SecondSonPC's decoder (pm4.h) counts it:
//   * per engine and opcode: packets, dwords, the thread's time from the packet to the next one (the packet's own
//     work: a draw packet's time is the renderer's work for that draw);
//   * registers written by SET_*_REG (per space, the first 1024 of each), NOP markers, event types, end-of-pipe
//     interrupt/data selects, DMA source/destination, waits, indirect buffers, draw indices, dispatch groups;
//   * spans outside packets: idle (no work), VideoOut label waits, the renderer's flush at a submit's end, queued
//     commands;
//   * one row per presented frame (the flip marker) with the frame's totals;
//   * packets SecondSonPC cannot name (and type-0 packets, decode errors): the first 32 kinds with their words.
// Bounded output: logs\gpu_stats.txt (tables), logs\gpu_frames.csv (at most `max_frames` rows),
// logs\gpu_stats_windows.csv (cumulative opcode counts every `snapshot_seconds`, at most 720 snapshots); every
// `report_seconds` a line in SecondSonPC.log. Written at the exit (an exit step) and by WriteNow().
#pragma once

#include <atomic>
#include <filesystem>

#include "base/abi.h"
#include "runtime/gpu/pm4.h"

namespace SecondSon::Gpu::Pm4Stats {

/// Set once by Enable() (release, after the capture's state exists); the command processor's thread may already run
/// then, so the hooks read it with acquire (a plain load on x86).
extern std::atomic<bool> g_enabled;

inline bool Enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

struct Options {
    std::filesystem::path dir;  ///< the files go here (logs)
    u32 report_seconds = 10;    ///< a SecondSonPC.log line (0: none)
    u32 snapshot_seconds = 5;   ///< cumulative opcode counts for per-window histograms (0: none)
    u32 max_frames = 20000;     ///< frame rows kept (about 5.5 min at 60 FPS)
    bool exit_step = true;      ///< write the files at the process's exit (Lifecycle)
};

/// --gpu-stats: allocates the tables, starts the report thread, adds the exit step. Once.
void Enable(const Options& options);

// --- the command processor's thread (one thread; the bridge's GpuCommandProcessor) ---

/// One packet at the start of `stream` (`available` dwords readable from it: the rest of its buffer).
void OnPacket(Pm4::Engine engine, const u32* stream, size_t available);

/// Time on the command processor's thread outside the packets' own work.
enum class Span : u8 {
    Idle,     ///< waiting for work
    VoWait,   ///< a WAIT_REG_MEM on a VideoOut label sleeping until the flip is done
    Flush,    ///< the renderer's flush at the end of the submits
    Commands, ///< commands other threads queued to the GPU thread (memory notifications, ...)
    Count,
};
void OnSpan(Span span, bool begin);

// --- any thread ---

/// A submission of command buffers (the guest's thread): dcb/acb and ccb sizes in dwords.
void OnSubmit(Pm4::Engine engine, size_t dwords, size_t ccb_dwords);

/// The renderer's own counts (atomics), to set against the packets.
enum class Counter : u8 {
    RendererDraws,       ///< draws the renderer executed
    RendererDispatches,  ///< dispatches the renderer executed
    PipelineBinds,       ///< graphics/compute pipelines bound for a draw/dispatch
    PipelineChanges,     ///< ... different from the previous one
    PipelinesCreated,    ///< pipelines created (not in the cache)
    ProgramsTranslated,  ///< shader permutations translated in this process
    RenderPassBegins,    ///< dynamic rendering scopes begun
    Barriers,            ///< pipeline barrier commands recorded
    Count,
};
void Count(Counter counter, u64 n = 1);

/// A pipeline bound for a draw or dispatch (the command processor's thread): PipelineBinds, and PipelineChanges
/// when it is not the previous one.
void OnPipelineBind(const void* pipeline);

/// Writes the files now (also the exit step). Thread-safe; returns false if a file cannot be written.
bool WriteNow();

namespace Test {
/// A self-test's own capture (no thread, no exit step; Enabled() stays false, so the command processor's hooks never
/// reach it; the test calls OnPacket/OnSpan itself); Reset() drops everything.
/// `command_thread`: the test runs a command thread of its own, so readers go through the real snapshot protocol
/// (want_snapshot / publication at a frame's end or at idle); otherwise the reader's thread publishes itself.
void EnableForTest(const Options& options, bool command_thread = false);
void Reset();
struct Summary {
    u64 packets = 0, dwords = 0, frames = 0, draws = 0, dispatches = 0, unknown_kinds = 0, decode_errors = 0;
    u64 context_regs = 0, sh_regs = 0, nop_markers[static_cast<size_t>(Pm4::NopMarker::Count)] = {};
};
Summary Take(); ///< from the command processor's thread (or with it stopped)
u64 OpcodeCount(Pm4::Engine engine, u8 opcode);
} // namespace Test

} // namespace SecondSon::Gpu::Pm4Stats
