// SecondSonPC - the Native Engine G-buffer draw path (SSPC_NATIVE_GBUFFER; off by default;
// docs/NATIVE_GBUFFER_DRAW_0x1a1770.md). The descriptors of a 0x1a1770 draw taken from the engine's own structures.
//
// The mesh pass draws the city's G-buffer with 0x1a1770 (engine/render/draw_runs.h). Its per-draw resources - the
// instance records' V#, the vertex-stream V#s, the per-object constant buffer - are built by the engine from
// (records, n, mesh); the engine commits them into the constant update engine, and today the command processor reads
// them back by walking the shader's SRT over the committed register snapshot. The native path instead takes those
// descriptors from the engine's source structures at the draw (draw_runs.cpp captures them with the GAME_VERIFIED
// Instanced formulas - gbuffer_draw.h Describe - into a side ring, indexed by the draw's trailing NOP), so no PM4/SRT
// processing of those resources is needed.
//
// This file is the command processor's consumer. verify: build the engine descriptors and the SRT walk both and
// compare them descriptor by descriptor (the gate before the walk is skipped); guarded / native use the engine
// descriptors and fall back to the walk when a draw has no valid engine record. The material's PS textures/samplers
// (0x448640) are the stable per-material part; they are not per-draw and are covered by the walk/caches for now
// (docs §5: what is engine-sourced and what is not, with addresses).
#pragma once

#include <array>
#include <atomic>
#include <string>

#include "base/abi.h"
#include "native_engine/reconstructed/gbuffer_draw.h"
#include "runtime/gpu/shader/shader_resources.h"

namespace SecondSon::Gpu {

namespace GDR = NativeEngine::Reconstructed::GBufferDraw;
namespace GS = Gpu::Shader;

class NativeGBuffer {
public:
    enum class Mode : u8 { Off, Verify, Guarded, Native };

    NativeGBuffer();

    Mode mode() const { return mode_; }
    bool On() const { return mode_ != Mode::Off; }

    /// A tagged G-buffer draw was prepared; `had_record`: a valid engine record was found for it (false: the ring
    /// slot was reused, or the draw carried no engine descriptors - the full path is used).
    void NoteDraw(bool had_record) {
        draws_.fetch_add(1, std::memory_order_relaxed);
        (had_record ? with_record_ : no_record_).fetch_add(1, std::memory_order_relaxed);
    }

    /// verify: compare the engine descriptors `d` of a draw with the SRT walk's plans; count matches and mismatches.
    /// A descriptor matches when some binding of the walk's plan holds the same V# (value match - the walk resolves
    /// the same bytes the engine supplied). `vs` / `ps` are the draw's VS / PS resource plans.
    void Verify(const GDR::DrawDescription& d, const GS::ResourceBindingPlan& vs,
                const GS::ResourceBindingPlan& ps);

    std::string Report() const;

private:
    Mode mode_ = Mode::Off;
    std::atomic<u64> draws_{0}, with_record_{0}, no_record_{0};
    // verify, by resource kind (0 buffer / instance V#, 1 vertex stream, 2 draw constant buffer): engine descriptors
    // seen, those a walk binding reproduced (value match), those none did
    std::array<std::atomic<u64>, 3> seen_{}, matched_{}, unmatched_{};
    std::atomic<u64> verified_draws_{0};  ///< draws whose every engine descriptor matched a walk binding
    std::atomic<u64> mismatch_draws_{0};  ///< draws with at least one engine descriptor no walk binding held
    std::atomic<u64> mismatch_logged_{0};
};

} // namespace SecondSon::Gpu
