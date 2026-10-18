// SecondSonPC — the auxiliary tessellation stages of SecondSonPC's own shader translator (see gcn_aux.h).
#include "runtime/gpu/gcn/gcn_aux.h"

#include <algorithm>
#include <array>

#include "runtime/gpu/gcn/spirv_builder.h"

namespace SecondSon::Gpu::Gcn {

namespace {

using Op = spv::Op;

class AuxEmitter {
public:
    explicit AuxEmitter(const GS::RuntimeParams* fragment) {
        b_.Capability(spv::Capability::Shader);
        b_.Capability(spv::Capability::Tessellation);
        b_.MemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
        void_ = b_.TypeVoid();
        bool_ = b_.TypeBool();
        f32_ = b_.TypeFloat(32);
        i32_ = b_.TypeInt(32, true);
        vec2_ = b_.TypeVector(f32_, 2);
        vec3_ = b_.TypeVector(f32_, 3);
        vec4_ = b_.TypeVector(f32_, 4);
        bvec2_ = b_.TypeVector(bool_, 2);
        // the varyings: the fragment program's inputs that are not a default value (SPI_PS_INPUT_CNTL: OFFSET bits 0-4,
        // DEFAULT bit 5 - with FLAT_SHADE, bit 10, a pass-through input all the same), once per location
        if (fragment != nullptr) {
            for (u32 i = 0; i < std::min<u32>(fragment->num_interp, 32); ++i) {
                const u32 c = fragment->ps_input_cntl[i];
                if (((c >> 5) & 1) != 0 && ((c >> 10) & 1) == 0) {
                    continue;
                }
                const u32 loc = c & 0x1F;
                if (std::ranges::find(locations_, loc) == locations_.end()) {
                    locations_.push_back(loc);
                }
            }
        }
        // gl_PerVertex: the position
        const Id members[] = {vec4_};
        per_vertex_ = b_.TypeStruct(members);
        b_.Decorate(per_vertex_, spv::Decoration::Block);
        b_.MemberDecorate(per_vertex_, 0, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::Position)});
        ptr_in_vec4_ = b_.TypePointer(spv::StorageClass::Input, vec4_);
        ptr_out_vec4_ = b_.TypePointer(spv::StorageClass::Output, vec4_);
    }

    /// A rect's patch: three corners; the fourth is the one across from the right angle's (the corner whose x and y
    /// its neighbours share), each output the sum of the three with weights -1 for the right angle's, 1 the others'
    std::vector<u32> RectTcs() {
        BeginControl();
        std::array<Id, 3> pos{};
        for (u32 i = 0; i < 3; ++i) {
            pos[i] = b_.Op(Op::OpLoad, vec4_, {b_.Op(Op::OpAccessChain, ptr_in_vec4_, {gl_in_, Int(i), Int(0)})});
        }
        std::array<Id, 3> equal{};
        for (u32 i = 0; i < 3; ++i) {
            const Id l = b_.Op(Op::OpVectorShuffle, vec2_, {pos[i], pos[i], 0, 1});
            const Id r = b_.Op(Op::OpVectorShuffle, vec2_, {pos[(i + 1) % 3], pos[(i + 1) % 3], 0, 1});
            equal[i] = b_.Op(Op::OpFOrdEqual, bvec2_, {l, r});
        }
        const auto comp = [&](Id v, u32 c) { return b_.Op(Op::OpCompositeExtract, bool_, {v, c}); };
        std::array<Id, 3> edge{}, weight{};
        for (u32 i = 0; i < 3; ++i) {
            const Id xy = b_.Op(Op::OpLogicalAnd, bool_, {comp(equal[i], 0), comp(equal[(i + 2) % 3], 1)});
            const Id yx = b_.Op(Op::OpLogicalAnd, bool_, {comp(equal[i], 1), comp(equal[(i + 2) % 3], 0)});
            edge[i] = b_.Op(Op::OpLogicalOr, bool_, {xy, yx});
            weight[i] = b_.Op(Op::OpSelect, f32_, {edge[i], b_.F32(-1.0f), b_.F32(1.0f)});
        }
        const auto interpolate = [&](Id v0, Id v1, Id v2) {
            const Id p0 = b_.Op(Op::OpVectorTimesScalar, vec4_, {v0, weight[0]});
            const Id p1 = b_.Op(Op::OpVectorTimesScalar, vec4_, {v1, weight[1]});
            const Id p2 = b_.Op(Op::OpVectorTimesScalar, vec4_, {v2, weight[2]});
            return b_.Op(Op::OpFAdd, vec4_, {p0, b_.Op(Op::OpFAdd, vec4_, {p1, p2})});
        };
        // the corners from the right angle's on (outputs 0-2), the interpolated one last
        Id first = b_.Op(Op::OpSelect, i32_, {edge[2], Int(2), Int(0)});
        first = b_.Op(Op::OpSelect, i32_, {edge[1], Int(1), first});
        const Id invocation = b_.Op(Op::OpLoad, i32_, {invocation_id_});
        const Id fourth = b_.Op(Op::OpIEqual, bool_, {invocation, Int(3)});
        const Id index = b_.Op(Op::OpSMod, i32_, {b_.Op(Op::OpIAdd, i32_, {first, invocation}), Int(3)});
        const Id in_pos = b_.Op(Op::OpLoad, vec4_, {b_.Op(Op::OpAccessChain, ptr_in_vec4_, {gl_in_, index, Int(0)})});
        b_.OpNoResult(Op::OpStore, {b_.Op(Op::OpAccessChain, ptr_out_vec4_, {gl_out_, invocation, Int(0)}),
                                    b_.Op(Op::OpSelect, vec4_, {fourth, interpolate(pos[0], pos[1], pos[2]), in_pos})});
        for (size_t k = 0; k < inputs_.size(); ++k) {
            const auto at = [&](Id i) { return b_.Op(Op::OpLoad, vec4_, {b_.Op(Op::OpAccessChain, ptr_in_vec4_, {inputs_[k], i})}); };
            const Id v = b_.Op(Op::OpSelect, vec4_, {fourth, interpolate(at(Int(0)), at(Int(1)), at(Int(2))), at(index)});
            b_.OpNoResult(Op::OpStore, {b_.Op(Op::OpAccessChain, ptr_out_vec4_, {outputs_[k], invocation}), v});
        }
        return End(spv::ExecutionModel::TessellationControl);
    }

