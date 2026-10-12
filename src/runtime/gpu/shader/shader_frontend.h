// SecondSonPC — SecondSonPC's shader frontend: from a draw or dispatch of the command processor to translated
// modules, resolved resources and a pipeline (Phase 4, P4.3; docs/PHASE4_P43_SHADER_TRANSLATION.md §1, §8).
//
//   CommandProcessor (P4.2) ── DrawDescriptor / DispatchDescriptor + register file
//        │
//   ShaderFrontend::PrepareDraw / PrepareDispatch   (the owner thread: the command processor's)
//        │  per stage (fragment first: bindings are numbered across the pipeline's stages)
//        │    program at PGM_LO/HI ─ ProgramCache (content identity, revalidated once a frame or at every bind)
//        │    runtime parameters  ─ rebuilt only when their register groups changed (P4.2 versions)
//        │    layout              ─ the program's resources for these runtime parameters (its first translation)
//        │    fetch shader        ─ FetchCache
//        │    resources           ─ SRT walk + descriptors → ResourceBindingPlan
//        │    specialization key  ─ module: hit, or a new one queued for translation
//        ▼  pipeline: modules + fixed-function state → PipelineStateCache (skipped while nothing it reads changed)
//   DrawPreparation: ready (pipeline id, modules, plans) or waiting (named), for the executor (P4.4)
//
// Translations run on the frontend's worker thread (async) or at once in the caller (tests, offline). A draw whose
// modules are not ready yet is reported as waiting, never executed with something else. Nothing here executes GPU
// work, writes guest memory or tracks pages.
#pragma once

#include <intrin.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/runtime_params.h"
#include "runtime/gpu/shader/shader_cache.h"

