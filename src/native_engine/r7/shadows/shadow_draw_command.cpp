// SecondSonPC — R7-S: 0x43d480's draw as a Native Render Layer command (see shadow_draw_command.h).
#include "native_engine/r7/shadows/shadow_draw_command.h"

#include <cstring>

#include <fmt/format.h>

#include "engine/render/draw_runs.h"
#include "engine/render/render_layout.h"

namespace SecondSon::NativeEngine::R7Shadows {

namespace {

namespace L = Engine::Render::Layout;
namespace ST = Reconstructed::STrace;

template <typename T>
T At(u64 address) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(address), sizeof(T));
    return v;
}

/// The pass key of a context (native_draw.cpp ContextPass, read-only copy): -1 when `r` is none of the frame's.
void ContextPass(u64 base, u64 r, s32& pass, u32& sub) {
    pass = -1;
    sub = 0;
    if (base == 0) {
        return;
    }
    const u64 frame = base + L::kFrame;
    const u64 pool = frame + L::kFramePool;
    if (r >= pool && r < pool + u64{L::kPoolSlots} * L::kPoolSlotStride && (r - pool) % L::kPoolSlotStride == 0) {
        pass = At<s32>(r + L::kSlotPass);
        sub = At<u32>(r + L::kSlotSub);
    } else if (r == frame + L::kFramePreContext) {
        pass = -2;
    } else if (r == frame + L::kFramePostContext) {
        pass = -3;
    } else {
        const u64 deform = base + L::kDeformContexts;
        if (r >= deform && r < deform + u64{L::kDeformContextCount} * L::kContextSize &&
            (r - deform) % L::kContextSize == 0) {
            pass = -4;
        }
    }
}

// the callees' positions in shadow_loop.cpp's kDrawCallees (the order 0x43d480 calls them)
enum : u32 { kCommit = 0, kStage, kSetBuffers, kSetVertex, kInstances, kPrim, kIndex, kCue, kDraw, kCalls };
constexpr u64 kRva[kCalls] = {0x120280, 0x27530, 0x16b70, 0x16aa0, 0x6250b0, 0x623740, 0x624f80, 0x14ad0, 0x623c60};

} // namespace

void BuildInstancedDraw(u64 module_base, u64 pass, u64 r, u64 records, u64 n, u64 mesh, NRL::NativeRenderContext& ctx,
                        NRL::NativeDrawCommand& cmd, ShadowDrawInputs& inputs) {
    ctx = {};
    ctx.r = r;
    ctx.cue = r + L::kContextCue;
    ctx.dcb = r + L::kContextGfx;
    ContextPass(module_base, r, ctx.pass, ctx.sub);

    cmd = {};
    cmd.producer = NRL::Producer::Shadows;
    cmd.fn = NRL::EngineFn::ShadowInstanced;
    cmd.indexed = true;
    cmd.index_count = At<u32>(mesh + 0x68);
    cmd.index_base = At<u64>(mesh + 0x58);
    cmd.run_tag = static_cast<u32>(Engine::Render::DrawRuns::Source::Shadow);
    cmd.state.pass = ctx.pass;
    cmd.state.sub = ctx.sub;
    cmd.state.shaders = false;
    cmd.state.prim_type = 4;  // VGT_PRIMITIVE_TYPE TriList (0x623740(dcb, 4))
    cmd.state.index_size = 0; // 16-bit (0x624f80(dcb, 0, 2))
    cmd.state.num_instances = static_cast<u32>(n);
    cmd.resources.cue = ctx.cue;

    inputs = {};
    inputs.records = records;
    inputs.record_stride = 0x48;
    inputs.instances = static_cast<u32>(n);
    inputs.instance_slot = At<u8>(pass + 0x121);
    inputs.vertex_table = At<u64>(mesh + 0x60);
    inputs.vertex_buffers = At<u8>(mesh + 0x6c);
}

