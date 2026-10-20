"""Static recompiler, steps 2-3: Ghidra P-code -> C++ -> native object (docs/NATIVE_RECOMP_COMPILER.md).

Input: the raw P-code of whole functions (pcode_dump.py; Ghidra's SLEIGH x86-64 is the decoder and the lifter).
Output: one C++ file with a System V entry `recomp_<rva>` per function (compiled by clang-cl into a DLL) and, with
--lift, a second C++ file with one function per guest instruction for the instruction-level conformance check
(sspc_native_engine_tests --lift-check: every instruction against the CPU executing its own bytes).

Model:
  - register space -> a byte array R[] in which the registers a function names are packed (sub-registers alias as
    in hardware); unique space -> a byte array U[] packed the same way;
  - ram space      -> guest memory (guest address = host address); a ram varnode of the instruction itself (an
                      absolute or RIP-relative operand) is eboot-relative in Ghidra and is rebased on recomp_image_base;
  - values up to 8 bytes are u64 expressions, wider ones V<N> byte vectors (XMM/YMM/ZMM, 128-bit products);
  - floating point (FLOAT_* P-code ops) and the AVX operations SLEIGH leaves as CALLOTHER run as the CPU's own
    instruction with the guest's operand order (`{vex3}`: the assembler may not swap commutative operands), on the
    guest thread's MXCSR: the NaN payload, signed zero, rounding, FTZ/DAZ and approximations (vrsqrtps) are the
    hardware's, as in the original; the lane ops of one packed instruction are fused back into it;
  - the entry is a naked System V thunk: RDI..R9, RAX (AL of varargs), XMM0-7 and the caller's stack arguments in;
    RAX, RDX, XMM0, XMM1 out; the body runs on a private stack; callee-saved registers live in R[] only;
  - CALL/CALLIND -> a thunk that passes RDI..R9, RAX, XMM0-7 and the stack arguments the code put on its private
    stack, and takes RAX, RDX, XMM0, XMM1 back; a jump to a function entry out of the body is a tail call;
  - BRANCHIND -> a switch over the targets Ghidra resolved; a target outside them traps; without resolved targets it
    is a tail call, and a target inside the body traps (never a silent wrong jump);
  - LOCK CMPXCHG -> one __atomic_compare_exchange (SLEIGH lifts it as a plain load/compare/store between
    CALLOTHER LOCK/UNLOCK markers);
  - anything else -> refused with a category (BLOCKED_BY_INSTRUCTION / _CONTROL_FLOW / _ABI / _MEMORY /
    _EXTERNAL_CALL) and the reason.

usage: py -I -X utf8 pcode2c.py <pcode.json> <out.cpp> [--report <txt>] [--lift <lift.cpp>] [--coverage <tsv>]
                                [--only <rva,...>]
"""
import bisect
import json
import re
import sys

INT_OPS = {"INT_ADD": "+", "INT_SUB": "-", "INT_AND": "&", "INT_OR": "|", "INT_XOR": "^", "INT_MULT": "*"}
CMP_OPS = {"INT_EQUAL": "==", "INT_NOTEQUAL": "!="}
UNSIGNED_CMP = {"INT_LESS": "<", "INT_LESSEQUAL": "<="}
SIGNED_CMP = {"INT_SLESS": "<", "INT_SLESSEQUAL": "<="}
FLOAT_BIN = {"FLOAT_ADD": "add", "FLOAT_SUB": "sub", "FLOAT_MULT": "mul", "FLOAT_DIV": "div"}
FLOAT_CMP = {"FLOAT_EQUAL": "feq", "FLOAT_NOTEQUAL": "fne", "FLOAT_LESS": "flt", "FLOAT_LESSEQUAL": "fle"}

# AVX operations SLEIGH keeps as CALLOTHER: (instruction, form). Forms (Intel operand order of the CALLOTHER inputs):
#   a: dst = op(a)   ab: dst = op(a, b)   abi: dst = op(a, b, imm)   ai: dst = op(a, imm)   abc: dst = op(a, b, c)
#   g_a: GPR = op(a)   g_ai: GPR = op(a, imm)
# Every use is checked instruction by instruction against the CPU (--lift-check) before a function is trusted.
SIMD = {}
for _m in ("vaddps vaddpd vsubps vsubpd vmulps vmulpd vdivps vdivpd vminps vminpd vmaxps vmaxpd vaddss vaddsd "
           "vsubss vsubsd vmulss vmulsd vdivss vdivsd vminss vminsd vmaxss vmaxsd vhaddps vhaddpd vhsubps vhsubpd "
           "vaddsubps vaddsubpd vandps vandpd vandnps vandnpd vorps vorpd vxorps vxorpd vunpcklps vunpckhps "
           "vunpcklpd vunpckhpd vpand vpandn vpor vpxor vpaddb vpaddw vpaddd vpaddq vpsubb vpsubw vpsubd vpsubq "
           "vpmulld vpmullw vpmuludq vpminsd vpmaxsd vpminud vpmaxud vpcmpeqb vpcmpeqw vpcmpeqd vpcmpeqq vpcmpgtb "
           "vpcmpgtw vpcmpgtd vpunpckldq vpunpckhdq vpunpcklqdq vpunpckhqdq vpackssdw vpackusdw vpshufb vpermilps "
           "vpermilpd vmovlhps vmovhlps vsqrtss vsqrtsd vrsqrtss vrcpss").split():
    SIMD[_m + "_avx"] = (_m, "ab")
for _m in ("vcmpps vcmppd vcmpss vcmpsd vblendps vblendpd vpblendw vshufps vshufpd vdpps vinsertps vpalignr "
           "vroundss vroundsd").split():
    SIMD[_m + "_avx"] = (_m, "abi")
for _m in ("vrsqrtps vrcpps vsqrtps vsqrtpd vmovshdup vmovsldup vmovddup vcvtdq2ps vcvtps2dq vcvttps2dq vcvtps2pd "
           "vcvtpd2ps").split():
    SIMD[_m + "_avx"] = (_m, "a")
for _m in ("vpshufd vpshuflw vpshufhw vpslld vpsrld vpsrad vpsllq vpsrlq vpsllw vpsrlw vpsraw vpslldq vpsrldq "
           "vroundps vroundpd").split():
    SIMD[_m + "_avx"] = (_m, "ai")
for _m in ("vblendvps vblendvpd vpblendvb").split():
    SIMD[_m + "_avx"] = (_m, "abc")
for _m in ("vmovmskps vmovmskpd vpmovmskb").split():
    SIMD[_m + "_avx"] = (_m, "g_a")
for _m in ("vpextrd vpextrq vextractps").split():
    SIMD[_m + "_avx"] = (_m, "g_ai")
SHIFTS = {"vpslld_avx", "vpsrld_avx", "vpsrad_avx", "vpsllq_avx", "vpsrlq_avx", "vpsllw_avx", "vpsrlw_avx",
          "vpsraw_avx"}

