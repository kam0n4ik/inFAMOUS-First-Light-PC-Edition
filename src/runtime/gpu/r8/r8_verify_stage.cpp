// SecondSonPC R8 - the verification of prepared stages (r8_verify.h: CompareStageView, ComparePlan). Field by field,
// as parallel_cp's SameStageView (bindings built twice differ in their padding bytes), but naming the first field that
// differs.
#include "runtime/gpu/r8/r8_verify.h"

#include <algorithm>

#include "engine/render/native_render_ops.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/shader/shader_resources.h"

namespace SecondSon::Gpu::R8::Verify {

namespace GS = Gpu::Shader;

namespace {

template <size_t N>
bool Raw(Diff& d, std::string_view name, const std::array<u32, N>& e, const std::array<u32, N>& a) {
    for (size_t i = 0; i < N; ++i) {
        if (e[i] != a[i]) {
            Diff::Scope s(d, name, static_cast<s64>(i));
            d.Note("", {}, e[i], a[i]);
            return false;
        }
    }
    return true;
}

bool Buffer(Diff& d, const GS::BufferBinding& e, const GS::BufferBinding& a) {
    bool same = d.Field("binding", e.binding, a.binding);
    same &= d.Field("kind", e.kind, a.kind);
    same &= Raw(d, "desc", e.desc.raw, a.desc.raw);
    same &= d.Field("address", e.address, a.address);
    same &= d.Field("size", e.size, a.size);
    same &= d.Field("written", e.written, a.written);
    same &= d.Field("formatted", e.formatted, a.formatted);
    same &= d.Field("status", e.status, a.status);
    return same;
}

bool Image(Diff& d, const GS::ImageBinding& e, const GS::ImageBinding& a) {
    bool same = d.Field("binding", e.binding, a.binding);
    same &= d.Field("count", e.count, a.count);
    same &= Raw(d, "desc", e.desc.raw, a.desc.raw);
    same &= d.Field("view_type", e.view_type, a.view_type);
    same &= d.Field("storage", e.storage, a.storage);
    same &= d.Field("depth", e.depth, a.depth);
    same &= d.Field("array", e.array, a.array);
    same &= d.Field("atomic", e.atomic, a.atomic);
    same &= d.Field("fmask", e.fmask, a.fmask);
    same &= d.Field("status", e.status, a.status);
    return same;
}

bool Sampler(Diff& d, const GS::SamplerBinding& e, const GS::SamplerBinding& a) {
    bool same = d.Field("binding", e.binding, a.binding);
    same &= Raw(d, "desc", e.desc.raw, a.desc.raw);
    same &= d.Field("status", e.status, a.status);
    return same;
}

bool Vertex(Diff& d, const GS::VertexBinding& e, const GS::VertexBinding& a) {
    bool same = d.Field("attribute", e.attribute, a.attribute);
    {
        Diff::Scope s(d, "fetch");
        same &= d.Field("dest_vgpr", e.fetch.dest_vgpr, a.fetch.dest_vgpr);
        same &= d.Field("num_elements", e.fetch.num_elements, a.fetch.num_elements);
        same &= d.Field("sgpr_base", e.fetch.sgpr_base, a.fetch.sgpr_base);
        same &= d.Field("dword_offset", e.fetch.dword_offset, a.fetch.dword_offset);
        same &= d.Field("inst_offset", e.fetch.inst_offset, a.fetch.inst_offset);
        same &= d.Field("instance_data", e.fetch.instance_data, a.fetch.instance_data);
        same &= d.Field("data_format", e.fetch.data_format, a.fetch.data_format);
        same &= d.Field("num_format", e.fetch.num_format, a.fetch.num_format);
    }
    same &= Raw(d, "read", e.read, a.read);
    same &= Raw(d, "desc", e.desc.raw, a.desc.raw);
    same &= d.Field("address", e.address, a.address);
    same &= d.Field("size", e.size, a.size);
    same &= d.Field("status", e.status, a.status);
    return same;
}

template <typename T, typename F>
bool List(Diff& d, std::string_view name, const std::vector<T>& e, const std::vector<T>& a, F&& one) {
    bool same = d.Size(name, e.size(), a.size());
    const size_t n = std::min(e.size(), a.size());
    for (size_t i = 0; i < n; ++i) {
        Diff::Scope s(d, name, static_cast<s64>(i));
        same &= one(d, e[i], a[i]);
    }
    return same;
}

} // namespace

bool ComparePlan(const GS::ResourceBindingPlan& e, const GS::ResourceBindingPlan& a, Diff& d) {
    bool same = d.Field("stage", e.stage, a.stage);
    same &= d.Size("flat", e.flat.size(), a.flat.size());
    for (size_t i = 0, n = std::min(e.flat.size(), a.flat.size()); i < n; ++i) {
        if (e.flat[i] != a.flat[i]) {
            Diff::Scope s(d, "flat", static_cast<s64>(i));
            d.Note("", {}, e.flat[i], a.flat[i]);
            same = false;
            break; // the first word names it; the rest of a moved table would only add noise
        }
    }
    same &= List(d, "buffers", e.buffers, a.buffers, Buffer);
    same &= List(d, "images", e.images, a.images, Image);
    same &= List(d, "samplers", e.samplers, a.samplers, Sampler);
    same &= List(d, "vertices", e.vertices, a.vertices, Vertex);
    for (size_t i = 0; i < e.status_counts.size(); ++i) {
        if (e.status_counts[i] != a.status_counts[i]) {
            Diff::Scope s(d, "status_counts", static_cast<s64>(i));
            d.Note("", {}, e.status_counts[i], a.status_counts[i]);
            same = false;
        }
    }
    if (e.error != a.error) {
        d.Note("error", {}, e.error.size(), a.error.size());
        same = false;
    }
    return same;
}

bool CompareStageView(const GS::StageView& e, const GS::StageView& a, Diff& d) {
    bool same = d.Field("status", e.status, a.status);
    same &= d.Field("address", e.address, a.address);
    same &= d.Field("program", e.program, a.program);
    same &= d.Field("footer_hash", e.footer_hash, a.footer_hash);
    same &= d.Field("module", e.module, a.module);
    same &= d.Bytes("runtime", &e.runtime, &a.runtime, sizeof(e.runtime));
    same &= d.Field("start.unified", e.start.unified, a.start.unified);
    same &= d.Field("start.buffer", e.start.buffer, a.start.buffer);
    same &= d.Field("end.unified", e.end.unified, a.end.unified);
    same &= d.Field("end.buffer", e.end.buffer, a.end.buffer);
    same &= d.Field("program_error", e.program_error, a.program_error);
    Diff::Scope s(d, "plan");
    same &= ComparePlan(e.plan, a.plan, d);
    return same;
}

bool CompareDrawOp(const Engine::Render::DrawOp& e, const Engine::Render::DrawOp& a, Diff& d) {
    namespace ER = Engine::Render;
    bool same = d.Field("pass", e.pass, a.pass);
    same &= d.Field("sub", e.sub, a.sub);
    same &= d.Field("canary", e.canary, a.canary);
    same &= d.Field("indexed", e.indexed, a.indexed);
    same &= d.Field("index_count", e.index_count, a.index_count);
    same &= d.Field("index_base", e.index_base, a.index_base);
    same &= d.Field("shaders", e.shaders, a.shaders);
    same &= d.Field("has_ps", e.has_ps, a.has_ps);
    for (u32 i = 0; i < ER::kVsRegs; ++i) {
        if (e.vs[i] != a.vs[i]) {
            Diff::Scope s(d, "vs", i);
            d.Note("", {}, e.vs[i], a.vs[i]);
            same = false;
        }
    }
    for (u32 i = 0; i < ER::kPsRegs; ++i) {
        if (e.ps[i] != a.ps[i]) {
            Diff::Scope s(d, "ps", i);
            d.Note("", {}, e.ps[i], a.ps[i]);
            same = false;
        }
    }
    for (u32 t = 0; t < ER::kDrawStages; ++t) {
        Diff::Scope s(d, "table", t);
        const ER::StageTable &x = e.table[t], &y = a.table[t];
        same &= d.Field("present", x.present, y.present);
        same &= d.Field("dwords", x.dwords, y.dwords);
        same &= d.Field("hash", x.hash, y.hash);
        same &= d.Field("refs", x.refs, y.refs);
        for (u32 r = 0; r < std::min<u32>(std::max(x.refs, y.refs), 7); ++r) {
            Diff::Scope sr(d, "ref", r);
            same &= d.Field("sgpr", x.ref[r].sgpr, y.ref[r].sgpr);
            same &= d.Field("offset", x.ref[r].offset, y.ref[r].offset);
        }
    }
    return same;
}

} // namespace SecondSon::Gpu::R8::Verify
