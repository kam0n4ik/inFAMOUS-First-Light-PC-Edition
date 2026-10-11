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
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/runtime_params.h"
#include "runtime/gpu/shader/shader_cache.h"

namespace SecondSon::Gpu::Shader {

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
extern const bool g_fine_timing;
inline u64 FineTicks() {
    return g_fine_timing ? __rdtsc() : 0;
}

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
    std::array<u64, static_cast<size_t>(ResourceStatus::Count)> resources{};
    u64 epochs = 0;
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
    /// Waits until no translation is queued or running.
    void Drain();
    /// Stops the worker (the running translation finishes, queued ones are dropped); nothing is queued afterwards.
    /// Any thread (the process's exit step).
    void Stop();

    const StageView& Stage(HwStage stage) const;
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
    DrawPreparation prep_;
    u32 last_vs_ = 0, last_ps_ = 0, last_pipeline_ = 0;
    u32 next_module_ = 1;
    FrontendStats stats_;
    Timings times_;

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
};

/// Guest memory through the GPU memory contract (CopySparse: never faults), for the frontend in the game.
GuestReader& GameMemory();

} // namespace SecondSon::Gpu::Shader
