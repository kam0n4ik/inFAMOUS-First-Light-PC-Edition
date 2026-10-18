// SecondSonPC — where a GCN program's scalar data comes from (gcn_resources.h).
#include "runtime/gpu/gcn/gcn_resources.h"

#include <algorithm>
#include <optional>
#include <string_view>

#include <fmt/format.h>

namespace SecondSon::Gpu::Gcn {

namespace {

constexpr u32 kRegs = 128; // SGPRs 0-103, VCC 106/107, M0 124 (the codes of the operand space)

using State = std::array<ScalarValue, kRegs>;

ScalarValue Meet(const ScalarValue& a, const ScalarValue& b) {
    return a == b ? a : ScalarValue{};
}

/// The value of a scalar source operand in `s` (constants of the operand space included).
ScalarValue Value(const State& s, u32 code, const Inst& in) {
    if (code <= 103 || code == 106 || code == 107 || code == 124) {
        return s[code];
    }
    if (code == 128) {
        return {ScalarValue::Kind::Const, 0};
    }
    if (code >= 129 && code <= 192) {
        return {ScalarValue::Kind::Const, code - 128};
    }
    if (code >= 193 && code <= 208) {
        return {ScalarValue::Kind::Const, static_cast<u32>(-static_cast<s32>(code - 192))};
    }
    if (code == kLiteralOperand) {
        return {ScalarValue::Kind::Const, in.lit};
    }
    return {};
}

void Write(State& s, u32 code, ScalarValue v) {
    if (code < kRegs) {
        s[code] = v;
    }
}

u32 SmrdDwords(std::string_view name) {
    if (name.ends_with("x16")) {
        return 16;
    }
    if (name.ends_with("x8")) {
        return 8;
    }
    if (name.ends_with("x4")) {
        return 4;
    }
    if (name.ends_with("x2")) {
        return 2;
    }
    return 1;
}

class Analysis {
public:
    Analysis(std::span<const Inst> insts, ResourceInfo& out) : insts_(insts), out_(out) {}

    void Step(State& s, u32 index, bool record);

private:
    s32 Buffer(const State& s, u32 first_sgpr, bool written, u32 index, bool record);

