// SecondSonPC R7-G — the geometry engine's draws as Native Render Layer commands (native_render_layer.h; input of
// R7-Core's direct path C3, docs/NATIVE_ENGINE_R7_GBUFFER.md §5).
//
// Built at the ENTRY of the engine's draw function, from the engine's objects only (no PM4 read):
//   0x1a1770(M, R, records, n, mesh)  GBuffer::InstancedDraw
//     producer GBuffer, fn GBufferInstanced; indexed, index_count = u32 mesh + 0x68, index_base = u64 mesh + 0x58 (what
//     the function hands drawIndex 0x623c60); state: the context's pass / sub, the CUE's VS / PS register blocks (the
//     material bind 0x448640 set them before the call), prim_type 4 (0x623740(R + 8, 4)), index_size 0 (16-bit:
//     0x624f80(R + 8, 0, 2)), num_instances = n (0x6250b0(R + 8, n)); run_tag DrawRuns::Source::GBuffer (3) |
//     kMaterialChanged (0x100) when the caller says the material changed since this context's last 0x1a1770.
//   0x1a1960(M, R, inst)              GBuffer::MeshDraw
//     the same with fn GBufferMesh, mesh = **(inst + 0x18), run_tag Source::Mesh (1); num_instances 1 [?: 0x1a1960
//     writes no NUM_INSTANCES - the register keeps the context's last value; R7-Core must not take 1 for granted].
// Resources: cue = R + 0x128 and, per stage, the table shape from the user-data layout (dwords, the SGPR / offset
// references); `version` 0 - resources are R7-R's (NativeResourceBindings), so StateKey reuses nothing until R7-R
// versions them. At the entry the VS table does not yet hold the draw's own V#s (the instance records / vertex
// streams 0x1a1770 / 0x1a1960 set): `snapshot` 0, `present` from the layout.
#pragma once

#include "base/abi.h"
#include "engine/render/native_render_layer.h"

namespace SecondSon::NativeEngine::R7G {

/// False when `r` is not a context the frame knows (nothing filled).
bool BuildInstancedDrawCommand(u64 module, u64 m, u64 r, u64 records, u32 n, u64 mesh, bool material_changed,
                               Engine::Render::R7::NativeDrawCommand& out);
bool BuildMeshDrawCommand(u64 module, u64 m, u64 r, u64 inst, Engine::Render::R7::NativeDrawCommand& out);

/// The context the command goes into (R7-Core's Submit argument).
Engine::Render::R7::NativeRenderContext ContextOf(u64 r, const Engine::Render::R7::NativeDrawCommand& cmd);

} // namespace SecondSon::NativeEngine::R7G
