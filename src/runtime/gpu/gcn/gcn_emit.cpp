// SecondSonPC — GCN -> SPIR-V emission (gcn_emit.h).
#include "runtime/gpu/gcn/gcn_emit.h"

#include <optional>
#include <algorithm>
#include <array>
#include <bit>
#include <unordered_map>

#include <fmt/format.h>
#include <spirv/unified1/GLSL.std.450.h>

#include "runtime/gpu/gcn/spirv_builder.h"

namespace SecondSon::Gpu::Gcn {

namespace {

using spv::Op;

constexpr u32 kLdsDwords = 8192;
constexpr u32 kSccOperand = 253, kVccz = 251, kExecz = 252, kExecLo = 126, kExecHi = 127, kM0 = 124;

struct Unsupported {
    std::string what;
};

/// A block of the emitted function: one of the program's blocks, or a merge block of the translator's own.
struct Node {
    s32 orig = -1;         ///< the program's block (-1: an inserted merge block - just a branch on)
    std::vector<s32> succ; ///< [taken, next] / [only] / [] (exit)
    Cond cond = Cond::Always;
    bool exit = false;
    bool loop_header = false;
    s32 loop_merge = -1, loop_continue = -1; ///< nodes (merge -1: an unreachable merge of our own)
    s32 selection_merge = -1;                ///< node; -2: both sides end the program (an unreachable merge)
    bool break_branch = false;               ///< a conditional break / continue / back edge: no merge instruction
};

class Emitter {
public:
    Emitter(std::span<const Inst> insts, const Cfg& cfg, const Structure& st, const EmitOptions& options)
        : insts_(insts), cfg_(cfg), st_(st), options_(options) {}

    EmitResult Run();

private:
    void BuildGraph();
    std::vector<u32> Rpo() const;
    void Setup();
    Id RegVar(u32 code);
    Id Get(u32 code, const Inst& in);
    Id GetF(u32 code, const Inst& in) {
        return Bit(t_f32_, Get(code, in));
    }
    void Set(u32 code, Id value);
    void SetF(u32 code, Id fvalue) {
        Set(code, Bit(t_u32_, fvalue));
    }
    Id Bit(Id type, Id v) {
        return b_.Op(Op::OpBitcast, type, {v});
    }
    Id LaneBit(Id lo, Id hi);
    std::pair<Id, Id> Ballot(Id cond);
    /// a 64-bit source operand: EXEC as its lanes, a register pair, or a constant sign-extended to 64 bits (not the
    /// operand code after it: 128 + 1 is the constant 1)
    std::pair<Id, Id> Src64(u32 code, const Inst& in);
    Id Exec() {
        return b_.Op(Op::OpLoad, t_bool_, {exec_});
    }
    Id Scc() {
        return b_.Op(Op::OpLoad, t_bool_, {scc_});
    }
    void SetScc(Id v) {
        b_.OpNoResult(Op::OpStore, {scc_, v});
    }
    Id Glsl(GLSLstd450 f, Id type, std::initializer_list<u32> args);
    Id Select(Id type, Id c, Id a, Id b2) {
        return b_.Op(Op::OpSelect, type, {c, a, b2});
    }
    Id SelBool(Id c) {
        return Select(t_u32_, c, b_.U32(1), b_.U32(0));
    }
    Id Not(Id c) {
        return b_.Op(Op::OpLogicalNot, t_bool_, {c});
    }
    Id And(Id p, Id q) {
        return b_.Op(Op::OpLogicalAnd, t_bool_, {p, q});
    }
    Id Or(Id p, Id q) {
        return b_.Op(Op::OpLogicalOr, t_bool_, {p, q});
    }

    bool IsVector(const Inst& in) const;
    bool WritesExec(const Inst& in) const;
    void Emit(const Inst& in);
    void EmitStub(const Inst& in, std::string_view name);
    void EmitScalar(const Inst& in, std::string_view name);
    void EmitVector(const Inst& in, std::string_view name);
    bool EmitCompare(const Inst& in, std::string_view name);
    Id VSrcF(const Inst& in, u32 i, u16 code);       ///< a float source with the VOP3 input modifiers
    void VDstF(const Inst& in, u32 code, Id fvalue); ///< a float result with VOP3 omod / clamp

    std::span<const Inst> insts_;
    const Cfg& cfg_;
    const Structure& st_;
    const EmitOptions& options_;
    SpirvBuilder b_;
    std::vector<Node> nodes_;
    std::vector<Id> labels_;
    bool writes_exec_ = false;

