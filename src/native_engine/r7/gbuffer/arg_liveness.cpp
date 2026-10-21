// SecondSonPC R7-G — argument contract of a guest function from its code (see arg_liveness.h).
#include "native_engine/r7/gbuffer/arg_liveness.h"

#include <unordered_map>
#include <vector>

#include <Zydis/Zydis.h>

namespace SecondSon::NativeEngine::R7G {

namespace {

constexpr s32 kUnknown = 0x7fffffff;
constexpr u16 kAllArgs = 0x3fff; // 6 GPRs + 8 XMMs
constexpr u32 kMaxInstructions = 400000;

struct State {
    u16 written = 0; ///< bits 0..5 GPR arguments, 6..13 XMM0..7
    u8 low8 = 0;     ///< GPR arguments whose low byte was written (a later low-byte read is not an argument read)
    u8 low16 = 0;    ///< ... low word
    s32 rsp = 0;     ///< entry rsp - rsp (bytes pushed / reserved), kUnknown when lost
    s32 rbp = kUnknown;
};

int GprIndex(ZydisRegister r) {
    switch (ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, r)) {
    case ZYDIS_REGISTER_RDI:
        return 0;
    case ZYDIS_REGISTER_RSI:
        return 1;
    case ZYDIS_REGISTER_RDX:
        return 2;
    case ZYDIS_REGISTER_RCX:
        return 3;
    case ZYDIS_REGISTER_R8:
        return 4;
    case ZYDIS_REGISTER_R9:
        return 5;
    default:
        return -1;
    }
}

int XmmIndex(ZydisRegister r) {
    int i = -1;
    if (r >= ZYDIS_REGISTER_XMM0 && r <= ZYDIS_REGISTER_XMM31) {
        i = r - ZYDIS_REGISTER_XMM0;
    } else if (r >= ZYDIS_REGISTER_YMM0 && r <= ZYDIS_REGISTER_YMM31) {
        i = r - ZYDIS_REGISTER_YMM0;
    } else if (r >= ZYDIS_REGISTER_ZMM0 && r <= ZYDIS_REGISTER_ZMM31) {
        i = r - ZYDIS_REGISTER_ZMM0;
    }
    return i >= 0 && i < 8 ? i : -1;
}

/// Bit of the tracked register `r` (-1: not tracked).
int Bit(ZydisRegister r) {
    const int g = GprIndex(r);
    if (g >= 0) {
        return g;
    }
    const int x = XmmIndex(r);
    return x >= 0 ? 6 + x : -1;
}

bool ZeroIdiom(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_XOR:
    case ZYDIS_MNEMONIC_SUB:
    case ZYDIS_MNEMONIC_PXOR:
    case ZYDIS_MNEMONIC_XORPS:
    case ZYDIS_MNEMONIC_XORPD:
    case ZYDIS_MNEMONIC_VPXOR:
    case ZYDIS_MNEMONIC_VPXORD:
    case ZYDIS_MNEMONIC_VPXORQ:
    case ZYDIS_MNEMONIC_VXORPS:
    case ZYDIS_MNEMONIC_VXORPD:
        break;
    default:
        return false;
    }
    const u32 n = in.operand_count_visible;
    if (n < 2 || ops[n - 1].type != ZYDIS_OPERAND_TYPE_REGISTER || ops[n - 2].type != ZYDIS_OPERAND_TYPE_REGISTER) {
        return false;
    }
    return ops[n - 1].reg.value == ops[n - 2].reg.value;
}

bool HighByte(ZydisRegister r) {
    return r == ZYDIS_REGISTER_AH || r == ZYDIS_REGISTER_BH || r == ZYDIS_REGISTER_CH || r == ZYDIS_REGISTER_DH;
}

/// A read of GPR argument `b` through `r` that only sees bits this path already wrote (mov cl, ...; and cl, 2).
bool LowDefined(ZydisRegister r, int b, const State& st) {
    const ZydisRegisterWidth w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, r);
    if (HighByte(r)) {
        return false;
    }
    return (w == 8 && ((st.low8 >> b) & 1u) != 0) || (w == 16 && ((st.low16 >> b) & 1u) != 0);
}

/// AVX scalar operations whose first source only supplies the upper lanes of the result (lane 0 comes from the other
/// source): vcvtsi2ss xmm0, xmm0, edx is not a read of an XMM argument (the upper lanes of a scalar are not one).
bool UpperLanesOnly(const ZydisDecodedInstruction& in, u32 operand) {
    if (operand != 1 || in.operand_count_visible != 3) {
        return false;
    }
    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_VCVTSI2SS:
    case ZYDIS_MNEMONIC_VCVTSI2SD:
    case ZYDIS_MNEMONIC_VCVTSS2SD:
    case ZYDIS_MNEMONIC_VCVTSD2SS:
    case ZYDIS_MNEMONIC_VSQRTSS:
    case ZYDIS_MNEMONIC_VSQRTSD:
    case ZYDIS_MNEMONIC_VRCPSS:
    case ZYDIS_MNEMONIC_VRSQRTSS:
    case ZYDIS_MNEMONIC_VMOVSS:
    case ZYDIS_MNEMONIC_VMOVSD:
        return true;
    default:
        return false;
    }
}

