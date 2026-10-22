// SecondSonPC - the Native Engine G-buffer draw path (SSPC_NATIVE_GBUFFER; off by default;
// docs/NATIVE_GBUFFER_DRAW_0x1a1770.md). A 0x1a1770 draw prepared from the engine's own state, not from PM4/SRT.
//
// The mesh pass draws the city's G-buffer with 0x1a1770 (engine/render/draw_runs.h). At each such draw the engine
// side (draw_runs.cpp) records into a side ring, indexed by the draw's trailing NOP:
//   * its descriptors from their source structures (gbuffer_draw.h Describe: the instance records' V#, the vertex
//     streams, the per-object constants - the GAME_VERIFIED Instanced formulas), and
//   * its VS / PS constant-update-engine state at the commit (gbuffer_draw.h CaptureStage): the CUE shadow 0x14ad0
//     copies to frame memory, and the user-data registers it points into that copy.
// What a stage's SRT walk reads through those registers *is* that shadow. This file is the command processor's
// consumer:
//   * a stage map, once per (module, CUE layout): which shadow dword each flattened user-data dword is - the module's
//     own walk run once over tagged registers (TagReader), never over guest memory;
//   * a draw's stages built from the engine's state through the map (no SRT walk: the flattened user data, the
//     vertex V#s), decoded as the full preparation decodes (DecodeResources), the specialization key checked;
//   * the heads (program, module, runtime parameters, pipeline) from a template: an earlier full preparation of
//     the same programs, pipeline registers (the processor's pipeline key), targets and CUE layouts. A template
//     outlives its frame: at its first use in a new one its programs are looked up again in the program cache (as
//     the full preparation looks them up: read again, or trusted by write tracking) and its fetch shader's code is
//     read and compared; a variant whose program or fetch shader is another now is dropped.
// verify: every draw prepared in full (the walk, what the draw uses) and built from the engine too, the two plans
// compared binding by binding (raw V#/T#/S#, addresses, sizes, bindings, statuses) and the flattened data dword by
// dword. guarded / native: the engine build is the draw's preparation; anything it does not describe (no record, a
// map the walk does not fit, no template, another key) takes the full path. guarded also checks every 64th engine
// build against the walk and turns the engine path off at the first difference.
#pragma once

#include <array>
#include <atomic>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "engine/render/draw_runs.h"
#include "native_engine/reconstructed/gbuffer_draw.h"
#include "runtime/gpu/command_processor.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/shader_frontend.h"

namespace SecondSon::Gpu {

namespace GDR = NativeEngine::Reconstructed::GBufferDraw;
namespace GS = Gpu::Shader;

class NativeGBuffer {
public:
    enum class Mode : u8 { Off, Verify, Guarded, Native };

    NativeGBuffer();

    Mode mode() const { return mode_; }
    bool On() const { return mode_ != Mode::Off; }
    /// guarded / native: the engine build is used (and not turned off by a guarded difference).
    bool Takes() const { return (mode_ == Mode::Guarded || mode_ == Mode::Native) && !tripped_; }

    /// A tagged G-buffer draw reached the preparation; `has`: its engine record was found (false: no record, or the
    /// ring slot reused since - the full path).
    void NoteRecord(bool has) {
        Bump(draws_);
        Bump(has ? with_record_ : no_record_);
    }

    /// guarded / native: prepare `d` from the engine record `e` and a template (see above) into `prep`, `state`, `vs`,
    /// `ps`. false: the full path (the reason is counted). `programs`, `memory`: the frontend's program cache and its
    /// guest reader (a template's validation in a new frame); `targets`: a hash of the draw's colour/depth target
    /// registers (the register file's group hashes).
    bool Prepare(const Cp::DrawDescriptor& d, const Engine::Render::DrawRuns::EngineDraw& e,
                 GS::ProgramCache& programs, GS::GuestReader& memory, u64 targets, GS::DrawPreparation& prep,
                 const GS::GraphicsPipelineState*& state, GS::StageView& vs, GS::StageView& ps);

    /// After `d`'s full preparation (`prep`, `state`, its VS/PS views): keep it as its key's template; `check` (verify:
    /// every draw, guarded: its samples) builds the draw from `e` as Prepare would and compares it with the walk's.
    void AfterFull(const Cp::DrawDescriptor& d, const Engine::Render::DrawRuns::EngineDraw& e,
                   GS::ProgramCache& programs, GS::GuestReader& memory, u64 targets, const GS::DrawPreparation& prep,
                   const GS::GraphicsPipelineState* state, const GS::StageView& vs, const GS::StageView& ps,
                   bool check);

    /// guarded: this engine build is one to check against a full preparation (every 64th).
    bool SampleCheck() { return mode_ == Mode::Guarded && (++sample_ & 63) == 0; }

    std::string Report() const;

private:
    static constexpr u16 kZero = 0xffff; ///< a flattened dword the walk does not write (0 in both)

