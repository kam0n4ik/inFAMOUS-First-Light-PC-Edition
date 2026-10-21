// SecondSonPC R7-G — native draw commands of the geometry engine (see draw_command.h).
#include "native_engine/r7/gbuffer/draw_command.h"

#include <cstring>

#include "engine/render/draw_runs.h"
#include "engine/render/native_draw.h"
#include "engine/render/render_layout.h"

namespace SecondSon::NativeEngine::R7G {

namespace R = Engine::Render;
namespace R7 = Engine::Render::R7;

namespace {

template <typename T>
T Ld(u64 a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}

bool Fill(u64 module, u64 r, u64 mesh, R7::EngineFn fn, u32 run_tag, u32 instances, R7::NativeDrawCommand& out) {
    const u32 count = Ld<u32>(mesh + 0x68);
    const u64 indices = Ld<u64>(mesh + 0x58);
    R::DrawOp op;
    if (!R::NativeDraw::BuildDrawOp(module, r, true, count, indices, op)) {
        return false;
    }
    out = R7::NativeDrawCommand{};
    out.producer = R7::Producer::GBuffer;
    out.fn = fn;
    out.indexed = true;
    out.index_count = count;
    out.index_base = indices;
    out.run_tag = run_tag;
    R7::NativeDrawState& s = out.state;
    s.pass = op.pass;
    s.sub = op.sub;
    s.shaders = op.shaders;
    s.has_ps = op.has_ps;
    std::memcpy(s.vs, op.vs, sizeof(s.vs));
    std::memcpy(s.ps, op.ps, sizeof(s.ps));
    s.prim_type = 4;
    s.index_size = 0;
    s.num_instances = instances;
    out.resources.cue = r + R::Layout::kContextCue;
    for (u32 k = 0; k < R::kDrawStages; ++k) {
        const R::StageTable& t = op.table[k];
        R7::NativeStageResources& st = out.resources.stage[k];
        st.present = t.present;
        st.dwords = t.dwords;
        st.shadow = out.resources.cue + R::Layout::kCueShadow + (k == R::kStageVs ? 2u : 1u) * R::Layout::kCueShadowStride;
        st.shape.dwords = static_cast<u16>(t.dwords);
        st.shape.refs = static_cast<u8>(t.refs);
        for (u32 i = 0; i < t.refs && i < 7; ++i) {
            st.shape.sgpr[i] = t.ref[i].sgpr;
            st.shape.offset[i] = t.ref[i].offset;
        }
        st.version = 0;
    }
    return true;
}

} // namespace

bool BuildInstancedDrawCommand(u64 module, u64 m, u64 r, u64 records, u32 n, u64 mesh, bool material_changed,
                               R7::NativeDrawCommand& out) {
    (void)m, (void)records;
    const u32 tag = static_cast<u32>(R::DrawRuns::Source::GBuffer) | (material_changed ? R::DrawRuns::kMaterialChanged : 0u);
    return Fill(module, r, mesh, R7::EngineFn::GBufferInstanced, tag, n, out);
}

bool BuildMeshDrawCommand(u64 module, u64 m, u64 r, u64 inst, R7::NativeDrawCommand& out) {
    (void)m;
    const u64 mesh = Ld<u64>(Ld<u64>(inst + 0x18));
    return Fill(module, r, mesh, R7::EngineFn::GBufferMesh, static_cast<u32>(R::DrawRuns::Source::Mesh), 1, out);
}

R7::NativeRenderContext ContextOf(u64 r, const R7::NativeDrawCommand& cmd) {
    R7::NativeRenderContext c;
    c.r = r;
    c.cue = r + R::Layout::kContextCue;
    c.dcb = r + R::Layout::kContextGfx;
    c.pass = cmd.state.pass;
    c.sub = cmd.state.sub;
    return c;
}

} // namespace SecondSon::NativeEngine::R7G