    Id t_void_ = 0, t_bool_ = 0, t_u32_ = 0, t_i32_ = 0, t_f32_ = 0, t_uvec4_ = 0, t_ptr_u32_ = 0, t_ptr_bool_ = 0;
    Id t_f64_ = 0, t_uvec2_ = 0; ///< (on first use: Float64)
    void Float64();
    Id GetF64(u32 code, const Inst& in, u32 i);  ///< a 64-bit float source (a register pair, or a constant); VOP3 abs/neg
    void SetF64(const Inst& in, u32 code, Id v); ///< a 64-bit float result into a register pair; VOP3 omod/clamp
    Id glsl_ = 0;
    Id exec_ = 0, scc_ = 0;
    Id lane_id_ = 0;            ///< SubgroupLocalInvocationId (on first use)
    // resources
    ResourceInfo res_;
    Id flat_ = 0, push_ = 0;  ///< the flattened user data (binding buffers.size()), PushData
    std::vector<Id> buffers_; ///< binding i
    Id t_ptr_sb_u32_ = 0, t_ptr_pc_u32_ = 0;
    void DeclareResources();
    Id FlatDword(Id index);   ///< flat[index]
    Id BufOffset(u32 buffer); ///< buf_offsets[buffer] (bytes the descriptor's start was moved down)
    Id SharpDword(u32 buffer, u32 dword);
    bool BufferKnown(s32 buffer) const;
    void EmitSmrd(const Inst& in, u32 index, std::string_view name);
    bool EmitBuffer(const Inst& in, u32 index, std::string_view name);
    bool EmitTbuffer(const Inst& in, u32 index, std::string_view name);
    /// a typed load of `n` components: the data / number format and DST_SEL (3 bits per component: 0 zero, 1 one,
    /// 4-7 the format's x-w) given
    bool EmitTypedLoad(const Inst& in, u32 index, u32 n, u32 dfmt, u32 nfmt, u32 dst_sel);
    bool EmitFormattedBuffer(const Inst& in, u32 index, std::string_view name); ///< buffer_*_format: the V#'s format
    bool EmitDs(const Inst& in, std::string_view name);
    std::string decline_; ///< why the last memory instruction was not translated (in its Unsupported)
    bool Decline(std::string why) {
        decline_ = std::move(why);
        return false;
    }
    /// An image as declared: its T#'s type and number format read at the translation (the module is specialized on
    /// them: the runtime's key holds its view type, data and number format).
    struct ImageDecl {
        bool ok = false;
        bool null = false; ///< a T# of no image type (a null descriptor bound): loads 0, stores nothing
        Id var = 0, type = 0, texel = 0, scalar = 0; ///< texel: vec4 of the sampled type (scalar)
        bool storage = false, arrayed = false;
        bool one_d = false; ///< a 1D image: a 2D view of one row (the bridge's and the runtime's), y = 0 inserted
        u32 coords = 0;     ///< the address's coordinates (s, t, r / the slice)
        u32 kind = 0;    ///< 0 float, 1 uint, 2 sint
        u32 dst_sel = 0; ///< DST_SEL (the shader applies it to a storage image)
    };
    std::vector<ImageDecl> images_;
    std::vector<Id> samplers_;
    void DeclareImages(u32 first_binding);
    bool EmitImage(const Inst& in, u32 index, std::string_view name);
    std::optional<u32> FlatValue(const ScalarValue& v) const;
    Id gds_ = 0, lds_ = 0, t_ptr_wg_u32_ = 0;
    Id BufferAddress(const Inst& in, u32 buffer);              ///< MUBUF / MTBUF: the byte address in the binding
    Id ReadBits(u32 buffer, Id byte_addr, u32 bit, u32 width); ///< `width` (<= 32) bits at byte_addr * 8 + bit
    u32 cur_index_ = 0;
    std::vector<Id> interface_; ///< the entry point's interface (every global variable used: SPIR-V 1.4+)
    Id LaneId();
    Id local_index_ = 0; ///< LocalInvocationIndex (compute; on first use)
    Id LocalIndex();
    /// a source operand's value when it is a constant of the operand space (inline or literal)
    static std::optional<u32> ConstOperand(u32 code, const Inst& in);
    void InitCompute(); ///< the thread ids (v0-v2) and the group ids / TG_SIZE after the user data (COMPUTE_PGM_RSRC2)
    // the vertex stage (EmitOptions::vertex)
    void InitVertex();   ///< v0-v3 (the vertex / instance ids), the base vertex / instance SGPRs, the inputs
    void EmitFetch();    ///< the fetch shader's call: its attributes into their VGPRs
    void EmitExport(const Inst& in);
    void VertexEpilogue(); ///< before the return: the position's depth mode and clip space
    Id OutputComponent(Id var, u32 c) {
        return b_.Op(Op::OpAccessChain, t_ptr_out_f32_, {var, b_.U32(c)});
    }
    Id t_vec4_ = 0, t_ptr_out_f32_ = 0;
    Id position_ = 0, point_size_ = 0, clip_dist_ = 0, cull_dist_ = 0;
    std::array<Id, 32> params_{};
    std::vector<Id> vinputs_;
    std::unordered_map<u32, Id> regs_; ///< register code -> its function variable
};

// ---------------------------------------------------------------- graph

void Emitter::BuildGraph() {
    const u32 n = static_cast<u32>(cfg_.blocks.size());
    nodes_.resize(n);
    std::vector<s32> idom(cfg_.idom.begin(), cfg_.idom.end());
    for (u32 b = 0; b < n; ++b) {
        const Block& blk = cfg_.blocks[b];
        Node& nd = nodes_[b];
        nd.orig = static_cast<s32>(b);
        nd.exit = blk.exit;
        nd.cond = blk.cond;
        if (blk.taken >= 0) {
            nd.succ.push_back(blk.taken);
        }
        if (blk.next >= 0) {
            nd.succ.push_back(blk.next);
        }
    }
    for (const LoopInfo& l : st_.loops) {
        Node& h = nodes_[l.header];
        h.loop_header = true;
        h.loop_merge = l.merge;
        h.loop_continue = static_cast<s32>(l.latches.front());
        if (l.merge >= 0) {
            // a merge block of the loop's own (its exits go to it, it goes on): the block the loop exits to may merge
            // an enclosing selection too (a selection around the loop that skips it), and a block merges one construct
            const s32 m = static_cast<s32>(nodes_.size());
            for (u32 x = 0; x < n; ++x) {
                if (l.body[x]) {
                    for (s32& s : nodes_[x].succ) {
                        if (s == l.merge) {
                            s = m;
                        }
                    }
                }
            }
            Node mn;
            mn.succ = {l.merge};
            nodes_.push_back(mn);
            idom.push_back(static_cast<s32>(l.header));
            nodes_[l.header].loop_merge = m;
        }
    }
    // a loop's merge as the emitted graph has it (its own block)
    const auto merge_of = [&](const LoopInfo& l) { return nodes_[l.header].loop_merge; };
    const auto dominates = [&](s32 a, s32 x) {
        for (; x >= 0; x = idom[x]) {
            if (x == a) {
                return true;
            }
        }
        return false;
    };
    // the enclosing loop's merge / header / continue: branches there are breaks / continues, not selections
    const auto loop_target = [&](u32 b, s32 target) {
        const s32 li = st_.loop_of[b];
        if (li < 0) {
            return false;
        }
        const LoopInfo& l = st_.loops[li];
        return target == merge_of(l) || target == static_cast<s32>(l.header) ||
               target == static_cast<s32>(l.latches.front());
    };
    std::vector<u32> sel;
    for (u32 b = 0; b < n; ++b) {
        if (nodes_[b].succ.size() == 2) {
            sel.push_back(b);
        }
    }
    const auto depth = [&](u32 b) {
        u32 d = 0;
        for (s32 x = static_cast<s32>(b); x > 0; x = idom[x]) {
            ++d;
        }
        return d;
    };
    std::sort(sel.begin(), sel.end(), [&](u32 a, u32 c) { return depth(a) > depth(c); }); // inner first
    for (const u32 b : sel) {
        Node& nd = nodes_[b];
        if (st_.selection_merge[b] == -1 || loop_target(b, nd.succ[0]) || loop_target(b, nd.succ[1])) {
            nd.break_branch = true; // (a latch's back edge / a break / a continue)
            continue;
        }
        s32 merge = st_.selection_merge[b];
        for (const LoopInfo& l : st_.loops) {
            if (merge >= 0 && merge == l.merge && b < l.body.size() && l.body[b]) {
                merge = merge_of(l); // (a selection in the loop merging where the loop exits: the loop's merge block)
            }
        }
        if (merge == -2) {
            nd.selection_merge = -2;
            continue;
        }
        if (dominates(static_cast<s32>(b), merge) && !loop_target(b, merge)) {
            nd.selection_merge = merge;
            continue;
        }
        // a merge block of its own: the region's edges into `merge` go to it, it goes on to `merge`
        const s32 m = static_cast<s32>(nodes_.size());
        std::vector<u8> in(nodes_.size(), 0);
        std::vector<s32> work{static_cast<s32>(b)};
        while (!work.empty()) {
            const s32 x = work.back();
            work.pop_back();
            if (x < 0 || x == merge || in[x] || !dominates(static_cast<s32>(b), x)) {
                continue;
            }
            in[x] = 1;
            for (const s32 s : nodes_[x].succ) {
                work.push_back(s);
            }
        }
        for (u32 x = 0; x < in.size(); ++x) {
            if (in[x]) {
                for (s32& s : nodes_[x].succ) {
                    if (s == merge) {
                        s = m;
                    }
                }
            }
        }
        Node mn;
        mn.succ = {merge};
        nodes_.push_back(mn);
        idom.push_back(static_cast<s32>(b));
        nodes_[b].selection_merge = m;
    }
}

std::vector<u32> Emitter::Rpo() const {
    std::vector<u8> seen(nodes_.size(), 0);
    std::vector<u32> post;
    std::vector<std::pair<u32, u32>> stack{{0, 0}};
    seen[0] = 1;
    while (!stack.empty()) {
        auto& [x, k] = stack.back();
        const auto& s = nodes_[x].succ;
        if (k < s.size()) {
            const s32 y = s[s.size() - 1 - k++];
            if (y >= 0 && !seen[y]) {
                seen[y] = 1;
                stack.push_back({static_cast<u32>(y), 0});
            }
            continue;
        }
        post.push_back(x);
        stack.pop_back();
    }
    return {post.rbegin(), post.rend()};
}

// ---------------------------------------------------------------- types and registers

void Emitter::Setup() {
    b_.Capability(spv::Capability::Shader);
    b_.Capability(spv::Capability::GroupNonUniform);
    b_.Capability(spv::Capability::GroupNonUniformBallot);
    b_.Capability(spv::Capability::GroupNonUniformShuffle);
    b_.MemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::GLSL450);
    glsl_ = b_.ExtInstImport("GLSL.std.450");
    t_void_ = b_.TypeVoid();
    t_bool_ = b_.TypeBool();
    t_u32_ = b_.TypeInt(32, false);
    t_i32_ = b_.TypeInt(32, true);
    t_f32_ = b_.TypeFloat(32);
    t_uvec4_ = b_.TypeVector(t_u32_, 4);
    t_ptr_u32_ = b_.TypePointer(spv::StorageClass::Function, t_u32_);
    t_ptr_bool_ = b_.TypePointer(spv::StorageClass::Function, t_bool_);
}

Id Emitter::RegVar(u32 code) {
    if (const auto it = regs_.find(code); it != regs_.end()) {
        return it->second;
    }
    const Id v = b_.LocalVariable(t_ptr_u32_, b_.U32(0));
    regs_.emplace(code, v);
    return v;
}

Id Emitter::Get(u32 code, const Inst& in) {
    if (code <= 103 || code == 106 || code == 107 || code == kM0 || code >= kVgpr) {
        return b_.Op(Op::OpLoad, t_u32_, {RegVar(code)});
    }
    if (code == 128) {
        return b_.U32(0);
    }
    if (code >= 129 && code <= 192) {
        return b_.U32(code - 128);
    }
    if (code >= 193 && code <= 208) {
        return b_.U32(static_cast<u32>(-static_cast<s32>(code - 192)));
    }
    if (code >= 240 && code <= 247) {
        static constexpr float kF[8] = {0.5f, -0.5f, 1.0f, -1.0f, 2.0f, -2.0f, 4.0f, -4.0f};
        return b_.U32(std::bit_cast<u32>(kF[code - 240]));
    }
    if (code == kLiteralOperand) {
        return b_.U32(in.lit);
    }
    if (code == kExecLo || code == kExecHi) {
        const auto [lo, hi] = Ballot(Exec());
        return code == kExecLo ? lo : hi;
    }
    if (code == kSccOperand) {
        return SelBool(Scc());
    }
    if (code == kExecz) {
        return SelBool(Not(Exec()));
    }
    if (code == kVccz) {
        return SelBool(Not(LaneBit(Get(106, in), Get(107, in))));
    }
    throw Unsupported{fmt::format("operand {}", code)};
}

void Emitter::Set(u32 code, Id value) {
    if (code == kExecLo || code == kExecHi) {
        throw Unsupported{"a 32-bit write of EXEC"};
    }
    if (code == 125 || (code > 107 && code < kM0) || (code > kM0 && code < kVgpr)) {
        throw Unsupported{fmt::format("destination {}", code)};
    }
    b_.OpNoResult(Op::OpStore, {RegVar(code), value});
}

Id Emitter::LaneBit(Id lo, Id hi) {
    const Id v = b_.Op(Op::OpCompositeConstruct, t_uvec4_, {lo, hi, b_.U32(0), b_.U32(0)});
    return b_.Op(Op::OpGroupNonUniformInverseBallot, t_bool_, {b_.U32(static_cast<u32>(spv::Scope::Subgroup)), v});
}

std::pair<Id, Id> Emitter::Src64(u32 c, const Inst& in) {
    if (c == kExecLo) {
        return Ballot(Exec());
    }
    if (c <= 107 || c == kM0 || c >= kVgpr) {
        return {Get(c, in), Get(c + 1, in)};
    }
    const Id lo = Get(c, in); // a constant: sign-extended to 64 bits
    const Id neg = b_.Op(Op::OpSLessThan, t_bool_, {Bit(t_i32_, lo), b_.Constant(t_i32_, 0)});
    return {lo, Select(t_u32_, neg, b_.U32(~0u), b_.U32(0))};
}

std::pair<Id, Id> Emitter::Ballot(Id cond) {
    const Id v =
        b_.Op(Op::OpGroupNonUniformBallot, t_uvec4_, {b_.U32(static_cast<u32>(spv::Scope::Subgroup)), cond});
    return {b_.Op(Op::OpCompositeExtract, t_u32_, {v, 0}), b_.Op(Op::OpCompositeExtract, t_u32_, {v, 1})};
}

bool Emitter::BufferKnown(s32 buffer) const {
    for (const ScalarValue& v : res_.buffers[buffer].sharp) {
        if (v.kind == ScalarValue::Kind::Unknown) {
            return false;
        }
    }
    return true;
}

void Emitter::DeclareResources() {
    // a storage buffer of dwords: struct { uint data[]; } (std430: array stride 4)
    const auto ssbo = [&](bool written, u32 binding) {
        const Id rta = b_.TypeRuntimeArray(t_u32_);
        b_.Decorate(rta, spv::Decoration::ArrayStride, {4});
        const Id members[] = {rta};
        const Id st = b_.TypeStruct(members);
        b_.Decorate(st, spv::Decoration::Block);
        b_.MemberDecorate(st, 0, spv::Decoration::Offset, {0});
        if (!written) {
            b_.MemberDecorate(st, 0, spv::Decoration::NonWritable);
        }
        const Id v = b_.Variable(b_.TypePointer(spv::StorageClass::StorageBuffer, st), spv::StorageClass::StorageBuffer);
        b_.Decorate(v, spv::Decoration::DescriptorSet, {0});
        b_.Decorate(v, spv::Decoration::Binding, {binding});
        interface_.push_back(v);
        return v;
    };
    t_ptr_sb_u32_ = b_.TypePointer(spv::StorageClass::StorageBuffer, t_u32_);
    const u32 base = options_.binding_base;
    for (u32 i = 0; i < res_.buffers.size(); ++i) {
        buffers_.push_back(ssbo(res_.buffers[i].written, base + i));
    }
    // the bridge's order (the executor's game fixes know the buffers by it): the V#s, the flattened user data, GDS
    flat_ = ssbo(false, base + static_cast<u32>(res_.buffers.size()));
    if (res_.uses_gds) {
        gds_ = ssbo(true, base + 1 + static_cast<u32>(res_.buffers.size()));
    }
    if (res_.uses_lds && (options_.vertex || options_.fragment)) {
        throw Unsupported{"the local data share outside a compute program"};
    }
    if (options_.buffer_base + res_.buffers.size() > 56) {
        throw Unsupported{fmt::format("buffers {}..{} (PushData has 56 offsets)", options_.buffer_base,
                                      options_.buffer_base + res_.buffers.size())};
    }
    if (res_.uses_lds) {
        // the local data share: 32 KiB of dwords (the most a GCN workgroup gets)
        t_ptr_wg_u32_ = b_.TypePointer(spv::StorageClass::Workgroup, t_u32_);
        const Id arr = b_.TypeArray(t_u32_, b_.U32(kLdsDwords));
        lds_ = b_.Variable(b_.TypePointer(spv::StorageClass::Workgroup, arr), spv::StorageClass::Workgroup);
        interface_.push_back(lds_);
    }
    DeclareImages(base + static_cast<u32>(res_.buffers.size()) + 1 + (res_.uses_gds ? 1 : 0));
    // PushData: float xoffset, yoffset, xscale, yscale; u8 buf_offsets[56] (as 14 dwords)
    const Id f4 = b_.TypeArray(t_f32_, b_.U32(4));
    b_.Decorate(f4, spv::Decoration::ArrayStride, {4});
    const Id u14 = b_.TypeArray(t_u32_, b_.U32(14));
    b_.Decorate(u14, spv::Decoration::ArrayStride, {4});
    const Id members[] = {f4, u14};
    const Id pd = b_.TypeStruct(members);
    b_.Decorate(pd, spv::Decoration::Block);
    b_.MemberDecorate(pd, 0, spv::Decoration::Offset, {0});
    b_.MemberDecorate(pd, 1, spv::Decoration::Offset, {16});
    push_ = b_.Variable(b_.TypePointer(spv::StorageClass::PushConstant, pd), spv::StorageClass::PushConstant);
    interface_.push_back(push_);
    t_ptr_pc_u32_ = b_.TypePointer(spv::StorageClass::PushConstant, t_u32_);
}

Id Emitter::FlatDword(Id index) {
    return b_.Op(Op::OpLoad, t_u32_, {b_.Op(Op::OpAccessChain, t_ptr_sb_u32_, {flat_, b_.U32(0), index})});
}

Id Emitter::BufOffset(u32 buffer) {
    const u32 k = options_.buffer_base + buffer; // (the V#s are the stage's first buffers)
    const Id word =
        b_.Op(Op::OpLoad, t_u32_, {b_.Op(Op::OpAccessChain, t_ptr_pc_u32_, {push_, b_.U32(1), b_.U32(k / 4)})});
    return b_.Op(Op::OpBitFieldUExtract, t_u32_, {word, b_.U32((k % 4) * 8), b_.U32(8)});
}

Id Emitter::SharpDword(u32 buffer, u32 dword) {
    const ScalarValue& v = res_.buffers[buffer].sharp[dword];
    return v.kind == ScalarValue::Kind::Const ? b_.U32(v.value) : FlatDword(b_.U32(v.value));
}

void Emitter::EmitSmrd(const Inst& in, u32 index, std::string_view name) {
    const Operands o = GetOperands(in);
    const u32 n = name.ends_with("x16") ? 16 : name.ends_with("x8") ? 8 : name.ends_with("x4") ? 4
                  : name.ends_with("x2") ? 2 : 1;
    if (res_.load_of[index] >= 0) {
        // s_load_dword*: the walk copied it into flat
        const WalkLoad& w = res_.walk[res_.load_of[index]];
        for (u32 i = 0; i < n; ++i) {
            Set(o.dst[0] + i, FlatDword(b_.U32(w.dst + i)));
        }
        return;
    }
    // s_buffer_load_dword*: from the buffer of its V#, the offset in bytes
    const u32 buffer = static_cast<u32>(res_.buffer_of[index]);
    Id offset = 0;
    if (((in.w0 >> 8) & 1) != 0) {
        offset = b_.U32((in.w0 & 0xFF) * 4);
    } else if ((in.w0 & 0xFF) == kLiteralOperand) {
        offset = b_.U32(in.lit * 4);
    } else {
        offset = b_.Op(Op::OpBitwiseAnd, t_u32_, {Get(in.w0 & 0xFF, in), b_.U32(~3u)});
    }
    const Id first =
        b_.Op(Op::OpShiftRightLogical, t_u32_, {b_.Op(Op::OpIAdd, t_u32_, {offset, BufOffset(buffer)}), b_.U32(2)});
    for (u32 i = 0; i < n; ++i) {
        const Id idx = i == 0 ? first : b_.Op(Op::OpIAdd, t_u32_, {first, b_.U32(i)});
        Set(o.dst[0] + i,
            b_.Op(Op::OpLoad, t_u32_, {b_.Op(Op::OpAccessChain, t_ptr_sb_u32_, {buffers_[buffer], b_.U32(0), idx})}));
    }
}

bool Emitter::EmitBuffer(const Inst& in, u32 index, std::string_view name) {
    // raw dword loads / stores and the integer atomics; the formatted ones need the V#'s format (not yet)
    u32 n = 0;
    enum class Kind { Load, Store, Atomic } kind = Kind::Load;
    Op atomic = Op::OpNop;
    const auto width = [&]() -> u32 {
        return name.ends_with("x4") ? 4 : name.ends_with("x3") ? 3 : name.ends_with("x2") ? 2 : 1;
    };
    if (name.starts_with("buffer_load_dword")) {
        n = width();
    } else if (name.starts_with("buffer_store_dword")) {
        kind = Kind::Store;
        n = width();
    } else if (name.starts_with("buffer_atomic_")) {
        kind = Kind::Atomic;
        n = 1;
        static constexpr std::pair<std::string_view, Op> kAtomics[] = {
            {"buffer_atomic_add", Op::OpAtomicIAdd},  {"buffer_atomic_sub", Op::OpAtomicISub},
            {"buffer_atomic_umax", Op::OpAtomicUMax}, {"buffer_atomic_umin", Op::OpAtomicUMin},
            {"buffer_atomic_smax", Op::OpAtomicSMax}, {"buffer_atomic_smin", Op::OpAtomicSMin},
            {"buffer_atomic_and", Op::OpAtomicAnd},   {"buffer_atomic_or", Op::OpAtomicOr},
            {"buffer_atomic_xor", Op::OpAtomicXor},   {"buffer_atomic_swap", Op::OpAtomicExchange},
        };
        for (const auto& [nm, op] : kAtomics) {
            if (name == nm) {
                atomic = op;
            }
        }
        if (atomic == Op::OpNop) {
            return false; // (inc/dec wrap, cmpswap, 64-bit: not yet)
        }
    } else {
        return false;
    }
    if (((in.w0 >> 15) & 1) != 0 || ((in.w0 >> 16) & 1) != 0) {
        return false; // addr64 / lds
    }
    const u32 buffer = static_cast<u32>(res_.buffer_of[index]);
    const bool offen = ((in.w0 >> 12) & 1) != 0, idxen = ((in.w0 >> 13) & 1) != 0, glc = ((in.w0 >> 14) & 1) != 0;
    const u32 vaddr = kVgpr + (in.w1 & 0xFF), vdata = kVgpr + ((in.w1 >> 8) & 0xFF);
    // byte address = soffset + offset12 + (idxen ? index * stride) + (offen ? voffset)
    Id addr = b_.Op(Op::OpIAdd, t_u32_, {Get((in.w1 >> 24) & 0xFF, in), b_.U32(in.w0 & 0xFFF)});
    if (idxen) {
        const Id stride = b_.Op(Op::OpBitFieldUExtract, t_u32_, {SharpDword(buffer, 1), b_.U32(16), b_.U32(14)});
        addr = b_.Op(Op::OpIAdd, t_u32_, {addr, b_.Op(Op::OpIMul, t_u32_, {Get(vaddr, in), stride})});
    }
    if (offen) {
        addr = b_.Op(Op::OpIAdd, t_u32_, {addr, Get(idxen ? vaddr + 1 : vaddr, in)});
    }
    const Id first =
        b_.Op(Op::OpShiftRightLogical, t_u32_, {b_.Op(Op::OpIAdd, t_u32_, {addr, BufOffset(buffer)}), b_.U32(2)});
    for (u32 i = 0; i < n; ++i) {
        const Id idx = i == 0 ? first : b_.Op(Op::OpIAdd, t_u32_, {first, b_.U32(i)});
        const Id ptr = b_.Op(Op::OpAccessChain, t_ptr_sb_u32_, {buffers_[buffer], b_.U32(0), idx});
        switch (kind) {
        case Kind::Load:
            Set(vdata + i, b_.Op(Op::OpLoad, t_u32_, {ptr}));
            break;
        case Kind::Store:
            b_.OpNoResult(Op::OpStore, {ptr, Get(vdata + i, in)});
            break;
        case Kind::Atomic: {
            const Id old =
                b_.Op(atomic, t_u32_, {ptr, b_.U32(static_cast<u32>(spv::Scope::Device)), b_.U32(0), Get(vdata, in)});
            if (glc) {
                Set(vdata, old); // (GLC: the value before)
            }
            break;
        }
        }
    }
    return true;
}

Id Emitter::BufferAddress(const Inst& in, u32 buffer) {
    const bool offen = ((in.w0 >> 12) & 1) != 0, idxen = ((in.w0 >> 13) & 1) != 0;
    const u32 vaddr = kVgpr + (in.w1 & 0xFF);
    Id addr = b_.Op(Op::OpIAdd, t_u32_, {Get((in.w1 >> 24) & 0xFF, in), b_.U32(in.w0 & 0xFFF)});
    if (idxen) {
        const Id stride = b_.Op(Op::OpBitFieldUExtract, t_u32_, {SharpDword(buffer, 1), b_.U32(16), b_.U32(14)});
        addr = b_.Op(Op::OpIAdd, t_u32_, {addr, b_.Op(Op::OpIMul, t_u32_, {Get(vaddr, in), stride})});
    }
    if (offen) {
        addr = b_.Op(Op::OpIAdd, t_u32_, {addr, Get(idxen ? vaddr + 1 : vaddr, in)});
    }
    return b_.Op(Op::OpIAdd, t_u32_, {addr, BufOffset(buffer)});
}

Id Emitter::ReadBits(u32 buffer, Id byte_addr, u32 bit, u32 width) {
    // bits [s, s + width) of (word[i] | word[i + 1] << 32): i = addr / 4 + (bit / 32), s = (addr % 4) * 8 + bit % 32
    const auto load = [&](Id i) {
        return b_.Op(Op::OpLoad, t_u32_, {b_.Op(Op::OpAccessChain, t_ptr_sb_u32_, {buffers_[buffer], b_.U32(0), i})});
    };
    const Id bitpos = b_.Op(Op::OpIAdd, t_u32_,
                            {b_.Op(Op::OpShiftLeftLogical, t_u32_, {b_.Op(Op::OpBitwiseAnd, t_u32_, {byte_addr, b_.U32(3)}), b_.U32(3)}),
                             b_.U32(bit)});
    const Id idx = b_.Op(Op::OpIAdd, t_u32_, {b_.Op(Op::OpShiftRightLogical, t_u32_, {byte_addr, b_.U32(2)}),
                                             b_.Op(Op::OpShiftRightLogical, t_u32_, {bitpos, b_.U32(5)})});
    const Id shift = b_.Op(Op::OpBitwiseAnd, t_u32_, {bitpos, b_.U32(31)});
    const Id lo = load(idx);
    const Id hi = load(b_.Op(Op::OpIAdd, t_u32_, {idx, b_.U32(1)}));
    const Id joined = b_.Op(Op::OpBitwiseOr, t_u32_,
                            {b_.Op(Op::OpShiftRightLogical, t_u32_, {lo, shift}),
                             b_.Op(Op::OpShiftLeftLogical, t_u32_, {hi, b_.Op(Op::OpISub, t_u32_, {b_.U32(32), shift})})});
    // (a shift by 32 is undefined: within one dword the field is lo's)
    const Id inside = b_.Op(Op::OpULessThanEqual, t_bool_, {b_.Op(Op::OpIAdd, t_u32_, {shift, b_.U32(width)}), b_.U32(32)});
    if (width == 32) {
        return Select(t_u32_, b_.Op(Op::OpIEqual, t_bool_, {shift, b_.U32(0)}), lo, joined);
    }
    const Id simple = b_.Op(Op::OpBitFieldUExtract, t_u32_, {lo, shift, b_.U32(width)});
    const Id spanned = b_.Op(Op::OpBitFieldUExtract, t_u32_, {joined, b_.U32(0), b_.U32(width)});
    return Select(t_u32_, inside, simple, spanned);
}

bool Emitter::EmitTbuffer(const Inst& in, u32 index, std::string_view name) {
    if (!name.starts_with("tbuffer_load_format_") || ((in.w0 >> 15) & 1) != 0) {
        return false; // (stores: not used by the game; addr64)
    }
    const u32 n = name.ends_with("xyzw") ? 4 : name.ends_with("xyz") ? 3 : name.ends_with("xy") ? 2 : 1;
    // the instruction's format, the components in order (the bridge: no DST_SEL for typed instructions)
    return EmitTypedLoad(in, index, n, (in.w0 >> 19) & 0xF, (in.w0 >> 23) & 7, 4 | 5 << 3 | 6 << 6 | 7 << 9);
}

bool Emitter::EmitFormattedBuffer(const Inst& in, u32 index, std::string_view name) {
    if (((in.w0 >> 15) & 1) != 0 || ((in.w0 >> 16) & 1) != 0) {
        return false; // addr64 / lds
    }
    const u32 buffer = static_cast<u32>(res_.buffer_of[index]);
    const ScalarValue& w3 = res_.buffers[buffer].sharp[3];
    u32 dw3 = 0;
    if (w3.kind == ScalarValue::Kind::Const) {
        dw3 = w3.value;
    } else if (w3.kind == ScalarValue::Kind::Flat && w3.value < options_.flat.size()) {
        dw3 = options_.flat[w3.value];
    } else {
        return false; // (no flattened user data at the translation)
    }
    const u32 dfmt = (dw3 >> 15) & 0xF, nfmt = (dw3 >> 12) & 7, dst_sel = dw3 & 0xFFF;
    const u32 n = name.ends_with("xyzw") ? 4 : name.ends_with("xyz") ? 3 : name.ends_with("xy") ? 2 : 1;
    if (name.starts_with("buffer_load_format_")) {
        return EmitTypedLoad(in, index, n, dfmt, nfmt, dst_sel);
    }
    // stores: formats of 32-bit integer / float components in order (a narrower component would be a read-modify-write
    // of a dword other lanes may write too); a null V# stores nothing
    if (dfmt == 0) {
        return true;
    }
    const bool dwords = dfmt == 4 || dfmt == 11 || dfmt == 13 || dfmt == 14;
    if (!dwords && (dfmt == 1 || dfmt == 2 || dfmt == 3 || dfmt == 5) && (nfmt == 0 || nfmt == 4 || nfmt == 5 || nfmt == 7) &&
        !(nfmt == 7 && (dfmt == 1 || dfmt == 3))) {
        // 8 / 16-bit components: each its bits of a dword, cleared and set by two atomics (lanes writing the other
        // parts of the same dword do not disturb them)
        const u32 w = (dfmt == 1 || dfmt == 3) ? 8 : 16, ncomp = (dfmt == 3 || dfmt == 5) ? 2 : 1;
        const u32 mask = (1u << w) - 1;
        const u32 vdata = kVgpr + ((in.w1 >> 8) & 0xFF);
        const Id addr = BufferAddress(in, buffer);
        const Id scope = b_.U32(static_cast<u32>(spv::Scope::Device));
        for (u32 c = 0; c < std::min(n, ncomp); ++c) {
            if (((dst_sel >> (3 * c)) & 7) != 4 + c) {
                return Decline(fmt::format(" (a swizzled store: DST_SEL {:#x})", dst_sel));
            }
            const Id v = Get(vdata + c, in);
            Id raw = v;
            if (nfmt == 7) {
                const Id v2 = b_.Op(Op::OpCompositeConstruct, b_.TypeVector(t_f32_, 2), {Bit(t_f32_, v), b_.F32(0.0f)});
                raw = Glsl(GLSLstd450PackHalf2x16, t_u32_, {v2});
            } else if (nfmt == 0) {
                const Id clamped = Glsl(GLSLstd450NClamp, t_f32_, {Bit(t_f32_, v), b_.F32(0.0f), b_.F32(1.0f)});
                raw = b_.Op(Op::OpConvertFToU, t_u32_,
                            {Glsl(GLSLstd450RoundEven, t_f32_, {b_.Op(Op::OpFMul, t_f32_, {clamped, b_.F32(static_cast<float>(mask))})})});
            }
            raw = b_.Op(Op::OpBitwiseAnd, t_u32_, {raw, b_.U32(mask)});
            const Id bitpos = b_.Op(Op::OpIAdd, t_u32_,
                                    {b_.Op(Op::OpShiftLeftLogical, t_u32_, {b_.Op(Op::OpBitwiseAnd, t_u32_, {addr, b_.U32(3)}), b_.U32(3)}),
                                     b_.U32(c * w)});
            const Id idx = b_.Op(Op::OpIAdd, t_u32_, {b_.Op(Op::OpShiftRightLogical, t_u32_, {addr, b_.U32(2)}),
                                                     b_.Op(Op::OpShiftRightLogical, t_u32_, {bitpos, b_.U32(5)})});
            const Id shift = b_.Op(Op::OpBitwiseAnd, t_u32_, {bitpos, b_.U32(31)});
            const Id ptr = b_.Op(Op::OpAccessChain, t_ptr_sb_u32_, {buffers_[buffer], b_.U32(0), idx});
            const Id keep = b_.Op(Op::OpNot, t_u32_, {b_.Op(Op::OpShiftLeftLogical, t_u32_, {b_.U32(mask), shift})});
            b_.Op(Op::OpAtomicAnd, t_u32_, {ptr, scope, b_.U32(0), keep});
            b_.Op(Op::OpAtomicOr, t_u32_, {ptr, scope, b_.U32(0), b_.Op(Op::OpShiftLeftLogical, t_u32_, {raw, shift})});
        }
        return true;
    }
    const u32 comps = dfmt == 4 ? 1 : dfmt == 11 ? 2 : dfmt == 13 ? 3 : 4;
    if (!dwords || (nfmt != 4 && nfmt != 5 && nfmt != 7)) {
        return Decline(fmt::format(" (a store of dfmt {} nfmt {})", dfmt, nfmt));
    }
    for (u32 c = 0; c < std::min(n, comps); ++c) {
        if (((dst_sel >> (3 * c)) & 7) != 4 + c) {
            return Decline(fmt::format(" (a swizzled store: DST_SEL {:#x})", dst_sel));
        }
    }
    const u32 vdata = kVgpr + ((in.w1 >> 8) & 0xFF);
    const Id first = b_.Op(Op::OpShiftRightLogical, t_u32_, {BufferAddress(in, buffer), b_.U32(2)});
    for (u32 c = 0; c < std::min(n, comps); ++c) {
        const Id idx = c == 0 ? first : b_.Op(Op::OpIAdd, t_u32_, {first, b_.U32(c)});
        b_.OpNoResult(Op::OpStore, {b_.Op(Op::OpAccessChain, t_ptr_sb_u32_, {buffers_[buffer], b_.U32(0), idx}),
                                    Get(vdata + c, in)});
    }
    return true;
}

bool Emitter::EmitTypedLoad(const Inst& in, u32 index, u32 n, u32 dfmt, u32 nfmt, u32 dst_sel) {
    const u32 vdata = kVgpr + ((in.w1 >> 8) & 0xFF);
    if (dfmt == 0) {
        for (u32 c = 0; c < n; ++c) {
            Set(vdata + c, b_.U32(0)); // a null V#: zeros (as the bridge)
        }
        return true;
    }
    struct Fmt {
        u32 comps;
        std::array<u8, 4> bits;
    };
    // the data formats (dfmt): components and their widths, from bit 0
    static constexpr Fmt kFmt[15] = {
        {0, {0, 0, 0, 0}},     {1, {8, 0, 0, 0}},    {1, {16, 0, 0, 0}},   {2, {8, 8, 0, 0}},
        {1, {32, 0, 0, 0}},    {2, {16, 16, 0, 0}},  {3, {10, 11, 11, 0}}, {3, {11, 11, 10, 0}},
        {4, {10, 10, 10, 2}},  {4, {2, 10, 10, 10}}, {4, {8, 8, 8, 8}},    {2, {32, 32, 0, 0}},
        {4, {16, 16, 16, 16}}, {3, {32, 32, 32, 0}}, {4, {32, 32, 32, 32}},
    };
    if (dfmt > 14 || dfmt == 6 || dfmt == 7) {
        return Decline(fmt::format(" (dfmt {} nfmt {})", dfmt, nfmt)); // (invalid / packed floats / reserved)
    }
    const Fmt& f = kFmt[dfmt];
    for (u32 c = 0; c < f.comps; ++c) {
        if (nfmt == 7 && f.bits[c] != 16 && f.bits[c] != 32) {
            return Decline(fmt::format(" (dfmt {} nfmt {}: floats of {} bits)", dfmt, nfmt, f.bits[c]));
        }
    }
    const u32 buffer = static_cast<u32>(res_.buffer_of[index]);
    const Id addr = BufferAddress(in, buffer);
    const bool int_format = nfmt == 4 || nfmt == 5;
    const Id one = int_format ? b_.U32(1) : b_.U32(0x3F800000u);
    // the components DST_SEL picks
    u32 needed = 0;
    for (u32 c = 0; c < n; ++c) {
        if (const u32 sel = (dst_sel >> (3 * c)) & 7; sel >= 4) {
            needed |= 1u << (sel - 4);
        }
    }
    std::array<Id, 4> comp{};
    u32 bit = 0;
    for (u32 c = 0; c < 4; ++c) {
        if (c >= f.comps) {
            // missing components: 0, 0, 0 and 1 (1.0 or 1 by the number format)
            comp[c] = c == 3 ? one : b_.U32(0);
            continue;
        }
        if (((needed >> c) & 1) == 0) {
            bit += f.bits[c];
            continue;
        }
        const u32 w = f.bits[c];
        const Id raw = ReadBits(buffer, addr, bit, w);
        bit += w;
        const auto sext = [&]() {
            return w == 32 ? Bit(t_i32_, raw) : b_.Op(Op::OpBitFieldSExtract, t_i32_, {Bit(t_i32_, raw), b_.U32(0), b_.U32(w)});
        };
        const float max_u = static_cast<float>(w == 32 ? 4294967295.0 : static_cast<double>((1ull << w) - 1));
        const float max_s = static_cast<float>((1ull << (w - 1)) - 1);
        Id v = raw;
        switch (nfmt) {
        case 0: // unorm
            v = Bit(t_u32_, b_.Op(Op::OpFMul, t_f32_, {b_.Op(Op::OpConvertUToF, t_f32_, {raw}), b_.F32(1.0f / max_u)}));
            break;
        case 1: // snorm
            v = Bit(t_u32_, Glsl(GLSLstd450FMax, t_f32_,
                                 {b_.Op(Op::OpFMul, t_f32_, {b_.Op(Op::OpConvertSToF, t_f32_, {sext()}), b_.F32(1.0f / max_s)}),
                                  b_.F32(-1.0f)}));
            break;
        case 2: // uscaled
            v = Bit(t_u32_, b_.Op(Op::OpConvertUToF, t_f32_, {raw}));
            break;
        case 3: // sscaled
            v = Bit(t_u32_, b_.Op(Op::OpConvertSToF, t_f32_, {sext()}));
            break;
        case 5: // sint
            v = Bit(t_u32_, sext());
            break;
        case 6: // snorm_nz: (2s + 1) / (2^w - 1) (the bridge's)
            v = Bit(t_u32_, b_.Op(Op::OpFMul, t_f32_,
                                  {b_.Op(Op::OpConvertSToF, t_f32_,
                                         {b_.Op(Op::OpIAdd, t_i32_, {b_.Op(Op::OpIMul, t_i32_, {sext(), b_.Constant(t_i32_, 2)}),
                                                                     b_.Constant(t_i32_, 1)})}),
                                   b_.F32(1.0f / max_u)}));
            break;
        case 7: // float
            if (w == 16) {
                v = Bit(t_u32_, b_.Op(Op::OpCompositeExtract, t_f32_,
                                      {Glsl(GLSLstd450UnpackHalf2x16, b_.TypeVector(t_f32_, 2), {raw}), 0}));
            }
            break;
        default: // uint
            break;
        }
        comp[c] = v;
    }
    for (u32 c = 0; c < n; ++c) {
        const u32 sel = (dst_sel >> (3 * c)) & 7;
        Set(vdata + c, sel >= 4 ? comp[sel - 4] : sel == 1 ? one : b_.U32(0));
    }
    return true;
}

std::optional<u32> Emitter::FlatValue(const ScalarValue& v) const {
    if (v.kind == ScalarValue::Kind::Const) {
        return v.value;
    }
    if (v.kind == ScalarValue::Kind::Flat && v.value < options_.flat.size()) {
        return options_.flat[v.value];
    }
    return std::nullopt;
}

void Emitter::DeclareImages(u32 binding) {
    for (const ImageUse& use : res_.images) {
        ImageDecl d;
        const auto dw1 = FlatValue(use.sharp[1]), dw3 = FlatValue(use.sharp[3]);
        const u32 type = dw3 ? (*dw3 >> 28) : 0;
        // a cube: a 2D array of its faces, the face the third coordinate (the bridge's type; the runtime binds its view
        // so)
        const bool cube = type == 11;
        const bool type_array = type == 12 || type == 13;
        if (dw1 && dw3 && (type == 8 || type == 9 || type == 10 || type_array || cube)) {
            const u32 nfmt = (*dw1 >> 26) & 0xF;
            d.kind = nfmt == 4 ? 1 : nfmt == 5 ? 2 : 0;
            d.scalar = d.kind == 1 ? t_u32_ : d.kind == 2 ? t_i32_ : t_f32_;
            d.texel = b_.TypeVector(d.scalar, 4);
            d.storage = use.written;
            // (the runtime's view: an array type without DA is a single layer)
            d.arrayed = cube || (use.array && type_array);
            d.dst_sel = *dw3 & 0xFFF;
            d.one_d = type == 8 || type == 12;
            const spv::Dim dim = type == 10 ? spv::Dim::Dim3D : spv::Dim::Dim2D;
            d.coords = (d.one_d ? 1 : dim == spv::Dim::Dim2D ? 2 : 3) + (d.arrayed ? 1 : 0);
            if (d.storage) {
                b_.Capability(spv::Capability::StorageImageReadWithoutFormat);
                b_.Capability(spv::Capability::StorageImageWriteWithoutFormat);
            }
            d.type = b_.TypeImage(d.scalar, dim, 0, d.arrayed ? 1 : 0, 0, d.storage ? 2 : 1, spv::ImageFormat::Unknown);
            d.ok = true;
        } else {
            d.null = dw1 && dw3 && type < 8; // (the hardware reads zeros, drops writes; a real T# is another key)
            d.ok = d.null;
            // (an unknown T#, a cube or a multisampled image: the instructions on it are not translated; the binding
            // is declared all the same)
            d.scalar = t_f32_;
            d.texel = b_.TypeVector(t_f32_, 4);
            d.type = b_.TypeImage(t_f32_, spv::Dim::Dim2D, 0, 0, 0, use.written ? 2 : 1, spv::ImageFormat::Unknown);
            if (use.written) {
                b_.Capability(spv::Capability::StorageImageReadWithoutFormat);
                b_.Capability(spv::Capability::StorageImageWriteWithoutFormat);
            }
        }
        d.var = b_.Variable(b_.TypePointer(spv::StorageClass::UniformConstant, d.type), spv::StorageClass::UniformConstant);
        b_.Decorate(d.var, spv::Decoration::DescriptorSet, {0});
        b_.Decorate(d.var, spv::Decoration::Binding, {binding++});
        interface_.push_back(d.var);
        images_.push_back(d);
    }
    for (size_t i = 0; i < res_.samplers.size(); ++i) {
        const Id v = b_.Variable(b_.TypePointer(spv::StorageClass::UniformConstant, b_.TypeSampler()),
                                 spv::StorageClass::UniformConstant);
        b_.Decorate(v, spv::Decoration::DescriptorSet, {0});
        b_.Decorate(v, spv::Decoration::Binding, {binding++});
        interface_.push_back(v);
        samplers_.push_back(v);
    }
}

bool Emitter::EmitImage(const Inst& in, u32 index, std::string_view name) {
    const s32 ii = res_.image_of[index];
    if (ii < 0 || !images_[ii].ok) {
        if (ii >= 0) {
            const auto dw3 = FlatValue(res_.images[ii].sharp[3]);
            return Decline(dw3 ? fmt::format(" (T# type {})", *dw3 >> 28) : std::string(" (T# not at the translation)"));
        }
        return false;
    }
    const ImageDecl& im = images_[ii];
    const u32 dmask = (in.w0 >> 8) & 0xF;
    if (im.null) {
        if (name.starts_with("image_load") || name.starts_with("image_sample") || name.starts_with("image_gather")) {
            const u32 vd = kVgpr + ((in.w1 >> 8) & 0xFF);
            for (u32 c = 0, k = 0; c < 4; ++c) {
                if (((dmask >> c) & 1) != 0) {
                    Set(vd + k++, b_.U32(0));
                }
            }
            return true;
        }
        return name == "image_store";
    }
    const bool unorm = ((in.w0 >> 12) & 1) != 0, tfe = ((in.w0 >> 16) & 1) != 0, lwe = ((in.w0 >> 17) & 1) != 0;
    if (tfe || lwe) {
        return false;
    }
    const u32 vdata = kVgpr + ((in.w1 >> 8) & 0xFF);
    // the address: [offset] then the coordinates (then the LOD)
    const bool has_offset = name.ends_with("_o");
    const u32 offset_reg = kVgpr + (in.w1 & 0xFF);
    const u32 vaddr = offset_reg + (has_offset ? 1 : 0);
    const Id image = b_.Op(Op::OpLoad, im.type, {im.var});
    // the coordinates: integer (loads, stores) or float (sampling), one or a vector
    const auto coords = [&](bool integer) {
        const Id ty = integer ? t_i32_ : t_f32_;
        std::vector<u32> c;
        for (u32 k = 0; k < im.coords; ++k) {
            c.push_back(Bit(ty, Get(vaddr + k, in)));
            if (im.one_d && k == 0) {
                c.push_back(integer ? b_.Constant(t_i32_, 0) : b_.F32(0.0f)); // (the row of a 1D image)
            }
        }
        if (c.size() == 1) {
            return c[0];
        }
        return b_.Op(Op::OpCompositeConstruct, b_.TypeVector(ty, static_cast<u32>(c.size())),
                     std::span<const u32>(c.data(), c.size()));
    };
    const auto results = [&](Id texel, bool swizzle) {
        // the components dmask selects, packed into consecutive VGPRs (a storage image's DST_SEL by the shader)
        u32 k = 0;
        const Id one = im.kind == 0 ? b_.U32(0x3F800000u) : b_.U32(1);
        for (u32 c = 0; c < 4; ++c) {
            if (((dmask >> c) & 1) == 0) {
                continue;
            }
            const u32 sel = swizzle ? (im.dst_sel >> (3 * c)) & 7 : 4 + c;
            Id v = sel == 1 ? one : b_.U32(0);
            if (sel >= 4) {
                const Id comp = b_.Op(Op::OpCompositeExtract, im.scalar, {texel, sel - 4});
                v = im.kind == 1 ? comp : Bit(t_u32_, comp);
            }
            Set(vdata + k++, v);
        }
    };
    const bool identity = im.dst_sel == (4 | 5 << 3 | 6 << 6 | 7 << 9);
    if (name == "image_load" || name == "image_load_mip") {
        const bool mip = name == "image_load_mip";
        if (im.storage) {
            if (mip) {
                return Decline(" (a storage image's mip level)"); // (bound one by one, not yet)
            }
            results(b_.Op(Op::OpImageRead, im.texel, {image, coords(true)}), true);
            return true;
        }
        const Id lod = mip ? Bit(t_i32_, Get(vaddr + im.coords, in)) : b_.Constant(t_i32_, 0);
        results(b_.Op(Op::OpImageFetch, im.texel,
                      {image, coords(true), static_cast<u32>(spv::ImageOperandsMask::Lod), lod}),
                false);
        return true;
    }
    if (name == "image_store") {
        if (!im.storage) {
            return Decline(" (a store to a sampled image)");
        }
        // the shader's component j goes to the image's channel DST_SEL[j] - 4 (the inverse of the view's swizzle)
        std::array<u32, 4> c{};
        std::array<bool, 4> set{};
        u32 k = 0;
        for (u32 j = 0; j < 4; ++j) {
            const Id v = ((dmask >> j) & 1) != 0 ? Get(vdata + k++, in) : b_.U32(0);
            const u32 sel = identity ? 4 + j : (im.dst_sel >> (3 * j)) & 7;
            if (sel >= 4 && !set[sel - 4]) {
                c[sel - 4] = im.kind == 1 ? v : Bit(im.scalar, v);
                set[sel - 4] = true;
            }
        }
        for (u32 j = 0; j < 4; ++j) {
            if (!set[j]) {
                c[j] = im.kind == 1 ? b_.U32(0) : Bit(im.scalar, b_.U32(0));
            }
        }
        const Id texel = b_.Op(Op::OpCompositeConstruct, im.texel, std::span<const u32>(c.data(), 4));
        b_.OpNoResult(Op::OpImageWrite, {image, coords(true), texel});
        return true;
    }
    if (name == "image_sample_lz" || name == "image_sample_l" || name == "image_sample_lz_o" || name == "image_sample_l_o") {
        const s32 si = res_.sampler_of[index];
        if (si < 0 || im.storage || unorm) {
            return Decline(unorm ? " (unnormalized coordinates)" : " (no sampler / a storage image)");
        }
        const Id sampler = b_.Op(Op::OpLoad, b_.TypeSampler(), {samplers_[si]});
        const Id sampled = b_.Op(Op::OpSampledImage, b_.TypeSampledImage(im.type), {image, sampler});
        const Id lod = name.starts_with("image_sample_l_") || name == "image_sample_l" ? GetF(vaddr + im.coords, in)
                                                                                         : b_.F32(0.0f);
        if (!has_offset) {
            results(b_.Op(Op::OpImageSampleExplicitLod, im.texel,
                          {sampled, coords(false), static_cast<u32>(spv::ImageOperandsMask::Lod), lod}),
                    false);
            return true;
        }
        // the texel offset: 6-bit signed fields at bits 0, 8, 16 of one VGPR (x, y[, z]). Vulkan allows a dynamic
        // Offset only on gathers: the coordinates move by offset / the level's size instead (what the offset means)
        b_.Capability(spv::Capability::ImageQuery);
        const Id packed = Get(offset_reg, in);
        const u32 dims = im.coords - (im.arrayed ? 1 : 0) + (im.one_d ? 1 : 0); // (the SPIR-V image's dimensions)
        const u32 qn = dims + (im.arrayed ? 1 : 0);
        const Id lod_i = b_.Op(Op::OpConvertFToS, t_i32_, {lod});
        const Id size = b_.Op(Op::OpImageQuerySizeLod, b_.TypeVector(t_i32_, qn), {image, lod_i});
        std::vector<u32> c;
        u32 reg = 0;
        for (u32 k = 0; k < qn; ++k) {
            const bool layer = im.arrayed && k == qn - 1;
            const bool row = im.one_d && k == 1;
            Id v = row ? b_.F32(0.0f) : GetF(vaddr + reg++, in);
            if (!layer && !row) {
                const Id off = b_.Op(Op::OpConvertSToF, t_f32_,
                                     {b_.Op(Op::OpBitFieldSExtract, t_i32_, {Bit(t_i32_, packed), b_.U32(8 * (im.one_d ? 0 : k)), b_.U32(6)})});
                const Id extent = b_.Op(Op::OpConvertSToF, t_f32_, {b_.Op(Op::OpCompositeExtract, t_i32_, {size, k})});
                v = b_.Op(Op::OpFAdd, t_f32_, {v, b_.Op(Op::OpFDiv, t_f32_, {off, extent})});
            }
            c.push_back(v);
        }
        const Id coord = b_.Op(Op::OpCompositeConstruct, b_.TypeVector(t_f32_, qn), std::span<const u32>(c.data(), c.size()));
        results(b_.Op(Op::OpImageSampleExplicitLod, im.texel,
                      {sampled, coord, static_cast<u32>(spv::ImageOperandsMask::Lod), lod}),
                false);
        return true;
    }
    return false;
}

bool Emitter::EmitDs(const Inst& in, std::string_view name) {
    const u32 off0 = in.w0 & 0xFF, off1 = (in.w0 >> 8) & 0xFF, off16 = off0 | (off1 << 8);
    const bool gds = ((in.w0 >> 17) & 1) != 0;
    const u32 addr_reg = kVgpr + (in.w1 & 0xFF), data0 = kVgpr + ((in.w1 >> 8) & 0xFF),
              data1 = kVgpr + ((in.w1 >> 16) & 0xFF), vdst = kVgpr + ((in.w1 >> 24) & 0xFF);
    const Id scope_sg = b_.U32(static_cast<u32>(spv::Scope::Subgroup));
    if (name == "ds_append" || name == "ds_consume") {
        // once per subgroup: +/- the active lanes, by the lowest one; all lanes get the value before (the v_mbcnt that
        // follows gives each lane its place - the hardware does it once per wave)
        const Id base = b_.Op(Op::OpBitFieldUExtract, t_u32_, {Get(kM0, in), b_.U32(16), b_.U32(16)});
        const Id dw = b_.Op(Op::OpShiftRightLogical, t_u32_, {b_.Op(Op::OpIAdd, t_u32_, {base, b_.U32(off16)}), b_.U32(2)});
        const Id all = b_.Op(Op::OpGroupNonUniformBallot, t_uvec4_, {scope_sg, b_.ConstantBool(true)});
        const Id count = b_.Op(Op::OpGroupNonUniformBallotBitCount, t_u32_,
                               {scope_sg, static_cast<u32>(spv::GroupOperation::Reduce), all});
        const Id first = b_.Op(Op::OpGroupNonUniformElect, t_bool_, {scope_sg});
        const Id slot = b_.LocalVariable(t_ptr_u32_, b_.U32(0));
        const Id then = b_.NewId(), merge = b_.NewId();
        b_.OpNoResult(Op::OpSelectionMerge, {merge, 0});
        b_.OpNoResult(Op::OpBranchConditional, {first, then, merge});
        b_.Label(then);
        const Id ptr = b_.Op(Op::OpAccessChain, t_ptr_sb_u32_, {gds_, b_.U32(0), dw});
        const Id old = b_.Op(name == "ds_append" ? Op::OpAtomicIAdd : Op::OpAtomicISub, t_u32_,
                             {ptr, b_.U32(static_cast<u32>(spv::Scope::Device)), b_.U32(0), count});
        b_.OpNoResult(Op::OpStore, {slot, old});
        b_.OpNoResult(Op::OpBranch, {merge});
        b_.Label(merge);
        Set(vdst, b_.Op(Op::OpGroupNonUniformBroadcastFirst, t_u32_, {scope_sg, b_.Op(Op::OpLoad, t_u32_, {slot})}));
        return true;
    }
    if (name == "ds_swizzle_b32") {
        const Id lane = LaneId();
        Id src_lane = 0;
        if ((off16 & 0x8000) != 0) {
            // quad permute: lane 4q + i reads lane 4q + offset[2i + 1:2i]
            const Id i = b_.Op(Op::OpBitwiseAnd, t_u32_, {lane, b_.U32(3)});
            const Id sel = b_.Op(Op::OpBitFieldUExtract, t_u32_,
                                 {b_.U32(off16 & 0xFF), b_.Op(Op::OpShiftLeftLogical, t_u32_, {i, b_.U32(1)}), b_.U32(2)});
            src_lane = b_.Op(Op::OpBitwiseOr, t_u32_, {b_.Op(Op::OpBitwiseAnd, t_u32_, {lane, b_.U32(~3u)}), sel});
        } else {
            // within groups of 32 lanes: ((lane & and) | or) ^ xor
            const u32 and_m = off16 & 0x1F, or_m = (off16 >> 5) & 0x1F, xor_m = (off16 >> 10) & 0x1F;
            const Id low = b_.Op(Op::OpBitwiseXor, t_u32_,
                                 {b_.Op(Op::OpBitwiseOr, t_u32_, {b_.Op(Op::OpBitwiseAnd, t_u32_, {lane, b_.U32(and_m)}), b_.U32(or_m)}),
                                  b_.U32(xor_m)});
            src_lane = b_.Op(Op::OpBitwiseOr, t_u32_, {b_.Op(Op::OpBitwiseAnd, t_u32_, {lane, b_.U32(~31u)}), low});
        }
        Set(vdst, b_.Op(Op::OpGroupNonUniformShuffle, t_u32_, {scope_sg, Get(data0, in), src_lane}));
        return true;
    }
    if (gds || lds_ == 0) {
        return false; // (GDS accesses other than append / consume: not used by the game)
    }
    const auto at = [&](Id byte_addr) {
        const Id dw = b_.Op(Op::OpShiftRightLogical, t_u32_, {byte_addr, b_.U32(2)});
        return b_.Op(Op::OpAccessChain, t_ptr_wg_u32_, {lds_, b_.Op(Op::OpBitwiseAnd, t_u32_, {dw, b_.U32(kLdsDwords - 1)})});
    };
    const Id addr = Get(addr_reg, in);
    const auto plus = [&](u32 bytes) { return bytes == 0 ? addr : b_.Op(Op::OpIAdd, t_u32_, {addr, b_.U32(bytes)}); };
    const auto load = [&](u32 bytes) { return b_.Op(Op::OpLoad, t_u32_, {at(plus(bytes))}); };
    const auto store = [&](u32 bytes, Id v) { b_.OpNoResult(Op::OpStore, {at(plus(bytes)), v}); };
    if (name == "ds_write_b32") {
        store(off16, Get(data0, in));
    } else if (name == "ds_write_b64") {
        store(off16, Get(data0, in));
        store(off16 + 4, Get(data0 + 1, in));
    } else if (name == "ds_write2_b32" || name == "ds_write2st64_b32") {
        const u32 m = name == "ds_write2_b32" ? 4 : 256;
        store(off0 * m, Get(data0, in));
        store(off1 * m, Get(data1, in));
    } else if (name == "ds_write2_b64" || name == "ds_write2st64_b64") {
        const u32 m = name == "ds_write2_b64" ? 8 : 512;
        store(off0 * m, Get(data0, in));
        store(off0 * m + 4, Get(data0 + 1, in));
        store(off1 * m, Get(data1, in));
        store(off1 * m + 4, Get(data1 + 1, in));
    } else if (name == "ds_read_b32") {
        Set(vdst, load(off16));
    } else if (name == "ds_read_b64") {
        const Id a = load(off16), c = load(off16 + 4);
        Set(vdst, a);
        Set(vdst + 1, c);
    } else if (name == "ds_read2_b32" || name == "ds_read2st64_b32") {
        const u32 m = name == "ds_read2_b32" ? 4 : 256;
        const Id a = load(off0 * m), c = load(off1 * m);
        Set(vdst, a);
        Set(vdst + 1, c);
    } else if (name == "ds_read2_b64" || name == "ds_read2st64_b64") {
        const u32 m = name == "ds_read2_b64" ? 8 : 512;
        const Id a = load(off0 * m), a2 = load(off0 * m + 4), c = load(off1 * m), c2 = load(off1 * m + 4);
        Set(vdst, a);
        Set(vdst + 1, a2);
        Set(vdst + 2, c);
        Set(vdst + 3, c2);
    } else {
        static constexpr std::pair<std::string_view, Op> kAtomics[] = {
            {"ds_add_u32", Op::OpAtomicIAdd},     {"ds_sub_u32", Op::OpAtomicISub},     {"ds_min_i32", Op::OpAtomicSMin},
            {"ds_max_i32", Op::OpAtomicSMax},     {"ds_min_u32", Op::OpAtomicUMin},     {"ds_max_u32", Op::OpAtomicUMax},
            {"ds_and_b32", Op::OpAtomicAnd},      {"ds_or_b32", Op::OpAtomicOr},        {"ds_xor_b32", Op::OpAtomicXor},
            {"ds_add_rtn_u32", Op::OpAtomicIAdd}, {"ds_sub_rtn_u32", Op::OpAtomicISub}, {"ds_min_rtn_i32", Op::OpAtomicSMin},
            {"ds_max_rtn_i32", Op::OpAtomicSMax}, {"ds_min_rtn_u32", Op::OpAtomicUMin}, {"ds_max_rtn_u32", Op::OpAtomicUMax},
            {"ds_wrxchg_rtn_b32", Op::OpAtomicExchange},
        };
        for (const auto& [nm, op] : kAtomics) {
            if (name == nm) {
                const Id old = b_.Op(op, t_u32_, {at(plus(off16)), b_.U32(static_cast<u32>(spv::Scope::Workgroup)), b_.U32(0),
                                                  Get(data0, in)});
                if (name.find("_rtn_") != std::string_view::npos) {
                    Set(vdst, old);
                }
                return true;
            }
        }
        return false;
    }
    return true;
}

void Emitter::InitCompute() {
    const Id uvec3 = b_.TypeVector(t_u32_, 3);
    const auto input = [&](spv::BuiltIn builtin, Id type) {
        const Id v = b_.Variable(b_.TypePointer(spv::StorageClass::Input, type), spv::StorageClass::Input);
        b_.Decorate(v, spv::Decoration::BuiltIn, {static_cast<u32>(builtin)});
        interface_.push_back(v);
        return b_.Op(Op::OpLoad, type, {v});
    };
    const u32 rsrc2 = options_.rsrc2;
    const Id tid = input(spv::BuiltIn::LocalInvocationId, uvec3);
    // v0-v2 the thread's x, y, z in the group: all three (the runtime's rsrc2 is Rsrc2TranslationMask's - no
    // TIDIG_COMP_CNT - and a program reads only those it was given)
    for (u32 c = 0; c < 3; ++c) {
        Set(kVgpr + c, b_.Op(Op::OpCompositeExtract, t_u32_, {tid, c}));
    }
    u32 sgpr = (rsrc2 >> 1) & 0x1F; // after the user data
    const Id group = ((rsrc2 >> 7) & 7) != 0 ? input(spv::BuiltIn::WorkgroupId, uvec3) : 0;
    for (u32 c = 0; c < 3; ++c) {
        if (((rsrc2 >> (7 + c)) & 1) != 0) {
            Set(sgpr++, b_.Op(Op::OpCompositeExtract, t_u32_, {group, c}));
        }
    }
    {
        // TG_SIZE after the group ids: the wave's index in the group (bits 0-5), the group's waves (bits 6-11) -
        // always (TG_SIZE_EN is not in the runtime's rsrc2; a program not given it does not read that register)
        const u32 threads = std::max(options_.threads[0], 1u) * std::max(options_.threads[1], 1u) *
                            std::max(options_.threads[2], 1u);
        const u32 waves = (threads + 63) / 64;
        const Id wave = b_.Op(Op::OpShiftRightLogical, t_u32_, {LocalIndex(), b_.U32(6)});
        Set(sgpr++, b_.Op(Op::OpBitwiseOr, t_u32_, {wave, b_.U32(waves << 6)}));
    }
}

void Emitter::InitVertex() {
    const EmitOptions::VertexOptions& vs = options_.vs;
    t_vec4_ = b_.TypeVector(t_f32_, 4);
    t_ptr_out_f32_ = b_.TypePointer(spv::StorageClass::Output, t_f32_);
    const auto input = [&](spv::BuiltIn builtin) {
        const Id v = b_.Variable(b_.TypePointer(spv::StorageClass::Input, t_u32_), spv::StorageClass::Input);
        b_.Decorate(v, spv::Decoration::BuiltIn, {static_cast<u32>(builtin)});
        interface_.push_back(v);
        return b_.Op(Op::OpLoad, t_u32_, {v});
    };
    Id vertex = input(spv::BuiltIn::VertexIndex);
    Id instance = input(spv::BuiltIn::InstanceIndex);
    if (vs.base_vertex_sgpr >= 0 || vs.base_instance_sgpr >= 0) {
        b_.Capability(spv::Capability::DrawParameters);
    }
    // an indirect draw's base vertex / start instance: the SGPRs the GPU writes them to; the vertex / instance index
    // without them unless the fetch shader adds that SGPR (VertexIndex / InstanceIndex include them - the bridge's)
    if (vs.base_vertex_sgpr >= 0) {
        const Id base = input(spv::BuiltIn::BaseVertex);
        Set(static_cast<u32>(vs.base_vertex_sgpr), base);
        if (!vs.fetch_adds_base_vertex) {
            vertex = b_.Op(Op::OpISub, t_u32_, {vertex, base});
        }
    }
    if (vs.base_instance_sgpr >= 0) {
        const Id base = input(spv::BuiltIn::BaseInstance);
        Set(static_cast<u32>(vs.base_instance_sgpr), base);
        if (!vs.fetch_adds_base_instance) {
            instance = b_.Op(Op::OpISub, t_u32_, {instance, base});
        }
    }
    // v0 the vertex index; VGPR_COMP_CNT: v1 the instance over step rate 0, v2 over step rate 1, v3 the instance
    Set(kVgpr, vertex);
    const auto stepped = [&](u32 rate) { return rate != 0 ? b_.Op(Op::OpUDiv, t_u32_, {instance, b_.U32(rate)}) : b_.U32(0); };
    if (vs.input_vgprs > 0) {
        Set(kVgpr + 1, stepped(vs.step_rate0));
    }
    if (vs.input_vgprs > 1) {
        Set(kVgpr + 2, stepped(vs.step_rate1));
    }
    if (vs.input_vgprs > 2) {
        Set(kVgpr + 3, instance);
    }
    // the fetch shader's attributes: location i, the format's number class
    for (u32 i = 0; i < vs.inputs.size(); ++i) {
        const Id scalar = vs.inputs[i].kind == 0 ? t_f32_ : (vs.inputs[i].kind == 1 ? t_u32_ : t_i32_);
        const Id v = b_.Variable(b_.TypePointer(spv::StorageClass::Input, b_.TypeVector(scalar, 4)),
                                 spv::StorageClass::Input);
        b_.Decorate(v, spv::Decoration::Location, {i});
        interface_.push_back(v);
        vinputs_.push_back(v);
    }
    position_ = b_.Variable(b_.TypePointer(spv::StorageClass::Output, t_vec4_), spv::StorageClass::Output);
    b_.Decorate(position_, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::Position)});
    interface_.push_back(position_);
}

