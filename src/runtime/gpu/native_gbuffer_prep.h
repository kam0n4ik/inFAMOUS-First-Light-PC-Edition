// SecondSonPC - the Native Engine G-buffer draw preparation (0x1a1770), keyed by the draw's engine identity instead of
// the exact bytes of its register snapshot (SSPC_NATIVE_GBUFFER; off by default; docs/NATIVE_GBUFFER_DRAW_0x1a1770.md).
//
// The mesh pass draws the city's G-buffer with 0x1a1770 (engine/render/draw_runs.h, Source::GBuffer). Draw after draw
// the engine changes only the per-object data - the instance V# (0x61e260) and the dynamic constant buffer (0x120280,
// CUE slot 12) - while the program, the material's descriptor tables and the render targets stay. The frontend still
// prepares ~1263 such draws a frame in full (a program lookup, the module search, the SRT walk, the key, the pipeline
// lookup): 5.18 ms of the frontend thread at ring 128 (R8 CP profile, docs §0).
//
// This cache keeps, per engine identity (the VS/PS programs, the pipeline-state registers and the colour/depth targets),
// the preparation the frontend made for the first such draw. A later draw of the same identity does not reach the
// frontend: its stage plans are re-derived from its own live registers and guest memory through the command processor's
// RedecodeStage (ParallelCp; the same module, layout, binding start and key as the anchor, the descriptors re-read for
// this draw). No SRT program search, module lookup, key build or pipeline search; the SRT walk is taken again only for
// the registers that point at the per-object data, exactly as the walk did. The reuse is a true native path - it does
// not call the frontend's generic preparation at all; it is not a wrapper around it.
//
// Correctness is the anchor's: RedecodeStage recomputes the specialization key and the binding end from the re-derived
// plan and refuses (the draw falls back to the full frontend path) whenever they differ, so a coarse identity key only
// loses hits, never changes an image. Modes: off (the frontend prepares every draw), verify (the native plan is built
// and the frontend's is built too and the two are compared field by field; the frontend's is what the draw uses),
// guarded (the native plan is used; every SSPC_NATIVE_GBUFFER_SAMPLE-th hit is verified and the entry dropped on a
// mismatch, the cache disabled after too many), native (the native plan is used, no comparison).
#pragma once

#include <array>
#include <atomic>
#include <string>
#include <unordered_map>

#include "base/abi.h"
#include "runtime/gpu/shader/shader_frontend.h"

namespace SecondSon::Gpu {

class NativeGBuffer {
public:
    enum class Mode : u8 { Off, Verify, Guarded, Native };

    NativeGBuffer();

    Mode mode() const { return mode_; }
    bool On() const { return mode_ != Mode::Off; }
    /// guarded: verify (and keep or drop) every n-th hit (0: never).
    u32 sample() const { return sample_; }
    /// The cache is still trusted (guarded: not yet disabled by mismatches).
    bool trusted() const { return trusted_.load(std::memory_order_relaxed); }

    /// The identity a G-buffer draw is keyed on. Only what makes two draws' preparations share a module, layout and
    /// pipeline; the per-object registers are not here (they are re-read for every draw).
    struct Key {
        u64 vs_addr = 0, ps_addr = 0;
        u32 vs_rsrc1 = 0, vs_rsrc2 = 0, ps_rsrc1 = 0, ps_rsrc2 = 0;
        u64 pipeline_key = 0; ///< the hash of every pipeline-state register (Cp::kPipelineGroups)
        u64 color_hash = 0;   ///< the colour/depth target registers (Cp::Group::ColorTarget; not in the pipeline key)
        bool operator==(const Key&) const = default;
    };
    /// The frontend's preparation of one such draw, kept to re-derive later draws of the same identity.
    struct Entry {
        Key key;
        Shader::StageView vs, ps;
        Shader::DrawPreparation prep;
        const Shader::GraphicsPipelineState* state = nullptr;
        u64 last_frame = 0;
        bool usable = false; ///< false: this identity is known not reusable (its anchor could not be re-derived)
    };

    static u64 Hash(const Key& k);
    /// The entry for `k` (its hash `h`), or null; `lookup` counted. Not owning: valid until the next Store.
    Entry* Find(u64 h, const Key& k);
    /// Keep (or replace) the preparation of `k` as an anchor; `usable` false marks the identity not reusable.
    void Store(u64 h, const Entry& e);
    /// A draw reused an anchor (the native path, not the frontend).
    void NoteHit() { hits_.fetch_add(1, std::memory_order_relaxed); }
    /// A hit was refused by RedecodeStage (reason 2..6, see ParallelCp): the draw used the frontend after all.
    void NoteRefused(u32 why) { refused_[why & 7].fetch_add(1, std::memory_order_relaxed); }
    /// verify / guarded: a hit compared with the full preparation; `mismatch`: the two differed.
    void NoteVerified(bool mismatch);
    u64 frame() const { return frame_.load(std::memory_order_relaxed); }
    void NextFrame() { frame_.fetch_add(1, std::memory_order_relaxed); }

    std::string Report() const;

private:
    Mode mode_ = Mode::Off;
    u32 sample_ = 0;
    u32 max_entries_ = 32768;
    u32 disable_after_ = 64; ///< guarded: mismatches before the cache is turned off
    std::atomic<bool> trusted_{true};
    std::atomic<u64> frame_{0};
    std::unordered_map<u64, Entry> entries_; ///< by Hash(key) (collisions refused: the Key is compared)
    std::atomic<u64> lookups_{0}, hits_{0}, learned_{0}, unusable_{0}, collisions_{0}, full_{0}, verified_{0},
        mismatches_{0};
    std::array<std::atomic<u64>, 8> refused_{};
};

} // namespace SecondSon::Gpu
