// SecondSonPC R7 — the Native Render Layer: the contract between the native engines and the existing backend
// (owned by R7-Core; docs/NATIVE_ENGINE_R7_CORE.md §4).
//
//   Game engine  ->  Native Render Layer  ->  the existing backend (CommandProcessor -> Executor -> Vulkan)
//
// There is no second GPU backend and no second command processor. A native operation is the P5.3 native stream's
// (native_render_ops.h: StreamOp::Registers / State / Draw, a run token at the place in the context's DCB where the
// game's packets would be): the processor runs it through the entry a PM4 draw takes (Draw -> BuildDraw -> the
// frontend's stage preparation -> the executor), so pipelines, descriptors, memory, barriers and submission stay the
// existing ones, and the order relative to the game's packets (barriers, labels, other contexts) is the token's place.
// Whatever the layer cannot carry, the game's own path runs, unchanged.
//
// Who fills what (from the engine's own objects - no PM4 is read to build them):
//   R7-G / R7-S  NativeDrawCommand and its NativeDrawState for the geometry / shadow engines' draws;
//   R7-R         NativeResourceBindings - a stage's resources as the CUE holds them, versioned per stage;
//   R7-Core      NativeRenderContext and Submit (the stream, the fallback, the verification, the counters).
// A subsystem that needs another field adds it here only through R7-Core (a "[shared]" commit, §3 of the doc).
#pragma once

#include <atomic>

#include "base/abi.h"
#include "engine/render/native_render_ops.h"

namespace SecondSon::Engine::Render::R7 {

/// The subsystem that built a command (counters, routing).
enum class Producer : u8 { None = 0, GBuffer = 1, Shadows = 2, Resources = 3, Core = 4 };

/// The engine's draw function a command stands for (an eboot offset; 0: another caller of the draw wrappers).
enum class EngineFn : u32 {
    Other = 0,
    GBufferInstanced = 0x1a1770, ///< the mesh pass's instanced draw
    GBufferMesh = 0x1a1960,      ///< the mesh pass's single draw
    ShadowInstanced = 0x43d480,  ///< the shadow casters' instanced draw
};

/// One shader stage's resources as the CUE holds them (VS / PS: kStageVs / kStagePs of native_render_ops.h).
struct NativeStageResources {
    bool present = false;
    u64 shadow = 0;         ///< the stage's resource shadow (CUE + 0x90 + stage * 0x1800)
    u32 dwords = 0;         ///< what the commit copies of it (the layout table's first entry)
    u64 snapshot = 0;       ///< the commit's copy in the frame ring the user data point at (0: not committed)
    TableShape shape{};     ///< where the user-data pointers into the snapshot sit
    u32 direct_mask = 0;    ///< user-data SGPRs written directly (setUserDataRegion), one bit each
    u32 direct[16] = {};
    /// R7-R: changes whenever anything of the stage's resources changes (setters, material, commit); equal versions of
    /// one CUE and stage mean equal resources. 0: not tracked - nothing may be reused by it.
    u64 version = 0;
};

struct NativeResourceBindings {
    u64 cue = 0;
    NativeStageResources stage[kDrawStages];
};

/// What the draw's shaders and fixed function see, besides the resources.
struct NativeDrawState {
    s32 pass = -1; ///< 0..40; -2 pre, -3 post, -4 deformation, -1 another context (as DrawOp::pass)
    u32 sub = 0;
    bool shaders = false; ///< vs / ps known (the commit's ordinary shader path)
    bool has_ps = false;
    u32 vs[kVsRegs] = {};
    u32 ps[kPsRegs] = {};
    u32 prim_type = 0;     ///< VGT_PRIMITIVE_TYPE
    u32 index_size = 0;    ///< INDEX_TYPE (0: 16-bit, 1: 32-bit)
    u32 num_instances = 1; ///< NUM_INSTANCES
};

/// One draw as the engine meant it.
struct NativeDrawCommand {
    Producer producer = Producer::None;
    EngineFn fn = EngineFn::Other;
    bool indexed = false;
    u32 index_count = 0;
    u64 index_base = 0;
    /// CITY-OPT run hint (DrawRuns::Source in the low byte, DrawRuns::kMaterialChanged): what the engine's draw functions
    /// tag their draws with today, so a native draw keeps the Phase 5/6 runs (0: none)
    u32 run_tag = 0;
    NativeDrawState state;
    NativeResourceBindings resources;
};

/// The engine context a command is recorded into.
struct NativeRenderContext {
    u64 r = 0;   ///< the render context (pool slot, pre, post or deformation)
    u64 cue = 0; ///< its CUE (r + 0x128)
    u64 dcb = 0; ///< its command buffer (r + 8)
    s32 pass = -1;
    u32 sub = 0;
};

enum class SubmitResult : u8 {
    Native,   ///< the command is in the native stream; the caller must not run the game's path
    Fallback, ///< not carried (no room, an unsupported form, verification only): the caller runs the game's path
    Off,      ///< the layer is off (SSPC_R7_NATIVE_RENDER unset): the caller runs the game's path
};

/// SSPC_R7_NATIVE_RENDER (read once; off by default). R7-Core defines its values as the direct path grows.
bool Enabled();

/// The draw in the context's native stream at the DCB's write pointer, or Fallback/Off - then nothing was written.
SubmitResult Submit(const NativeRenderContext& ctx, const NativeDrawCommand& cmd);

/// A key of the state two draws must share to reuse one prepared draw state (shader registers, prim / index type, the
/// tables' shapes and versions); 0 when something in it is not known (no reuse).
u64 StateKey(const NativeDrawCommand& cmd);

struct LayerCounters {
    std::atomic<u64> submitted[5]{};  ///< by Producer
    std::atomic<u64> native[5]{};
    std::atomic<u64> fallback[5]{};
};
LayerCounters& Counters();

} // namespace SecondSon::Engine::Render::R7