void Emitter::EmitFetch() {
    const EmitOptions::VertexOptions& vs = options_.vs;
    for (u32 i = 0; i < vs.inputs.size(); ++i) {
        const EmitOptions::VertexInput& a = vs.inputs[i];
        const Id scalar = a.kind == 0 ? t_f32_ : (a.kind == 1 ? t_u32_ : t_i32_);
        const Id value = b_.Op(Op::OpLoad, b_.TypeVector(scalar, 4), {vinputs_[i]});
        // the number conversion of the V#'s format (the bridge's ApplyReadNumberConversion): the register's bits
        const bool float_result = a.kind == 0 || a.conversion != 0;
        std::array<Id, 4> comp{};
        for (u32 c = 0; c < 4; ++c) {
            const Id x = b_.Op(Op::OpCompositeExtract, scalar, {value, c});
            Id f = 0;
            switch (a.conversion) {
            case 0:
                comp[c] = scalar == t_u32_ ? x : Bit(t_u32_, x);
                continue;
            case 1:
                f = b_.Op(Op::OpConvertUToF, t_f32_, {x});
                break;
            case 2:
                f = b_.Op(Op::OpConvertSToF, t_f32_, {x});
                break;
            case 3:
                f = b_.Op(Op::OpFSub, t_f32_, {b_.Op(Op::OpFMul, t_f32_, {x, b_.F32(2.0f)}), b_.F32(1.0f)});
                break;
            case 4:
            case 5: {
                const Id s = Bit(t_u32_, x);
                const Id odd = b_.Op(Op::OpIAdd, t_u32_, {b_.Op(Op::OpIMul, t_u32_, {s, b_.U32(2)}), b_.U32(1)});
                f = b_.Op(Op::OpFDiv, t_f32_, {b_.Op(Op::OpConvertSToF, t_f32_, {Bit(t_i32_, odd)}),
                                               b_.F32(a.conversion == 4 ? 255.0f : 65535.0f)});
                break;
            }
            case 6:
                f = b_.Op(Op::OpFDiv, t_f32_, {b_.Op(Op::OpConvertUToF, t_f32_, {x}), b_.F32(4294967295.0f)});
                break;
            default:
                throw Unsupported{fmt::format("vertex attribute conversion {}", a.conversion)};
            }
            comp[c] = Bit(t_u32_, f);
        }
        // DST_SEL (0 zero, 1 one, 4-7 x-w), then the registers the load wrote
        for (u32 j = 0; j < a.num_elements && j < 4; ++j) {
            const u32 sel = a.swizzle[j];
            const Id v = sel >= 4 ? comp[sel - 4]
                                  : (sel == 1 ? b_.U32(float_result ? std::bit_cast<u32>(1.0f) : 1u) : b_.U32(0));
            Set(kVgpr + a.dest_vgpr + j, v);
        }
    }
}