    /// A quad's patch: its four vertices in the order the evaluation stage's (u, v) takes them (1, 2, 0, 3)
    std::vector<u32> QuadTcs() {
        const Id arr = b_.TypeArray(i32_, Int(4));
        const std::array<Id, 4> order{Int(1), Int(2), Int(0), Int(3)};
        BeginControl();
        const Id order_var = b_.LocalVariable(b_.TypePointer(spv::StorageClass::Function, arr), b_.ConstantComposite(arr, order));
        const Id invocation = b_.Op(Op::OpLoad, i32_, {invocation_id_});
        const Id index = b_.Op(Op::OpLoad, i32_, {b_.Op(Op::OpAccessChain, b_.TypePointer(spv::StorageClass::Function, i32_), {order_var, invocation})});
        b_.OpNoResult(Op::OpStore, {b_.Op(Op::OpAccessChain, ptr_out_vec4_, {gl_out_, invocation, Int(0)}),
                                    b_.Op(Op::OpLoad, vec4_, {b_.Op(Op::OpAccessChain, ptr_in_vec4_, {gl_in_, index, Int(0)})})});
        for (size_t k = 0; k < inputs_.size(); ++k) {
            b_.OpNoResult(Op::OpStore, {b_.Op(Op::OpAccessChain, ptr_out_vec4_, {outputs_[k], invocation}),
                                        b_.Op(Op::OpLoad, vec4_, {b_.Op(Op::OpAccessChain, ptr_in_vec4_, {inputs_[k], index})})});
        }
        return End(spv::ExecutionModel::TessellationControl);
    }

    /// The quad's corner (u, v) in {0, 1}^2: control point 2v + u
    std::vector<u32> PassthroughTes() {
        const Id ptr_in_f32 = b_.TypePointer(spv::StorageClass::Input, f32_);
        tess_coord_ = Input(vec3_);
        b_.Decorate(tess_coord_, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::TessCoord)});
        gl_in_ = Input(b_.TypeArray(per_vertex_, b_.U32(32)));
        const Id in_arr = b_.TypeArray(vec4_, b_.U32(32));
        for (const u32 loc : locations_) {
            inputs_.push_back(Input(in_arr));
            b_.Decorate(inputs_.back(), spv::Decoration::Location, {loc});
        }
        gl_out_ = Output(per_vertex_);
        for (const u32 loc : locations_) {
            outputs_.push_back(Output(vec4_));
            b_.Decorate(outputs_.back(), spv::Decoration::Location, {loc});
        }
        Begin();
        const auto coord = [&](u32 c) {
            return b_.Op(Op::OpConvertFToS, i32_, {b_.Op(Op::OpLoad, f32_, {b_.Op(Op::OpAccessChain, ptr_in_f32, {tess_coord_, Int(c)})})});
        };
        const Id index = b_.Op(Op::OpIAdd, i32_, {b_.Op(Op::OpIMul, i32_, {coord(1), Int(2)}), coord(0)});
        b_.OpNoResult(Op::OpStore, {b_.Op(Op::OpAccessChain, ptr_out_vec4_, {gl_out_, Int(0)}),
                                    b_.Op(Op::OpLoad, vec4_, {b_.Op(Op::OpAccessChain, ptr_in_vec4_, {gl_in_, index, Int(0)})})});
        for (size_t k = 0; k < inputs_.size(); ++k) {
            b_.OpNoResult(Op::OpStore, {outputs_[k], b_.Op(Op::OpLoad, vec4_, {b_.Op(Op::OpAccessChain, ptr_in_vec4_, {inputs_[k], index})})});
        }
        return End(spv::ExecutionModel::TessellationEvaluation);
    }

