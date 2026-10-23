// SecondSonPC R8-A2 — the upper bound of reusing stage preparations by the native engine's knowledge (SSPC_R8_CP_DIAG=1,
// diagnostic, off by default; docs/NATIVE_ENGINE_R8_CP_OPTIMIZATION.md §4).
//
// Thread 1, around each full ShaderFrontend::PrepareDraw of ParallelCp::PrepareDraw (the draws GEOFAST, the runs and
// the filtered ones do not get there): the call's time, and per VS / PS stage the frontend really prepared (not its
// P4.7 memo) the key a reuse would need, compared with the last preparations of the same program on that stage:
//   base   program address, RSRC1/2, the runtime parameters, the binding start, the 16 user-data registers except the
//          pointers into the stage's snapshot (the direct SGPRs of 0x16fb0, the single values of 0x16970 / 0x169a0, the
//          global table pointer, the fetch shader: all of them registers the CP sees)
//   V      base + the R7 binding version of the snapshot (cp_native_table.h) + the frontend's memory generation
//   V0     base + the version (memory generation not compared)
//   C      base + the snapshot's bytes equal (read here) + the memory generation
//   C0     base + the bytes
// A key that matches: the stage's preparation now is compared with the one stored under it (module, bindings' end, the
// flattened data past the pointers, every binding) - "bad" counts the matches whose preparation differs (a reuse by that
// key would have been wrong). A draw whose two stages are each a memo hit or a match without a resource inside its
// snapshot is "covered": its whole frontend call is what a draw-level reuse could save. Every 8th stage prepared in full
// has its resource part (SRT walk, decoding, specialization key) run again here and timed: what a stage-level reuse
// saves; its walk's reads are watched too: a walk that read nothing outside the snapshot makes the preparation a function
// of the base key and the snapshot's bytes alone (what a reuse can rely on without the memory generation).
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/shader/shader_frontend.h"

namespace SecondSon::Gpu::Native::R8 {

namespace GS = Gpu::Shader;

class PrepDiag {
public:
    /// SSPC_R8_CP_DIAG=1 (read once).
    static bool On();
    PrepDiag();

    /// Thread 1, right before ShaderFrontend::PrepareDraw.
    void Before(const GS::ShaderFrontend& f);
    /// Thread 1, right after it: `ticks` the call (TSC); `memory` the frontend's reader.
    void After(const Cp::DrawDescriptor& d, const GS::ShaderFrontend& f, GS::GuestReader& memory, u64 ticks);
    /// The lines for gpu_native.txt.
    std::string Report() const;

    enum Key : u32 { kV, kV0, kC, kC0, kKeys };

private:
    struct Entry {
        bool valid = false;
        u64 address = 0;
        u32 rsrc1 = 0, rsrc2 = 0;
        GS::RuntimeParams runtime;
        GS::BindingStart start, end;
        std::array<u32, 16> ud{}; ///< pointers into the snapshot zeroed
        u32 pointer_mask = 0;
        u64 version = 0;
        u64 memory_gen = 0;
        std::vector<u32> bytes; ///< the snapshot's (empty: none found)
        const GS::Module* module = nullptr;
        GS::ResourceBindingPlan plan;
        u64 stamp = 0;
    };
    static constexpr u32 kWays = 4;
    struct Ways {
        std::array<Entry, kWays> e;
    };
    struct StageC {
        std::atomic<u64> full{0}, memo{0}, unready{0}, snap{0}, versioned{0}, prev{0}, touches{0}, stale{0};
        std::atomic<u64> cost_n{0}, cost_t{0}, cost_hit_n{0}, cost_hit_t{0};
        std::array<std::atomic<u64>, kKeys> hit{}, mru{}, bad{};
        /// of the sampled: the SRT walk read nothing outside the snapshot (and left the registers' part alone); of those,
        /// a C0 match with the same preparation and no binding inside the snapshot (provably reusable by bytes alone)
        std::atomic<u64> inside_n{0}, inside_c0_n{0}, inside_c0_t{0};
    };
    struct DrawC {
        std::atomic<u64> calls{0}, ticks{0};
        std::array<std::atomic<u64>, kKeys> both{}, both_t{}, ps_only{}, ps_only_t{}, vs_only{}, vs_only_t{};
    };
    static bool SamePlan(const GS::StageView& v, const Entry& e, u32 pointer_mask);
    /// The stage's resource part again (ResolveResources + BuildSpecializationKey as PrepareStage does them), TSC.
    /// `inside`: the walk read only [lo, hi) (lo == hi: no snapshot - false).
    u64 ResourceCost(const GS::StageView& v, std::span<const u32, 16> ud, GS::GuestReader& memory, u64 lo, u64 hi,
                     bool& inside);
    /// One line of every counter (the order Report's header names), for the window's deltas offline.
    std::string Line() const;

    std::unordered_map<u64, Ways> cache_[2]; ///< (VS, PS): by program address and RSRC1/2
    u64 stamp_ = 0;
    u64 gen_before_[2] = {}, memory_gen_ = 0;
    u32 sample_ = 0;
    std::vector<u32> bytes_;
    GS::ResourceBindingPlan scratch_;
    GS::SpecializationKey key_;
    std::array<std::array<StageC, 2>, 4> st_{}; ///< (engine_run, stage)
    std::array<DrawC, 4> dr_{};
    u64 calls_ = 0, last_log_ = 0, log_period_ = 0, t_start_ = 0;
};

} // namespace SecondSon::Gpu::Native::R8