PRELUDE = r'''// Generated by tools/native-reconstruction/recomp/pcode2c.py from Ghidra's P-code of the eboot - do not edit.
// docs/NATIVE_RECOMP_COMPILER.md. Derived from the game's code: never committed.
#include <cstdint>
#include <cstring>
#include <immintrin.h>
typedef std::uint8_t u8;
typedef std::uint16_t u16;
typedef std::uint32_t u32;
typedef std::int32_t s32;
typedef std::uint64_t u64;
typedef std::int64_t s64;
typedef unsigned __int128 u128;
typedef __int128 s128;
#define SYSV __attribute__((sysv_abi))
#define INL static inline __attribute__((always_inline))
extern "C" __declspec(dllexport) u64 recomp_image_base = 0;
template <int N> struct V { u8 b[N]; };
INL u64 mask(int n) { return n >= 8 ? ~0ull : ((1ull << (8 * n)) - 1); }
INL u64 ld(const u8* b, u64 off, int n) { u64 v = 0; std::memcpy(&v, b + off, n); return v; }
INL void st(u8* b, u64 off, int n, u64 v) { std::memcpy(b + off, &v, n); }
template <int N> INL V<N> ldv(const u8* p) { V<N> v; std::memcpy(v.b, p, N); return v; }
template <int N> INL void stv(u8* p, const V<N>& v) { std::memcpy(p, v.b, N); }
INL u64 mload(u64 a, int n) { u64 v = 0; std::memcpy(&v, reinterpret_cast<const void*>(a), n); return v; }
INL void mstore(u64 a, int n, u64 v) { std::memcpy(reinterpret_cast<void*>(a), &v, n); }
INL s64 sx(u64 v, int n) { return n >= 8 ? static_cast<s64>(v) : static_cast<s64>(v << (64 - 8 * n)) >> (64 - 8 * n); }
INL u64 carry(u64 a, u64 b, int n) { return ((a & mask(n)) + (b & mask(n))) > mask(n) || (n == 8 && a + b < a); }
INL u64 scarry(u64 a, u64 b, int n) { s64 x = sx(a, n), y = sx(b, n); s64 r = sx((a + b) & mask(n), n); return (x >= 0 && y >= 0 && r < 0) || (x < 0 && y < 0 && r >= 0); }
INL u64 sborrow(u64 a, u64 b, int n) { s64 x = sx(a, n), y = sx(b, n); s64 r = sx((a - b) & mask(n), n); return (x >= 0 && y < 0 && r < 0) || (x < 0 && y >= 0 && r >= 0); }
[[noreturn]] static void recomp_fault() { __builtin_trap(); }
// --- wide values ---
template <int N> INL V<N> zx(u64 v, int n) { V<N> r{}; std::memcpy(r.b, &v, n); return r; }
template <int N, int M> INL V<N> zxv(const V<M>& v) { V<N> r{}; std::memcpy(r.b, v.b, M < N ? M : N); return r; }
template <int N> INL V<N> sxw(u64 v, int n) { V<N> r; std::memset(r.b, sx(v, n) < 0 ? 0xff : 0, N); const u64 x = static_cast<u64>(sx(v, n)); std::memcpy(r.b, &x, 8); return r; }
template <int N> INL V<N> bxor(const V<N>& a, const V<N>& b) { V<N> r; for (int i = 0; i < N; ++i) r.b[i] = a.b[i] ^ b.b[i]; return r; }
template <int N> INL V<N> band(const V<N>& a, const V<N>& b) { V<N> r; for (int i = 0; i < N; ++i) r.b[i] = a.b[i] & b.b[i]; return r; }
template <int N> INL V<N> bor(const V<N>& a, const V<N>& b) { V<N> r; for (int i = 0; i < N; ++i) r.b[i] = a.b[i] | b.b[i]; return r; }
template <int N> INL V<N> bnot(const V<N>& a) { V<N> r; for (int i = 0; i < N; ++i) r.b[i] = static_cast<u8>(~a.b[i]); return r; }
template <int N> INL bool beq(const V<N>& a, const V<N>& b) { return std::memcmp(a.b, b.b, N) == 0; }
template <int N, int L, int H> INL V<N> piece(const void* lo, const void* hi) { V<N> r; std::memcpy(r.b, lo, L); std::memcpy(r.b + L, hi, H); return r; }
INL u128 w(const V<16>& v) { u128 x; std::memcpy(&x, v.b, 16); return x; }
INL V<16> vw(u128 x) { V<16> r; std::memcpy(r.b, &x, 16); return r; }
INL V<16> udiv128(const V<16>& a, const V<16>& b, bool rem) {
    const u128 x = w(a), y = w(b);
    const u64 hi = static_cast<u64>(x >> 64), lo = static_cast<u64>(x), d = static_cast<u64>(y);
    if ((y >> 64) != 0 || d == 0 || hi >= d) recomp_fault(); // DIV r64's shape only; #DE like the hardware
    u64 q, r;
    __asm__("divq %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(d));
    return vw(rem ? r : q);
}
INL V<16> sdiv128(const V<16>& a, const V<16>& b, bool rem) {
    const u128 x = w(a), y = w(b);
    const u64 hi = static_cast<u64>(x >> 64), lo = static_cast<u64>(x), d = static_cast<u64>(y);
    const s64 sd = static_cast<s64>(d);
    if (static_cast<s128>(y) != static_cast<s128>(sd) || d == 0) recomp_fault();
    const s128 hi_s = static_cast<s128>(x) >> 64;
    if (hi_s != (static_cast<s128>(static_cast<s64>(lo)) >> 64)) recomp_fault(); // only a sign-extended RDX:RAX
    if (sd == -1 && static_cast<s64>(lo) == static_cast<s64>(0x8000000000000000ull)) recomp_fault();
    s64 q, r;
    __asm__("idivq %4" : "=a"(q), "=d"(r) : "a"(lo), "d"(hi), "r"(sd));
    return rem ? vw(static_cast<u128>(static_cast<s128>(r))) : vw(static_cast<u128>(static_cast<s128>(q)));
}
// --- floating point: the CPU's own instructions, the guest's operand order, the current MXCSR ---
INL float f32(u64 b) { const u32 x = static_cast<u32>(b); float f; std::memcpy(&f, &x, 4); return f; }
INL u64 b32(float f) { u32 x; std::memcpy(&x, &f, 4); return x; }
INL double f64(u64 b) { double f; std::memcpy(&f, &b, 8); return f; }
INL u64 b64(double f) { u64 x; std::memcpy(&x, &f, 8); return x; }
#define SSPC_FBIN(name, insn, T, conv, back) INL u64 name(u64 a, u64 b) { T x = conv(a), y = conv(b), r; __asm__ volatile("%{vex3%} " insn " %2, %1, %0" : "=x"(r) : "x"(x), "x"(y)); return back(r); }
SSPC_FBIN(fadd4, "vaddss", float, f32, b32) SSPC_FBIN(fsub4, "vsubss", float, f32, b32)
SSPC_FBIN(fmul4, "vmulss", float, f32, b32) SSPC_FBIN(fdiv4, "vdivss", float, f32, b32)
SSPC_FBIN(fadd8, "vaddsd", double, f64, b64) SSPC_FBIN(fsub8, "vsubsd", double, f64, b64)
SSPC_FBIN(fmul8, "vmulsd", double, f64, b64) SSPC_FBIN(fdiv8, "vdivsd", double, f64, b64)
INL u64 fsqrt4(u64 a) { float x = f32(a), r; __asm__ volatile("%{vex3%} vsqrtss %1, %1, %0" : "=x"(r) : "x"(x)); return b32(r); }
INL u64 fsqrt8(u64 a) { double x = f64(a), r; __asm__ volatile("%{vex3%} vsqrtsd %1, %1, %0" : "=x"(r) : "x"(x)); return b64(r); }
INL u64 fnan4(u64 a) { return (a & 0x7f800000u) == 0x7f800000u && (a & 0x7fffffu) != 0; }
INL u64 fnan8(u64 a) { return (a & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (a & 0xfffffffffffffull) != 0; }
INL void ucom4(u64 a, u64 b, u8& z, u8& p, u8& c) { float x = f32(a), y = f32(b); __asm__ volatile("%{vex3%} vucomiss %4, %3\n\tsetz %0\n\tsetp %1\n\tsetc %2" : "=r"(z), "=r"(p), "=r"(c) : "x"(x), "x"(y)); }
INL void ucom8(u64 a, u64 b, u8& z, u8& p, u8& c) { double x = f64(a), y = f64(b); __asm__ volatile("%{vex3%} vucomisd %4, %3\n\tsetz %0\n\tsetp %1\n\tsetc %2" : "=r"(z), "=r"(p), "=r"(c) : "x"(x), "x"(y)); }
#define SSPC_FCMP(n) \
    INL u64 feq##n(u64 a, u64 b) { u8 z, p, c; ucom##n(a, b, z, p, c); return z && !p; } \
    INL u64 fne##n(u64 a, u64 b) { u8 z, p, c; ucom##n(a, b, z, p, c); return !(z && !p); } \
    INL u64 flt##n(u64 a, u64 b) { u8 z, p, c; ucom##n(a, b, z, p, c); return c && !p; } \
    INL u64 fle##n(u64 a, u64 b) { u8 z, p, c; ucom##n(a, b, z, p, c); return (c || z) && !p; }
SSPC_FCMP(4) SSPC_FCMP(8)
INL u64 i2f4_4(u64 a) { float r = 0; const s32 x = static_cast<s32>(a); __asm__ volatile("%{vex3%} vcvtsi2ssl %1, %0, %0" : "+x"(r) : "r"(x)); return b32(r); }
INL u64 i2f4_8(u64 a) { float r = 0; const s64 x = static_cast<s64>(a); __asm__ volatile("%{vex3%} vcvtsi2ssq %1, %0, %0" : "+x"(r) : "r"(x)); return b32(r); }
INL u64 i2f8_4(u64 a) { double r = 0; const s32 x = static_cast<s32>(a); __asm__ volatile("%{vex3%} vcvtsi2sdl %1, %0, %0" : "+x"(r) : "r"(x)); return b64(r); }
INL u64 i2f8_8(u64 a) { double r = 0; const s64 x = static_cast<s64>(a); __asm__ volatile("%{vex3%} vcvtsi2sdq %1, %0, %0" : "+x"(r) : "r"(x)); return b64(r); }
INL u64 f2f4_8(u64 a) { float x = f32(a); double r = 0; __asm__ volatile("%{vex3%} vcvtss2sd %1, %0, %0" : "+x"(r) : "x"(x)); return b64(r); }
INL u64 f2f8_4(u64 a) { double x = f64(a); float r = 0; __asm__ volatile("%{vex3%} vcvtsd2ss %1, %0, %0" : "+x"(r) : "x"(x)); return b32(r); }
INL u64 trunc4_4(u64 a) { float x = f32(a); s32 r; __asm__ volatile("%{vex3%} vcvttss2si %1, %0" : "=r"(r) : "x"(x)); return static_cast<u32>(r); }
INL u64 trunc4_8(u64 a) { float x = f32(a); s64 r; __asm__ volatile("%{vex3%} vcvttss2si %1, %0" : "=r"(r) : "x"(x)); return static_cast<u64>(r); }
INL u64 trunc8_4(u64 a) { double x = f64(a); s32 r; __asm__ volatile("%{vex3%} vcvttsd2si %1, %0" : "=r"(r) : "x"(x)); return static_cast<u32>(r); }
INL u64 trunc8_8(u64 a) { double x = f64(a); s64 r; __asm__ volatile("%{vex3%} vcvttsd2si %1, %0" : "=r"(r) : "x"(x)); return static_cast<u64>(r); }
// --- vectors as the CPU's types ---
INL __m128 m4(const V<16>& v) { __m128 x; std::memcpy(&x, v.b, 16); return x; }
INL V<16> v4(__m128 x) { V<16> r; std::memcpy(r.b, &x, 16); return r; }
INL __m256 m8(const V<32>& v) { __m256 x; std::memcpy(&x, v.b, 32); return x; }
INL V<32> v8(__m256 x) { V<32> r; std::memcpy(r.b, &x, 32); return r; }
// --- the ABI ---
struct RegIO { // the entry thunk's frame: arguments in, results out
    u64 gpr[7];  // rdi rsi rdx rcx r8 r9 rax
    u64 args;    // the caller's stack arguments (its rsp + 8 at the call)
    u8 xmm[8][16];
    u64 out_rax, out_rdx;
    u8 out_xmm[2][16];
};
static_assert(sizeof(RegIO) == 0xf0, "RegIO layout is shared with the entry thunks");
struct CallIO { // a call out of recompiled code
    u64 gpr[7];  // rdi rsi rdx rcx r8 r9 rax
    u64 target;
    u64 args;    // the stack arguments the code placed (its private rsp after the return slot)
    u64 pad;
    u8 xmm[8][16];
    u64 out_rax, out_rdx;
    u8 out_xmm[2][16];
};
static_assert(sizeof(CallIO) == 0x100, "CallIO layout is shared with recomp_call");
// System V call with every argument register, XMM0-7 and 32 stack slots copied from the private stack; the callee runs
// on the real stack. System V callee-saved registers are kept by the callee itself.
extern "C" __attribute__((naked, sysv_abi)) void recomp_call(CallIO*) {
    __asm__ volatile(
        "pushq %rbx\n\tpushq %rbp\n\tmovq %rsp, %rbp\n\tmovq %rdi, %rbx\n\t"
        "subq $0x100, %rsp\n\tandq $-16, %rsp\n\t"
        "movq 0x40(%rbx), %rsi\n\txorl %ecx, %ecx\n"
        "1:\n\tmovq (%rsi,%rcx,8), %rax\n\tmovq %rax, (%rsp,%rcx,8)\n\tincl %ecx\n\tcmpl $32, %ecx\n\tjb 1b\n\t"
        "vmovdqu 0x50(%rbx), %xmm0\n\tvmovdqu 0x60(%rbx), %xmm1\n\tvmovdqu 0x70(%rbx), %xmm2\n\t"
        "vmovdqu 0x80(%rbx), %xmm3\n\tvmovdqu 0x90(%rbx), %xmm4\n\tvmovdqu 0xa0(%rbx), %xmm5\n\t"
        "vmovdqu 0xb0(%rbx), %xmm6\n\tvmovdqu 0xc0(%rbx), %xmm7\n\t"
        "movq 0x8(%rbx), %rsi\n\tmovq 0x10(%rbx), %rdx\n\tmovq 0x18(%rbx), %rcx\n\tmovq 0x20(%rbx), %r8\n\t"
        "movq 0x28(%rbx), %r9\n\tmovq 0x30(%rbx), %rax\n\tmovq 0x0(%rbx), %rdi\n\t"
        "callq *0x38(%rbx)\n\t"
        "movq %rax, 0xd0(%rbx)\n\tmovq %rdx, 0xd8(%rbx)\n\tvmovdqu %xmm0, 0xe0(%rbx)\n\tvmovdqu %xmm1, 0xf0(%rbx)\n\t"
        "movq %rbp, %rsp\n\tpopq %rbp\n\tpopq %rbx\n\tretq\n\t");
}
#define SSPC_ENTRY(rva) \
    extern "C" SYSV void body_##rva(RegIO*); \
    extern "C" __declspec(dllexport) __attribute__((naked)) void recomp_##rva() { \
        __asm__ volatile( \
            "subq $0x108, %rsp\n\t" \
            "movq %rdi, 0x0(%rsp)\n\tmovq %rsi, 0x8(%rsp)\n\tmovq %rdx, 0x10(%rsp)\n\tmovq %rcx, 0x18(%rsp)\n\t" \
            "movq %r8, 0x20(%rsp)\n\tmovq %r9, 0x28(%rsp)\n\tmovq %rax, 0x30(%rsp)\n\t" \
            "leaq 0x110(%rsp), %rax\n\tmovq %rax, 0x38(%rsp)\n\t" \
            "vmovdqu %xmm0, 0x40(%rsp)\n\tvmovdqu %xmm1, 0x50(%rsp)\n\tvmovdqu %xmm2, 0x60(%rsp)\n\t" \
            "vmovdqu %xmm3, 0x70(%rsp)\n\tvmovdqu %xmm4, 0x80(%rsp)\n\tvmovdqu %xmm5, 0x90(%rsp)\n\t" \
            "vmovdqu %xmm6, 0xa0(%rsp)\n\tvmovdqu %xmm7, 0xb0(%rsp)\n\t" \
            "movq %rsp, %rdi\n\tcallq body_" #rva "\n\t" \
            "movq 0xc0(%rsp), %rax\n\tmovq 0xc8(%rsp), %rdx\n\t" \
            "vmovdqu 0xd0(%rsp), %xmm0\n\tvmovdqu 0xe0(%rsp), %xmm1\n\t" \
            "addq $0x108, %rsp\n\tretq\n\t"); \
    }
'''