class Analyzer {
public:
    explicit Analyzer(u32 max_depth) : max_depth_(max_depth) {
        ZydisDecoderInit(&decoder_, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    }

    ArgUse Run(u64 entry, u32 depth) {
        if (auto it = memo_.find(entry); it != memo_.end()) {
            return it->second;
        }
        memo_[entry] = ArgUse{0, 0, 0, 0, 0, true}; // recursion: a cycle contributes nothing more
        ArgUse use{};
        use.ok = true;
        std::unordered_map<u64, State> seen;
        std::vector<std::pair<u64, State>> work;
        work.push_back({entry, State{}});
        while (!work.empty()) {
            auto [pc, st] = work.back();
            work.pop_back();
            for (;;) {
                if (auto it = seen.find(pc); it != seen.end()) {
                    State& old = it->second;
                    const u16 meet = old.written & st.written;
                    const u8 meet8 = old.low8 & st.low8, meet16 = old.low16 & st.low16;
                    const bool same_stack = old.rsp == st.rsp && old.rbp == st.rbp;
                    if (meet == old.written && meet8 == old.low8 && meet16 == old.low16 && same_stack) {
                        break; // nothing new on this path
                    }
                    old.written = meet;
                    old.low8 = meet8;
                    old.low16 = meet16;
                    if (old.rsp != st.rsp) {
                        old.rsp = kUnknown;
                    }
                    if (old.rbp != st.rbp) {
                        old.rbp = kUnknown;
                    }
                    st = old;
                } else {
                    seen[pc] = st;
                }
                if (++total_ > kMaxInstructions) {
                    use.ok = false;
                    break;
                }
                ++use.instructions;
                ZydisDecodedInstruction in;
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder_, reinterpret_cast<const void*>(pc), 15, &in, ops))) {
                    use.ok = false;
                    break;
                }
                const u64 next = pc + in.length;
                Reads(in, ops, st, use, pc);
                const ZydisInstructionCategory cat = in.meta.category;
                if (cat == ZYDIS_CATEGORY_RET || in.mnemonic == ZYDIS_MNEMONIC_UD2 || in.mnemonic == ZYDIS_MNEMONIC_INT3 ||
                    in.mnemonic == ZYDIS_MNEMONIC_HLT) {
                    break;
                }
                if (cat == ZYDIS_CATEGORY_CALL) {
                    u64 target = 0;
                    if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                        ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in, &ops[0], pc, &target))) {
                        if (depth < max_depth_) {
                            const ArgUse callee = Run(target, depth + 1);
                            Use(callee.gpr | static_cast<u16>(callee.xmm << 6), st, use, pc);
                            use.unknown += callee.unknown;
                            use.ok = use.ok && callee.ok;
                        }
                    } else {
                        ++use.unknown;
                    }
                    st.written = kAllArgs; // caller-saved: clobbered
                    pc = next;
                    continue;
                }
                Writes(in, ops, st);
                if (cat == ZYDIS_CATEGORY_UNCOND_BR || cat == ZYDIS_CATEGORY_COND_BR) {
                    u64 target = 0;
                    if (ops[0].type == ZYDIS_OPERAND_TYPE_IMMEDIATE &&
                        ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&in, &ops[0], pc, &target))) {
                        if (cat == ZYDIS_CATEGORY_COND_BR) {
                            work.push_back({target, st});
                            pc = next;
                        } else {
                            pc = target;
                        }
                        continue;
                    }
                    ++use.unknown; // a jump table / tail call through memory
                    if (cat == ZYDIS_CATEGORY_COND_BR) {
                        pc = next;
                        continue;
                    }
                    break;
                }
                pc = next;
            }
        }
        memo_[entry] = use;
        return use;
    }