namespace SecondSon::Gpu::Shader {

class ShaderDiskCache;

/// Log-scale histogram of nanoseconds (8 buckets per power of two): P50/P95/P99 within ~9 %.
class Histogram {
public:
    void Add(u64 ns);
    u64 Count() const { return count_; }
    double MeanNs() const { return count_ == 0 ? 0.0 : static_cast<double>(sum_) / static_cast<double>(count_); }
    u64 Percentile(double p) const; ///< ns (the bucket's upper bound)
    u64 Max() const { return max_; }
    void Merge(const Histogram& other);
    std::string Text() const; ///< "n, mean, P50/P95/P99, max"

private:
    std::array<u64, 512> buckets_{};
    u64 count_ = 0, sum_ = 0, max_ = 0;
};

/// The TSC as a clock (calibrated once): cheap timestamps around the frontend's steps.
inline u64 Ticks() {
    return __rdtsc();
}
u64 TicksToNs(u64 ticks);
/// The per-stage timers of the hot path (frontend steps, binding, buffer classification, ...): on with
/// SSPC_GPU_TIMINGS=1 only - tens of TSC reads a draw were a measurable share of the recording (P4.6). Off: FineTicks()
/// is 0 (the stage sums stay 0); the coarse timers (recording, submission, waits) always count.
/// P4.9: SSPC_GPU_TIMINGS=sample[:N] (N 32 when not given) - the same timers for every N-th draw/dispatch only (a
/// FineSample around it sets g_fine_now): the stage sums are 1/N of the whole; reports multiply them by FineScale().
extern const bool g_fine_timing;
extern const u32 g_fine_every;
extern thread_local bool g_fine_now; ///< per thread (P4.10: the frontend and the executor may run on two threads)
inline u64 FineTicks() {
    return (g_fine_timing || g_fine_now) ? __rdtsc() : 0;
}
inline u64 FineScale() {
    return g_fine_every != 0 ? g_fine_every : 1;
}
/// Around one draw/dispatch: its stage timers run when it is the counter's N-th (sampled mode only).
class FineSample {
public:
    explicit FineSample(u32& counter) {
        if (g_fine_every != 0 && ++counter >= g_fine_every) {
            counter = 0;
            g_fine_now = true;
        }
    }
    ~FineSample() { g_fine_now = false; }
    FineSample(const FineSample&) = delete;
    FineSample& operator=(const FineSample&) = delete;
};

enum class StageStatus : u8 {
    Ready,       ///< module ready, resources resolved
    Pending,     ///< a translation for it is queued or running
    Failed,      ///< its translation failed (the reason in the module)
    NoProgram,   ///< no program at PGM_LO/HI (unreadable, no footer, ...)
    FetchError,  ///< the fetch shader is unreadable or not of the driver's form
    Unsupported, ///< a stage SecondSonPC does not prepare (GS/ES/HS/LS)
    Count,
};
std::string_view StageStatusName(StageStatus status);

struct StageView {
    StageStatus status = StageStatus::NoProgram;
    u64 address = 0;
    Program* program = nullptr;
    u64 footer_hash = 0; ///< the footer at this address (a program by content may have several)
    Module* module = nullptr;
    RuntimeParams runtime;
    BindingStart start, end;
    ResourceBindingPlan plan;
    SpecializationKey key;
    ProgramError program_error = ProgramError::None;
};

struct DrawPreparation {
    bool ready = false;
    bool unsupported = false;
    u32 pipeline = 0; ///< PipelineStateCache id (ready draws)
    bool pipeline_reused = false;
};

struct FrontendOptions {
    bool async = true;             ///< translations on the worker thread (false: at once, in the caller)
    bool translate = true;         ///< false: programs, layouts already known, resources only (no new translation)
    Validation validation = Validation::PerFrame;
    u32 max_queue = 1024;          ///< queued translations at most (more wait for a later draw)
    const DeviceProfile* profile = nullptr; ///< required to translate
    ShaderTranslator* translator = nullptr; ///< nullptr: GetShaderTranslator()
    /// May a program with this footer hash be translated now? nullptr: always. In the game's shadow: only programs
    /// the bridge has translated (its recompiler asserts on code it cannot take; a program read mid-write or never
    /// drawn by the bridge is not handed to it).
    bool (*may_translate)(void* ctx, u64 footer_hash) = nullptr;
    void* may_translate_ctx = nullptr;
    u32 layout_retries = 2; ///< a first translation that failed reading guest memory (SRT, fetch shader) is retried
    /// P4.7: a stage with the inputs of its last ready preparation takes it again. Only for an owner that reports every
    /// guest memory change through GuestMemoryChanged() (the native backend's executor).
    bool stage_memo = false;
};

struct FrontendStats {
    u64 draws = 0, dispatches = 0, ready = 0, waiting = 0, unsupported = 0;
    std::array<u64, static_cast<size_t>(StageStatus::Count)> stage_status{};
    u64 program_reuse = 0;      ///< the stage kept its program without a cache lookup (same address, same epoch)
    u64 runtime_builds = 0, runtime_reuse = 0;
    u64 layouts = 0;            ///< first translations of a program for a runtime
    u64 module_hits = 0, module_misses = 0, module_collisions = 0, layout_mismatch = 0;
    u64 queued = 0, queue_full = 0, gated = 0, layout_retries = 0;
    u64 translations = 0, translation_failures = 0;
    u64 translate_us = 0, emit_us = 0, spirv_bytes = 0;
    u64 srt_compile_failures = 0, srt_walk_failures = 0, srt_faults = 0, srt_reads = 0;
    u64 pipeline_reuse = 0;     ///< the draw kept the previous pipeline (modules and its register groups unchanged)
    /// P4.7 stage memo: the stage's last preparation taken again (same program address, runtime, binding start, user
    /// data; no guest memory change since); misses: guest memory changed since / user data differ / anything else
    u64 memo_hits = 0, memo_miss_memory = 0, memo_miss_user_data = 0, memo_miss_other = 0;
    /// SSPC_DIAG_STAGE_MEMO=verify: hits prepared in full anyway / those whose full preparation differed
    u64 memo_verified = 0, memo_mismatches = 0;
    std::array<u64, static_cast<size_t>(ResourceStatus::Count)> resources{};
    u64 epochs = 0;
    /// P4.14 (the persistent shader cache): modules found on disk for programs met (stubs), read instead of translated,
    /// records that could not be used (translated instead), the translation time those reads saved (as recorded)
    u64 disk_stubs = 0, disk_loads = 0, disk_load_failures = 0, disk_saved_us = 0;
};

class ShaderFrontend {
public:
    explicit ShaderFrontend(const FrontendOptions& options);
    ~ShaderFrontend(); ///< stops the worker (queued translations are dropped)
    ShaderFrontend(const ShaderFrontend&) = delete;
    ShaderFrontend& operator=(const ShaderFrontend&) = delete;