LIFT_PRELUDE = r'''
struct LiftEntry { u64 addr; u32 len; u32 flags; SYSV void (*fn)(u8*, u8*); };  // flags bit 0: AF unread
struct LiftReg { char name[8]; u32 off; u32 size; };
'''

ARGS = ("RDI", "RSI", "RDX", "RCX", "R8", "R9", "RAX")


class Unsupported(Exception):
    def __init__(self, category, message):
        super().__init__(message)
        self.category = category


def blocked(category, message):
    return Unsupported("BLOCKED_BY_" + category, message)


def signed(off):
    return off if off < (1 << 63) else off - (1 << 64)


class Layout:
    """Packs the byte ranges a function names into one array; overlapping ranges (sub-registers) stay aliased."""

    def __init__(self, ranges):
        merged = []
        for s, e in sorted(ranges):
            if merged and s < merged[-1][1]:
                merged[-1][1] = max(merged[-1][1], e)
            else:
                merged.append([s, e])
        self.starts = []
        self.blocks = []
        cur = 0
        for s, e in merged:
            base = ((cur + 15) & ~15) + (s % 16)
            self.starts.append(s)
            self.blocks.append((s, e, base))
            cur = base + (e - s)
        self.size = ((cur + 15) & ~15) + 16

    def __call__(self, off, size):
        i = bisect.bisect_right(self.starts, off) - 1
        if i < 0:
            raise KeyError(off)
        s, e, base = self.blocks[i]
        if off + size > e:
            raise KeyError(off)
        return base + (off - s)


class Identity:
    def __init__(self, size):
        self.size = size

    def __call__(self, off, size):
        return off