private:
    static void Use(u16 bits, const State& st, ArgUse& use, u64 pc) {
        const u16 live = bits & static_cast<u16>(~st.written);
        for (u32 b = 0; b < 14; ++b) {
            if ((live >> b) & 1u && use.where[b] == 0) {
                use.where[b] = pc;
            }
        }
        use.gpr |= static_cast<u8>(live & 0x3f);
        use.xmm |= static_cast<u8>(live >> 6);
    }

    static void StackRead(s32 off, ArgUse& use) {
        if (off >= 8 && off < 8 + 64) {
            use.stack |= static_cast<u8>(1u << ((off - 8) / 8));
        }
    }

    static void Reads(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, const State& st, ArgUse& use,
                      u64 pc) {
        const bool zero = ZeroIdiom(in, ops);
        u16 read = 0;
        for (u32 i = 0; i < in.operand_count; ++i) {
            const ZydisDecodedOperand& o = ops[i];
            if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                if ((o.actions & ZYDIS_OPERAND_ACTION_MASK_READ) != 0 && !zero && !UpperLanesOnly(in, i)) {
                    const int b = Bit(o.reg.value);
                    if (b >= 0 && !(b < 6 && LowDefined(o.reg.value, b, st))) {
                        read |= static_cast<u16>(1u << b);
                    }
                }
            } else if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                for (ZydisRegister r : {o.mem.base, o.mem.index}) {
                    const int b = Bit(r);
                    if (b >= 0) {
                        read |= static_cast<u16>(1u << b);
                    }
                }
                if (o.mem.type == ZYDIS_MEMOP_TYPE_AGEN || (o.actions & ZYDIS_OPERAND_ACTION_MASK_READ) == 0 ||
                    o.mem.index != ZYDIS_REGISTER_NONE) {
                    continue;
                }
                const s64 disp = o.mem.disp.value;
                if (o.mem.base == ZYDIS_REGISTER_RSP && st.rsp != kUnknown) {
                    StackRead(static_cast<s32>(disp - st.rsp), use);
                } else if (o.mem.base == ZYDIS_REGISTER_RBP && st.rbp != kUnknown) {
                    StackRead(static_cast<s32>(disp - st.rbp), use);
                }
            }
        }
        Use(read, st, use, pc);
    }

    static void Writes(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops, State& st) {
        const ZydisDecodedOperand& d = ops[0];
        const bool d_reg = in.operand_count_visible >= 1 && d.type == ZYDIS_OPERAND_TYPE_REGISTER;
        // the stack and frame pointers
        switch (in.mnemonic) {
        case ZYDIS_MNEMONIC_PUSH:
            st.rsp = st.rsp == kUnknown ? kUnknown : st.rsp + 8;
            break;
        case ZYDIS_MNEMONIC_POP:
            st.rsp = st.rsp == kUnknown ? kUnknown : st.rsp - 8;
            if (d_reg && d.reg.value == ZYDIS_REGISTER_RBP) {
                st.rbp = kUnknown;
            }
            break;
        case ZYDIS_MNEMONIC_LEAVE:
            st.rsp = st.rbp == kUnknown ? kUnknown : st.rbp - 8;
            st.rbp = kUnknown;
            break;
        default:
            if (d_reg && (d.reg.value == ZYDIS_REGISTER_RSP || d.reg.value == ZYDIS_REGISTER_RBP)) {
                const bool is_rsp = d.reg.value == ZYDIS_REGISTER_RSP;
                s32& v = is_rsp ? st.rsp : st.rbp;
                const ZydisDecodedOperand& s = ops[1];
                if ((in.mnemonic == ZYDIS_MNEMONIC_SUB || in.mnemonic == ZYDIS_MNEMONIC_ADD) &&
                    s.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && v != kUnknown) {
                    const s64 imm = s.imm.value.s;
                    v = in.mnemonic == ZYDIS_MNEMONIC_SUB ? v + static_cast<s32>(imm) : v - static_cast<s32>(imm);
                } else if (in.mnemonic == ZYDIS_MNEMONIC_MOV && s.type == ZYDIS_OPERAND_TYPE_REGISTER &&
                           (s.reg.value == ZYDIS_REGISTER_RSP || s.reg.value == ZYDIS_REGISTER_RBP)) {
                    v = s.reg.value == ZYDIS_REGISTER_RSP ? st.rsp : st.rbp;
                } else if (in.mnemonic == ZYDIS_MNEMONIC_LEA && s.type == ZYDIS_OPERAND_TYPE_MEMORY &&
                           s.mem.index == ZYDIS_REGISTER_NONE &&
                           (s.mem.base == ZYDIS_REGISTER_RSP || s.mem.base == ZYDIS_REGISTER_RBP)) {
                    const s32 base = s.mem.base == ZYDIS_REGISTER_RSP ? st.rsp : st.rbp;
                    v = base == kUnknown ? kUnknown : base - static_cast<s32>(s.mem.disp.value);
                } else {
                    v = kUnknown; // e.g. and rsp, -32
                }
            }
            break;
        }
        // tracked registers written
        for (u32 i = 0; i < in.operand_count; ++i) {
            const ZydisDecodedOperand& o = ops[i];
            if (o.type != ZYDIS_OPERAND_TYPE_REGISTER || (o.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) == 0 ||
                (o.actions & ZYDIS_OPERAND_ACTION_CONDWRITE) != 0) {
                continue;
            }
            const int b = Bit(o.reg.value);
            if (b < 0) {
                continue;
            }
            if (b < 6) {
                const ZydisRegisterWidth w = ZydisRegisterGetWidth(ZYDIS_MACHINE_MODE_LONG_64, o.reg.value);
                if (w < 32) { // a partial write keeps the rest of the incoming value
                    if (!HighByte(o.reg.value)) {
                        st.low8 |= static_cast<u8>(1u << b);
                        if (w == 16) {
                            st.low16 |= static_cast<u8>(1u << b);
                        }
                    }
                    continue;
                }
            }
            st.written |= static_cast<u16>(1u << b);
        }
    }

    ZydisDecoder decoder_{};
    u32 max_depth_;
    u64 total_ = 0;
    std::unordered_map<u64, ArgUse> memo_;
};

} // namespace

ArgUse ArgumentsRead(u64 address, u32 max_depth) {
    Analyzer a(max_depth);
    return a.Run(address, 0);
}

} // namespace SecondSon::NativeEngine::R7G