    /// A draw: its descriptor and the register file it was built from (the command processor's, at the draw).
    const DrawPreparation& PrepareDraw(const Cp::DrawDescriptor& draw, const Cp::RegisterFile& regs,
                                       GuestReader& memory);
    /// A dispatch: its descriptor and its queue's compute registers.
    const DrawPreparation& PrepareDispatch(const Cp::DispatchDescriptor& dispatch,
                                           std::span<const u32, Cp::kComputeCount> cs, GuestReader& memory);
    /// A frame ended (the flip marker): an epoch for the programs' and fetch shaders' validation.
    void NewFrame();
    /// Guest memory written (P4.4's listener): programs there are read again at their next use.
    void Invalidate(u64 addr, u64 size);
    /// Guest memory may have changed (a GPU command's write, the command processor's, a met wait, a new submission):
    /// no stage takes its memoized preparation again - its resources are read anew (P4.7).
    void GuestMemoryChanged() { memory_gen_.fetch_add(1, std::memory_order_relaxed); }
    /// The generation of those changes (P4.7: the executor's streamed copies are reused while it stays).
    u64 MemoryGeneration() const { return memory_gen_.load(std::memory_order_relaxed); }
    /// SSPC_DIAG_MEMO_STATS=1: the stage memo's misses by cause (the user data dwords that differ, the input that
    /// changed); empty without it.
    std::string MemoReport() const;
    /// P4.14: the persistent shader cache (nullptr: none). A program met for the first time gets the modules the cache
    /// has for it (read when first needed instead of translated); new translations are added to it. Before the first
    /// draw.
    void SetDiskCache(ShaderDiskCache* cache) { disk_ = cache; }
    /// Modules read from the disk cache so far (any thread: the frame log samples it).
    u64 DiskLoads() const { return disk_loads_total_.load(std::memory_order_relaxed); }
    /// Waits until no translation is queued or running.
    void Drain();
    /// Stops the worker (the running translation finishes, queued ones are dropped); nothing is queued afterwards.
    /// Any thread (the process's exit step).
    void Stop();

    const StageView& Stage(HwStage stage) const;
    /// Translations run so far and their time (any thread: the frame log samples it at each flip).
    struct TranslationTotals {
        u64 count = 0, ns = 0, max_ns = 0;
    };
    TranslationTotals Translations() const {
        return {runs_total_.load(std::memory_order_relaxed), run_ns_total_.load(std::memory_order_relaxed),
                run_ns_max_.load(std::memory_order_relaxed)};
    }
    const FrontendStats& Stats() const { return stats_; }
    const ProgramCache& Programs() const { return programs_; }
    ProgramCache& MutablePrograms() { return programs_; }
    const PipelineStateCache& Pipelines() const { return pipelines_; }
    PipelineStateCache& MutablePipelines() { return pipelines_; }
    u32 Modules() const { return next_module_ - 1; }
    /// Bytes the frontend holds (code copies, SPIR-V, reflections, keys, pipelines).
    u64 MemoryBytes() const;
    std::string Report() const;

    struct Timings {
        Histogram program, resources, key, pipeline, draw, dispatch, translate, wait;
    };
    const Timings& Times() const { return times_; }

private:
    struct Job {
        Program* program = nullptr;
        Module* module = nullptr;
        u64 address = 0;
        GuestReader* memory = nullptr;
        std::vector<u32> flatbuf, fetch_code;
        std::vector<std::array<u32, 4>> vertex_sharps;
    };