void Emitter::EmitExport(const Inst& in) {
    const u32 en = in.w0 & 0xF, target = (in.w0 >> 4) & 0x3F;
    const bool compr = ((in.w0 >> 10) & 1) != 0;
    const auto vsrc = [&](u32 c) { return GetF(kVgpr + ((in.w1 >> (8 * c)) & 0xFF), in); };
    if (compr) {
        throw Unsupported{fmt::format("exp to target {} compressed (a vertex program)", target)};
    }
    if (target == 12) {
        for (u32 c = 0; c < 4; ++c) {
            if (((en >> c) & 1) != 0) {
                b_.OpNoResult(Op::OpStore, {OutputComponent(position_, c), vsrc(c)});
            }
        }
        return;
    }
    if (target >= 13 && target <= 15) {
        // POS1..3: what PA_CL_VS_OUT_CNTL makes of them (the bridge's MapOutputs / ExportPosition)
        for (u32 c = 0; c < 4; ++c) {
            if (((en >> c) & 1) == 0) {
                continue;
            }
            const u32 out = options_.vs.pos_outputs[target - 13][c];
            if (out == 0) {
                continue; // (not mapped: the bridge skips it too)
            }
            const auto array8 = [&](spv::BuiltIn builtin) {
                const Id type = b_.TypeArray(t_f32_, b_.U32(8));
                const std::array<Id, 8> zero{b_.F32(0), b_.F32(0), b_.F32(0), b_.F32(0),
                                             b_.F32(0), b_.F32(0), b_.F32(0), b_.F32(0)};
                const Id v = b_.Variable(b_.TypePointer(spv::StorageClass::Output, type), spv::StorageClass::Output,
                                         b_.ConstantComposite(type, zero));
                b_.Decorate(v, spv::Decoration::BuiltIn, {static_cast<u32>(builtin)});
                interface_.push_back(v);
                return v;
            };
            if (out == 1) {
                if (point_size_ == 0) {
                    point_size_ = b_.Variable(t_ptr_out_f32_, spv::StorageClass::Output);
                    b_.Decorate(point_size_, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::PointSize)});
                    interface_.push_back(point_size_);
                }
                b_.OpNoResult(Op::OpStore, {point_size_, vsrc(c)});
            } else if (out >= 2 && out < 10) {
                if (clip_dist_ == 0) {
                    b_.Capability(spv::Capability::ClipDistance);
                    clip_dist_ = array8(spv::BuiltIn::ClipDistance);
                }
                b_.OpNoResult(Op::OpStore, {OutputComponent(clip_dist_, out - 2), vsrc(c)});
            } else if (out >= 10 && out < 18) {
                if (cull_dist_ == 0) {
                    b_.Capability(spv::Capability::CullDistance);
                    cull_dist_ = array8(spv::BuiltIn::CullDistance);
                }
                b_.OpNoResult(Op::OpStore, {OutputComponent(cull_dist_, out - 10), vsrc(c)});
            } else {
                throw Unsupported{fmt::format("exp to POS{} component {} (PA_CL_VS_OUT_CNTL output {})", target - 12, c,
                                              out)};
            }
        }
        return;
    }
    if (target >= 32 && target < 64) {
        // PARAMn: an output vec4 at location n (the fragment program's SPI_PS_INPUT_CNTL offsets name it)
        Id& p = params_[target - 32];
        if (p == 0) {
            p = b_.Variable(b_.TypePointer(spv::StorageClass::Output, t_vec4_), spv::StorageClass::Output);
            b_.Decorate(p, spv::Decoration::Location, {target - 32});
            interface_.push_back(p);
        }
        for (u32 c = 0; c < 4; ++c) {
            if (((en >> c) & 1) != 0) {
                b_.OpNoResult(Op::OpStore, {OutputComponent(p, c), vsrc(c)});
            }
        }
        return;
    }
    throw Unsupported{fmt::format("exp to target {} (a vertex program)", target)};
}