std::string CheckAgainstTrace(const NRL::NativeRenderContext& ctx, const NRL::NativeDrawCommand& cmd,
                              const ShadowDrawInputs& inputs, const ST::Trace& t) {
    if (t.ncalls != kCalls) {
        return fmt::format("{} recorded calls (9 expected)", t.ncalls);
    }
    for (u32 i = 0; i < kCalls; ++i) {
        if (t.callees[t.calls[i].callee].rva != kRva[i]) {
            return fmt::format("call {}: {:#x}, expected {:#x}", i, t.callees[t.calls[i].callee].rva, kRva[i]);
        }
    }
    const auto& c = t.calls;
    const auto lo = [](u64 v) { return static_cast<u32>(v); };
    if (c[kCommit].gpr[0] != ctx.r || c[kSetBuffers].gpr[0] != ctx.cue || c[kSetVertex].gpr[0] != ctx.cue ||
        c[kCue].gpr[0] != ctx.cue || c[kInstances].gpr[0] != ctx.dcb || c[kPrim].gpr[0] != ctx.dcb ||
        c[kIndex].gpr[0] != ctx.dcb || c[kDraw].gpr[0] != ctx.dcb) {
        return "context / CUE / DCB";
    }
    if (lo(c[kInstances].gpr[1]) != cmd.state.num_instances) {
        return fmt::format("NUM_INSTANCES {} vs {}", lo(c[kInstances].gpr[1]), cmd.state.num_instances);
    }
    if (lo(c[kPrim].gpr[1]) != cmd.state.prim_type) {
        return fmt::format("primitive type {} vs {}", lo(c[kPrim].gpr[1]), cmd.state.prim_type);
    }
    if (lo(c[kIndex].gpr[1]) != cmd.state.index_size || lo(c[kIndex].gpr[2]) != 2) {
        return fmt::format("index size {} vs {}", lo(c[kIndex].gpr[1]), cmd.state.index_size);
    }
    if (!cmd.indexed || lo(c[kDraw].gpr[1]) != cmd.index_count || c[kDraw].gpr[2] != cmd.index_base ||
        lo(c[kDraw].gpr[3]) != 0) {
        return fmt::format("draw index ({}, {:#x}) vs ({}, {:#x})", lo(c[kDraw].gpr[1]), c[kDraw].gpr[2],
                           cmd.index_count, cmd.index_base);
    }
    if (lo(c[kSetBuffers].gpr[2]) != inputs.instance_slot || lo(c[kSetBuffers].gpr[3]) != 1) {
        return fmt::format("instance slot {} vs {}", lo(c[kSetBuffers].gpr[2]), inputs.instance_slot);
    }
    if (lo(c[kSetVertex].gpr[1]) != 2 || lo(c[kSetVertex].gpr[2]) != 0 ||
        lo(c[kSetVertex].gpr[3]) != inputs.vertex_buffers || c[kSetVertex].gpr[4] != inputs.vertex_table) {
        return "vertex buffer table";
    }
    // the records' V# (Gnm::Buffer, 16 bytes): base 44 bits (dword 0 + dword 1 [11:0]; 0x61e260 keeps only those of
    // the pointer), stride dword 1 [29:16], num_records dword 2
    if (c[kSetBuffers].snap_len[0] != 16) {
        return "no V# snapshot";
    }
    u32 v[4];
    std::memcpy(v, t.snap + c[kSetBuffers].snap_off[0], 16);
    const u64 base = v[0] | (u64{v[1] & 0xfff} << 32);
    const u32 stride = (v[1] >> 16) & 0x3fff;
    if (base != (inputs.records & 0xfff'ffff'ffffull) || stride != inputs.record_stride || v[2] != inputs.instances) {
        return fmt::format("records V# (base {:#x}, stride {:#x}, n {}) vs ({:#x}, {:#x}, {})", base, stride, v[2],
                           inputs.records, inputs.record_stride, inputs.instances);
    }
    return {};
}

} // namespace SecondSon::NativeEngine::R7Shadows