class Emitter:
    def __init__(self, func, data, lift=False):
        self.f = func
        self.lift = lift
        self.regfile = data["register_file"]
        self.reg_names = data["registers"]
        self.entry = func["entry"]
        self.addrs = {i["addr"] for i in func["instructions"]}
        self.callother = func["callother"]
        self.stats = {"instructions": len(func["instructions"]), "pcode": 0, "atomics": 0, "calls": 0,
                      "tailcalls": 0, "indirect": 0, "switches": 0, "fused": 0, "simd": 0, "fp": 0}
        self.R = None
        self.U = None
        self.arg_escape = False

    # --- registers -------------------------------------------------------------------------------------------
    def regoff(self, name):
        if name not in self.regfile:
            raise blocked("ABI", "register %s not in the dump (a v2 dump is needed)" % name)
        return self.regfile[name]

    def reg(self, name, size=None):
        off, sz = self.regoff(name)
        return self.acc(("register", off, size or sz))[1]

    def name_of(self, v):
        return self.reg_names.get("%d:%d" % (v[1], v[2]))

    def acc(self, v):
        space, off, size = v
        layout = self.R if space == "register" else self.U
        try:
            return ("R" if space == "register" else "U"), layout(off, size)
        except KeyError:
            raise blocked("ABI", "%s %#x:%d outside the layout" % (space, off, size))

    def check_register(self, v):
        name = self.name_of(v) or ""
        if name.startswith("FS_OFFSET") or name.startswith("GS_OFFSET"):
            raise blocked("MEMORY", "fs/gs segment access (TLS)")
        if name == "MXCSR":
            raise blocked("INSTRUCTION", "MXCSR as a register (ldmxcsr/stmxcsr): the CPU's MXCSR is not R[]")

    def build_layouts(self, extra, instructions=None):
        regs, uniq = set(extra), set()
        for ins in (instructions if instructions is not None else self.f["instructions"]):
            fix = self.scalar_fix(ins)
            for name in (fix[:2] if fix else ()):  # the scalar fix reads src1 and writes dst whole
                off = self.regoff(name)[0]
                regs.add((off, off + 16))
            for o in ins["pcode"]:
                for v in [o["out"]] + o["in"]:
                    if v is None:
                        continue
                    if v[0] == "register":
                        self.check_register(v)
                        regs.add((v[1], v[1] + v[2]))
                    elif v[0] == "unique":
                        uniq.add((v[1], v[1] + v[2]))
        if self.lift:
            self.R = Identity(max([e for _, e in regs] + [0]) + 64)
            self.U = Identity(max([e for _, e in uniq] + [0]) + 64)
        else:
            self.R = Layout(regs)
            self.U = Layout(uniq or {(0, 8)})

    # --- values ----------------------------------------------------------------------------------------------
    def read(self, v):
        space, off, n = v
        if space == "const":
            if n > 8:
                raise blocked("INSTRUCTION", "a %d-byte constant" % n)
            return "0x%xull" % (off & ((1 << (8 * n)) - 1))
        if space in ("register", "unique"):
            a, o = self.acc(v)
            return ("ld(%s, %d, %d)" % (a, o, n)) if n <= 8 else ("ldv<%d>(%s + %d)" % (n, a, o))
        if space == "ram":
            if n <= 8:
                return "mload(B + 0x%xull, %d)" % (off, n)
            return "ldv<%d>(reinterpret_cast<const u8*>(B + 0x%xull))" % (n, off)
        raise blocked("MEMORY", "space %s" % space)

    def write(self, v, expr):
        space, off, n = v
        if space in ("register", "unique"):
            a, o = self.acc(v)
            return ("st(%s, %d, %d, %s);" % (a, o, n, expr)) if n <= 8 else ("stv<%d>(%s + %d, %s);" % (n, a, o, expr))
        if space == "ram":
            if n <= 8:
                return "mstore(B + 0x%xull, %d, %s);" % (off, n, expr)
            return "stv<%d>(reinterpret_cast<u8*>(B + 0x%xull), %s);" % (n, off, expr)
        raise blocked("MEMORY", "write to space %s" % space)

    # --- control flow ----------------------------------------------------------------------------------------
    def target(self, ins, idx, v):
        space, off, _ = v
        if space == "ram":
            if self.lift and off == ins["addr"] + ins["len"]:
                return "P_%x_next" % ins["addr"]  # CMOVcc and the like skip to the next instruction
            return ("L_%x" % off) if off in self.addrs else None
        if space == "const":
            return "P_%x_%d" % (ins["addr"], idx + signed(off))
        raise blocked("CONTROL_FLOW", "branch target space %s" % space)

    def call_code(self, target_expr, tail):
        """A call through recomp_call: argument registers and XMM0-7 from R[], the results back."""
        rsp = self.reg("RSP")
        parts = ["{ CallIO c_;"]
        for i, name in enumerate(ARGS):
            parts.append("c_.gpr[%d] = ld(R, %d, 8);" % (i, self.reg(name, 8)))
        for k in range(8):
            parts.append("std::memcpy(c_.xmm[%d], R + %d, 16);" % (k, self.reg("XMM%d" % k, 16)))
        parts.append("c_.target = %s; c_.args = ld(R, %d, 8) + 8; recomp_call(&c_);" % (target_expr, rsp))
        parts.append("st(R, %d, 8, c_.out_rax); st(R, %d, 8, c_.out_rdx);" % (self.reg("RAX", 8), self.reg("RDX", 8)))
        for k in range(2):
            parts.append("std::memcpy(R + %d, c_.out_xmm[%d], 16);" % (self.reg("XMM%d" % k, 16), k))
        if tail:
            parts.append("goto L_return; }")
        else:
            parts.append("st(R, %d, 8, ld(R, %d, 8) + 8); }" % (rsp, rsp))  # pops the return slot the CALL pushed
        return " ".join(parts)

    def tail_call(self, target_expr):
        if self.lift:
            raise blocked("CONTROL_FLOW", "control flow in a lifted instruction")
        self.stats["tailcalls"] += 1
        return self.call_code(target_expr, True)

    # --- one P-code op ---------------------------------------------------------------------------------------
    def op(self, ins, idx, o):
        name = o["op"]
        out = o["out"]
        ins_ = o["in"]
        n = out[2] if out else 0
        wide = n > 8 or any(v is not None and v[2] > 8 for v in ins_[:2])
        if name == "COPY":
            if out[2] != ins_[0][2]:
                raise blocked("INSTRUCTION", "COPY of different sizes")
            return self.write(out, self.read(ins_[0]))
        if name == "LOAD":
            if n <= 8:
                return self.write(out, "mload(%s, %d)" % (self.read(ins_[1]), n))
            return self.write(out, "ldv<%d>(reinterpret_cast<const u8*>(%s))" % (n, self.read(ins_[1])))
        if name == "STORE":
            val = ins_[2]
            if val[2] <= 8:
                return "mstore(%s, %d, %s);" % (self.read(ins_[1]), val[2], self.read(val))
            return "stv<%d>(reinterpret_cast<u8*>(%s), %s);" % (val[2], self.read(ins_[1]), self.read(val))
        if name == "INT_ZEXT":
            m = ins_[0][2]
            if n <= 8:
                return self.write(out, "(%s & mask(%d))" % (self.read(ins_[0]), m))
            if m <= 8:
                return self.write(out, "zx<%d>(%s, %d)" % (n, self.read(ins_[0]), m))
            return self.write(out, "zxv<%d, %d>(%s)" % (n, m, self.read(ins_[0])))
        if name == "INT_SEXT":
            m = ins_[0][2]
            if n <= 8:
                return self.write(out, "(static_cast<u64>(sx(%s, %d)) & mask(%d))" % (self.read(ins_[0]), m, n))
            if m <= 8:
                return self.write(out, "sxw<%d>(%s, %d)" % (n, self.read(ins_[0]), m))
            raise blocked("INSTRUCTION", "INT_SEXT of a vector")
        if name == "SUBPIECE":
            src = ins_[0]
            shift = ins_[1][1]
            if src[2] > 8 or n > 8:
                if src[0] not in ("register", "unique"):
                    raise blocked("INSTRUCTION", "SUBPIECE of a wide %s value" % src[0])
                a, off = self.acc(src)
                return self.write(out, ("ld(%s, %d, %d)" % (a, off + shift, n)) if n <= 8
                                  else ("ldv<%d>(%s + %d)" % (n, a, off + shift)))
            return self.write(out, "((%s >> %d) & mask(%d))" % (self.read(src), 8 * shift, n))
        if name == "PIECE":
            hi, lo = ins_[0], ins_[1]
            if n <= 8:
                return self.write(out, "((((%s) << %d) | (%s)) & mask(%d))" % (self.read(hi), 8 * lo[2], self.read(lo), n))
            return self.write(out, "[&]{ const auto lo_ = %s; const auto hi_ = %s; return piece<%d, %d, %d>(&lo_, &hi_); }()"
                              % (self.read(lo), self.read(hi), n, lo[2], hi[2]))
        if name in ("INT_AND", "INT_OR", "INT_XOR") and wide:
            fn = {"INT_AND": "band", "INT_OR": "bor", "INT_XOR": "bxor"}[name]
            return self.write(out, "%s<%d>(%s, %s)" % (fn, n, self.read(ins_[0]), self.read(ins_[1])))
        if name == "INT_NEGATE" and wide:
            return self.write(out, "bnot<%d>(%s)" % (n, self.read(ins_[0])))
        if name in CMP_OPS and ins_[0][2] > 8:
            return self.write(out, "(%sbeq<%d>(%s, %s) ? 1 : 0)" % ("" if name == "INT_EQUAL" else "!", ins_[0][2],
                                                                   self.read(ins_[0]), self.read(ins_[1])))
        if wide and n == 16 and name in ("INT_ADD", "INT_SUB", "INT_MULT", "INT_LEFT", "INT_RIGHT", "INT_SRIGHT",
                                          "INT_2COMP", "INT_DIV", "INT_REM", "INT_SDIV", "INT_SREM"):
            return self.op128(name, out, ins_)
        if wide and name in ("INT_LESS", "INT_SLESS", "INT_LESSEQUAL", "INT_SLESSEQUAL") and ins_[0][2] == 16:
            sym = "<" if name in ("INT_LESS", "INT_SLESS") else "<="
            cast = "static_cast<s128>" if name.startswith("INT_S") else ""
            return self.write(out, "((%s(w(%s)) %s %s(w(%s))) ? 1 : 0)" % (cast, self.read(ins_[0]), sym, cast,
                                                                          self.read(ins_[1])))
        if wide:
            raise blocked("INSTRUCTION", "%s on a %d-byte value" % (name, max([n] + [v[2] for v in ins_ if v])))
        if name in INT_OPS:
            return self.write(out, "((%s %s %s) & mask(%d))" % (self.read(ins_[0]), INT_OPS[name], self.read(ins_[1]), n))
        if name in CMP_OPS:
            m = ins_[0][2]
            return self.write(out, "(((%s & mask(%d)) %s (%s & mask(%d))) ? 1 : 0)"
                              % (self.read(ins_[0]), m, CMP_OPS[name], self.read(ins_[1]), m))
        if name in UNSIGNED_CMP:
            m = ins_[0][2]
            return self.write(out, "(((%s & mask(%d)) %s (%s & mask(%d))) ? 1 : 0)"
                              % (self.read(ins_[0]), m, UNSIGNED_CMP[name], self.read(ins_[1]), m))
        if name in SIGNED_CMP:
            m = ins_[0][2]
            return self.write(out, "((sx(%s, %d) %s sx(%s, %d)) ? 1 : 0)"
                              % (self.read(ins_[0]), m, SIGNED_CMP[name], self.read(ins_[1]), m))
        if name == "INT_LEFT":
            return self.write(out, "((%s) >= %d ? 0 : ((%s << (%s)) & mask(%d)))"
                              % (self.read(ins_[1]), 8 * n, self.read(ins_[0]), self.read(ins_[1]), n))
        if name == "INT_RIGHT":
            return self.write(out, "((%s) >= %d ? 0 : (((%s) & mask(%d)) >> (%s)))"
                              % (self.read(ins_[1]), 8 * ins_[0][2], self.read(ins_[0]), ins_[0][2], self.read(ins_[1])))
        if name == "INT_SRIGHT":
            return self.write(out, "(static_cast<u64>(sx(%s, %d) >> ((%s) >= %d ? %d : (%s))) & mask(%d))"
                              % (self.read(ins_[0]), ins_[0][2], self.read(ins_[1]), 8 * ins_[0][2],
                                 8 * ins_[0][2] - 1, self.read(ins_[1]), n))
        if name in ("INT_DIV", "INT_REM"):
            sym = "/" if name == "INT_DIV" else "%"
            return self.write(out, "[&]{ const u64 a_ = %s & mask(%d), b_ = %s & mask(%d); if (b_ == 0) recomp_fault(); "
                                   "return (a_ %s b_) & mask(%d); }()" % (self.read(ins_[0]), ins_[0][2], self.read(ins_[1]),
                                                                        ins_[1][2], sym, n))
        if name in ("INT_SDIV", "INT_SREM"):
            sym = "/" if name == "INT_SDIV" else "%"
            m = ins_[0][2]
            return self.write(out, "[&]{ const s64 a_ = sx(%s, %d), b_ = sx(%s, %d); if (b_ == 0 || (b_ == -1 && a_ == "
                                   "sx(1ull << (8 * %d - 1), %d))) recomp_fault(); return static_cast<u64>(a_ %s b_) & mask(%d); }()"
                              % (self.read(ins_[0]), m, self.read(ins_[1]), m, m, m, sym, n))
        if name == "INT_NEGATE":
            return self.write(out, "((~%s) & mask(%d))" % (self.read(ins_[0]), n))
        if name == "INT_2COMP":
            return self.write(out, "((0 - %s) & mask(%d))" % (self.read(ins_[0]), n))
        if name == "INT_CARRY":
            return self.write(out, "carry(%s, %s, %d)" % (self.read(ins_[0]), self.read(ins_[1]), ins_[0][2]))
        if name == "INT_SCARRY":
            return self.write(out, "scarry(%s, %s, %d)" % (self.read(ins_[0]), self.read(ins_[1]), ins_[0][2]))
        if name == "INT_SBORROW":
            return self.write(out, "sborrow(%s, %s, %d)" % (self.read(ins_[0]), self.read(ins_[1]), ins_[0][2]))
        if name == "BOOL_NEGATE":
            return self.write(out, "(%s ? 0 : 1)" % self.read(ins_[0]))
        if name in ("BOOL_AND", "BOOL_OR", "BOOL_XOR"):
            sym = {"BOOL_AND": "&", "BOOL_OR": "|", "BOOL_XOR": "^"}[name]
            return self.write(out, "((%s %s %s) & 1)" % (self.read(ins_[0]), sym, self.read(ins_[1])))
        if name == "POPCOUNT":
            return self.write(out, "static_cast<u64>(__builtin_popcountll(%s & mask(%d)))" % (self.read(ins_[0]), ins_[0][2]))
        if name == "LZCOUNT":
            m = ins_[0][2]
            return self.write(out, "[&]{ const u64 a_ = %s & mask(%d); return a_ == 0 ? %du : static_cast<u64>(__builtin_clzll(a_) - %d); }()"
                              % (self.read(ins_[0]), m, 8 * m, 64 - 8 * m))
        if name.startswith("FLOAT") or name in ("INT2FLOAT", "TRUNC"):
            self.stats["fp"] += 1
            return self.float_op(name, out, ins_)
        if name == "BRANCH":
            t = self.target(ins, idx, ins_[0])
            if t is not None:
                return "goto %s;" % t
            if ins_[0][1] in ins.get("entries", []):
                return self.tail_call("B + 0x%xull" % ins_[0][1])
            raise blocked("CONTROL_FLOW", "jump out of the function to %#x (not a function entry)" % ins_[0][1])
        if name == "CBRANCH":
            t = self.target(ins, idx, ins_[0])
            if t is not None:
                return "if (%s) goto %s;" % (self.read(ins_[1]), t)
            if ins_[0][1] in ins.get("entries", []):
                return "if (%s) %s" % (self.read(ins_[1]), self.tail_call("B + 0x%xull" % ins_[0][1]))
            raise blocked("CONTROL_FLOW", "conditional jump out of the function to %#x" % ins_[0][1])
        if name == "BRANCHIND":
            if self.lift:
                raise blocked("CONTROL_FLOW", "control flow in a lifted instruction")
            dest = self.read(ins_[0])
            targets = ins.get("targets", [])
            if targets:
                bad = [t for t in targets if t not in self.addrs]
                if bad:
                    raise blocked("CONTROL_FLOW", "switch target %#x outside the body" % bad[0])
                self.stats["switches"] += 1
                cases = " ".join("case 0x%xull: goto L_%x;" % (t, t) for t in targets)
                return "switch (%s - B) { %s default: recomp_fault(); }" % (dest, cases)
            self.stats["indirect"] += 1
            return ("{ const u64 t_ = %s; if (t_ - B - 0x%xull < 0x%xull) recomp_fault(); %s }"
                    % (dest, self.entry, self.f["size"], self.tail_call("t_")))
        if name == "CALL":
            if self.lift:
                raise blocked("CONTROL_FLOW", "control flow in a lifted instruction")
            space, off, _ = ins_[0]
            if space != "ram":
                raise blocked("EXTERNAL_CALL", "CALL to %s" % space)
            self.stats["calls"] += 1
            return self.call_code("B + 0x%xull" % off, False)
        if name == "CALLIND":
            if self.lift:
                raise blocked("CONTROL_FLOW", "control flow in a lifted instruction")
            self.stats["calls"] += 1
            self.stats["indirect"] += 1
            return self.call_code(self.read(ins_[0]), False)
        if name == "RETURN":
            if self.lift:
                raise blocked("CONTROL_FLOW", "control flow in a lifted instruction")
            return "goto L_return;"
        raise blocked("INSTRUCTION", "P-code %s" % name)

    def op128(self, name, out, ins_):
        a = "w(%s)" % self.read_as16(ins_[0])
        if name == "INT_2COMP":
            return self.write(out, "vw(0 - %s)" % a)
        b = "w(%s)" % self.read_as16(ins_[1])
        if name in ("INT_DIV", "INT_REM"):
            return self.write(out, "udiv128(%s, %s, %s)" % (self.read_as16(ins_[0]), self.read_as16(ins_[1]),
                                                             "true" if name == "INT_REM" else "false"))
        if name in ("INT_SDIV", "INT_SREM"):
            return self.write(out, "sdiv128(%s, %s, %s)" % (self.read_as16(ins_[0]), self.read_as16(ins_[1]),
                                                             "true" if name == "INT_SREM" else "false"))
        if name in ("INT_LEFT", "INT_RIGHT", "INT_SRIGHT"):
            sh = "static_cast<u64>(%s)" % b
            if name == "INT_LEFT":
                return self.write(out, "vw(%s >= 128 ? 0 : (%s << %s))" % (sh, a, sh))
            if name == "INT_RIGHT":
                return self.write(out, "vw(%s >= 128 ? 0 : (%s >> %s))" % (sh, a, sh))
            return self.write(out, "vw(static_cast<u128>(static_cast<s128>(%s) >> (%s >= 128 ? 127 : %s)))" % (a, sh, sh))
        sym = {"INT_ADD": "+", "INT_SUB": "-", "INT_MULT": "*"}[name]
        return self.write(out, "vw(%s %s %s)" % (a, sym, b))

    def read_as16(self, v):
        if v[2] == 16:
            return self.read(v)
        if v[2] <= 8:
            return "zx<16>(%s, %d)" % (self.read(v), v[2])
        raise blocked("INSTRUCTION", "a %d-byte integer operand" % v[2])

    def float_op(self, name, out, ins_):
        n = out[2] if out else 0
        m = ins_[0][2]
        if name in FLOAT_BIN:
            if n not in (4, 8) or m != n or ins_[1][2] != n:
                raise blocked("INSTRUCTION", "%s on %d bytes" % (name, n))
            return self.write(out, "f%s%d(%s, %s)" % (FLOAT_BIN[name], n, self.read(ins_[0]), self.read(ins_[1])))
        if name in FLOAT_CMP:
            if m not in (4, 8):
                raise blocked("INSTRUCTION", "%s on %d bytes" % (name, m))
            return self.write(out, "%s%d(%s, %s)" % (FLOAT_CMP[name], m, self.read(ins_[0]), self.read(ins_[1])))
        if name == "FLOAT_NAN":
            if m not in (4, 8):
                raise blocked("INSTRUCTION", "FLOAT_NAN on %d bytes" % m)
            return self.write(out, "fnan%d(%s)" % (m, self.read(ins_[0])))
        if name == "FLOAT_NEG" and n in (4, 8):
            return self.write(out, "(%s ^ 0x%xull)" % (self.read(ins_[0]), 1 << (8 * n - 1)))
        if name == "FLOAT_ABS" and n in (4, 8):
            return self.write(out, "(%s & 0x%xull)" % (self.read(ins_[0]), (1 << (8 * n - 1)) - 1))
        if name == "FLOAT_SQRT" and n in (4, 8):
            return self.write(out, "fsqrt%d(%s)" % (n, self.read(ins_[0])))
        if name == "INT2FLOAT" and n in (4, 8) and m in (4, 8):
            return self.write(out, "i2f%d_%d(%s)" % (n, m, self.read(ins_[0])))
        if name == "FLOAT2FLOAT" and {n, m} == {4, 8}:
            return self.write(out, "f2f%d_%d(%s)" % (m, n, self.read(ins_[0])))
        if name == "TRUNC" and n in (4, 8) and m in (4, 8):
            return self.write(out, "trunc%d_%d(%s)" % (m, n, self.read(ins_[0])))
        raise blocked("INSTRUCTION", "P-code %s (%d -> %d bytes)" % (name, m, n))

    # --- CALLOTHER: the AVX operations, as the CPU's own instruction ------------------------------------------
    def simd(self, ins, o):
        nm = self.callother.get(str(o["in"][0][1]))
        args = o["in"][1:]
        out = o["out"]
        wides = [a for a in args if a[0] != "const"]
        consts = [a for a in args if a[0] == "const"]
        if nm in ("vmovlps_avx", "vmovhps_avx", "vmovlpd_avx", "vmovhpd_avx"):
            return self.simd_movlh(nm, out, args)
        if nm not in SIMD:
            raise blocked("INSTRUCTION", "CALLOTHER %s in %s" % (nm, ins["text"]))
        mnem, form = SIMD[nm]
        if nm in ("vpermilps_avx", "vpermilpd_avx") and consts:
            form = "ai"
        if nm in SHIFTS and not consts:
            form = "ab"
        want_w = {"a": 1, "ab": 2, "abi": 2, "ai": 1, "abc": 3, "g_a": 1, "g_ai": 1}[form]
        want_c = 1 if form.endswith("i") else 0
        if len(wides) != want_w or len(consts) != want_c or len(args) != want_w + want_c or \
                any(a[0] == "const" for a in args[:want_w]):
            raise blocked("INSTRUCTION", "unexpected operands of %s in %s" % (nm, ins["text"]))
        width = wides[0][2]
        if width not in (16, 32) or any(a[2] != width for a in wides):
            if not (nm in SHIFTS and len(wides) == 2 and wides[1][2] == 16):
                raise blocked("INSTRUCTION", "%s with %s-byte operands" % (nm, [a[2] for a in wides]))
        imm = consts[0][1] if consts else None
        if nm == "vinsertps_avx" and "ptr" in ins["text"]:
            imm &= 0x3f  # the memory form ignores imm[7:6]: the element comes from memory (SLEIGH zero-extends it)
        T, to, back = ("__m128", "m4", "v4") if width == 16 else ("__m256", "m8", "v8")
        names = ["a_", "b_", "c_"][:want_w]
        decl = []
        for nmv, a in zip(names, wides):
            t, cv = (T, to) if a[2] == width else ("__m128", "m4")
            decl.append("%s %s = %s(%s);" % (t, nmv, cv, self.read(a)))
        decl = " ".join(decl)
        self.stats["simd"] += 1
        regs = ["%%%d" % (k + 1) for k in range(want_w)]
        operands = list(reversed(regs))  # AT&T: the last Intel source first
        if imm is not None:
            operands = ["$0x%x" % imm] + operands
        inputs = ", ".join("\"x\"(%s)" % nmv for nmv in names)
        if form.startswith("g_"):
            if out is None or out[2] > 8:
                raise blocked("INSTRUCTION", "%s result of %s bytes" % (nm, out and out[2]))
            dst = "%0" if mnem == "vpextrq" else "%k0"
            text = "%%{vex3%%} %s %s, %s" % (mnem, ", ".join(operands), dst)
            return self.write(out, "[&]{ %s u64 r_; __asm__ volatile(\"%s\" : \"=r\"(r_) : %s); return r_ & mask(%d); }()"
                              % (decl, text, inputs, out[2]))
        if out is None or out[2] != width:
            raise blocked("INSTRUCTION", "%s result of %s bytes" % (nm, out and out[2]))
        text = "%%{vex3%%} %s %s, %%0" % (mnem, ", ".join(operands))
        return self.write(out, "[&]{ %s %s r_; __asm__ volatile(\"%s\" : \"=x\"(r_) : %s); return %s(r_); }()"
                          % (decl, T, text, inputs, back))

    def simd_movlh(self, nm, out, args):
        lo = nm in ("vmovlps_avx", "vmovlpd_avx")
        if len(args) == 1 and args[0][2] == 16 and out and out[2] == 8:  # store form: one quadword of the register
            return self.write(out, "[&]{ const auto a_ = %s; u64 r_; std::memcpy(&r_, a_.b + %d, 8); return r_; }()"
                              % (self.read(args[0]), 0 if lo else 8))
        if len(args) == 2 and args[0][2] == 16 and args[1][2] == 8 and out and out[2] == 16:  # load form
            return self.write(out, "[&]{ auto r_ = %s; const u64 q_ = %s; std::memcpy(r_.b + %d, &q_, 8); return r_; }()"
                              % (self.read(args[0]), self.read(args[1]), 0 if lo else 8))
        raise blocked("INSTRUCTION", "unexpected operands of %s" % nm)

    # --- the lane ops of one packed instruction -> the packed instruction -------------------------------------
    def fuse(self, ins, ops, i):
        o = ops[i]
        if o["op"] not in FLOAT_BIN or o["out"] is None:
            return None
        n = o["out"][2]
        if n not in (4, 8):
            return None
        lanes = 16 // n
        group = ops[i:i + lanes]
        if len(group) != lanes or any(g["op"] != o["op"] or g["out"] is None or g["out"][2] != n for g in group):
            return None
        bases = []
        for slot in range(3):
            vs = [g["out"] if slot == 0 else g["in"][slot - 1] for g in group]
            if any(v[0] not in ("register", "unique") or v[2] != n for v in vs):
                return None
            if any(vs[k][0] != vs[0][0] or vs[k][1] != vs[0][1] + k * n for k in range(lanes)):
                return None
            bases.append((vs[0][0], vs[0][1], 16))
        d = bases[0]  # each source is the destination itself or does not overlap it: lane order cannot matter
        for s in bases[1:]:
            if s[0] == d[0] and s[1] != d[1] and s[1] < d[1] + 16 and d[1] < s[1] + 16:
                return None
        try:
            for b in bases:
                self.acc(b)
        except Unsupported:
            return None
        mnem = {"FLOAT_ADD": "vadd", "FLOAT_SUB": "vsub", "FLOAT_MULT": "vmul", "FLOAT_DIV": "vdiv"}[o["op"]]
        mnem += "ps" if n == 4 else "pd"
        self.stats["fused"] += 1
        expr = ("[&]{ __m128 a_ = m4(%s), b_ = m4(%s), r_; __asm__ volatile(\"%%{vex3%%} %s %%2, %%1, %%0\" : \"=x\"(r_) "
                ": \"x\"(a_), \"x\"(b_)); return v4(r_); }()" % (self.read(bases[1]), self.read(bases[2]), mnem))
        return lanes, self.write(bases[0], expr)

    # --- the stack frame: private stack depth and the caller's stack arguments ---------------------------------
    def frame(self):
        rsp = ("register",) + tuple(self.regoff("RSP"))
        rbp = ("register",) + tuple(self.regoff("RBP"))
        at = {self.entry: (0, None)}
        lowest, arg_end = 0, 0
        cur = (0, None)
        key = lambda v: tuple(v) if v else None
        for ins in self.f["instructions"]:
            state = at.get(ins["addr"], cur)
            if state is None:
                raise blocked("ABI", "stack depth unknown at %#x" % ins["addr"])
            d, frame_ptr = state
            env = {rsp: d}
            if frame_ptr is not None:
                env[rbp] = frame_ptr
            for o in ins["pcode"]:
                out, ins_ = o["out"], o["in"]
                val = None
                used = [key(v) for v in ins_ if v and key(v) in env]
                if o["op"] == "COPY" and key(ins_[0]) in env:
                    val = env[key(ins_[0])]
                elif o["op"] in ("INT_ADD", "INT_SUB") and key(ins_[0]) in env and ins_[1][0] == "const":
                    c = signed(ins_[1][1]) if ins_[1][2] == 8 else ins_[1][1]
                    val = env[key(ins_[0])] + (c if o["op"] == "INT_ADD" else -c)
                elif o["op"] == "INT_ADD" and key(ins_[1]) in env and ins_[0][0] == "const":  # [RSP + d]: #d first
                    c = signed(ins_[0][1]) if ins_[0][2] == 8 else ins_[0][1]
                    val = env[key(ins_[1])] + c
                elif o["op"] in ("LOAD", "STORE") and key(ins_[1]) in env:
                    a = env[key(ins_[1])]
                    size = out[2] if o["op"] == "LOAD" else ins_[2][2]
                    if a >= 8:
                        arg_end = max(arg_end, a + size - 8)
                    elif o["op"] == "STORE" and a + size > 0:
                        raise blocked("ABI", "store into the return address slot at %#x" % ins["addr"])
                    if o["op"] == "STORE" and key(ins_[2]) in env:
                        self.note_escape(env[key(ins_[2])])
                elif used and out is not None and out[2] == 8:
                    for k in used:  # a stack address turned into another address (flags of SUB RSP are not one)
                        self.note_escape(env[k])
                if out is not None:
                    k = key(out)
                    if val is not None:
                        env[k] = val
                    else:
                        env.pop(k, None)
                        if out[0] == "register" and out[1] < rsp[1] + 8 and rsp[1] < out[1] + out[2]:
                            raise blocked("ABI", "dynamic stack pointer at %#x (%s)" % (ins["addr"], ins["text"]))
                        if out[0] == "register" and out[1] < rbp[1] + 8 and rbp[1] < out[1] + out[2]:
                            env.pop(rbp, None)
            if rsp not in env:
                raise blocked("ABI", "stack pointer lost at %#x" % ins["addr"])
            for k, v in env.items():  # only RSP/RBP are followed across instructions: a stack address kept elsewhere
                if k[0] == "register" and k not in (rsp, rbp):  # (lea rax, [rsp + x]) is not
                    self.note_escape(v)
            d_end = env[rsp]
            if any(o["op"] in ("CALL", "CALLIND") for o in ins["pcode"]):
                d_end += 8
            fp_end = env.get(rbp)
            lowest = min(lowest, d_end, d)
            for o in ins["pcode"]:
                if o["op"] in ("BRANCH", "CBRANCH") and o["in"][0][0] == "ram" and o["in"][0][1] in self.addrs:
                    t = o["in"][0][1]
                    if t in at and at[t][0] != d_end:
                        raise blocked("ABI", "stack depth differs at %#x" % t)
                    at[t] = (d_end, fp_end)
            ends = any(o["op"] in ("RETURN", "BRANCH", "BRANCHIND") for o in ins["pcode"]) and \
                not any(o["op"] == "CBRANCH" for o in ins["pcode"])
            cur = None if ends else (d_end, fp_end)
        if self.arg_escape:
            arg_end = max(arg_end, 256)  # the caller's arguments may be read through an address not followed
        return -lowest, (arg_end + 7) & ~7

    def note_escape(self, offset):
        """A stack address leaves the analysis. Below the entry rsp it points into the private frame (S, real memory):
        harmless. At or above it, it may reach the caller's stack arguments: the whole window is copied."""
        if offset >= 0:
            self.arg_escape = True

    # --- one function ----------------------------------------------------------------------------------------
    def run(self):
        f = self.f
        extra = []
        for name in ARGS + ("RSP", "RBP"):
            off, _ = self.regoff(name)
            extra.append((off, off + 8))
        for k in range(8):
            off, _ = self.regoff("XMM%d" % k)
            extra.append((off, off + 16))
        self.build_layouts(extra)
        if self.reads_af():
            raise blocked("INSTRUCTION", "reads AF, which SLEIGH does not compute for ADD/SUB/CMP/INC/DEC/NEG")
        depth, arg_bytes = self.frame()
        stack_bytes = (depth + 64 + 15) & ~15
        if arg_bytes > 256:
            raise blocked("ABI", "%d bytes of stack arguments" % arg_bytes)
        body = self.emit_body(f["instructions"])
        # above the top: the caller's stack arguments, and (with calls) the 256 bytes recomp_call copies from the rsp
        above = max(arg_bytes, 256 if self.stats["calls"] + self.stats["tailcalls"] else 0)
        L = ["SSPC_ENTRY(%x)" % self.entry,
             'extern "C" __attribute__((used)) SYSV void body_%x(RegIO* io) {' % self.entry,
             "    const u64 B = recomp_image_base;",
             "    alignas(64) u8 R[%d] = {};" % self.R.size,
             "    alignas(64) u8 U[%d];" % self.U.size,
             "    alignas(16) u8 S[%d];  // private stack: %d bytes of frame; the caller's %d bytes of stack arguments"
             % (stack_bytes + above + 16, depth, arg_bytes)]
        for i, name in enumerate(ARGS):
            L.append("    st(R, %d, 8, io->gpr[%d]);" % (self.reg(name, 8), i))
        for k in range(8):
            L.append("    std::memcpy(R + %d, io->xmm[%d], 16);" % (self.reg("XMM%d" % k, 16), k))
        L.append("    st(R, %d, 8, reinterpret_cast<u64>(S) + %d);  // rsp -> the return address slot"
                 % (self.reg("RSP", 8), stack_bytes - 8))
        if arg_bytes:
            L.append("    std::memcpy(S + %d, reinterpret_cast<const void*>(io->args), %d);" % (stack_bytes, arg_bytes))
        L.append("    goto L_%x;" % self.entry)
        L.extend(body)
        L.append("L_return:")
        L.append("    io->out_rax = ld(R, %d, 8);" % self.reg("RAX", 8))
        L.append("    io->out_rdx = ld(R, %d, 8);" % self.reg("RDX", 8))
        for k in range(2):
            L.append("    std::memcpy(io->out_xmm[%d], R + %d, 16);" % (k, self.reg("XMM%d" % k, 16)))
        L.append("}")
        self.stats["stack"] = depth
        self.stats["arg_bytes"] = arg_bytes
        self.stats["frame_R"] = self.R.size
        self.stats["frame_U"] = self.U.size
        return "\n".join(L)

    def scalar_fix(self, ins):
        """SLEIGH lifts the VEX scalar forms (VMULSS XMM0,XMM1,m32 ...) as ZMMd = INT_ZEXT result: bits above the
        result zeroed. The hardware copies them from the first source (src1[127:32] / [127:64]) and zeroes only
        bits 255:128 (found by --lift-check at 0x11cce6). Returns (dst XMM, src1 XMM, result bytes) or None."""
        m = re.match(r"^V(ADD|SUB|MUL|DIV|MIN|MAX|SQRT|RSQRT|RCP|ROUND)S([SD])$", ins["mnemonic"].upper())
        cvt = re.match(r"^VCVT(SI2SS|SI2SD|SS2SD|SD2SS|USI2SS|USI2SD)$", ins["mnemonic"].upper())
        if not m and not cvt:
            return None
        ops = re.match(r"^\S+\s+(XMM\d+),(XMM\d+),", ins["text"])
        if ops is None:
            raise blocked("INSTRUCTION", "scalar VEX form without an XMM first source: %s" % ins["text"])
        n = (4 if m.group(2) == "S" else 8) if m else (4 if cvt.group(1).endswith("SS") else 8)
        return ops.group(1), ops.group(2), n

    def reads_af(self):
        """SLEIGH does not compute AF for ADD/SUB/CMP/INC/DEC/NEG (--lift-check): a function whose code reads AF
        (LAHF, PUSHF, ...) would see a stale value. Flags are not part of the System V interface, so AF without a
        reader in the function is unobservable."""
        af = self.regfile.get("AF")
        return af is not None and any(v and v[0] == "register" and v[1] == af[0]
                                      for ins in self.f["instructions"] for o in ins["pcode"] for v in o["in"])

    def emit_instruction(self, ins):
        fix = self.scalar_fix(ins)
        body = self.emit_ops(ins)
        if fix is None:
            return body
        d, s1, n = self.reg(fix[0], 16), self.reg(fix[1], 16), fix[2]
        return (["    { const V<16> shi_ = ldv<16>(R + %d);  // %s[127:%d] (SLEIGH zeroes it)" % (s1, fix[1], 8 * n)] +
                body + ["    std::memcpy(R + %d, shi_.b + %d, %d); }" % (d + n, n, 16 - n)])

    def emit_ops(self, ins):
        L = []
        ops = ins["pcode"]
        self.stats["pcode"] += len(ops)
        internal = set()
        for i, o in enumerate(ops):
            if o["op"] in ("BRANCH", "CBRANCH") and o["in"][0][0] == "const":
                internal.add(i + signed(o["in"][0][1]))
        lock_at = next((i for i, o in enumerate(ops) if o["op"] == "CALLOTHER"
                        and self.callother.get(str(o["in"][0][1])) == "LOCK"), None)
        i = 0
        while i < len(ops):
            if i in internal:
                L.append("P_%x_%d:;" % (ins["addr"], i))
            o = ops[i]
            if lock_at is not None and i == lock_at:
                if self.lift:
                    raise blocked("INSTRUCTION", "LOCK in a lifted instruction")
                unlock_at = next(j for j in range(i + 1, len(ops)) if ops[j]["op"] == "CALLOTHER"
                                 and self.callother.get(str(ops[j]["in"][0][1])) == "UNLOCK")
                for s in self.emit_lock_cmpxchg(ins, ops, i, unlock_at):
                    L.append("    " + s)
                i = unlock_at + 1
                continue
            fused = None if any(k in internal for k in range(i + 1, i + 4)) else self.fuse(ins, ops, i)
            if fused is not None:
                L.append("    " + fused[1])
                i += fused[0]
                continue
            L.append("    " + (self.simd(ins, o) if o["op"] == "CALLOTHER" else self.op(ins, i, o)))
            i += 1
        if len(ops) in internal:
            L.append("P_%x_%d:;" % (ins["addr"], len(ops)))
        return L

    def emit_body(self, instructions):
        L = []
        for ins in instructions:
            L.append("L_%x:  // %s" % (ins["addr"], ins["text"]))
            L.extend(self.emit_instruction(ins))
            nxt = ins["addr"] + ins["len"]
            L.append(("    goto L_%x;" % nxt) if nxt in self.addrs else "    goto L_return;  // end of the body")
        return L

    def emit_lock_cmpxchg(self, ins, ops, lock_at, unlock_at):
        region = ops[lock_at + 1:unlock_at]
        load = next((o for o in region if o["op"] == "LOAD"), None)
        if load is None or "CMPXCHG" not in ins["mnemonic"].upper():
            raise blocked("INSTRUCTION", "LOCK region of %s" % ins["mnemonic"])
        n = load["out"][2]
        out = []
        for o in region:
            if o is load:
                break
            out.append(self.op(ins, -1, o))
        m = re.search(r",\s*([A-Z0-9]+)\s*$", ins["text"])
        if m is None:
            raise blocked("INSTRUCTION", "CMPXCHG source register in '%s'" % ins["text"])
        src = self.reg(m.group(1))
        acc = self.reg({8: "RAX", 4: "EAX", 2: "AX", 1: "AL"}[n])
        rax = self.reg("RAX")
        zf = self.reg("ZF")
        ty = {8: "u64", 4: "std::uint32_t", 2: "std::uint16_t", 1: "u8"}[n]
        out.append("{ %s expected = static_cast<%s>(ld(R, %d, %d)); const %s desired = static_cast<%s>(ld(R, %d, %d)); "
                   "const bool ok = __atomic_compare_exchange_n(reinterpret_cast<%s*>(%s), &expected, desired, false, "
                   "__ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); st(R, %d, 1, ok ? 1 : 0); if (!ok) { %s } }"
                   % (ty, ty, acc, n, ty, ty, src, n, ty, self.read(load["in"][1]), zf,
                      ("st(R, %d, 8, static_cast<u64>(expected));" % rax) if n == 4 else
                      ("st(R, %d, %d, static_cast<u64>(expected));" % (acc, n))))
        self.stats["atomics"] += 1
        return out

    # --- one instruction alone, for the conformance check ------------------------------------------------------
    def lift_function(self, ins):
        for o in ins["pcode"]:
            if o["op"] in ("BRANCH", "CBRANCH") and o["in"][0][0] != "const" and \
                    not (o["in"][0][0] == "ram" and o["in"][0][1] == ins["addr"] + ins["len"]):
                raise blocked("CONTROL_FLOW", "branch")
            if o["op"] in ("BRANCHIND", "CALL", "CALLIND", "RETURN"):
                raise blocked("CONTROL_FLOW", o["op"])
            for v in [o["out"]] + o["in"]:
                if v and v[0] == "register" and (self.name_of(v) or "") in ("RSP", "ESP", "SP"):
                    raise blocked("ABI", "stack pointer")
        body = self.emit_instruction(ins)
        return ['extern "C" __declspec(dllexport) SYSV void lift_%x(u8* R, u8* U) {  // %s' % (ins["addr"], ins["text"]),
                "    const u64 B = recomp_image_base;", "    (void)B; (void)R; (void)U;"] + body + \
               ["P_%x_next:;" % ins["addr"], "}"]