void Emitter::VertexEpilogue() {
    const EmitOptions::VertexOptions& vs = options_.vs;
    if (!vs.depth_minus_one_to_one && !vs.clip_space) {
        return;
    }
    Id pos = b_.Op(Op::OpLoad, t_vec4_, {position_});
    Id x = b_.Op(Op::OpCompositeExtract, t_f32_, {pos, 0}), y = b_.Op(Op::OpCompositeExtract, t_f32_, {pos, 1});
    Id z = b_.Op(Op::OpCompositeExtract, t_f32_, {pos, 2});
    const Id w = b_.Op(Op::OpCompositeExtract, t_f32_, {pos, 3});
    if (vs.depth_minus_one_to_one) {
        // [-w, w] to [0, w] (the bridge's ConvertDepthMode)
        z = b_.Op(Op::OpFMul, t_f32_, {b_.Op(Op::OpFAdd, t_f32_, {z, w}), b_.F32(0.5f)});
    }
    if (vs.clip_space) {
        // a screen-space position to the clip space of the device's largest viewport (ConvertPositionToClipSpace;
        // PushData's xoffset, yoffset, xscale, yscale)
        const Id ptr = b_.TypePointer(spv::StorageClass::PushConstant, t_f32_);
        const auto pd = [&](u32 k) {
            return b_.Op(Op::OpLoad, t_f32_, {b_.Op(Op::OpAccessChain, ptr, {push_, b_.U32(0), b_.U32(k)})});
        };
        const Id wx = b_.Op(Op::OpFAdd, t_f32_, {b_.Op(Op::OpFMul, t_f32_, {x, pd(2)}), pd(0)});
        x = b_.Op(Op::OpFSub, t_f32_, {b_.Op(Op::OpFDiv, t_f32_, {wx, b_.F32(vs.viewport_w)}), b_.F32(1.0f)});
        const Id wy = b_.Op(Op::OpFAdd, t_f32_, {b_.Op(Op::OpFMul, t_f32_, {y, pd(3)}), pd(1)});
        y = b_.Op(Op::OpFSub, t_f32_, {b_.Op(Op::OpFDiv, t_f32_, {wy, b_.F32(vs.viewport_h)}), b_.F32(1.0f)});
    }
    pos = b_.Op(Op::OpCompositeConstruct, t_vec4_, {x, y, z, w});
    b_.OpNoResult(Op::OpStore, {position_, pos});
}

void Emitter::Float64() {
    if (t_f64_ == 0) {
        b_.Capability(spv::Capability::Float64);
        t_f64_ = b_.TypeFloat(64);
        t_uvec2_ = b_.TypeVector(t_u32_, 2);
    }
}

Id Emitter::GetF64(u32 code, const Inst& in, u32 i) {
    Float64();
    Id v = 0;
    if (code <= 107 || code == kM0 || code >= kVgpr) {
        v = b_.Op(Op::OpBitcast, t_f64_,
                  {b_.Op(Op::OpCompositeConstruct, t_uvec2_, {Get(code, in), Get(code + 1, in)})});
    } else if (code >= 240 && code <= 247) {
        static constexpr double kF[8] = {0.5, -0.5, 1.0, -1.0, 2.0, -2.0, 4.0, -4.0};
        const u64 bits = std::bit_cast<u64>(kF[code - 240]);
        v = b_.Op(Op::OpBitcast, t_f64_,
                  {b_.Op(Op::OpCompositeConstruct, t_uvec2_, {b_.U32(static_cast<u32>(bits)), b_.U32(static_cast<u32>(bits >> 32))})});
    } else if (code == kLiteralOperand) {
        // a 64-bit float literal: its high 32 bits
        v = b_.Op(Op::OpBitcast, t_f64_, {b_.Op(Op::OpCompositeConstruct, t_uvec2_, {b_.U32(0), b_.U32(in.lit)})});
    } else if (const auto c = ConstOperand(code, in)) {
        v = b_.Op(Op::OpConvertSToF, t_f64_, {b_.Constant(t_i32_, static_cast<u32>(*c))}); // (an integer constant)
    } else {
        throw Unsupported{fmt::format("a 64-bit float source of code {}", code)};
    }
    if (in.format == Format::Vop3) {
        if ((in.w0 >> (8 + i)) & 1) {
            v = Glsl(GLSLstd450FAbs, t_f64_, {v});
        }
        if ((in.w1 >> (29 + i)) & 1) {
            v = b_.Op(Op::OpFNegate, t_f64_, {v});
        }
    }
    return v;
}

void Emitter::SetF64(const Inst& in, u32 code, Id v) {
    if (in.format == Format::Vop3) {
        static constexpr double kOmod[4] = {1.0, 2.0, 4.0, 0.5};
        const u32 omod = (in.w1 >> 27) & 3;
        if (omod != 0) {
            const u64 bits = std::bit_cast<u64>(kOmod[omod]);
            const Id k = b_.Op(Op::OpBitcast, t_f64_,
                               {b_.Op(Op::OpCompositeConstruct, t_uvec2_, {b_.U32(static_cast<u32>(bits)), b_.U32(static_cast<u32>(bits >> 32))})});
            v = b_.Op(Op::OpFMul, t_f64_, {v, k});
        }
        if ((in.w0 >> 11) & 1) {
            const Id zero = b_.Op(Op::OpConvertSToF, t_f64_, {b_.Constant(t_i32_, 0)});
            const Id one = b_.Op(Op::OpConvertSToF, t_f64_, {b_.Constant(t_i32_, 1)});
            v = Glsl(GLSLstd450NClamp, t_f64_, {v, zero, one});
        }
    }
    const Id pair = b_.Op(Op::OpBitcast, t_uvec2_, {v});
    Set(code, b_.Op(Op::OpCompositeExtract, t_u32_, {pair, 0}));
    Set(code + 1, b_.Op(Op::OpCompositeExtract, t_u32_, {pair, 1}));
}

Id Emitter::LocalIndex() {
    if (local_index_ == 0) {
        local_index_ = b_.Variable(b_.TypePointer(spv::StorageClass::Input, t_u32_), spv::StorageClass::Input);
        b_.Decorate(local_index_, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::LocalInvocationIndex)});
        interface_.push_back(local_index_);
    }
    return b_.Op(Op::OpLoad, t_u32_, {local_index_});
}

std::optional<u32> Emitter::ConstOperand(u32 code, const Inst& in) {
    if (code == 128) {
        return 0u;
    }
    if (code >= 129 && code <= 192) {
        return code - 128;
    }
    if (code >= 193 && code <= 208) {
        return static_cast<u32>(-static_cast<s32>(code - 192));
    }
    if (code == kLiteralOperand) {
        return in.lit;
    }
    return std::nullopt;
}

Id Emitter::LaneId() {
    if (lane_id_ == 0) {
        lane_id_ = b_.Variable(b_.TypePointer(spv::StorageClass::Input, t_u32_), spv::StorageClass::Input);
        b_.Decorate(lane_id_, spv::Decoration::BuiltIn, {static_cast<u32>(spv::BuiltIn::SubgroupLocalInvocationId)});
        interface_.push_back(lane_id_);
    }
    return b_.Op(Op::OpLoad, t_u32_, {lane_id_});
}

Id Emitter::Glsl(GLSLstd450 f, Id type, std::initializer_list<u32> args) {
    std::vector<u32> ops{glsl_, static_cast<u32>(f)};
    ops.insert(ops.end(), args.begin(), args.end());
    return b_.Op(Op::OpExtInst, type, std::span<const u32>(ops));
}

// ---------------------------------------------------------------- instructions

bool Emitter::IsVector(const Inst& in) const {
    switch (in.format) {
    case Format::Vop1:
        return in.op != 2; // v_readfirstlane_b32 ignores EXEC
    case Format::Vop2:
        return in.op != 1 && in.op != 2; // v_readlane / v_writelane
    case Format::Vop3:
        return in.op != 257 && in.op != 258 && in.op != 386; // (their VOP3 forms)
    case Format::Vopc:
    case Format::Vintrp:
    case Format::Ds:
    case Format::Mubuf:
    case Format::Mtbuf:
    case Format::Mimg:
    case Format::Exp:
        return true;
    default:
        return false;
    }
}

bool Emitter::WritesExec(const Inst& in) const {
    const std::string_view name = Mnemonic(in.format, in.op);
    if (name.starts_with("v_cmpx") || name.find("saveexec") != std::string_view::npos) {
        return true;
    }
    if (in.format != Format::Sop1 && in.format != Format::Sop2 && in.format != Format::Sopk &&
        in.format != Format::Vop3) {
        return false;
    }
    const Operands o = GetOperands(in);
    return o.num_dst > 0 && (o.dst[0] == kExecLo || o.dst[0] == kExecHi);
}

void Emitter::Emit(const Inst& in) {
    const std::string_view name = Mnemonic(in.format, in.op);
    if (name.empty()) {
        throw Unsupported{fmt::format("{} #{}", FormatName(in.format), in.op)};
    }
    switch (in.format) {
    case Format::Sop1:
    case Format::Sop2:
    case Format::Sopk:
    case Format::Sopc:
    case Format::Sopp:
        EmitScalar(in, name);
        return;
    case Format::Vop1:
    case Format::Vop2:
    case Format::Vop3:
    case Format::Vopc:
        EmitVector(in, name);
        return;
    case Format::Smrd:
        if (res_.buffer_of[cur_index_] >= 0 ? BufferKnown(res_.buffer_of[cur_index_]) : res_.load_of[cur_index_] >= 0) {
            EmitSmrd(in, cur_index_, name);
            return;
        }
        if (!options_.stub_memory) {
            throw Unsupported{fmt::format("{} (resource: {})", name, res_.error)};
        }
        EmitStub(in, name);
        return;
    case Format::Mimg:
        if (EmitImage(in, cur_index_, name)) {
            return;
        }
        if (!options_.stub_memory) {
            throw Unsupported{std::string(name) + decline_};
        }
        EmitStub(in, name);
        return;
    case Format::Ds:
        if (EmitDs(in, name)) {
            return;
        }
        if (!options_.stub_memory) {
            throw Unsupported{std::string(name) + decline_};
        }
        EmitStub(in, name);
        return;
    case Format::Mtbuf:
        if (res_.buffer_of[cur_index_] >= 0 && BufferKnown(res_.buffer_of[cur_index_]) &&
            EmitTbuffer(in, cur_index_, name)) {
            return;
        }
        if (!options_.stub_memory) {
            throw Unsupported{std::string(name) + decline_};
        }
        EmitStub(in, name);
        return;
    case Format::Exp:
        if (options_.vertex) {
            EmitExport(in);
            return;
        }
        if (options_.stub_memory) {
            EmitStub(in, name);
            return;
        }
        throw Unsupported{std::string(name)};
    case Format::Mubuf:
        if (res_.buffer_of[cur_index_] >= 0 && BufferKnown(res_.buffer_of[cur_index_]) &&
            (name.starts_with("buffer_load_format_") || name.starts_with("buffer_store_format_")
                 ? EmitFormattedBuffer(in, cur_index_, name)
                 : EmitBuffer(in, cur_index_, name))) {
            return;
        }
        if (!options_.stub_memory) {
            throw Unsupported{std::string(name) + decline_};
        }
        EmitStub(in, name);
        return;
    default:
        if (options_.stub_memory) {
            EmitStub(in, name);
            return;
        }
        throw Unsupported{std::string(name)};
    }
}

void Emitter::EmitStub(const Inst& in, std::string_view name) {
    // tests only: what a load would write gets zeros
    const u32 w0 = in.w0, w1 = in.w1;
    u32 first = 0, count = 0;
    const auto width = [&]() -> u32 {
        if (name.ends_with("x16")) {
            return 16;
        }
        if (name.ends_with("x8")) {
            return 8;
        }
        if (name.ends_with("xyzw") || name.ends_with("x4")) {
            return 4;
        }
        if (name.ends_with("xyz") || name.ends_with("x3")) {
            return 3;
        }
        if (name.ends_with("xy") || name.ends_with("x2")) {
            return 2;
        }
        return 1;
    };
    if (in.format == Format::Smrd) {
        first = (w0 >> 15) & 0x7F;
        count = width();
    } else if ((in.format == Format::Mubuf || in.format == Format::Mtbuf) &&
               name.find("store") == std::string_view::npos) {
        first = kVgpr + ((w1 >> 8) & 0xFF);
        count = width();
    } else if (in.format == Format::Mimg) {
        first = kVgpr + ((w1 >> 8) & 0xFF);
        count = static_cast<u32>(std::popcount((w0 >> 8) & 0xFu));
    } else if (in.format == Format::Ds && name.find("write") == std::string_view::npos) {
        first = kVgpr + ((w1 >> 24) & 0xFF);
        count = name.find("2") != std::string_view::npos || name.find("b64") != std::string_view::npos ? 2 : 1;
    } else if (in.format == Format::Vintrp) {
        first = kVgpr + ((w0 >> 18) & 0xFF);
        count = 1;
    }
    for (u32 i = 0; i < count; ++i) {
        Set(first + i, b_.U32(0));
    }
}

