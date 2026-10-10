// SecondSonPC — AMD SSE4a for CPUs without it (see sse4a.h).
#include "runtime/cpu/sse4a.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

#include <intrin.h>
#include <windows.h>

#include <Zydis/Zydis.h>

namespace SecondSon::Cpu::Sse4a {

namespace {

const ZydisDecoder& Decoder() {
    static const ZydisDecoder decoder = [] {
        ZydisDecoder d;
        ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
        return d;
    }();
    return decoder;
}

bool Xmm(const ZydisDecodedOperand& op, u8& number) {
    if (op.type != ZYDIS_OPERAND_TYPE_REGISTER || op.reg.value < ZYDIS_REGISTER_XMM0 ||
        op.reg.value > ZYDIS_REGISTER_XMM15) {
        return false;
    }
    number = static_cast<u8>(op.reg.value - ZYDIS_REGISTER_XMM0);
    return true;
}

u64 Mask(u32 length) {
    length &= 63;
    return length == 0 ? ~0ull : (1ull << length) - 1;
}

// ------------------------------------------------------------------------------------- encoding

constexpr u8 kRax = 0, kRcx = 1, kRdx = 2, kR8 = 8;

class Emitter {
public:
    Emitter(u8* out, u64 capacity, u64 at) : begin_{out}, p_{out}, end_{out + capacity}, at_{at} {}

    u64 Size() const {
        return ok_ ? static_cast<u64>(p_ - begin_) : 0;
    }
    u64 Here() const {
        return static_cast<u64>(p_ - begin_);
    }
    void Bytes(std::initializer_list<u8> bytes) {
        for (const u8 b : bytes) {
            Byte(b);
        }
    }
    void Byte(u8 v) {
        if (p_ < end_) {
            *p_++ = v;
        } else {
            ok_ = false;
        }
    }
    void Qword(u64 v) {
        for (int i = 0; i < 8; ++i) {
            Byte(static_cast<u8>(v >> (8 * i)));
        }
    }
    /// The byte at `offset` (a forward rel8 to patch).
    void Patch(u64 offset, u8 value) {
        if (begin_ + offset < end_) {
            begin_[offset] = value;
        }
    }

