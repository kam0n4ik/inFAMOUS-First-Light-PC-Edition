// SecondSonPC — R7-S: the shadow engine's instanced draw as a Native Render Layer command (docs/NATIVE_ENGINE_R7_SHADOWS.md
// §4; the contract: src/engine/render/native_render_layer.h, R7-Core).
//
// At the entry of 0x43d480 (pass, R, records, n, mesh) everything the draw will be is known from the engine's own
// objects - no PM4 is read:
//   index buffer   mesh + 0x58 (base), mesh + 0x68 (u32 index count), 16-bit indices (INDEX_TYPE 0), TriList (4);
//   instances      n (32-bit), their records (object x cascade view-projection, 0x48 bytes each) at `records`,
//                  bound as one V# (stride 0x48, n records) into the VS slot pass + 0x121;
//   vertices       the V# table at mesh + 0x60, mesh + 0x6c of them, VS slot 0;
//   context        R: CUE R + 0x128, DCB R + 8; the pass key of the pool slot (as native_draw.cpp's ContextPass).
// What the entry cannot know is left unset: the shaders (state.shaders = false; the material bind 0x448640 set them in
// the CUE before) and the resources (R7-R: NativeResourceBindings, from the CUE at the commit).
//
// CheckAgainstTrace compares a command with the calls the original 0x43d480 made (a shadow_trace recording): the
// verify of the registry's shadow_instanced_draw does that for every checked call (shadow_loop.cpp).
#pragma once

#include <string>

#include "base/abi.h"
#include "engine/render/native_render_layer.h"
#include "native_engine/reconstructed/shadow_trace.h"

namespace SecondSon::NativeEngine::R7Shadows {

namespace NRL = Engine::Render::R7;

/// The shadow draw's own inputs beyond NativeDrawCommand (candidates for R7-Core's [shared] fields).
struct ShadowDrawInputs {
    u64 records = 0;          ///< the instance records (V# base)
    u32 record_stride = 0x48; ///< V# stride
    u32 instances = 0;        ///< V# num_records = NUM_INSTANCES
    u32 instance_slot = 0;    ///< VS buffer slot of the records (pass + 0x121)
    u64 vertex_table = 0;     ///< mesh + 0x60: the vertex buffers' V# table
    u32 vertex_buffers = 0;   ///< mesh + 0x6c
};

/// The command 0x43d480(pass, r, records, n, mesh) stands for, filled at its entry. `module_base`: for the pass key.
void BuildInstancedDraw(u64 module_base, u64 pass, u64 r, u64 records, u64 n, u64 mesh, NRL::NativeRenderContext& ctx,
                        NRL::NativeDrawCommand& cmd, ShadowDrawInputs& inputs);

/// "" when the recorded calls of one original 0x43d480 run (the 9 callees of shadow_loop.cpp's table, in order) made
/// exactly the draw `cmd` / `inputs` describe; else the first difference.
std::string CheckAgainstTrace(const NRL::NativeRenderContext& ctx, const NRL::NativeDrawCommand& cmd,
                              const ShadowDrawInputs& inputs, const Reconstructed::STrace::Trace& t);

} // namespace SecondSon::NativeEngine::R7Shadows