void Emitter::EmitScalar(const Inst& in, std::string_view name) {
    const Operands o = GetOperands(in);
    const auto src = [&](u32 i) { return Get(o.src[i], in); };
    const auto src64 = [&](u32 i) { return Src64(o.src[i], in); };
    const auto set64 = [&](u32 code, std::pair<Id, Id> v) {
        if (code == kExecLo) {
            b_.OpNoResult(Op::OpStore, {exec_, LaneBit(v.first, v.second)});
            return;
        }
        Set(code, v.first);
        Set(code + 1, v.second);
    };
    const auto nz32 = [&](Id v) { return b_.Op(Op::OpINotEqual, t_bool_, {v, b_.U32(0)}); };
    const auto nz64 = [&](std::pair<Id, Id> v) { return LaneBit(v.first, v.second); }; // (the oracle's SCC of a mask)
    const auto simm = [&]() { return b_.U32(static_cast<u32>(static_cast<s32>(static_cast<s16>(in.w0 & 0xFFFF)))); };
    if (options_.vertex && name == "s_swappc_b64") {
        if (!options_.vs.fetch) {
            throw Unsupported{"s_swappc_b64 (no fetch shader parsed)"};
        }
        EmitFetch(); // (its return address in SDST: nothing reads it)
        return;
    }
    if (options_.stub_memory && (name == "s_swappc_b64" || name == "s_getpc_b64")) {
        return; // (tests: the fetch shader's call)
    }
    if (in.format == Format::Sopp) {
        if (name == "s_barrier") {
            b_.OpNoResult(Op::OpControlBarrier,
                          {b_.U32(static_cast<u32>(spv::Scope::Workgroup)), b_.U32(static_cast<u32>(spv::Scope::Workgroup)),
                           b_.U32(static_cast<u32>(spv::MemorySemanticsMask::AcquireRelease) |
                                  static_cast<u32>(spv::MemorySemanticsMask::WorkgroupMemory))});
            return;
        }
        if (name == "s_waitcnt" || name == "s_nop" || name == "s_endpgm" || name.starts_with("s_cbranch") ||
            name == "s_branch" || name == "s_setprio" || name == "s_sleep" || name == "s_ttracedata" ||
            name == "s_icache_inv" || name == "s_incperflevel" || name == "s_decperflevel") {
            return; // (no effect, or the graph's)
        }
        throw Unsupported{std::string(name)};
    }
    struct Bin {
        std::string_view name;
        Op op;
        bool scc_nonzero;
        bool shift;
    };
    static constexpr Bin kBin[] = {
        {"s_and_b32", Op::OpBitwiseAnd, true, false},
        {"s_or_b32", Op::OpBitwiseOr, true, false},
        {"s_xor_b32", Op::OpBitwiseXor, true, false},
        {"s_lshl_b32", Op::OpShiftLeftLogical, true, true},
        {"s_lshr_b32", Op::OpShiftRightLogical, true, true},
        {"s_mul_i32", Op::OpIMul, false, false},
    };
    for (const Bin& bn : kBin) {
        if (name != bn.name) {
            continue;
        }
        const Id b1 = bn.shift ? b_.Op(Op::OpBitwiseAnd, t_u32_, {src(1), b_.U32(31)}) : src(1);
        const Id r = b_.Op(bn.op, t_u32_, {src(0), b1});
        Set(o.dst[0], r);
        if (bn.scc_nonzero) {
            SetScc(nz32(r));
        }
        return;
    }
    if (name == "s_ashr_i32") {
        const Id r = Bit(t_u32_, b_.Op(Op::OpShiftRightArithmetic, t_i32_,
                                       {Bit(t_i32_, src(0)), b_.Op(Op::OpBitwiseAnd, t_u32_, {src(1), b_.U32(31)})}));
        Set(o.dst[0], r);
        SetScc(nz32(r));
        return;
    }
    if (name == "s_andn2_b32" || name == "s_orn2_b32") {
        const Id n1 = b_.Op(Op::OpNot, t_u32_, {src(1)});
        const Id r = b_.Op(name == "s_andn2_b32" ? Op::OpBitwiseAnd : Op::OpBitwiseOr, t_u32_, {src(0), n1});
        Set(o.dst[0], r);
        SetScc(nz32(r));
        return;
    }
    if (name == "s_add_u32" || name == "s_add_i32" || name == "s_sub_u32" || name == "s_sub_i32") {
        const bool add = name.starts_with("s_add");
        const Id a = src(0), c = src(1);
        const Id r = b_.Op(add ? Op::OpIAdd : Op::OpISub, t_u32_, {a, c});
        Set(o.dst[0], r);
        if (name.ends_with("u32")) {
            SetScc(add ? b_.Op(Op::OpULessThan, t_bool_, {r, a}) : b_.Op(Op::OpULessThan, t_bool_, {a, c}));
        } else {
            const Id sa = b_.Op(Op::OpShiftRightLogical, t_u32_, {a, b_.U32(31)});
            const Id sc = b_.Op(Op::OpShiftRightLogical, t_u32_, {c, b_.U32(31)});
            const Id sr = b_.Op(Op::OpShiftRightLogical, t_u32_, {r, b_.U32(31)});
            const Id same = b_.Op(add ? Op::OpIEqual : Op::OpINotEqual, t_bool_, {sa, sc});
            SetScc(And(same, b_.Op(Op::OpINotEqual, t_bool_, {sr, sa})));
        }
        return;
    }
    if (name == "s_addc_u32" || name == "s_subb_u32") {
        const bool add = name == "s_addc_u32";
        const Id cin = Scc();
        const Id a = src(0), c = src(1);
        const Id r = b_.Op(add ? Op::OpIAdd : Op::OpISub, t_u32_,
                           {b_.Op(add ? Op::OpIAdd : Op::OpISub, t_u32_, {a, c}), SelBool(cin)});
        Set(o.dst[0], r);
        SetScc(add ? Or(b_.Op(Op::OpULessThan, t_bool_, {r, a}), And(cin, b_.Op(Op::OpIEqual, t_bool_, {r, a})))
                   : Or(b_.Op(Op::OpULessThan, t_bool_, {a, c}), And(cin, b_.Op(Op::OpIEqual, t_bool_, {a, c}))));
        return;
    }
    if (name == "s_min_i32" || name == "s_max_i32" || name == "s_min_u32" || name == "s_max_u32") {
        const bool is_signed = name.ends_with("i32");
        const bool is_min = name.starts_with("s_min");
        const Id a = src(0), c = src(1);
        const Id lt = is_signed ? b_.Op(Op::OpSLessThan, t_bool_, {Bit(t_i32_, a), Bit(t_i32_, c)})
                                : b_.Op(Op::OpULessThan, t_bool_, {a, c});
        Set(o.dst[0], is_min ? Select(t_u32_, lt, a, c) : Select(t_u32_, lt, c, a));
        SetScc(is_min ? lt : Not(lt)); // (SCC: src0 was chosen)
        return;
    }
    if (name == "s_cselect_b32") {
        Set(o.dst[0], Select(t_u32_, Scc(), src(0), src(1)));
        return;
    }
    if (name == "s_cselect_b64") {
        const auto a = src64(0), c = src64(1);
        set64(o.dst[0], {Select(t_u32_, Scc(), a.first, c.first), Select(t_u32_, Scc(), a.second, c.second)});
        return;
    }
    if (name == "s_bfe_u32" || name == "s_bfe_i32") {
        const Id c = src(1);
        const Id off = b_.Op(Op::OpBitwiseAnd, t_u32_, {c, b_.U32(31)});
        const Id cnt = b_.Op(Op::OpBitwiseAnd, t_u32_, {b_.Op(Op::OpShiftRightLogical, t_u32_, {c, b_.U32(16)}), b_.U32(0x7F)});
        const Id r = name == "s_bfe_u32"
                         ? b_.Op(Op::OpBitFieldUExtract, t_u32_, {src(0), off, cnt})
                         : Bit(t_u32_, b_.Op(Op::OpBitFieldSExtract, t_i32_, {Bit(t_i32_, src(0)), off, cnt}));
        Set(o.dst[0], r);
        SetScc(nz32(r));
        return;
    }
    if (name == "s_bfm_b32") {
        const Id cnt = b_.Op(Op::OpBitwiseAnd, t_u32_, {src(0), b_.U32(31)});
        const Id off = b_.Op(Op::OpBitwiseAnd, t_u32_, {src(1), b_.U32(31)});
        const Id mask = b_.Op(Op::OpISub, t_u32_, {b_.Op(Op::OpShiftLeftLogical, t_u32_, {b_.U32(1), cnt}), b_.U32(1)});
        Set(o.dst[0], b_.Op(Op::OpShiftLeftLogical, t_u32_, {mask, off}));
        return;
    }
    struct Bin64 {
        std::string_view name;
        Op op;
        bool not1, notr;
    };
    static constexpr Bin64 kBin64[] = {
        {"s_and_b64", Op::OpBitwiseAnd, false, false}, {"s_or_b64", Op::OpBitwiseOr, false, false},
        {"s_xor_b64", Op::OpBitwiseXor, false, false}, {"s_andn2_b64", Op::OpBitwiseAnd, true, false},
        {"s_orn2_b64", Op::OpBitwiseOr, true, false},  {"s_nand_b64", Op::OpBitwiseAnd, false, true},
        {"s_nor_b64", Op::OpBitwiseOr, false, true},   {"s_xnor_b64", Op::OpBitwiseXor, false, true},
    };
    for (const Bin64& bn : kBin64) {
        if (name != bn.name) {
            continue;
        }
        const auto a = src64(0);
        auto c = src64(1);
        if (bn.not1) {
            c = {b_.Op(Op::OpNot, t_u32_, {c.first}), b_.Op(Op::OpNot, t_u32_, {c.second})};
        }
        std::pair<Id, Id> r{b_.Op(bn.op, t_u32_, {a.first, c.first}), b_.Op(bn.op, t_u32_, {a.second, c.second})};
        if (bn.notr) {
            r = {b_.Op(Op::OpNot, t_u32_, {r.first}), b_.Op(Op::OpNot, t_u32_, {r.second})};
        }
        SetScc(nz64(r));
        set64(o.dst[0], r);
        return;
    }
    if (name == "s_not_b64") {
        const auto a = src64(0);
        const std::pair r{b_.Op(Op::OpNot, t_u32_, {a.first}), b_.Op(Op::OpNot, t_u32_, {a.second})};
        SetScc(nz64(r));
        set64(o.dst[0], r);
        return;
    }
    if (name == "s_not_b32") {
        const Id r = b_.Op(Op::OpNot, t_u32_, {src(0)});
        Set(o.dst[0], r);
        SetScc(nz32(r));
        return;
    }
    if (name == "s_mov_b32") {
        Set(o.dst[0], src(0));
        return;
    }
    if (name == "s_movk_i32") {
        Set(o.dst[0], simm());
        return;
    }
    if (name == "s_mov_b64") {
        set64(o.dst[0], src64(0));
        return;
    }
    if (name == "s_wqm_b64") {
        // whole quad mode: the oracle keeps the mask (helper lanes run the fragment program anyway)
        const auto a = src64(0);
        set64(o.dst[0], a);
        SetScc(nz64(a));
        return;
    }
    if (name == "s_brev_b32") {
        Set(o.dst[0], b_.Op(Op::OpBitReverse, t_u32_, {src(0)}));
        return;
    }
    if (name == "s_bcnt1_i32_b32") {
        const Id r = b_.Op(Op::OpBitCount, t_u32_, {src(0)});
        Set(o.dst[0], r);
        SetScc(nz32(r));
        return;
    }
    if (name == "s_bcnt1_i32_b64") {
        const auto a = src64(0);
        const Id r = b_.Op(Op::OpIAdd, t_u32_,
                           {b_.Op(Op::OpBitCount, t_u32_, {a.first}), b_.Op(Op::OpBitCount, t_u32_, {a.second})});
        Set(o.dst[0], r);
        SetScc(nz32(r));
        return;
    }
    if (name == "s_ff1_i32_b32") {
        Set(o.dst[0], Bit(t_u32_, Glsl(GLSLstd450FindILsb, t_i32_, {src(0)})));
        return;
    }
    if (name == "s_ff1_i32_b64") {
        const auto a = src64(0);
        const Id lo = Bit(t_u32_, Glsl(GLSLstd450FindILsb, t_i32_, {a.first}));
        const Id hi = Bit(t_u32_, Glsl(GLSLstd450FindILsb, t_i32_, {a.second}));
        const Id hi_pos = Select(t_u32_, b_.Op(Op::OpINotEqual, t_bool_, {hi, b_.U32(~0u)}),
                                 b_.Op(Op::OpIAdd, t_u32_, {hi, b_.U32(32)}), b_.U32(~0u));
        Set(o.dst[0], Select(t_u32_, b_.Op(Op::OpINotEqual, t_bool_, {a.first, b_.U32(0)}), lo, hi_pos));
        return;
    }
    if (name.ends_with("saveexec_b64")) {
        const auto saved = Ballot(Exec());
        const auto s = src64(0);
        set64(o.dst[0], saved);
        const Id e = Exec();
        const Id sb = LaneBit(s.first, s.second);
        Id r = 0;
        if (name == "s_and_saveexec_b64") {
            r = And(e, sb);
        } else if (name == "s_or_saveexec_b64") {
            r = Or(e, sb);
        } else if (name == "s_andn2_saveexec_b64") {
            r = And(sb, Not(e));
        } else {
            throw Unsupported{std::string(name)};
        }
        b_.OpNoResult(Op::OpStore, {exec_, r});
        SetScc(r);
        return;
    }
    if (in.format == Format::Sopc) {
        static constexpr std::pair<std::string_view, Op> kCmp[] = {
            {"s_cmp_eq_i32", Op::OpIEqual},           {"s_cmp_lg_i32", Op::OpINotEqual},
            {"s_cmp_gt_i32", Op::OpSGreaterThan},     {"s_cmp_ge_i32", Op::OpSGreaterThanEqual},
            {"s_cmp_lt_i32", Op::OpSLessThan},        {"s_cmp_le_i32", Op::OpSLessThanEqual},
            {"s_cmp_eq_u32", Op::OpIEqual},           {"s_cmp_lg_u32", Op::OpINotEqual},
            {"s_cmp_gt_u32", Op::OpUGreaterThan},     {"s_cmp_ge_u32", Op::OpUGreaterThanEqual},
            {"s_cmp_lt_u32", Op::OpULessThan},        {"s_cmp_le_u32", Op::OpULessThanEqual},
        };
        for (const auto& [n, op] : kCmp) {
            if (name == n) {
                const bool s = n.ends_with("i32") && op != Op::OpIEqual && op != Op::OpINotEqual;
                SetScc(s ? b_.Op(op, t_bool_, {Bit(t_i32_, src(0)), Bit(t_i32_, src(1))})
                         : b_.Op(op, t_bool_, {src(0), src(1)}));
                return;
            }
        }
        if (name == "s_bitcmp0_b32" || name == "s_bitcmp1_b32") {
            const Id sh = b_.Op(Op::OpShiftRightLogical, t_u32_,
                                {src(0), b_.Op(Op::OpBitwiseAnd, t_u32_, {src(1), b_.U32(31)})});
            const Id bit = b_.Op(Op::OpBitwiseAnd, t_u32_, {sh, b_.U32(1)});
            SetScc(b_.Op(name == "s_bitcmp1_b32" ? Op::OpINotEqual : Op::OpIEqual, t_bool_, {bit, b_.U32(0)}));
            return;
        }
    }
    if (name == "s_cmpk_eq_i32" || name == "s_cmpk_lg_i32") {
        SetScc(b_.Op(name == "s_cmpk_eq_i32" ? Op::OpIEqual : Op::OpINotEqual, t_bool_, {Get(o.dst[0], in), simm()}));
        return;
    }
    if (name == "s_addk_i32" || name == "s_mulk_i32") {
        Set(o.dst[0], b_.Op(name == "s_addk_i32" ? Op::OpIAdd : Op::OpIMul, t_u32_, {Get(o.dst[0], in), simm()}));
        return;
    }
    throw Unsupported{std::string(name)};
}

Id Emitter::VSrcF(const Inst& in, u32 i, u16 code) {
    Id v = GetF(code, in);
    if (in.format == Format::Vop3) {
        if ((in.w0 >> (8 + i)) & 1) {
            v = Glsl(GLSLstd450FAbs, t_f32_, {v});
        }
        if ((in.w1 >> (29 + i)) & 1) {
            v = b_.Op(Op::OpFNegate, t_f32_, {v});
        }
    }
    return v;
}

void Emitter::VDstF(const Inst& in, u32 code, Id v) {
    if (in.format == Format::Vop3) {
        static constexpr float kOmod[4] = {1.0f, 2.0f, 4.0f, 0.5f};
        const u32 omod = (in.w1 >> 27) & 3;
        if (omod != 0) {
            v = b_.Op(Op::OpFMul, t_f32_, {v, b_.F32(kOmod[omod])});
        }
        if ((in.w0 >> 11) & 1) {
            v = Glsl(GLSLstd450FClamp, t_f32_, {v, b_.F32(0.0f), b_.F32(1.0f)});
        }
    }
    SetF(code, v);
}