    /// movq r64, xmm (66 REX.W 0F 7E /r)
    void MovqFromXmm(u8 gpr, u8 xmm) {
        Bytes({0x66, static_cast<u8>(0x48 | (xmm >= 8 ? 4 : 0) | (gpr >= 8 ? 1 : 0)), 0x0F, 0x7E,
               static_cast<u8>(0xC0 | ((xmm & 7) << 3) | (gpr & 7))});
    }
    /// movq xmm, r64 (66 REX.W 0F 6E /r): bits 127:64 zeroed, 255:128 kept
    void MovqToXmm(u8 xmm, u8 gpr) {
        Bytes({0x66, static_cast<u8>(0x48 | (xmm >= 8 ? 4 : 0) | (gpr >= 8 ? 1 : 0)), 0x0F, 0x6E,
               static_cast<u8>(0xC0 | ((xmm & 7) << 3) | (gpr & 7))});
    }
    /// pextrq r64, xmm, 1 (66 REX.W 0F 3A 16 /r ib)
    void PextrqHigh(u8 gpr, u8 xmm) {
        Bytes({0x66, static_cast<u8>(0x48 | (xmm >= 8 ? 4 : 0) | (gpr >= 8 ? 1 : 0)), 0x0F, 0x3A, 0x16,
               static_cast<u8>(0xC0 | ((xmm & 7) << 3) | (gpr & 7)), 0x01});
    }
    /// movabs r64, imm64
    void MovImm(u8 gpr, u64 value) {
        Bytes({static_cast<u8>(0x48 | (gpr >= 8 ? 1 : 0)), static_cast<u8>(0xB8 | (gpr & 7))});
        Qword(value);
    }
    void Push(u8 gpr) {
        if (gpr >= 8) {
            Byte(0x41);
        }
        Byte(static_cast<u8>(0x50 | (gpr & 7)));
    }
    void Pop(u8 gpr) {
        if (gpr >= 8) {
            Byte(0x41);
        }
        Byte(static_cast<u8>(0x58 | (gpr & 7)));
    }
    /// Saves rax, rcx, rdx, r8 and the flags below the System V red zone.
    void Prologue() {
        Bytes({0x48, 0x8D, 0x64, 0x24, 0x80}); // lea rsp, [rsp-128]
        Byte(0x9C);                            // pushfq
        Push(kRax);
        Push(kRcx);
        Push(kRdx);
        Push(kR8);
    }
    void Epilogue() {
        Pop(kR8);
        Pop(kRdx);
        Pop(kRcx);
        Pop(kRax);
        Byte(0x9D);                                              // popfq
        Bytes({0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00}); // lea rsp, [rsp+128]
    }
    void Jump(u64 target) {
        const u64 next = at_ + Here() + 5;
        const s64 rel = static_cast<s64>(target - next);
        if (rel != static_cast<s32>(rel)) {
            ok_ = false;
            return;
        }
        Byte(0xE9);
        const u32 r = static_cast<u32>(static_cast<s32>(rel));
        for (int i = 0; i < 4; ++i) {
            Byte(static_cast<u8>(r >> (8 * i)));
        }
    }

private:
    u8* begin_;
    u8* p_;
    u8* end_;
    u64 at_;
    bool ok_ = true;
};

} // namespace

std::optional<Insn> Decode(const u8* code, u64 available) {
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), code, std::min<u64>(available, ZYDIS_MAX_INSTRUCTION_LENGTH),
                                             &insn, ops))) {
        return std::nullopt;
    }
    Insn out{};
    out.length = insn.length;
    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_EXTRQ:
        if (!Xmm(ops[0], out.dst)) {
            return std::nullopt;
        }
        if (ops[1].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            out.op = Op::ExtrqImm;
            out.len = static_cast<u8>(ops[1].imm.value.u & 63);
            out.idx = static_cast<u8>(ops[2].imm.value.u & 63);
            return out;
        }
        if (!Xmm(ops[1], out.src)) {
            return std::nullopt;
        }
        out.op = Op::ExtrqReg;
        return out;
    case ZYDIS_MNEMONIC_INSERTQ:
        if (!Xmm(ops[0], out.dst) || !Xmm(ops[1], out.src)) {
            return std::nullopt;
        }
        if (ops[2].type == ZYDIS_OPERAND_TYPE_IMMEDIATE && ops[3].type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
            out.op = Op::InsertqImm;
            out.len = static_cast<u8>(ops[2].imm.value.u & 63);
            out.idx = static_cast<u8>(ops[3].imm.value.u & 63);
            return out;
        }
        out.op = Op::InsertqReg;
        return out;
    case ZYDIS_MNEMONIC_MOVNTSS:
    case ZYDIS_MNEMONIC_MOVNTSD: {
        out.op = insn.mnemonic == ZYDIS_MNEMONIC_MOVNTSS ? Op::Movntss : Op::Movntsd;
        if (!Xmm(ops[1], out.dst)) {
            return std::nullopt;
        }
        // Prefixes (F2/F3, REX, segment) are never 0F: the first 0F 2B is the opcode.
        for (u8 i = 0; i + 1 < insn.length; ++i) {
            if (code[i] == 0x0F && code[i + 1] == 0x2B) {
                out.opcode_at = static_cast<u8>(i + 1);
                return out;
            }
        }
        return std::nullopt;
    }
    default:
        return std::nullopt;
    }
}

void Execute(const Insn& insn, u64 dst[2], const u64 src[2]) {
    switch (insn.op) {
    case Op::ExtrqImm:
    case Op::ExtrqReg: {
        const u32 len = insn.op == Op::ExtrqImm ? insn.len : static_cast<u32>(src[0] & 63);
        const u32 idx = insn.op == Op::ExtrqImm ? insn.idx : static_cast<u32>((src[0] >> 8) & 63);
        dst[0] = (dst[0] >> idx) & Mask(len);
        dst[1] = 0;
        break;
    }
    case Op::InsertqImm:
    case Op::InsertqReg: {
        const u32 len = insn.op == Op::InsertqImm ? insn.len : static_cast<u32>(src[1] & 63);
        const u32 idx = insn.op == Op::InsertqImm ? insn.idx : static_cast<u32>((src[1] >> 8) & 63);
        const u64 m = Mask(len);
        dst[0] = (dst[0] & ~(m << idx)) | ((src[0] & m) << idx);
        dst[1] = 0;
        break;
    }
    default:
        break;
    }
}