private:
    Id Int(u32 v) {
        return b_.Constant(i32_, v);
    }
    Id Input(Id type) {
        const Id v = b_.Variable(b_.TypePointer(spv::StorageClass::Input, type), spv::StorageClass::Input);
        interface_.push_back(v);
        return v;
    }
    Id Output(Id type) {
        const Id v = b_.Variable(b_.TypePointer(spv::StorageClass::Output, type), spv::StorageClass::Output);
        interface_.push_back(v);
        return v;
    }
    /// A control stage's interface (patches of up to 32 vertices in, 4 out; the tessellation levels all 1) and start
    void BeginControl() {
        invocation_id_ = Input(i32_);
        b_.Decorate(invocation_id_, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::InvocationId)});
        gl_in_ = Input(b_.TypeArray(per_vertex_, b_.U32(32)));
        const Id in_arr = b_.TypeArray(vec4_, b_.U32(32));
        for (const u32 loc : locations_) {
            inputs_.push_back(Input(in_arr));
            b_.Decorate(inputs_.back(), spv::Decoration::Location, {loc});
        }
        gl_out_ = Output(b_.TypeArray(per_vertex_, b_.U32(4)));
        const Id out_arr = b_.TypeArray(vec4_, b_.U32(4));
        for (const u32 loc : locations_) {
            outputs_.push_back(Output(out_arr));
            b_.Decorate(outputs_.back(), spv::Decoration::Location, {loc});
        }
        const Id inner = Output(b_.TypeArray(f32_, b_.U32(2)));
        b_.Decorate(inner, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::TessLevelInner)});
        b_.Decorate(inner, spv::Decoration::Patch);
        const Id outer = Output(b_.TypeArray(f32_, b_.U32(4)));
        b_.Decorate(outer, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::TessLevelOuter)});
        b_.Decorate(outer, spv::Decoration::Patch);
        Begin();
        const Id ptr_out_f32 = b_.TypePointer(spv::StorageClass::Output, f32_);
        for (u32 i = 0; i < 4; ++i) {
            b_.OpNoResult(Op::OpStore, {b_.Op(Op::OpAccessChain, ptr_out_f32, {outer, Int(i)}), b_.F32(1.0f)});
        }
        for (u32 i = 0; i < 2; ++i) {
            b_.OpNoResult(Op::OpStore, {b_.Op(Op::OpAccessChain, ptr_out_f32, {inner, Int(i)}), b_.F32(1.0f)});
        }
    }
    void Begin() {
        fn_ = b_.BeginFunction(void_, b_.TypeFunction(void_));
        b_.Label(b_.NewId());
    }
    std::vector<u32> End(spv::ExecutionModel model) {
        b_.OpNoResult(Op::OpReturn, {});
        b_.EndFunction();
        b_.EntryPoint(model, fn_, "main", interface_);
        if (model == spv::ExecutionModel::TessellationControl) {
            b_.ExecutionMode(fn_, spv::ExecutionMode::OutputVertices, {4});
        } else {
            b_.ExecutionMode(fn_, spv::ExecutionMode::Quads);
            b_.ExecutionMode(fn_, spv::ExecutionMode::SpacingEqual);
            b_.ExecutionMode(fn_, spv::ExecutionMode::VertexOrderCw);
        }
        return b_.Assemble();
    }

    SpirvBuilder b_;
    Id void_ = 0, bool_ = 0, f32_ = 0, i32_ = 0, vec2_ = 0, vec3_ = 0, vec4_ = 0, bvec2_ = 0;
    Id per_vertex_ = 0, ptr_in_vec4_ = 0, ptr_out_vec4_ = 0;
    Id fn_ = 0, gl_in_ = 0, gl_out_ = 0, invocation_id_ = 0, tess_coord_ = 0;
    std::vector<u32> locations_;
    std::vector<Id> inputs_, outputs_, interface_;
};

} // namespace

std::vector<u32> EmitAuxTessStage(GS::AuxStage stage, const GS::RuntimeParams* fragment, std::string& error) {
    AuxEmitter e(fragment);
    switch (stage) {
    case GS::AuxStage::RectListTcs:
        return e.RectTcs();
    case GS::AuxStage::QuadListTcs:
        return e.QuadTcs();
    case GS::AuxStage::PassthroughTes:
        return e.PassthroughTes();
    }
    error = "an unknown auxiliary stage";
    return {};
}

} // namespace SecondSon::Gpu::Gcn