bool Emitter::EmitCompare(const Inst& in, std::string_view name) {
    if (!name.starts_with("v_cmp")) {
        return false;
    }
    const bool x = name.starts_with("v_cmpx_");
    const std::string_view rest = name.substr(x ? 7 : 6); // "<cond>_<type>"
    const size_t us = rest.rfind('_');
    const std::string_view cond = rest.substr(0, us), type = rest.substr(us + 1);
    const Operands o = GetOperands(in);
    Id r = 0;
    if (type == "f32") {
        static constexpr std::pair<std::string_view, Op> kF[] = {
            {"lt", Op::OpFOrdLessThan},     {"eq", Op::OpFOrdEqual},          {"le", Op::OpFOrdLessThanEqual},
            {"gt", Op::OpFOrdGreaterThan},  {"lg", Op::OpFOrdNotEqual},       {"ge", Op::OpFOrdGreaterThanEqual},
            {"o", Op::OpOrdered},           {"u", Op::OpUnordered},           {"nge", Op::OpFUnordLessThan},
            {"nlg", Op::OpFUnordEqual},     {"ngt", Op::OpFUnordLessThanEqual}, {"nle", Op::OpFUnordGreaterThan},
            {"neq", Op::OpFUnordNotEqual},  {"nlt", Op::OpFUnordGreaterThanEqual},
        };
        for (const auto& [n, op] : kF) {
            if (cond == n) {
                r = b_.Op(op, t_bool_, {VSrcF(in, 0, o.src[0]), VSrcF(in, 1, o.src[1])});
            }
        }
        if (cond == "f") {
            r = b_.ConstantBool(false);
        } else if (cond == "tru") {
            r = b_.ConstantBool(true);
        } else if (cond == "class") {
            // the class mask in src1: 0 sNaN, 1 qNaN, 2 -inf, 3 -normal, 4 -denormal, 5 -0, 6 +0, 7 +denormal,
            // 8 +normal, 9 +inf (denormals are flushed: zeros of their sign)
            const Id m = Get(o.src[1], in);
            const auto bit = [&](u32 k) {
                return b_.Op(Op::OpINotEqual, t_bool_, {b_.Op(Op::OpBitwiseAnd, t_u32_, {m, b_.U32(1u << k)}), b_.U32(0)});
            };
            const Id av = GetF(o.src[0], in);
            const Id nan = b_.Op(Op::OpIsNan, t_bool_, {av});
            const Id inf = b_.Op(Op::OpIsInf, t_bool_, {av});
            const Id neg = b_.Op(Op::OpSLessThan, t_bool_, {Bit(t_i32_, av), b_.Constant(t_i32_, 0)});
            const Id zero = b_.Op(Op::OpFOrdEqual, t_bool_, {av, b_.F32(0.0f)});
            const Id normal = Not(Or(Or(nan, inf), zero));
            const Id pos = Not(neg);
            r = And(nan, Or(bit(0), bit(1)));
            r = Or(r, And(And(inf, neg), bit(2)));
            r = Or(r, And(And(normal, neg), bit(3)));
            r = Or(r, And(And(zero, neg), Or(bit(4), bit(5))));
            r = Or(r, And(And(zero, pos), Or(bit(6), bit(7))));
            r = Or(r, And(And(normal, pos), bit(8)));
            r = Or(r, And(And(inf, pos), bit(9)));
        }
    } else if (type == "i32" || type == "u32") {
        const bool s = type == "i32";
        const Id a = Get(o.src[0], in), c = Get(o.src[1], in);
        const auto cmp = [&](Op so, Op uo) {
            return s ? b_.Op(so, t_bool_, {Bit(t_i32_, a), Bit(t_i32_, c)}) : b_.Op(uo, t_bool_, {a, c});
        };
        if (cond == "lt") {
            r = cmp(Op::OpSLessThan, Op::OpULessThan);
        } else if (cond == "le") {
            r = cmp(Op::OpSLessThanEqual, Op::OpULessThanEqual);
        } else if (cond == "gt") {
            r = cmp(Op::OpSGreaterThan, Op::OpUGreaterThan);
        } else if (cond == "ge") {
            r = cmp(Op::OpSGreaterThanEqual, Op::OpUGreaterThanEqual);
        } else if (cond == "eq") {
            r = b_.Op(Op::OpIEqual, t_bool_, {a, c});
        } else if (cond == "lg" || cond == "ne") {
            r = b_.Op(Op::OpINotEqual, t_bool_, {a, c});
        } else if (cond == "f") {
            r = b_.ConstantBool(false);
        } else if (cond == "t") {
            r = b_.ConstantBool(true);
        }
    }
    if (r == 0) {
        throw Unsupported{std::string(name)};
    }
    const u32 dst = in.format == Format::Vopc ? kVccLo : o.dst[0];
    if (dst == kExecLo) {
        b_.OpNoResult(Op::OpStore, {exec_, r});
    } else {
        const auto [lo, hi] = Ballot(r);
        Set(dst, lo);
        Set(dst + 1, hi);
    }
    if (x) {
        b_.OpNoResult(Op::OpStore, {exec_, r});
    }
    return true;
}

void Emitter::EmitVector(const Inst& in, std::string_view name) {
    if (EmitCompare(in, name)) {
        return;
    }
    const Operands o = GetOperands(in);
    const u32 d = o.num_dst > 0 ? o.dst[0] : 0;
    const auto f = [&](u32 i) { return VSrcF(in, i, o.src[i]); };
    const auto u = [&](u32 i) { return Get(o.src[i], in); };
    const auto si = [&](u32 i) { return Bit(t_i32_, u(i)); };
    const auto fop = [&](Op op, Id a, Id c) { return b_.Op(op, t_f32_, {a, c}); };
    const auto fbin = [&](Op op, bool rev = false) {
        const Id a = f(0), c = f(1);
        VDstF(in, d, rev ? fop(op, c, a) : fop(op, a, c));
    };
    const auto f1 = [&](GLSLstd450 g) { VDstF(in, d, Glsl(g, t_f32_, {f(0)})); };
    // the legacy (DX9) multiply: 0 * x is 0 for every x, infinities and NaN too (the bridge's LegacyMul)
    const auto legacy_mul = [&](Id a, Id c) {
        const Id zero = b_.F32(0.0f);
        const Id either = Or(b_.Op(Op::OpFOrdEqual, t_bool_, {a, zero}), b_.Op(Op::OpFOrdEqual, t_bool_, {c, zero}));
        return Select(t_f32_, either, zero, fop(Op::OpFMul, a, c));
    };
    const auto ubin = [&](Op op) { Set(d, b_.Op(op, t_u32_, {u(0), u(1)})); };
    const auto shift = [&](Op op, bool rev) {
        const Id a = u(0), c = u(1);
        const Id value = rev ? c : a;
        const Id amount = b_.Op(Op::OpBitwiseAnd, t_u32_, {rev ? a : c, b_.U32(31)});
        if (op == Op::OpShiftRightArithmetic) {
            Set(d, Bit(t_u32_, b_.Op(op, t_i32_, {Bit(t_i32_, value), amount})));
        } else {
            Set(d, b_.Op(op, t_u32_, {value, amount}));
        }
    };
    // a carry goes to VCC (VOP2) or the VOP3b SGPR pair (w0[14:8]) and comes from VCC / the pair in src2
    const auto carry_dst = [&]() -> u32 { return in.format == Format::Vop3 ? ((in.w0 >> 8) & 0x7F) : kVccLo; };
    const auto set_mask = [&](u32 code, Id cond) {
        const auto [lo, hi] = Ballot(cond);
        Set(code, lo);
        Set(code + 1, hi);
    };
    const auto carry_in = [&]() {
        const auto [lo, hi] = Src64(in.format == Format::Vop3 ? o.src[2] : kVccLo, in);
        return LaneBit(lo, hi);
    };
    const Id lit = in.literal ? Bit(t_f32_, b_.U32(in.lit)) : 0;
    if (name == "v_mov_b32") {
        Set(d, u(0));
    } else if (name == "v_nop") {
    } else if (name == "v_add_f32") {
        fbin(Op::OpFAdd);
    } else if (name == "v_sub_f32") {
        fbin(Op::OpFSub);
    } else if (name == "v_subrev_f32") {
        fbin(Op::OpFSub, true);
    } else if (name == "v_mul_f32") {
        fbin(Op::OpFMul);
    } else if (name == "v_mul_legacy_f32") {
        const Id a = f(0), c = f(1);
        VDstF(in, d, legacy_mul(a, c));
    } else if (name == "v_min_f32" || name == "v_min_legacy_f32") {
        const Id a = f(0), c = f(1);
        VDstF(in, d, Glsl(GLSLstd450NMin, t_f32_, {a, c}));
    } else if (name == "v_max_f32" || name == "v_max_legacy_f32") {
        const Id a = f(0), c = f(1);
        VDstF(in, d, Glsl(GLSLstd450NMax, t_f32_, {a, c}));
    } else if (name == "v_mac_f32" || name == "v_mac_legacy_f32") {
        const Id a = f(0), c = f(1);
        const Id m = name == "v_mac_legacy_f32" ? legacy_mul(a, c) : fop(Op::OpFMul, a, c);
        VDstF(in, d, fop(Op::OpFAdd, m, GetF(d, in)));
    } else if (name == "v_mad_f32" || name == "v_mad_legacy_f32") {
        const Id a = f(0), c = f(1), e = f(2);
        const Id m = name == "v_mad_legacy_f32" ? legacy_mul(a, c) : fop(Op::OpFMul, a, c);
        VDstF(in, d, fop(Op::OpFAdd, m, e));
    } else if (name == "v_fma_f32") {
        const Id a = f(0), c = f(1), e = f(2);
        VDstF(in, d, Glsl(GLSLstd450Fma, t_f32_, {a, c, e}));
    } else if (name == "v_madak_f32") {
        const Id a = f(0), c = f(1);
        VDstF(in, d, fop(Op::OpFAdd, fop(Op::OpFMul, a, c), lit)); // a * b + K
    } else if (name == "v_madmk_f32") {
        const Id a = f(0), c = f(1);
        VDstF(in, d, fop(Op::OpFAdd, fop(Op::OpFMul, a, lit), c)); // a * K + b
    } else if (name == "v_med3_f32") {
        const Id a = f(0), c = f(1), e = f(2);
        VDstF(in, d, Glsl(GLSLstd450NMax, t_f32_, {Glsl(GLSLstd450NMin, t_f32_, {a, c}),
                                                   Glsl(GLSLstd450NMin, t_f32_, {Glsl(GLSLstd450NMax, t_f32_, {a, c}), e})}));
    } else if (name == "v_max3_f32") {
        const Id a = f(0), c = f(1), e = f(2);
        VDstF(in, d, Glsl(GLSLstd450NMax, t_f32_, {Glsl(GLSLstd450NMax, t_f32_, {a, c}), e}));
    } else if (name == "v_min3_f32") {
        const Id a = f(0), c = f(1), e = f(2);
        VDstF(in, d, Glsl(GLSLstd450NMin, t_f32_, {Glsl(GLSLstd450NMin, t_f32_, {a, c}), e}));
    } else if (name == "v_rcp_legacy_f32") {
        // an infinite result is a zero of its sign (the bridge's)
        const Id r = fop(Op::OpFDiv, b_.F32(1.0f), f(0));
        const Id signed_zero = Bit(t_f32_, b_.Op(Op::OpBitwiseAnd, t_u32_, {Bit(t_u32_, r), b_.U32(0x80000000u)}));
        VDstF(in, d, Select(t_f32_, b_.Op(Op::OpIsInf, t_bool_, {r}), signed_zero, r));
    } else if (name == "v_rcp_f32" || name == "v_rcp_iflag_f32") {
        VDstF(in, d, fop(Op::OpFDiv, b_.F32(1.0f), f(0)));
    } else if (name == "v_rsq_f32" || name == "v_rsq_legacy_f32") {
        f1(GLSLstd450InverseSqrt);
    } else if (name == "v_sqrt_f32") {
        f1(GLSLstd450Sqrt);
    } else if (name == "v_floor_f32") {
        f1(GLSLstd450Floor);
    } else if (name == "v_ceil_f32") {
        f1(GLSLstd450Ceil);
    } else if (name == "v_trunc_f32") {
        f1(GLSLstd450Trunc);
    } else if (name == "v_rndne_f32") {
        f1(GLSLstd450RoundEven);
    } else if (name == "v_fract_f32") {
        f1(GLSLstd450Fract);
    } else if (name == "v_exp_f32" || name == "v_exp_legacy_f32") {
        f1(GLSLstd450Exp2);
    } else if (name == "v_log_f32" || name == "v_log_clamp_f32" || name == "v_log_legacy_f32") {
        f1(GLSLstd450Log2);
    } else if (name == "v_sin_f32" || name == "v_cos_f32") {
        const Id rad = fop(Op::OpFMul, f(0), b_.F32(6.28318530718f)); // (GCN: revolutions)
        VDstF(in, d, Glsl(name == "v_sin_f32" ? GLSLstd450Sin : GLSLstd450Cos, t_f32_, {rad}));
    } else if (name == "v_ldexp_f32") {
        VDstF(in, d, Glsl(GLSLstd450Ldexp, t_f32_, {f(0), si(1)}));
    } else if (name == "v_cndmask_b32") {
        const u32 m = in.format == Format::Vop3 ? o.src[2] : kVccLo; // VOP2: VCC; VOP3: the pair in src2
        const Id a = in.format == Format::Vop3 ? Bit(t_u32_, f(0)) : u(0);
        const Id c = in.format == Format::Vop3 ? Bit(t_u32_, f(1)) : u(1);
        const auto [mlo, mhi] = Src64(m, in);
        Set(d, Select(t_u32_, LaneBit(mlo, mhi), c, a));
    } else if (name == "v_cvt_f32_i32") {
        VDstF(in, d, b_.Op(Op::OpConvertSToF, t_f32_, {si(0)}));
    } else if (name == "v_cvt_f32_u32") {
        VDstF(in, d, b_.Op(Op::OpConvertUToF, t_f32_, {u(0)}));
    } else if (name == "v_cvt_i32_f32" || name == "v_cvt_flr_i32_f32" || name == "v_cvt_rpi_i32_f32") {
        Id v = f(0);
        if (name == "v_cvt_flr_i32_f32") {
            v = Glsl(GLSLstd450Floor, t_f32_, {v});
        } else if (name == "v_cvt_rpi_i32_f32") {
            v = Glsl(GLSLstd450Floor, t_f32_, {fop(Op::OpFAdd, v, b_.F32(0.5f))});
        }
        Set(d, Bit(t_u32_, b_.Op(Op::OpConvertFToS, t_i32_, {v})));
    } else if (name == "v_cvt_u32_f32") {
        Set(d, b_.Op(Op::OpConvertFToU, t_u32_, {Glsl(GLSLstd450FMax, t_f32_, {f(0), b_.F32(0.0f)})}));
    } else if (name.starts_with("v_cvt_f32_ubyte")) {
        const u32 k = static_cast<u32>(name.back() - '0');
        VDstF(in, d, b_.Op(Op::OpConvertUToF, t_f32_, {b_.Op(Op::OpBitFieldUExtract, t_u32_, {u(0), b_.U32(8 * k), b_.U32(8)})}));
    } else if (name == "v_and_b32") {
        ubin(Op::OpBitwiseAnd);
    } else if (name == "v_or_b32") {
        ubin(Op::OpBitwiseOr);
    } else if (name == "v_xor_b32") {
        ubin(Op::OpBitwiseXor);
    } else if (name == "v_not_b32") {
        Set(d, b_.Op(Op::OpNot, t_u32_, {u(0)}));
    } else if (name == "v_lshlrev_b32") {
        shift(Op::OpShiftLeftLogical, true);
    } else if (name == "v_lshrrev_b32") {
        shift(Op::OpShiftRightLogical, true);
    } else if (name == "v_ashrrev_i32") {
        shift(Op::OpShiftRightArithmetic, true);
    } else if (name == "v_lshl_b32") {
        shift(Op::OpShiftLeftLogical, false);
    } else if (name == "v_lshr_b32") {
        shift(Op::OpShiftRightLogical, false);
    } else if (name == "v_ashr_i32") {
        shift(Op::OpShiftRightArithmetic, false);
    } else if (name == "v_add_i32" || name == "v_sub_i32" || name == "v_subrev_i32") {
        const Id a = u(0), c = u(1);
        const bool add = name == "v_add_i32";
        const Id x = name == "v_subrev_i32" ? c : a, y = name == "v_subrev_i32" ? a : c;
        const Id r = b_.Op(add ? Op::OpIAdd : Op::OpISub, t_u32_, {x, y});
        Set(d, r);
        set_mask(carry_dst(), add ? b_.Op(Op::OpULessThan, t_bool_, {r, x}) : b_.Op(Op::OpULessThan, t_bool_, {x, y}));
    } else if (name == "v_addc_u32" || name == "v_subb_u32" || name == "v_subbrev_u32") {
        const Id a = u(0), c = u(1);
        const Id cin = carry_in();
        const bool add = name == "v_addc_u32";
        const Id x = name == "v_subbrev_u32" ? c : a, y = name == "v_subbrev_u32" ? a : c;
        const Op op = add ? Op::OpIAdd : Op::OpISub;
        const Id r = b_.Op(op, t_u32_, {b_.Op(op, t_u32_, {x, y}), SelBool(cin)});
        Set(d, r);
        set_mask(carry_dst(), add ? Or(b_.Op(Op::OpULessThan, t_bool_, {r, x}), And(cin, b_.Op(Op::OpIEqual, t_bool_, {r, x})))
                                  : Or(b_.Op(Op::OpULessThan, t_bool_, {x, y}), And(cin, b_.Op(Op::OpIEqual, t_bool_, {x, y}))));
    } else if (name == "v_mul_lo_u32" || name == "v_mul_lo_i32") {
        ubin(Op::OpIMul);
    } else if (name == "v_mul_hi_u32") {
        const Id pair = b_.TypeStruct(std::array<Id, 2>{t_u32_, t_u32_});
        Set(d, b_.Op(Op::OpCompositeExtract, t_u32_, {b_.Op(Op::OpUMulExtended, pair, {u(0), u(1)}), 1}));
    } else if (name == "v_mul_u32_u24" || name == "v_mad_u32_u24") {
        const Id a = b_.Op(Op::OpBitwiseAnd, t_u32_, {u(0), b_.U32(0xFFFFFF)});
        const Id c = b_.Op(Op::OpBitwiseAnd, t_u32_, {u(1), b_.U32(0xFFFFFF)});
        Id r = b_.Op(Op::OpIMul, t_u32_, {a, c});
        if (name == "v_mad_u32_u24") {
            r = b_.Op(Op::OpIAdd, t_u32_, {r, u(2)});
        }
        Set(d, r);
    } else if (name == "v_mul_i32_i24" || name == "v_mad_i32_i24") {
        const Id a = b_.Op(Op::OpBitFieldSExtract, t_i32_, {si(0), b_.U32(0), b_.U32(24)});
        const Id c = b_.Op(Op::OpBitFieldSExtract, t_i32_, {si(1), b_.U32(0), b_.U32(24)});
        Id r = b_.Op(Op::OpIMul, t_i32_, {a, c});
        if (name == "v_mad_i32_i24") {
            r = b_.Op(Op::OpIAdd, t_i32_, {r, si(2)});
        }
        Set(d, Bit(t_u32_, r));
    } else if (name == "v_min_i32" || name == "v_max_i32") {
        Set(d, Bit(t_u32_, Glsl(name == "v_min_i32" ? GLSLstd450SMin : GLSLstd450SMax, t_i32_, {si(0), si(1)})));
    } else if (name == "v_min_u32" || name == "v_max_u32") {
        Set(d, Glsl(name == "v_min_u32" ? GLSLstd450UMin : GLSLstd450UMax, t_u32_, {u(0), u(1)}));
    } else if (name == "v_bfe_u32") {
        Set(d, b_.Op(Op::OpBitFieldUExtract, t_u32_,
                     {u(0), b_.Op(Op::OpBitwiseAnd, t_u32_, {u(1), b_.U32(31)}), b_.Op(Op::OpBitwiseAnd, t_u32_, {u(2), b_.U32(31)})}));
    } else if (name == "v_bfe_i32") {
        Set(d, Bit(t_u32_, b_.Op(Op::OpBitFieldSExtract, t_i32_,
                                 {si(0), b_.Op(Op::OpBitwiseAnd, t_u32_, {u(1), b_.U32(31)}),
                                  b_.Op(Op::OpBitwiseAnd, t_u32_, {u(2), b_.U32(31)})})));
    } else if (name == "v_bfi_b32") {
        const Id m = u(0);
        Set(d, b_.Op(Op::OpBitwiseOr, t_u32_,
                     {b_.Op(Op::OpBitwiseAnd, t_u32_, {m, u(1)}), b_.Op(Op::OpBitwiseAnd, t_u32_, {b_.Op(Op::OpNot, t_u32_, {m}), u(2)})}));
    } else if (name == "v_bfm_b32") {
        const Id cnt = b_.Op(Op::OpBitwiseAnd, t_u32_, {u(0), b_.U32(31)});
        const Id off = b_.Op(Op::OpBitwiseAnd, t_u32_, {u(1), b_.U32(31)});
        const Id ones = b_.Op(Op::OpISub, t_u32_, {b_.Op(Op::OpShiftLeftLogical, t_u32_, {b_.U32(1), cnt}), b_.U32(1)});
        Set(d, b_.Op(Op::OpShiftLeftLogical, t_u32_, {ones, off}));
    } else if (name == "v_bcnt_u32_b32") {
        Set(d, b_.Op(Op::OpIAdd, t_u32_, {b_.Op(Op::OpBitCount, t_u32_, {u(0)}), u(1)}));
    } else if (name == "v_sad_u32") {
        const Id a = u(0), c = u(1);
        const Id diff = Select(t_u32_, b_.Op(Op::OpUGreaterThan, t_bool_, {a, c}), b_.Op(Op::OpISub, t_u32_, {a, c}),
                               b_.Op(Op::OpISub, t_u32_, {c, a}));
        Set(d, b_.Op(Op::OpIAdd, t_u32_, {diff, u(2)}));
    } else if (name == "v_cvt_pkrtz_f16_f32") {
        const Id v2 = b_.Op(Op::OpCompositeConstruct, b_.TypeVector(t_f32_, 2), {f(0), f(1)});
        Set(d, Glsl(GLSLstd450PackHalf2x16, t_u32_, {v2}));
    } else if (name == "v_cvt_pknorm_u16_f32" || name == "v_cvt_pknorm_i16_f32") {
        const Id v2 = b_.Op(Op::OpCompositeConstruct, b_.TypeVector(t_f32_, 2), {f(0), f(1)});
        Set(d, Glsl(name == "v_cvt_pknorm_u16_f32" ? GLSLstd450PackUnorm2x16 : GLSLstd450PackSnorm2x16, t_u32_, {v2}));
    } else if (name == "v_mbcnt_lo_u32_b32" || name == "v_mbcnt_hi_u32_b32") {
        // the set bits of the mask below this lane (lo: lanes 0-31, hi: 32-63) + src1: exact for subgroups <= 64
        const bool lo = name == "v_mbcnt_lo_u32_b32";
        if (options_.compute && ConstOperand(o.src[0], in).has_value()) {
            // a constant mask: by the lane's index in the wave of 64 (exact whatever the subgroup size)
            const Id lane64 = b_.Op(Op::OpBitwiseAnd, t_u32_, {LocalIndex(), b_.U32(63)});
            const Id below = lo ? Glsl(GLSLstd450UMin, t_u32_, {lane64, b_.U32(32)})
                                : b_.Op(Op::OpISub, t_u32_, {Glsl(GLSLstd450UMax, t_u32_, {lane64, b_.U32(32)}), b_.U32(32)});
            const Id ones = b_.Op(Op::OpISub, t_u32_,
                                  {b_.Op(Op::OpShiftLeftLogical, t_u32_, {b_.U32(1), Glsl(GLSLstd450UMin, t_u32_, {below, b_.U32(31)})}),
                                   b_.U32(1)});
            const Id bits = Select(t_u32_, b_.Op(Op::OpIEqual, t_bool_, {below, b_.U32(32)}), b_.U32(~0u), ones);
            const Id n = b_.Op(Op::OpBitCount, t_u32_, {b_.Op(Op::OpBitwiseAnd, t_u32_, {u(0), bits})});
            Set(d, b_.Op(Op::OpIAdd, t_u32_, {n, u(1)}));
            return;
        }
        const Id m = b_.Op(Op::OpCompositeConstruct, t_uvec4_,
                           {lo ? u(0) : b_.U32(0), lo ? b_.U32(0) : u(0), b_.U32(0), b_.U32(0)});
        const Id n = b_.Op(Op::OpGroupNonUniformBallotBitCount, t_u32_,
                           {b_.U32(static_cast<u32>(spv::Scope::Subgroup)),
                            static_cast<u32>(spv::GroupOperation::ExclusiveScan), m});
        Set(d, b_.Op(Op::OpIAdd, t_u32_, {n, u(1)}));
    } else if (name == "v_writelane_b32") {
        // the lane src1 selects gets src0 (a scalar); the others keep their value
        if (const auto c = ConstOperand(o.src[1], in); c && (*c & 63) >= options_.subgroup_size) {
            throw Unsupported{fmt::format("v_writelane_b32 of lane {} (subgroups of {})", *c & 63, options_.subgroup_size)};
        }
        const Id here = b_.Op(Op::OpIEqual, t_bool_,
                              {LaneId(), b_.Op(Op::OpBitwiseAnd, t_u32_, {u(1), b_.U32(options_.subgroup_size - 1)})});
        Set(d, Select(t_u32_, here, u(0), Get(d, in)));
    } else if (name == "v_readlane_b32") {
        if (const auto c = ConstOperand(o.src[1], in); c && (*c & 63) >= options_.subgroup_size) {
            throw Unsupported{fmt::format("v_readlane_b32 of lane {} (subgroups of {})", *c & 63, options_.subgroup_size)};
        }
        const Id lane = b_.Op(Op::OpBitwiseAnd, t_u32_, {u(1), b_.U32(options_.subgroup_size - 1)});
        Set(d, b_.Op(Op::OpGroupNonUniformShuffle, t_u32_, {b_.U32(static_cast<u32>(spv::Scope::Subgroup)), u(0), lane}));
    } else if (name == "v_mad_u64_u32") {
        // VOP3b: {vdst+1, vdst} = src0 * src1 + {src2+1, src2}; the carry to the SGPR pair in w0[14:8]
        const Id pair = b_.TypeStruct(std::array<Id, 2>{t_u32_, t_u32_});
        const Id prod = b_.Op(Op::OpUMulExtended, pair, {u(0), u(1)});
        const Id plo = b_.Op(Op::OpCompositeExtract, t_u32_, {prod, 0});
        const Id phi = b_.Op(Op::OpCompositeExtract, t_u32_, {prod, 1});
        const auto [clo, chi] = Src64(o.src[2], in); // (an inline 0 is 0:0, not 0:1)
        const Id rlo = b_.Op(Op::OpIAdd, t_u32_, {plo, clo});
        const Id c0 = SelBool(b_.Op(Op::OpULessThan, t_bool_, {rlo, plo}));
        const Id h1 = b_.Op(Op::OpIAdd, t_u32_, {phi, chi});
        const Id rhi = b_.Op(Op::OpIAdd, t_u32_, {h1, c0});
        Set(d, rlo);
        Set(d + 1, rhi);
        set_mask(carry_dst(), Or(b_.Op(Op::OpULessThan, t_bool_, {h1, phi}), b_.Op(Op::OpULessThan, t_bool_, {rhi, h1})));
    } else if (name == "v_cubeid_f32" || name == "v_cubesc_f32" || name == "v_cubetc_f32" || name == "v_cubema_f32") {
        // the oracle's formulas (without VK_AMD_gcn_shader)
        const Id x = f(0), y = f(1), z = f(2);
        const Id ax = Glsl(GLSLstd450FAbs, t_f32_, {x}), ay = Glsl(GLSLstd450FAbs, t_f32_, {y}),
                 az = Glsl(GLSLstd450FAbs, t_f32_, {z});
        const Id zf = And(b_.Op(Op::OpFOrdGreaterThanEqual, t_bool_, {az, ax}),
                          b_.Op(Op::OpFOrdGreaterThanEqual, t_bool_, {az, ay}));
        const Id yf = b_.Op(Op::OpFOrdGreaterThanEqual, t_bool_, {ay, ax});
        const auto pick = [&](Id xr, Id yr, Id zr) { return Select(t_f32_, zf, zr, Select(t_f32_, yf, yr, xr)); };
        const auto lt0 = [&](Id v) { return b_.Op(Op::OpFOrdLessThan, t_bool_, {v, b_.F32(0.0f)}); };
        const auto neg = [&](Id v) { return b_.Op(Op::OpFNegate, t_f32_, {v}); };
        Id r = 0;
        if (name == "v_cubeid_f32") {
            r = pick(Select(t_f32_, lt0(x), b_.F32(1.0f), b_.F32(0.0f)), Select(t_f32_, lt0(y), b_.F32(3.0f), b_.F32(2.0f)),
                     Select(t_f32_, lt0(z), b_.F32(5.0f), b_.F32(4.0f)));
        } else if (name == "v_cubesc_f32") {
            r = pick(Select(t_f32_, lt0(x), z, neg(z)), x, Select(t_f32_, lt0(z), neg(x), x));
        } else if (name == "v_cubetc_f32") {
            r = pick(neg(y), Select(t_f32_, lt0(y), neg(z), z), neg(y));
        } else {
            r = pick(fop(Op::OpFMul, x, b_.F32(2.0f)), fop(Op::OpFMul, y, b_.F32(2.0f)), fop(Op::OpFMul, z, b_.F32(2.0f)));
        }
        VDstF(in, d, r);
    } else if (name == "v_bfrev_b32") {
        Set(d, b_.Op(Op::OpBitReverse, t_u32_, {u(0)}));
    } else if (name == "v_min3_i32" || name == "v_max3_i32") {
        const GLSLstd450 g = name == "v_min3_i32" ? GLSLstd450SMin : GLSLstd450SMax;
        Set(d, Bit(t_u32_, Glsl(g, t_i32_, {Glsl(g, t_i32_, {si(0), si(1)}), si(2)})));
    } else if (name == "v_min3_u32" || name == "v_max3_u32") {
        const GLSLstd450 g = name == "v_min3_u32" ? GLSLstd450UMin : GLSLstd450UMax;
        Set(d, Glsl(g, t_u32_, {Glsl(g, t_u32_, {u(0), u(1)}), u(2)}));
    } else if (name == "v_med3_i32" || name == "v_med3_u32") {
        const bool s = name == "v_med3_i32";
        const GLSLstd450 mn = s ? GLSLstd450SMin : GLSLstd450UMin, mx = s ? GLSLstd450SMax : GLSLstd450UMax;
        const Id ty = s ? t_i32_ : t_u32_;
        const Id a = s ? si(0) : u(0), c = s ? si(1) : u(1), e = s ? si(2) : u(2);
        const Id r = Glsl(mx, ty, {Glsl(mn, ty, {a, c}), Glsl(mn, ty, {Glsl(mx, ty, {a, c}), e})});
        Set(d, s ? Bit(t_u32_, r) : r);
    } else if (name.find("_f64") != std::string_view::npos) {
        // 64-bit floats (register pairs)
        const auto f64 = [&](u32 i) { return GetF64(o.src[i], in, i); };
        if (name == "v_cvt_f64_i32") {
            Float64();
            SetF64(in, d, b_.Op(Op::OpConvertSToF, t_f64_, {si(0)}));
        } else if (name == "v_cvt_f64_u32") {
            Float64();
            SetF64(in, d, b_.Op(Op::OpConvertUToF, t_f64_, {u(0)}));
        } else if (name == "v_cvt_f64_f32") {
            Float64();
            SetF64(in, d, b_.Op(Op::OpFConvert, t_f64_, {f(0)}));
        } else if (name == "v_cvt_f32_f64") {
            VDstF(in, d, b_.Op(Op::OpFConvert, t_f32_, {f64(0)}));
        } else if (name == "v_add_f64") {
            SetF64(in, d, b_.Op(Op::OpFAdd, t_f64_, {f64(0), f64(1)}));
        } else if (name == "v_mul_f64") {
            SetF64(in, d, b_.Op(Op::OpFMul, t_f64_, {f64(0), f64(1)}));
        } else if (name == "v_fma_f64") {
            SetF64(in, d, Glsl(GLSLstd450Fma, t_f64_, {f64(0), f64(1), f64(2)}));
        } else if (name == "v_min_f64" || name == "v_max_f64") {
            SetF64(in, d, Glsl(name == "v_min_f64" ? GLSLstd450NMin : GLSLstd450NMax, t_f64_, {f64(0), f64(1)}));
        } else if (name == "v_rcp_f64") {
            const Id x = f64(0);
            SetF64(in, d, b_.Op(Op::OpFDiv, t_f64_, {b_.Op(Op::OpConvertSToF, t_f64_, {b_.Constant(t_i32_, 1)}), x}));
        } else if (name == "v_sqrt_f64") {
            SetF64(in, d, Glsl(GLSLstd450Sqrt, t_f64_, {f64(0)}));
        } else {
            throw Unsupported{std::string(name)};
        }
    } else if (name == "v_readfirstlane_b32") {
        Set(d, b_.Op(Op::OpGroupNonUniformBroadcastFirst, t_u32_, {b_.U32(static_cast<u32>(spv::Scope::Subgroup)), u(0)}));
    } else {
        throw Unsupported{std::string(name)};
    }
}