u64 EncodeTrampoline(const Insn& insn, u64 site, u64 at, u8* out, u64 capacity) {
    Emitter e(out, capacity, at);
    e.Prologue();
    switch (insn.op) {
    case Op::ExtrqImm:
        e.MovqFromXmm(kRax, insn.dst); // rax = dst >> idx & mask(len)
        if (insn.idx != 0) {
            e.Bytes({0x48, 0xC1, 0xE8, insn.idx}); // shr rax, idx
        }
        if (insn.len != 0) {
            e.MovImm(kRdx, Mask(insn.len));
            e.Bytes({0x48, 0x21, 0xD0}); // and rax, rdx
        }
        e.MovqToXmm(insn.dst, kRax);
        break;
    case Op::ExtrqReg: {
        e.MovqFromXmm(kRdx, insn.src);                       // rdx = control
        e.MovqFromXmm(kRax, insn.dst);                       // rax = value
        e.Bytes({0x89, 0xD1});                               // mov ecx, edx
        e.Bytes({0xC1, 0xE9, 0x08});                         // shr ecx, 8: cl & 63 = index
        e.Bytes({0x48, 0xD3, 0xE8});                         // shr rax, cl
        e.Bytes({0x89, 0xD1});                               // mov ecx, edx
        e.Bytes({0x83, 0xE1, 0x3F});                         // and ecx, 63: length (0 = 64: no mask)
        e.Bytes({0x74, 0x00});                               // jz done
        const u64 jz = e.Here() - 1;
        e.Bytes({0x48, 0xC7, 0xC2, 0xFF, 0xFF, 0xFF, 0xFF}); // mov rdx, -1
        e.Bytes({0x48, 0xD3, 0xE2});                         // shl rdx, cl
        e.Bytes({0x48, 0xF7, 0xD2});                         // not rdx
        e.Bytes({0x48, 0x21, 0xD0});                         // and rax, rdx
        e.Patch(jz, static_cast<u8>(e.Here() - (jz + 1)));
        e.MovqToXmm(insn.dst, kRax);
        break;
    }
    case Op::InsertqImm: {
        const u64 m = Mask(insn.len);
        e.MovqFromXmm(kRdx, insn.src); // rdx = field source
        e.MovqFromXmm(kRax, insn.dst); // rax = destination
        e.MovImm(kR8, m);
        e.Bytes({0x4C, 0x21, 0xC2}); // and rdx, r8
        if (insn.idx != 0) {
            e.Bytes({0x48, 0xC1, 0xE2, insn.idx}); // shl rdx, idx
        }
        e.MovImm(kR8, ~(m << insn.idx));
        e.Bytes({0x4C, 0x21, 0xC0}); // and rax, r8
        e.Bytes({0x48, 0x09, 0xD0}); // or rax, rdx
        e.MovqToXmm(insn.dst, kRax);
        break;
    }
    case Op::InsertqReg: {
        e.PextrqHigh(kRcx, insn.src);                        // rcx = control (src bits 127:64)
        e.MovqFromXmm(kRdx, insn.src);                       // rdx = field source
        e.MovqFromXmm(kRax, insn.dst);                       // rax = destination
        e.Push(kRcx);
        e.Bytes({0x49, 0xC7, 0xC0, 0xFF, 0xFF, 0xFF, 0xFF}); // mov r8, -1
        e.Bytes({0x83, 0xE1, 0x3F});                         // and ecx, 63: length
        e.Bytes({0x74, 0x00});                               // jz full (0 = 64)
        const u64 jz = e.Here() - 1;
        e.Bytes({0x49, 0xD3, 0xE0});                         // shl r8, cl
        e.Bytes({0x49, 0xF7, 0xD0});                         // not r8
        e.Patch(jz, static_cast<u8>(e.Here() - (jz + 1)));
        e.Pop(kRcx);
        e.Bytes({0xC1, 0xE9, 0x08});                         // shr ecx, 8: cl & 63 = index
        e.Bytes({0x4C, 0x21, 0xC2});                         // and rdx, r8
        e.Bytes({0x48, 0xD3, 0xE2});                         // shl rdx, cl
        e.Bytes({0x49, 0xD3, 0xE0});                         // shl r8, cl
        e.Bytes({0x49, 0xF7, 0xD0});                         // not r8
        e.Bytes({0x4C, 0x21, 0xC0});                         // and rax, r8
        e.Bytes({0x48, 0x09, 0xD0});                         // or rax, rdx
        e.MovqToXmm(insn.dst, kRax);
        break;
    }
    default:
        return 0;
    }
    e.Epilogue();
    e.Jump(site + insn.length);
    return e.Size();
}

bool EmulateInContext(const Insn& insn, _CONTEXT* context) {
    if (insn.op == Op::Movntss || insn.op == Op::Movntsd) {
        return false; // a store: rewritten at load time, never emulated here
    }
    auto* regs = context->FltSave.XmmRegisters;
    u64 dst[2] = {regs[insn.dst].Low, static_cast<u64>(regs[insn.dst].High)};
    const u64 src[2] = {regs[insn.src].Low, static_cast<u64>(regs[insn.src].High)};
    Execute(insn, dst, src);
    regs[insn.dst].Low = dst[0];
    regs[insn.dst].High = static_cast<LONGLONG>(dst[1]);
    context->Rip += insn.length;
    return true;
}

bool HostHasSse4a() {
    int regs[4]{};
    __cpuid(regs, static_cast<int>(0x80000000u));
    if (static_cast<u32>(regs[0]) < 0x80000001u) {
        return false;
    }
    __cpuid(regs, static_cast<int>(0x80000001u));
    return (regs[2] & (1 << 6)) != 0;
}

} // namespace SecondSon::Cpu::Sse4a
