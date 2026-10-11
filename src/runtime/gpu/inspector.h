// SecondSonPC — the Performance Inspector (Phase 4, P4.8; docs/PHASE4_P48_PERFORMANCE_INSPECTOR.md). Continuous
// profiling of the game's ordinary frames with the native backend (--gpu-backend sspc): what limits the frame rate in a
// scene, which subsystems cost what, where the frame's time is lost.
//
// Off unless SSPC_INSPECTOR=light[,<options>] - then nothing of it exists and every hook is one relaxed load and a
// branch. On, it streams to SSPC_INSPECTOR_DIR (default logs\performance_inspector\<time>), once a second:
//   frames.csv    one row per frame: the native backend's frame log (FrameLog: the game's submission and flip, the
//                 command processor's flip marker, GPU completion, presentation, the GPU thread's busy/idle time) and
//                 the inspector's columns: the GPU thread's time between flip markers split exactly (submissions and
//                 invalidations, command processor rounds, polls, flushes, idle by what it waited for and whether the
//                 GPU had work), its waits for the GPU by source, uploads/write-backs/streams, the game's file reads,
//                 the flipping game thread's CPU time, and per effect category the draws, dispatches, recording CPU
//                 time and GPU time (timestamps at category changes only, read when the batch completed - never
//                 waited for)
//   seconds.csv   per second: process and system memory (private, working set, system commit apart), VRAM (DXGI: the
//                 process as the OS counts it) and SecondSonPC's own images/buffers, instances/vertices/workgroups per
//                 category
//   threads.csv   per second: every thread's CPU time (QueryThreadCycleTime)
//   cpu_stacks.csv / stacks.csv / modules.csv  the GPU thread sampled with its call stack (and the flipping game
//                 thread's instruction pointer) - where the command processor's CPU time goes, by function; symbolized
//                 offline (tools\performance_inspector.py, llvm-symbolizer with the PDB)
//   markers.csv   Ctrl+F12 pressed (a moment the player marked)
//   session.json  what ran, the clocks, the files' totals
// Options: gpu=<N> (category timestamps in every N-th frame, 0 = none; default 1), sample=<Hz> (stack samples of the
// GPU thread, 0 = none; default 250), catalog=<file> (default tools\perf_catalog.txt).
#pragma once

#include <windows.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include "base/abi.h"