    /// Where a stage's flattened user data comes from in its CUE copy (one module's walk over one CUE layout).
    struct StageMap {
        const GS::Module* first = nullptr; ///< the layout's first module (whose walk PrepareStage runs)
        u64 layout = 0;                    ///< the CUE layout (GDR::StageShadow::layout)
        bool ok = false;
        u8 why = 0;       ///< not ok: 1 no walk / not ready, 2 the walk failed, 3 it read memory not of the copy,
                          ///< 4 it wrote the registers, 5 a dword of unknown origin, 6 past the copy, 7 a vertex V#
        u32 ud_read = 0;  ///< the registers the walk reads (RunStats::user_data_read)
        u32 flat_size = 0;
        std::vector<u16> flat;   ///< [16, flat_size): the copy's dword each is (kZero: not written)
        std::vector<u16> vertex; ///< per fetch attribute: the copy's dword its V# starts at
    };

    struct Key {
        u64 vs = 0, ps = 0;    ///< program addresses
        u64 pipeline_key = 0;  ///< the processor's hash of every pipeline-group register
        u64 targets = 0;       ///< the colour/depth target registers' hash (runtime parameters and pipeline read them)
        u64 vs_layout = 0, ps_layout = 0;
        bool operator==(const Key&) const = default;
    };
    /// One full preparation of a key: its modules (specialized on the plans' descriptors: a material's texture types
    /// select another module of the same programs), fetch shader and pipeline.
    struct Variant {
        GS::DrawPreparation prep;
        const GS::GraphicsPipelineState* state = nullptr;
        GS::StageView vs, ps; ///< the heads (their plans are each draw's)
        u64 fetch = 0;        ///< the VS fetch shader's address (user data at the fetch registers)
        u32 fetch_reg = 0xff; ///< (0xff: no fetch shader)
        std::vector<u32> fetch_code; ///< its code up to its s_setpc_b64 (a new frame compares it)
    };
    static constexpr u32 kVariants = 16; ///< (a material's texture formats select modules: city keys have more than 4)
    struct Template {
        Key key;
        u64 epoch = 0; ///< the program cache epoch its variants were validated in
        std::array<Variant, kVariants> v;
        u32 n = 0, next = 0; ///< variants filled; the one a new variant replaces when all are
    };
    static constexpr size_t kMaxTemplates = 8192; ///< (more keys: all dropped, made again)

    static void Bump(std::atomic<u64>& c, u64 n = 1) {
        c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed); // (one writer thread)
    }
    static Key KeyOf(const Cp::DrawDescriptor& d, const Engine::Render::DrawRuns::EngineDraw& e, u64 targets);
    static u64 HashOf(const Key& k);
    static u64 FetchOf(const GS::StageView& vs, std::span<const u32, 16> ud, u32& reg);
    /// The fetch shader at `addr` (its address pair at `reg`) up to its s_setpc_b64, as the fetch cache reads it.
    static bool ReadFetch(GS::GuestReader& memory, u64 addr, u32 reg, std::vector<u32>& code);
    /// A template first used in a new frame: its variants' programs and fetch shaders validated again (see above),
    /// those changed dropped. false: none is left.
    bool Revalidate(Template& t, GS::ProgramCache& programs, GS::GuestReader& memory);

    /// The map of `v`'s stage for CUE state `sh` (made on first use). nullptr: the view has no first module yet.
    const StageMap* MapOf(const GS::StageView& v, const GDR::StageShadow& sh);
    void MakeMap(StageMap& m, const GS::Module& first, const GDR::StageShadow& sh);
    /// `out` = `head`'s stage for this draw, its plan from the CUE state `sh` through `m`, `key` its specialization
    /// (the module it needs). false: the head's module is not ready.
    bool BuildStage(const GS::StageView& head, const StageMap& m, const GDR::StageShadow& sh,
                    std::span<const u32, 16> ud, GS::StageView& out, GS::SpecializationKey& key);
    /// verify: `built` against the walk's `full`, binding by binding; counts by part, logs the first ones. true: same.
    bool Compare(const GS::StageView& full, const GS::StageView& built, u32 stage);

    Mode mode_ = Mode::Off;
    bool tripped_ = false; ///< guarded: a checked build differed - the engine path is off from then on
    u64 sample_ = 0;
    std::unordered_map<u64, std::unique_ptr<StageMap>> maps_;
    std::unordered_map<u64, Template> templates_;
    std::vector<std::array<u32, 4>> vreads_;
    std::vector<u32> fetch_code_, fetch_read_;
    GS::SpecializationKey key_vs_, key_ps_;
    GS::StageView check_vs_, check_ps_; ///< verify: the engine builds compared with the walk's

    std::atomic<u64> draws_{0}, with_record_{0}, no_record_{0};
    std::atomic<u64> taken_{0};            ///< prepared from the engine (guarded / native)
    std::array<std::atomic<u64>, 12> refused_{}; ///< by reason (Report)
    std::array<std::atomic<u64>, 8> map_why_{};  ///< maps made, by StageMap::why (0: ok)
    std::atomic<u64> templates_made_{0}, variants_made_{0}, templates_dropped_{0};
    std::atomic<u64> revalidated_{0}, variants_changed_{0}, fetch_unread_{0};
    // verify / guarded checks: stages compared, the same, different by part (flat, buffers, images, samplers,
    // vertices, heads)
    std::atomic<u64> compared_{0}, same_{0};
    std::array<std::atomic<u64>, 6> diff_{};
    std::atomic<u64> built_failed_{0}; ///< verify: a draw whose engine build was refused (why: refused_)
    std::atomic<u64> logged_{0};
};

} // namespace SecondSon::Gpu