    StageStatus PrepareStage(StageView& s, HwStage hw, SwStage sw, u64 addr, std::span<const u32, 16> user_data,
                             const BindingStart& start, GuestReader& memory, bool may_queue);
    Module* NewModule(Program& program, const RuntimeParams& runtime, const BindingStart& start,
                      std::span<const u32, 16> user_data);
    bool Enqueue(Job job);
    void Run(Job& job);
    /// P4.14: the disk cache's modules of a program met for the first time, as Pending modules that read their record
    /// when first needed.
    void RestoreFromDisk(Program& program);
    /// A module's record read in place of its translation; false: unusable (the module is translated instead).
    bool LoadFromDisk(Module& module);
    void Prepare(Module& module);
    void WorkerLoop();

    FrontendOptions options_;
    ShaderTranslator* translator_ = nullptr;
    ProgramCache programs_;
    FetchCache fetches_;
    PipelineStateCache pipelines_;
    std::array<StageView, static_cast<size_t>(HwStage::Count)> stages_{};
    std::array<bool, static_cast<size_t>(HwStage::Count)> runtime_valid_{};
    std::array<u64, static_cast<size_t>(HwStage::Count)> stage_epoch_{};
    // P4.7 stage memo: the inputs of each stage's last ready preparation (its StageView still holds the result). A
    // stage with the same inputs and no guest memory change since is ready as it was - no program lookup, SRT walk,
    // descriptor decode, key or module search (the foliage's thousands of draws repeat them). SSPC_DIAG_STAGE_MEMO=off:
    // every stage prepared in full.
    struct StageMemo {
        bool valid = false;
        u64 memory_gen = 0, program_epoch = 0, runtime_ver = 0, addr = 0;
        BindingStart start;
        std::array<u32, 16> user_data{};
    };
    std::array<StageMemo, static_cast<size_t>(HwStage::Count)> memo_{};
    std::array<u64, static_cast<size_t>(HwStage::Count)> runtime_ver_{}; ///< bumped at every runtime rebuild
    std::atomic<u64> memory_gen_{1}; ///< (atomic: a change may be reported from another thread)
    bool memo_on_ = false;
    bool memo_diag_ = false;                       ///< SSPC_DIAG_MEMO_STATS=1
    bool memo_verify_ = false;                     ///< SSPC_DIAG_STAGE_MEMO=verify
    std::unordered_map<u64, u64> memo_ud_masks_;   ///< (stage << 16 | the user data dwords that differ) -> misses
    std::array<u64, 5> memo_other_by_{};           ///< misses by input: address, binding start, runtime, epoch, status
    std::array<std::array<std::pair<u64, u64>, 8>, static_cast<size_t>(HwStage::Count)> memo_hist_{}; ///< (inputs, gen)
    std::array<u32, static_cast<size_t>(HwStage::Count)> memo_hist_next_{};
    std::array<u64, static_cast<size_t>(HwStage::Count)> memo_would_hit_{}; ///< misses an 8-entry memo would have had
    DrawPreparation prep_;
    u32 last_vs_ = 0, last_ps_ = 0, last_pipeline_ = 0;
    u32 next_module_ = 1;
    FrontendStats stats_;
    Timings times_;
    ShaderDiskCache* disk_ = nullptr;
    std::atomic<u64> disk_loads_total_{0};

    // the worker
    std::mutex mutex_;
    std::condition_variable cv_, idle_cv_;
    std::deque<Job> queue_;
    u32 running_ = 0;
    bool stop_ = false;
    std::thread worker_;
    // what the worker counts (under mutex_), folded into stats_ by the owner
    u64 w_translations_ = 0, w_failures_ = 0, w_translate_us_ = 0, w_emit_us_ = 0, w_spirv_bytes_ = 0;
    Histogram w_translate_;
    std::atomic<u64> runs_total_{0}, run_ns_total_{0}, run_ns_max_{0}; ///< Translations()
};

/// Guest memory through the GPU memory contract (CopySparse: never faults), for the frontend in the game.
GuestReader& GameMemory();

} // namespace SecondSon::Gpu::Shader