namespace SecondSon::Gpu::Inspector {

// ------------------------------------------------------------------------------------------------ effect categories
// (inspector_catalog.cpp). A draw takes the category its pixel shader's catalog entry names, else its vertex shader's,
// else a rule from its targets (Basis::Rule), else Unknown; a dispatch its compute shader's, else ComputeOther (rule).
// Never by pipeline number (those are per-run).
enum class Category : u8 {
    Unknown,
    Smoke,
    Fire,
    Particles,
    ParticleSimulation,
    ParticleSorting,
    Foliage,
    Shadows,
    Water,
    Reflections,
    Lighting,
    PostProcessing,
    Terrain,
    Geometry,
    ComputeOther,
    Count,
};
constexpr size_t kCategories = static_cast<size_t>(Category::Count);
/// GPU time also has slots for work that is no effect's: write-backs and copies on the timeline, the flip copy, and
/// a batch's start before its first command.
constexpr size_t kGpuTransfer = kCategories, kGpuFrameCopy = kCategories + 1, kGpuBatchStart = kCategories + 2;
constexpr size_t kGpuSlots = kCategories + 3;

std::string_view CategoryName(Category c);
bool CategoryFromName(std::string_view name, Category& out);

/// Why a command has its category.
enum class Basis : u8 {
    None,      ///< Unknown
    Rule,      ///< inferred from the targets/shape (shadow maps: square depth-only targets; ...)
    Probable,  ///< a catalog entry: seen with the effect, not isolated by a measurement
    Shared,    ///< a catalog entry: serves several effects (the particle emitters); its time is not split among them
    Confirmed, ///< a catalog entry proved by a measurement (bisection, a skip test, a trace)
};
std::string_view BasisName(Basis b);
bool BasisFromName(std::string_view name, Basis& out);

struct Class {
    Category category = Category::Unknown;
    Basis basis = Basis::None;
};

/// What a draw looks like: the rules for draws no catalog entry names.
struct DrawShape {
    u32 colors = 0;   ///< colour targets bound
    bool depth = false;
    u32 width = 0, height = 0;
    u32 primitive = 0; ///< the guest's VGT primitive type
    u32 count = 0;     ///< vertices/indices (direct draws)
    u32 instances = 1;
    bool indexed = false, indirect = false;
};
/// Shadows: depth only, square, at least 256 (the cascades are 1024x1024); Geometry: depth only otherwise (depth
/// passes), or three or more colour targets (the G-buffer); PostProcessing: one or two colour targets, no depth, a direct
/// non-indexed draw of at most 6 vertices, one instance (a full-screen pass); otherwise Unknown.
Class RuleForDraw(const DrawShape& s);

/// The effect catalog: programs by stage and content hash (XXH3-128, the low 64 bits - the identity SSPC_DIAG_CMD_TRACE
/// prints), tools\perf_catalog.txt. Lines: `<vs|ps|cs> <hex content> <category> <confirmed|probable|shared> <evidence>`.
class Catalog {
public:
    /// Adds the text's entries; returns the number of malformed lines (the first one's text in `first_error`).
    u32 Parse(std::string_view text, std::string* first_error = nullptr);
    bool Load(const std::filesystem::path& file, std::string& error);
    /// `stage`: 'v', 'p' or 'c'. nullptr: not in the catalog.
    const Class* Find(char stage, u64 content) const;
    size_t Size() const { return vs_.size() + ps_.size() + cs_.size(); }
    /// The draw's class from the catalog only (nullptr: neither stage is in it).
    const Class* DrawEntry(u64 vs_content, u64 ps_content, bool has_ps) const;
    Class Draw(u64 vs_content, u64 ps_content, bool has_ps, const DrawShape& shape) const;
    Class Dispatch(u64 cs_content) const;

private:
    std::unordered_map<u64, Class> vs_, ps_, cs_;
};

// ------------------------------------------------------------------------------------------------ the inspector
namespace detail {
extern std::atomic<bool> g_on;
}
/// The inspector runs (SSPC_INSPECTOR set and the native backend started it).
inline bool On() {
    return detail::g_on.load(std::memory_order_relaxed);
}

struct StartInfo {
    std::filesystem::path logs_dir; ///< the backend's logs (the default session directory is under it)
    std::string device;
    double timestamp_period = 0.0;  ///< ns per GPU timestamp tick (0: no timestamps)
    bool fine_timing = false;       ///< SSPC_GPU_TIMINGS=1: the recording's stage timers run
};
/// Reads SSPC_INSPECTOR; false when it is not set (or not valid: logged). The native backend, before its threads.
bool Start(const StartInfo& info);
/// The GPU thread exists now: its stack samples start (if sample= is not 0).
void SetGpuThread(HANDLE thread);
/// Stops the stack samples (before the sampled threads end).
void StopSampling();
/// Flushes the files, stops the threads. The native backend, after its threads stopped.
void Stop();
/// The catalog (nullptr when off).
const Catalog* GetCatalog();
/// A file of the session (e.g. pipelines.csv at the stop). false: off or not written.
bool WriteFile(std::string_view name, const std::string& text);
/// Category timestamps in every N-th frame (0: none).
u32 GpuEvery();

/// Running totals per category, kept by the executor (GPU thread).
struct CategoryCounters {
    u64 draws = 0, instances = 0, vertices = 0, dispatches = 0, groups = 0;
    u64 cpu_ticks = 0; ///< TSC ticks recording them (Executor::Draw/Dispatch from start to end, the frontend included)
};
using CategoryTotals = std::array<CategoryCounters, kCategories>;

/// The GPU thread's running totals at a flip marker, beyond FrameLog::GpuThreadTotals (ns unless named).
struct CpTotals {
    u64 pre_ns = 0;       ///< the loop's work before a round: submissions taken, invalidations, the CPU's writes drained
    u64 round_ns = 0;     ///< command processor rounds (PM4, recording, its waits for the GPU)
    u64 poll_ns = 0, flush_ns = 0;
    u64 idle_label_ns = 0, idle_other_ns = 0, idle_empty_ns = 0; ///< idle by what it waited for (FrameLog's three)
    u64 idle_gpu_ns = 0;  ///< of the idle time: while the GPU had work
    std::array<u64, 3> sync_ns{}, syncs{}; ///< waits for the GPU: processor packets, frontend reads, processor reads
    u64 submit_ns = 0;    ///< vkQueueSubmit2
    u64 wb_bytes = 0, buf_upload_bytes = 0, img_uploads = 0, img_upload_bytes = 0, img_created = 0;
    u64 streamed_bytes = 0, mirrored_bytes = 0, barriers = 0, render_passes = 0;
    u64 img_resident_bytes = 0, buf_resident_bytes = 0; ///< levels, not totals
    /// SSPC_GPU_TIMINGS=1 only (TSC ticks): frontend, binding, guest ranges, images, pipeline, targets, vertex/index,
    /// barriers/layouts, commands, dispatches
    std::array<u64, 10> stage_ticks{};
    CategoryTotals categories{};
};
/// The GPU thread at flip marker `n` (FrameLog's index), right after FrameLog::CpFlip.
void CpFrame(u32 n, const CpTotals& totals);

/// The GPU thread once frame `n`'s last batch completed: its GPU time (the union of its batches' intervals), per slot
/// (kGpuSlots: the categories, then transfers, the flip copy, batch starts; all zero when the frame was not timed).
struct GpuFrameTimes {
    u64 busy_ns = 0, batches = 0;
    bool timed = false; ///< category timestamps were recorded in this frame
    u32 overflow = 0;   ///< batches whose category changes did not all fit (the rest counted to the last category)
    std::array<u64, kGpuSlots> slot_ns{};
};
void GpuFrame(u32 n, const GpuFrameTimes& times);

/// A game thread read a file (sceKernelRead/Pread/_read): bytes read, time.
void FileRead(u64 bytes, u64 ns);
/// The game thread that requested the flip (sceGnmSubmitAndFlip): its CPU time is sampled per frame.
void FlipThread();
/// A guest memory map/unmap (the backend's memory listener).
void GuestMap(bool unmap);

} // namespace SecondSon::Gpu::Inspector