// ---------------------------------------------------------------- the function

EmitResult Emitter::Run() {
    EmitResult res;
    try {
        BuildGraph();
        Setup();
        AnalyzeResources(insts_, cfg_, options_.user_sgprs, res_); // (an unknown V# fails at its instruction)
        DeclareResources();
        for (const Inst& in : insts_) {
            writes_exec_ = writes_exec_ || WritesExec(in);
        }
        const Id fn = b_.BeginFunction(t_void_, b_.TypeFunction(t_void_));
        labels_.resize(nodes_.size());
        for (Id& l : labels_) {
            l = b_.NewId();
        }
        std::vector<Id> unreachable_merges;
        const auto unreachable = [&]() {
            const Id l = b_.NewId();
            unreachable_merges.push_back(l);
            return l;
        };
        b_.Label(b_.NewId());
        exec_ = b_.LocalVariable(t_ptr_bool_, b_.ConstantBool(true));
        scc_ = b_.LocalVariable(t_ptr_bool_, b_.ConstantBool(false));
        for (u32 i = 0; i < std::min<u32>(options_.user_sgprs, 16); ++i) {
            Set(i, FlatDword(b_.U32(i))); // the user data registers
        }
        if (options_.compute) {
            InitCompute();
        }
        if (options_.vertex) {
            InitVertex();
        }
        b_.OpNoResult(Op::OpBranch, {labels_[0]});
        for (const u32 x : Rpo()) {
            const Node& nd = nodes_[x];
            b_.Label(labels_[x]);
            if (nd.loop_header) {
                const Id body = b_.NewId();
                const Id merge = nd.loop_merge >= 0 ? labels_[nd.loop_merge] : unreachable();
                b_.OpNoResult(Op::OpLoopMerge, {merge, labels_[nd.loop_continue], 0});
                b_.OpNoResult(Op::OpBranch, {body});
                b_.Label(body);
            }
            if (nd.orig >= 0) {
                const Block& blk = cfg_.blocks[nd.orig];
                for (u32 i = blk.first; i <= blk.last;) {
                    if (!writes_exec_ || !IsVector(insts_[i])) {
                        cur_index_ = i;
                        Emit(insts_[i++]);
                        continue;
                    }
                    // a run of vector instructions under `if (exec)` (it ends after one that writes EXEC)
                    const Id run = b_.NewId(), after = b_.NewId();
                    const Id e = Exec(); // (before the merge: it must come right before the branch)
                    b_.OpNoResult(Op::OpSelectionMerge, {after, 0});
                    b_.OpNoResult(Op::OpBranchConditional, {e, run, after});
                    b_.Label(run);
                    while (i <= blk.last && IsVector(insts_[i])) {
                        const bool ends = WritesExec(insts_[i]);
                        cur_index_ = i;
                        Emit(insts_[i++]);
                        if (ends) {
                            break;
                        }
                    }
                    b_.OpNoResult(Op::OpBranch, {after});
                    b_.Label(after);
                }
            }
            if (nd.exit || nd.succ.empty()) {
                if (options_.vertex) {
                    VertexEpilogue();
                }
                b_.OpNoResult(Op::OpReturn, {});
                continue;
            }
            if (nd.succ.size() == 1) {
                b_.OpNoResult(Op::OpBranch, {labels_[nd.succ[0]]});
                continue;
            }
            Id c = 0;
            switch (nd.cond) {
            case Cond::Scc0:
                c = Not(Scc());
                break;
            case Cond::Scc1:
                c = Scc();
                break;
            case Cond::Vccz:
            case Cond::Vccnz: {
                const Id bit = LaneBit(b_.Op(Op::OpLoad, t_u32_, {RegVar(106)}), b_.Op(Op::OpLoad, t_u32_, {RegVar(107)}));
                c = nd.cond == Cond::Vccnz ? bit : Not(bit);
                break;
            }
            case Cond::Execz:
                c = Not(Exec());
                break;
            default:
                throw Unsupported{fmt::format("branch condition {}", CondName(nd.cond))};
            }
            if (!nd.break_branch) {
                const Id merge = nd.selection_merge >= 0 ? labels_[nd.selection_merge] : unreachable();
                b_.OpNoResult(Op::OpSelectionMerge, {merge, 0});
            }
            b_.OpNoResult(Op::OpBranchConditional, {c, labels_[nd.succ[0]], labels_[nd.succ[1]]});
        }
        for (const Id l : unreachable_merges) {
            b_.Label(l);
            b_.OpNoResult(Op::OpUnreachable, {});
        }
        b_.EndFunction();
        if (options_.vertex) {
            b_.EntryPoint(spv::ExecutionModel::Vertex, fn, "main", interface_);
        } else {
            b_.EntryPoint(spv::ExecutionModel::GLCompute, fn, "main", interface_);
        }
        if (options_.vertex) {
            // (no execution mode)
        } else if (options_.compute) {
            b_.ExecutionMode(fn, spv::ExecutionMode::LocalSize,
                             {std::max(options_.threads[0], 1u), std::max(options_.threads[1], 1u),
                              std::max(options_.threads[2], 1u)});
        } else {
            b_.ExecutionMode(fn, spv::ExecutionMode::LocalSize, {64, 1, 1});
        }
        res.spirv = b_.Assemble();
        res.resources = res_;
        res.ok = true;
    } catch (const Unsupported& u) {
        res.unsupported = u.what;
        res.error = "unsupported: " + u.what;
    }
    return res;
}

} // namespace

EmitResult EmitProgram(std::span<const Inst> insts, const Cfg& cfg, const Structure& st, const EmitOptions& options) {
    return Emitter(insts, cfg, st, options).Run();
}

} // namespace SecondSon::Gpu::Gcn