def regfile_table(data):
    want = ["RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI"] + ["R%d" % i for i in range(8, 16)] + \
           ["CF", "PF", "AF", "ZF", "SF", "DF", "OF"] + ["YMM%d" % i for i in range(16)] + \
           ["ZMM%d" % i for i in range(16)]
    return ['{"%s", %d, %d}' % (nm, data["register_file"][nm][0], data["register_file"][nm][1])
            for nm in want if nm in data["register_file"]]


def main():
    data = json.load(open(sys.argv[1], encoding="utf-8"))
    out = sys.argv[2]
    opt = lambda k: sys.argv[sys.argv.index(k) + 1] if k in sys.argv else None
    report_path, lift_path, cov_path, only = opt("--report"), opt("--lift"), opt("--coverage"), opt("--only")
    if "register_file" not in data:
        raise SystemExit("a v2 dump (register_file) is needed: re-run pcode_dump.py")
    only = {int(x, 16) for x in only.split(",")} if only else None
    parts = [PRELUDE]
    report = []
    coverage = {}  # mnemonic -> [instances, in generated functions, lift functions, refusal]
    lift_parts, lift_rows = [], []
    max_r, max_u = 0, 0
    for f in data["functions"]:
        if only and f["entry"] not in only:
            continue
        for ins in f["instructions"]:
            coverage.setdefault(ins["mnemonic"], [0, 0, 0, ""])[0] += 1
        e = Emitter(f, data)
        try:
            parts.append(e.run())
            for ins in f["instructions"]:
                coverage[ins["mnemonic"]][1] += 1
            s = e.stats
            report.append("%x: SUPPORTED - %d instructions, %d P-code ops, %d SIMD, %d FP, %d fused, %d calls, %d tail "
                          "calls, %d indirect, %d switches, %d atomic CAS; private stack %d, stack arguments %d bytes, "
                          "R %d, U %d bytes"
                          % (f["entry"], s["instructions"], s["pcode"], s["simd"], s["fp"], s["fused"], s["calls"],
                             s["tailcalls"], s["indirect"], s["switches"], s["atomics"], s["stack"], s["arg_bytes"],
                             s["frame_R"], s["frame_U"]))
        except Unsupported as x:
            report.append("%x: %s - %s (%d instructions)" % (f["entry"], x.category, x, len(f["instructions"])))
        if lift_path:
            le = Emitter(f, data, lift=True)
            try:
                le.build_layouts([])
            except Unsupported as x:
                report.append("%x: lift functions skipped - %s" % (f["entry"], x))
                continue
            max_r, max_u = max(max_r, le.R.size), max(max_u, le.U.size)
            af_unread = 0 if le.reads_af() else 1  # LiftEntry.flags bit 0: AF has no reader (not compared)
            for ins in f["instructions"]:
                try:
                    lift_parts.append("\n".join(le.lift_function(ins)))
                    lift_rows.append("{0x%xull, %d, %d, &lift_%x}" % (ins["addr"], ins["len"], af_unread, ins["addr"]))
                    coverage[ins["mnemonic"]][2] += 1
                except Unsupported as x:
                    if x.category != "BLOCKED_BY_CONTROL_FLOW" and "stack pointer" not in str(x):
                        coverage[ins["mnemonic"]][3] = coverage[ins["mnemonic"]][3] or str(x)
    open(out, "w", encoding="utf-8").write("\n\n".join(parts) + "\n")
    if lift_path:
        rows = regfile_table(data)
        tail = ['extern "C" __declspec(dllexport) const LiftEntry lift_table[] = {%s};' % ", ".join(lift_rows),
                'extern "C" __declspec(dllexport) const u32 lift_count = %d;' % len(lift_rows),
                'extern "C" __declspec(dllexport) const u32 lift_reg_bytes = %d;' % (max_r + 64),
                'extern "C" __declspec(dllexport) const u32 lift_unique_bytes = %d;' % (max_u + 64),
                'extern "C" __declspec(dllexport) const LiftReg lift_regfile[] = {%s};' % ", ".join(rows),
                'extern "C" __declspec(dllexport) const u32 lift_regfile_count = %d;' % len(rows)]
        open(lift_path, "w", encoding="utf-8").write(PRELUDE + LIFT_PRELUDE + "\n\n".join(lift_parts) + "\n" +
                                                     "\n".join(tail) + "\n")
    if cov_path:
        with open(cov_path, "w", encoding="utf-8") as fo:
            fo.write("mnemonic\tinstances\tin_generated_functions\tlift_functions\trefusal\n")
            for m, (a, b, c, r) in sorted(coverage.items(), key=lambda x: -x[1][0]):
                fo.write("%s\t%d\t%d\t%d\t%s\n" % (m, a, b, c, r))
    text = "\n".join(report)
    print(text)
    if report_path:
        open(report_path, "w", encoding="utf-8").write(text + "\n")


if __name__ == "__main__":
    main()