    std::span<const Inst> insts_;
    ResourceInfo& out_;
};

s32 Analysis::Buffer(const State& s, u32 first, bool written, u32 index, bool record) {
    BufferUse use;
    for (u32 i = 0; i < 4; ++i) {
        use.sharp[i] = first + i < kRegs ? s[first + i] : ScalarValue{};
        if (use.sharp[i].kind == ScalarValue::Kind::Unknown && record && out_.error.empty()) {
            out_.error = fmt::format("dword {}: {} - the V# in s[{}:{}] is not found", insts_[index].offset,
                                     Mnemonic(insts_[index].format, insts_[index].op), first, first + 3);
        }
    }
    for (u32 b = 0; b < out_.buffers.size(); ++b) {
        if (out_.buffers[b].sharp == use.sharp) {
            out_.buffers[b].written = out_.buffers[b].written || written;
            return static_cast<s32>(b);
        }
    }
    use.written = written;
    out_.buffers.push_back(use);
    return static_cast<s32>(out_.buffers.size() - 1);
}

void Analysis::Step(State& s, u32 index, bool record) {
    const Inst& in = insts_[index];
    const std::string_view name = Mnemonic(in.format, in.op);
    const Operands o = GetOperands(in);
    const auto unknown_dsts = [&]() {
        for (u32 i = 0; i < o.num_dst; ++i) {
            Write(s, o.dst[i], {});
        }
    };
    switch (in.format) {
    case Format::Smrd: {
        const u32 n = SmrdDwords(name);
        const u32 dst = o.dst[0];
        const u32 base = o.src[0];
        const bool imm = ((in.w0 >> 8) & 1) != 0;
        if (name.starts_with("s_buffer_load")) {
            const s32 b = Buffer(s, base, false, index, record);
            if (record) {
                out_.buffer_of[index] = b;
            }
            for (u32 i = 0; i < n; ++i) {
                Write(s, dst + i, {});
            }
            return;
        }
        // s_load_dword*: through a pointer in two SGPRs
        const ScalarValue lo = s[base], hi = s[base + 1];
        u32 byte_offset = 0;
        bool offset_known = true;
        if (imm) {
            byte_offset = (in.w0 & 0xFF) * 4;
        } else if ((in.w0 & 0xFF) == kLiteralOperand) {
            byte_offset = in.lit * 4;
        } else {
            const ScalarValue v = Value(s, in.w0 & 0xFF, in);
            offset_known = v.kind == ScalarValue::Kind::Const;
            byte_offset = v.value;
        }
        if (lo.kind != ScalarValue::Kind::Flat || hi.kind != ScalarValue::Kind::Flat || hi.value != lo.value + 1 ||
            !offset_known) {
            if (record && out_.error.empty()) {
                out_.error = fmt::format("dword {}: {} - its pointer s[{}:{}] is not in the user data", in.offset, name,
                                         base, base + 1);
            }
            for (u32 i = 0; i < n; ++i) {
                Write(s, dst + i, {});
            }
            return;
        }
        WalkLoad load{lo.value, byte_offset, n, 0};
        s32 found = -1;
        for (u32 l = 0; l < out_.walk.size(); ++l) {
            const WalkLoad& w = out_.walk[l];
            if (w.ptr == load.ptr && w.byte_offset == load.byte_offset && w.dwords == load.dwords) {
                found = static_cast<s32>(l);
            }
        }
        if (found < 0) {
            load.dst = out_.flat_dwords;
            out_.flat_dwords += n;
            out_.walk.push_back(load);
            found = static_cast<s32>(out_.walk.size() - 1);
        }
        if (record) {
            out_.load_of[index] = found;
        }
        for (u32 i = 0; i < n; ++i) {
            Write(s, dst + i, {ScalarValue::Kind::Flat, out_.walk[found].dst + i});
        }
        return;
    }
    case Format::Mubuf:
    case Format::Mtbuf: {
        const bool written =
            name.find("store") != std::string_view::npos || name.find("atomic") != std::string_view::npos;
        const s32 b = Buffer(s, o.src[2], written, index, record);
        if (record) {
            out_.buffer_of[index] = b;
        }
        return;
    }
    case Format::Ds:
        if (((in.w0 >> 17) & 1) != 0 || name == "ds_append" || name == "ds_consume") {
            out_.uses_gds = true;
        } else if (name != "ds_swizzle_b32") {
            out_.uses_lds = true;
        }
        return;
    case Format::Sop1:
        if (name == "s_mov_b32") {
            Write(s, o.dst[0], Value(s, o.src[0], in));
            return;
        }
        if (name == "s_mov_b64") {
            const u32 c = o.src[0];
            const bool reg = c <= 103 || c == 106 || c == 124;
            Write(s, o.dst[0], Value(s, c, in));
            Write(s, o.dst[0] + 1, reg ? s[c + 1] : ScalarValue{ScalarValue::Kind::Const, 0});
            return;
        }
        unknown_dsts();
        if (name.ends_with("_b64") && o.num_dst > 0) {
            Write(s, o.dst[0] + 1, {});
        }
        return;
    case Format::Sopk:
        if (name == "s_movk_i32") {
            Write(s, o.dst[0],
                  {ScalarValue::Kind::Const, static_cast<u32>(static_cast<s32>(static_cast<s16>(in.w0 & 0xFFFF)))});
            return;
        }
        unknown_dsts();
        return;
    case Format::Sop2: {
        const ScalarValue a = Value(s, o.src[0], in), c = Value(s, o.src[1], in);
        std::optional<u32> r;
        if (a.kind == ScalarValue::Kind::Const && c.kind == ScalarValue::Kind::Const) {
            if (name == "s_add_u32" || name == "s_add_i32") {
                r = a.value + c.value;
            } else if (name == "s_sub_u32" || name == "s_sub_i32") {
                r = a.value - c.value;
            } else if (name == "s_mul_i32") {
                r = a.value * c.value;
            } else if (name == "s_lshl_b32") {
                r = a.value << (c.value & 31);
            } else if (name == "s_lshr_b32") {
                r = a.value >> (c.value & 31);
            } else if (name == "s_and_b32") {
                r = a.value & c.value;
            } else if (name == "s_or_b32") {
                r = a.value | c.value;
            }
        }
        Write(s, o.dst[0], r ? ScalarValue{ScalarValue::Kind::Const, *r} : ScalarValue{});
        if (name.ends_with("_b64")) {
            Write(s, o.dst[0] + 1, {});
        }
        return;
    }
    case Format::Vop1:
    case Format::Vop2:
    case Format::Vop3:
    case Format::Vopc: {
        // what a vector instruction writes into SGPRs (readlane, compares, carries) is data: one SGPR for
        // v_readlane / v_readfirstlane, a pair (a lane mask) for the others
        const bool one_sgpr = name == "v_readlane_b32" || name == "v_readfirstlane_b32";
        for (u32 i = 0; i < o.num_dst; ++i) {
            if (o.dst[i] < kVgpr) {
                Write(s, o.dst[i], {});
                if (!one_sgpr) {
                    Write(s, o.dst[i] + 1, {});
                }
            }
        }
        const bool carry = name == "v_add_i32" || name == "v_sub_i32" || name == "v_subrev_i32" ||
                           name == "v_addc_u32" || name == "v_subb_u32" || name == "v_subbrev_u32" ||
                           name == "v_mad_u64_u32" || name == "v_div_scale_f32";
        if (in.format == Format::Vopc || (in.format == Format::Vop2 && carry)) {
            Write(s, 106, {});
            Write(s, 107, {});
        }
        if (in.format == Format::Vop3 && carry) {
            const u32 sd = (in.w0 >> 8) & 0x7F; // (VOP3b's SGPR pair)
            Write(s, sd, {});
            Write(s, sd + 1, {});
        }
        return;
    }
    default:
        return; // (SOPC, SOPP, memory into VGPRs: no SGPR written)
    }
}

} // namespace

bool BuildSrtWalker(const ResourceInfo& info, std::vector<u8>& out, std::string& error) {
    out.clear();
    if (info.walk.empty()) {
        return true;
    }
    const auto u32le = [&](u32 v) {
        for (u32 i = 0; i < 4; ++i) {
            out.push_back(static_cast<u8>(v >> (8 * i)));
        }
    };
    // the displacements of the pointers leading to flat[p..p+1] (from the user data registers)
    std::vector<u32> steps;
    const auto chain = [&](auto&& self, u32 p, u32 depth) -> bool {
        if (depth > 16) {
            return false;
        }
        if (p + 1 < 16) {
            steps.push_back(p * 4);
            return true;
        }
        for (const WalkLoad& w : info.walk) {
            if (w.dst <= p && p + 1 < w.dst + w.dwords) {
                if (!self(self, w.ptr, depth + 1)) {
                    return false;
                }
                steps.push_back(w.byte_offset + (p - w.dst) * 4);
                return true;
            }
        }
        return false;
    };
    for (const WalkLoad& w : info.walk) {
        steps.clear();
        if (!chain(chain, w.ptr, 0)) {
            error = fmt::format("the walk's pointer flat[{}] is not two dwords of the user data or of one load", w.ptr);
            out.clear();
            return false;
        }
        for (const u32 d : steps) {
            out.push_back(0x57);                       // push rdi
            out.insert(out.end(), {0x48, 0x8B, 0xBF}); // mov rdi, qword [rdi + d]
            u32le(d);
            out.insert(out.end(), {0x49, 0xBA}); // mov r10, 0xFFFFFFFFFFFF
            u32le(0xFFFFFFFFu);
            u32le(0x0000FFFFu);
            out.insert(out.end(), {0x4C, 0x21, 0xD7}); // and rdi, r10
        }
        for (u32 i = 0; i < w.dwords; ++i) {
            out.insert(out.end(), {0x44, 0x8B, 0x97}); // mov r10d, dword [rdi + d]
            u32le(w.byte_offset + i * 4);
            out.insert(out.end(), {0x44, 0x89, 0x96}); // mov dword [rsi + 4 * dst], r10d
            u32le((w.dst + i) * 4);
        }
        for (size_t i = 0; i < steps.size(); ++i) {
            out.push_back(0x5F); // pop rdi
        }
    }
    out.push_back(0xC3); // ret
    return true;
}

bool AnalyzeResources(std::span<const Inst> insts, const Cfg& cfg, u32 user_sgprs, ResourceInfo& out) {
    out = {};
    out.buffer_of.assign(insts.size(), -1);
    out.load_of.assign(insts.size(), -1);
    const u32 n = static_cast<u32>(cfg.blocks.size());
    std::vector<State> entry(n);
    std::vector<u8> reached(n, 0);
    State start{};
    for (u32 i = 0; i < std::min<u32>(user_sgprs, 16); ++i) {
        start[i] = {ScalarValue::Kind::Flat, i};
    }
    entry[0] = start;
    reached[0] = 1;
    Analysis a(insts, out);
    // to a fixed point over the reverse post-order (a loop's header meets its back edges' states)
    for (u32 round = 0; round < 32; ++round) {
        bool changed = false;
        for (const u32 b : cfg.rpo) {
            if (!reached[b]) {
                continue;
            }
            State s = entry[b];
            for (u32 i = cfg.blocks[b].first; i <= cfg.blocks[b].last; ++i) {
                a.Step(s, i, false);
            }
            for (const s32 succ : {cfg.blocks[b].taken, cfg.blocks[b].next}) {
                if (succ < 0) {
                    continue;
                }
                if (!reached[succ]) {
                    reached[succ] = 1;
                    entry[succ] = s;
                    changed = true;
                    continue;
                }
                for (u32 r = 0; r < kRegs; ++r) {
                    const ScalarValue m = Meet(entry[succ][r], s[r]);
                    if (!(m == entry[succ][r])) {
                        entry[succ][r] = m;
                        changed = true;
                    }
                }
            }
        }
        if (!changed) {
            break;
        }
    }
    // the final pass: each instruction's resource with the states of the fixed point
    out.buffers.clear();
    for (const u32 b : cfg.rpo) {
        State s = entry[b];
        for (u32 i = cfg.blocks[b].first; i <= cfg.blocks[b].last; ++i) {
            a.Step(s, i, true);
        }
    }
    return out.error.empty();
}

} // namespace SecondSon::Gpu::Gcn
