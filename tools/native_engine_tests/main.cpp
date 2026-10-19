// SecondSonPC — standalone tests of Native Engine Reconstruction (src/native_engine), without the game and without
// the bridge.
//   sspc_native_engine_tests [--eboot <eboot.bin>] [--cases N] [--bench] [--replay <cases_<key>.txt>] [--recomp <dll>]
//     always      the System V boundary (an assembly caller checks callee-saved registers, stack pointer, DF and
//                 MXCSR around every reconstruction and dispatcher), clones (relocation, refusals), registry modes
//     --eboot     the eboot's own code mapped into this process: each reconstruction against the original
//                 instructions on N generated inputs (default 1 000 000), install/dispatch/verify/guarded/bench/
//                 fallback on the mapped image, a multi-threaded stress of the allocator
//     --bench     cost per call: original, reconstruction, both through the installed jump and dispatcher
//     --replay    re-runs recorded in-game mismatch cases against the eboot's code (needs --eboot)
//     --recomp    the static recompilation PoC module (recomp_build.ps1) against the eboot's code (needs --eboot)
// Exit code = failures.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <xbyak/xbyak.h>

#include "engine/guest_hooks.h"
#include "loader/image_file.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"
#include "native_engine/native_function_registry.h"
#include "native_engine/native_measure.h"
#include "native_engine/reconstructed/frame_scratch.h"
#include "native_engine/reconstructed/object_constants.h"

using namespace SecondSon;
using namespace SecondSon::NativeEngine;
namespace OC = SecondSon::NativeEngine::Reconstructed::ObjectConstants;
namespace FS = SecondSon::NativeEngine::Reconstructed::FrameScratch;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
        }                                                                                                              \
    } while (0)

// --- the System V boundary --------------------------------------------------------------------------------------

/// What a guest caller passes and what the check found.
struct CallFrame {
    u64 target;
    u64 gpr[6]; ///< rdi rsi rdx rcx r8 r9
    u64 stack[2];
    alignas(16) u8 xmm[8][16];
    u64 rax_out;
    u64 rdx_out;
    alignas(16) u8 xmm0_out[16];
    u64 expected_rsp;
    u32 mxcsr_before;
    u32 mxcsr_after;
    u64 violations; ///< 1 rbx, 2 rbp, 4 r12, 8 r13, 0x10 r14, 0x20 r15, 0x40 rsp, 0x80 DF, 0x100 MXCSR
};
CallFrame* g_frame = nullptr;

constexpr u64 kSentinel[6] = {0x1111111111111111ull, 0x2222222222222222ull, 0x3c3c3c3c3c3c3c3cull,
                              0x4d4d4d4d4d4d4d4dull, 0x5e5e5e5e5e5e5e5eull, 0x6f6f6f6f6f6f6f6full};

/// A guest-like caller (called from Windows x64 code with rcx = frame): System V call with two stack arguments,
/// callee-saved registers seeded with sentinels, then checks them, rsp, DF and MXCSR.
struct GuestCaller : Xbyak::CodeGenerator {
    GuestCaller() : Xbyak::CodeGenerator(4096) {
        using namespace Xbyak::util;
        const Xbyak::Reg64 saved[8] = {rbx, rbp, rdi, rsi, r12, r13, r14, r15};
        for (const auto& r : saved) {
            push(r);
        }
        sub(rsp, 0xb8); // xmm6-15 + mxcsr; 16-aligned after this
        for (int i = 0; i < 10; ++i) {
            movdqu(ptr[rsp + 16 * i], Xbyak::Xmm(6 + i));
        }
        stmxcsr(ptr[rsp + 0xa8]);
        mov(rax, rcx);
        mov(r11, reinterpret_cast<size_t>(&g_frame));
        mov(ptr[r11], rax);
        mov(r10d, ptr[rsp + 0xa8]);
        mov(ptr[rax + offsetof(CallFrame, mxcsr_before)], r10d);
        sub(rsp, 16); // the two stack arguments
        mov(r10, ptr[rax + offsetof(CallFrame, stack)]);
        mov(ptr[rsp], r10);
        mov(r10, ptr[rax + offsetof(CallFrame, stack) + 8]);
        mov(ptr[rsp + 8], r10);
        mov(ptr[rax + offsetof(CallFrame, expected_rsp)], rsp);
        mov(rbx, kSentinel[0]);
        mov(rbp, kSentinel[1]);
        mov(r12, kSentinel[2]);
        mov(r13, kSentinel[3]);
        mov(r14, kSentinel[4]);
        mov(r15, kSentinel[5]);
        for (int k = 0; k < 8; ++k) {
            movdqu(Xbyak::Xmm(k), ptr[rax + offsetof(CallFrame, xmm) + 16 * k]);
        }
        mov(rdi, ptr[rax + offsetof(CallFrame, gpr) + 0]);
        mov(rsi, ptr[rax + offsetof(CallFrame, gpr) + 8]);
        mov(rdx, ptr[rax + offsetof(CallFrame, gpr) + 16]);
        mov(rcx, ptr[rax + offsetof(CallFrame, gpr) + 24]);
        mov(r8, ptr[rax + offsetof(CallFrame, gpr) + 32]);
        mov(r9, ptr[rax + offsetof(CallFrame, gpr) + 40]);
        mov(r11, ptr[rax + offsetof(CallFrame, target)]);
        mov(eax, 8); // AL: vector registers used (varargs convention)
        cld();
        call(r11);
        mov(r11, reinterpret_cast<size_t>(&g_frame));
        mov(r11, ptr[r11]);
        mov(ptr[r11 + offsetof(CallFrame, rax_out)], rax);
        mov(ptr[r11 + offsetof(CallFrame, rdx_out)], rdx);
        movdqu(ptr[r11 + offsetof(CallFrame, xmm0_out)], xmm0);
        xor_(ecx, ecx);
        const Xbyak::Reg64 checked[6] = {rbx, rbp, r12, r13, r14, r15};
        for (int i = 0; i < 6; ++i) {
            Xbyak::Label ok;
            mov(rdx, kSentinel[i]);
            cmp(checked[i], rdx);
            je(ok);
            or_(ecx, 1 << i);
            L(ok);
        }
        Xbyak::Label rsp_ok, df_ok, mx_ok;
        cmp(rsp, ptr[r11 + offsetof(CallFrame, expected_rsp)]);
        je(rsp_ok);
        or_(ecx, 0x40);
        mov(rsp, ptr[r11 + offsetof(CallFrame, expected_rsp)]);
        L(rsp_ok);
        pushfq();
        pop(rdx);
        test(edx, 0x400);
        jz(df_ok);
        or_(ecx, 0x80);
        cld();
        L(df_ok);
        stmxcsr(ptr[r11 + offsetof(CallFrame, mxcsr_after)]);
        mov(edx, ptr[r11 + offsetof(CallFrame, mxcsr_after)]);
        cmp(edx, ptr[r11 + offsetof(CallFrame, mxcsr_before)]);
        je(mx_ok);
        or_(ecx, 0x100);
        L(mx_ok);
        mov(ptr[r11 + offsetof(CallFrame, violations)], rcx);
        add(rsp, 16);
        ldmxcsr(ptr[rsp + 0xa8]);
        for (int i = 0; i < 10; ++i) {
            movdqu(Xbyak::Xmm(6 + i), ptr[rsp + 16 * i]);
        }
        add(rsp, 0xb8);
        for (int i = 7; i >= 0; --i) {
            pop(saved[i]);
        }
        ret();
        ready();
    }
};

GuestCaller& Caller() {
    static GuestCaller c;
    return c;
}

u64 CallAsGuest(const void* target, CallFrame& f) {
    f.target = reinterpret_cast<u64>(target);
    f.violations = ~0ull;
    using Fn = void (*)(CallFrame*); // Windows x64: rcx = frame
    reinterpret_cast<Fn>(const_cast<u8*>(Caller().getCode()))(&f);
    return f.violations;
}

// Deliberately broken System V functions: the check must catch them.
struct Broken : Xbyak::CodeGenerator {
    explicit Broken(int kind) : Xbyak::CodeGenerator(64) {
        using namespace Xbyak::util;
        if (kind == 0) {
            xor_(ebx, ebx); // clobbers a callee-saved register
        } else if (kind == 1) {
            std(); // leaves DF set
        } else {
            push(rax); // leaves rsp moved: pops the return address into rax via a wrong path
            pop(rax);
            mov(r15, 0); // callee-saved
        }
        ret();
        ready();
    }
};

struct Matrices {
    alignas(16) float world[16];
    alignas(16) float previous[16];
    alignas(16) float vp[16];
    alignas(16) float extra[16];
    alignas(16) u8 object[0x20];
};

float Special(std::mt19937_64& rng) {
    const u32 pick = static_cast<u32>(rng() % 100);
    if (pick < 70) {
        return static_cast<float>(static_cast<double>(static_cast<s64>(rng() % 2000001) - 1000000) / 977.0);
    }
    static constexpr u32 kBits[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0x00000001u, 0x807fffffu,
                                    0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc12345u,
                                    0x7fa00001u /* signalling NaN */};
    u32 bits = (pick % 10 == 9) ? static_cast<u32>(rng()) : kBits[rng() % std::size(kBits)];
    if ((bits & 0x7fc00000u) == 0x7fc00000u) {
        bits |= static_cast<u32>(rng() & 0x3fffff); // payloads
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

/// A plausible transform: rotation (unit rows), scale, translation in row 3; and a perspective-like matrix.
void Plausible(std::mt19937_64& rng, float* m, bool projection) {
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            m[r * 4 + c] = u(rng) * (r == 3 ? 500.0f : 2.0f);
        }
    }
    if (!projection) {
        m[3] = m[7] = m[11] = 0.0f;
        m[15] = 1.0f;
    }
}

void TestAbiBoundary() {
    std::printf("ABI boundary (System V caller -> SecondSonPC):\n");
    CallFrame f{};
    // the checker itself: three broken functions are caught
    Broken b0(0), b1(1), b2(2);
    CHECK(CallAsGuest(b0.getCode(), f) == 0x1);
    CHECK(CallAsGuest(b1.getCode(), f) == 0x80);
    CHECK(CallAsGuest(b2.getCode(), f) == 0x20);

    // ObjectConstants::Native: 8 arguments, two on the stack
    Matrices m{};
    std::mt19937_64 rng{1};
    Plausible(rng, m.world, false);
    Plausible(rng, m.previous, false);
    Plausible(rng, m.vp, true);
    Plausible(rng, m.extra, false);
    alignas(16) u8 block[OC::kBlockBytes] = {};
    alignas(16) u8 expect[OC::kBlockBytes] = {};
    OC::Native(m.world, m.previous, m.vp, reinterpret_cast<u64>(m.object), m.extra, 77, 0xff, expect);
    f = CallFrame{};
    f.gpr[0] = reinterpret_cast<u64>(m.world);
    f.gpr[1] = reinterpret_cast<u64>(m.previous);
    f.gpr[2] = reinterpret_cast<u64>(m.vp);
    f.gpr[3] = reinterpret_cast<u64>(m.object);
    f.gpr[4] = reinterpret_cast<u64>(m.extra);
    f.gpr[5] = 77;
    f.stack[0] = 0xffffffff000000ffull; // flags: only the low dword is the argument
    f.stack[1] = reinterpret_cast<u64>(block);
    CHECK(CallAsGuest(reinterpret_cast<const void*>(&OC::Native), f) == 0);
    CHECK(std::memcmp(block, expect, sizeof(block)) == 0);

    // FrameScratch::Native
    alignas(16) u8 scratch[0x80] = {};
    alignas(64) static u8 arena[4096];
    const u64 base = reinterpret_cast<u64>(arena);
    u64 v = base;
    std::memcpy(scratch + 0x38, &v, 8);
    v = base + sizeof(arena);
    std::memcpy(scratch + 0x40, &v, 8);
    f = CallFrame{};
    f.gpr[0] = reinterpret_cast<u64>(scratch);
    f.gpr[1] = 0xdead000000000030ull; // size: esi = 0x30 (upper half must be ignored)
    f.gpr[2] = 0xbeef000000000040ull; // alignment 0x40
    CHECK(CallAsGuest(reinterpret_cast<const void*>(&FS::Native), f) == 0);
    CHECK(f.rax_out == ((base + 0x3f) & ~0x3full));
}

// --- clones ------------------------------------------------------------------------------------------------------

void TestClone() {
    std::printf("clones:\n");
    // a function with an internal branch, a rel32 call out of the body and a RIP-relative load out of the body
    auto* page = static_cast<u8*>(VirtualAlloc(nullptr, 0x2000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    CHECK(page != nullptr);
    // helper at page+0x100: mov eax, 40; ret
    const u8 helper[] = {0xB8, 40, 0, 0, 0, 0xC3};
    std::memcpy(page + 0x100, helper, sizeof(helper));
    // data at page+0x200: dword 2
    const u32 two = 2;
    std::memcpy(page + 0x200, &two, 4);
    // f at page+0x0 (System V, int f(int x)): x == 0 ? 0 : helper() + [rip->data]
    std::vector<u8> code;
    auto emit = [&](std::initializer_list<u8> b) { code.insert(code.end(), b); };
    emit({0x85, 0xFF});             // test edi, edi
    emit({0x74, 0x0E});             // jz +14 -> ret0
    emit({0xE8, 0, 0, 0, 0});       // call helper (patched below)
    emit({0x03, 0x05, 0, 0, 0, 0}); // add eax, [rip+data] (patched below)
    emit({0xC3});                   // ret
    emit({0x90, 0x90});             // padding to the jz target
    emit({0x31, 0xC0, 0xC3});       // ret0: xor eax, eax; ret
    const s32 rel_call = 0x100 - (4 + 5);
    std::memcpy(code.data() + 5, &rel_call, 4);
    const s32 rel_data = 0x200 - (9 + 6);
    std::memcpy(code.data() + 11, &rel_data, 4);
    // the jz must land on ret0: test(2)+jz(2)+call(5)+add(6)+ret(1)+nop(2) = 18 = 4 + 14
    std::memcpy(page, code.data(), code.size());
    using Fn = SSPC_SYSV_ABI int (*)(int);
    CHECK(reinterpret_cast<Fn>(page)(1) == 42 && reinterpret_cast<Fn>(page)(0) == 0);
    std::string error;
    const CloneResult c = CloneGuestFunction(reinterpret_cast<u64>(page), page, static_cast<u32>(code.size()), error);
    CHECK(c.code != nullptr && c.relocated == 2 && c.instructions == 9);
    if (c.code != nullptr) {
        CHECK(reinterpret_cast<Fn>(c.code)(1) == 42 && reinterpret_cast<Fn>(c.code)(0) == 0);
        CHECK(c.code != page);
    }
    // refused: a rel8 branch out of the body; a body cut inside an instruction
    const u8 out8[] = {0xEB, 0x40, 0xC3};
    std::memcpy(page + 0x400, out8, sizeof(out8));
    CHECK(CloneGuestFunction(reinterpret_cast<u64>(page + 0x400), page + 0x400, 3, error).code == nullptr);
    CHECK(error.find("8-bit") != std::string::npos);
    CHECK(CloneGuestFunction(reinterpret_cast<u64>(page), page, 6, error).code == nullptr); // cuts the call
}

// --- the eboot's own code mapped into this process --------------------------------------------------------------

struct Image {
    u64 base = 0;
    u64 size = 0;
};

bool MapEboot(const std::string& path, Image& image) {
    Loader::ImageFile file;
    std::string error;
    if (!file.Open(path, error)) {
        std::printf("  cannot open %s: %s\n", path.c_str(), error.c_str());
        return false;
    }
    u64 end = 0;
    for (const auto& p : file.ProgramHeaders()) {
        if (p.type == Loader::Elf::PT_LOAD) {
            end = std::max<u64>(end, p.vaddr + p.memsz);
        }
    }
    image.size = (end + 0xffff) & ~0xffffull;
    auto* mem = static_cast<u8*>(VirtualAlloc(nullptr, image.size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (mem == nullptr) {
        return false;
    }
    for (const auto& p : file.ProgramHeaders()) {
        if (p.type == Loader::Elf::PT_LOAD) {
            const std::vector<u8> bytes = file.ReadSegment(p);
            std::memcpy(mem + p.vaddr, bytes.data(), bytes.size());
        }
    }
    image.base = reinterpret_cast<u64>(mem);
    return true;
}

/// A second mapping of one function's bytes, untouched by installs: the reference original.
void* PristineCopy(const Image& image, const Engine::GuestFunction& f) {
    std::string error;
    const CloneResult c = CloneGuestFunction(image.base + f.rva, reinterpret_cast<const u8*>(image.base + f.rva),
                                             f.size, error);
    if (c.code == nullptr) {
        std::printf("  clone of %#llx failed: %s\n", static_cast<unsigned long long>(f.rva), error.c_str());
    }
    return c.code;
}

void TestObjectConstantsDifferential(OC::Fn original, u64 cases) {
    std::printf("ObjectConstants::Update (0x11c890) vs the eboot's code, %llu generated cases:\n",
                static_cast<unsigned long long>(cases));
    std::mt19937_64 rng{0x0c0ffee};
    Matrices m{};
    alignas(16) u8 block[OC::kBlockBytes], a[OC::kBlockBytes], b[OC::kBlockBytes];
    for (auto& x : block) {
        x = static_cast<u8>(rng());
    }
    u64 mismatches = 0, nan_cases = 0, special = 0;
    for (u64 n = 0; n < cases; ++n) {
        const bool plausible = (n % 4) != 0;
        if (plausible) {
            Plausible(rng, m.world, false);
            if (n % 8 == 1) {
                std::memcpy(m.previous, m.world, 64); // a static object: "unchanged" paths
            } else {
                Plausible(rng, m.previous, false);
            }
            Plausible(rng, m.vp, true);
            Plausible(rng, m.extra, false);
        } else {
            ++special;
            for (float* p : {m.world, m.previous, m.vp, m.extra}) {
                for (int i = 0; i < 16; ++i) {
                    p[i] = Special(rng);
                    nan_cases += std::isnan(p[i]) ? 1 : 0;
                }
            }
        }
        const u32 word = static_cast<u32>(rng());
        std::memcpy(m.object + 0x10, &word, 4);
        const u32 flags = static_cast<u32>(rng());
        const s32 value = (n % 3 == 0) ? 0 : static_cast<s32>(rng() % 5);
        if (n % 7 == 0) {
            for (auto& x : block) {
                x = static_cast<u8>(rng());
            }
        }
        if (!OC::Compare(original, m.world, m.previous, m.vp, reinterpret_cast<u64>(m.object), m.extra, value, flags,
                         block, a, b)) {
            if (++mismatches <= 25) {
                u32 at = 0;
                while (at < OC::kBlockBytes && a[at] == b[at]) {
                    ++at;
                }
                if (std::getenv("NE_DEBUG") != nullptr && at >= 0xb0 && at < 0xf0) {
                    const u32 row = (at - 0xb0) / 16;
                    std::printf("  world:");
                    for (int i = 0; i < 16; ++i) {
                        u32 x;
                        std::memcpy(&x, &m.world[i], 4);
                        std::printf(" %08x", x);
                    }
                    std::printf("\n  vp:   ");
                    for (int i = 0; i < 16; ++i) {
                        u32 x;
                        std::memcpy(&x, &m.vp[i], 4);
                        std::printf(" %08x", x);
                    }
                    std::printf("\n  row %u original:", row);
                    for (int i = 0; i < 4; ++i) {
                        u32 x;
                        std::memcpy(&x, a + 0xb0 + row * 16 + i * 4, 4);
                        std::printf(" %08x", x);
                    }
                    std::printf(" native:");
                    for (int i = 0; i < 4; ++i) {
                        u32 x;
                        std::memcpy(&x, b + 0xb0 + row * 16 + i * 4, 4);
                        std::printf(" %08x", x);
                    }
                    std::printf("\n");
                }
                u32 wa = 0, wb = 0;
                std::memcpy(&wa, a + (at & ~3u), 4);
                std::memcpy(&wb, b + (at & ~3u), 4);
                std::printf("  mismatch case %llu flags %#x at +%#x: original %08x native %08x\n",
                            static_cast<unsigned long long>(n), flags, at & ~3u, wa, wb);
            }
        }
        std::memcpy(block, a, sizeof(block));
    }
    std::printf("  %llu cases (%llu with special values, %llu NaN inputs): %llu mismatches\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(special),
                static_cast<unsigned long long>(nan_cases), static_cast<unsigned long long>(mismatches));
    CHECK(mismatches == 0);
}

void TestFrameScratchDifferential(FS::Fn original, u64 cases) {
    std::printf("FrameScratch::Alloc (0x1f0870) vs the eboot's code, %llu generated cases:\n",
                static_cast<unsigned long long>(cases));
    std::mt19937_64 rng{0xa110c};
    alignas(16) u8 object[FS::kObjectBytes], a[FS::kObjectBytes], b[FS::kObjectBytes];
    static constexpr u32 kAlign[] = {0, 1, 2, 3, 4, 8, 16, 32, 64, 128, 256, 1000, 4096, 0x80000000u, 0xffffffffu};
    u64 mismatches = 0, zero = 0, fallback = 0;
    for (u64 n = 0; n < cases; ++n) {
        for (auto& x : object) {
            x = static_cast<u8>(rng());
        }
        const auto field = [&](u32 off, u64 v) { std::memcpy(object + off, &v, 8); };
        const u64 base = (n % 9 == 0) ? 0 : (rng() & 0x7fffffff000ull);
        field(0x38, n % 11 == 0 ? 0 : base + rng() % 0x10000);
        field(0x40, n % 29 == 0 ? ~0ull : base + rng() % 0x20000);
        field(0x68, n % 31 == 0 ? ~0ull - (rng() & 0xfff) : base + 0x100000 + rng() % 0x1000);
        field(0x70, base + 0x100000 + rng() % 0x3000);
        const u32 size = (n % 13 == 0) ? 0 : (n % 37 == 0) ? static_cast<u32>(rng()) : static_cast<u32>(rng() % 0x4000);
        const u32 alignment = kAlign[rng() % std::size(kAlign)];
        u64 ra = 0, rb = 0;
        if (!FS::Compare(original, object, size, alignment, a, b, ra, rb)) {
            if (++mismatches <= 25) {
                std::printf("  mismatch case %llu: size %#x alignment %#x: original %#llx native %#llx\n",
                            static_cast<unsigned long long>(n), size, alignment, static_cast<unsigned long long>(ra),
                            static_cast<unsigned long long>(rb));
            }
        }
        zero += ra == 0 ? 1 : 0;
        u64 c0 = 0;
        std::memcpy(&c0, object + 0x68, 8);
        u64 c1 = 0;
        std::memcpy(&c1, a + 0x68, 8);
        fallback += c0 != c1 ? 1 : 0;
    }
    std::printf("  %llu cases (%llu returned 0, %llu used the fallback arena): %llu mismatches\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(zero),
                static_cast<unsigned long long>(fallback), static_cast<unsigned long long>(mismatches));
    CHECK(mismatches == 0);
}

/// Six threads allocate from one allocator at once: every success inside an arena, aligned, no overlap; failures
/// only once the arena is used up. Run for the original and for the reconstruction.
void StressAllocator(const char* what, FS::Fn fn) {
    constexpr u32 kThreads = 6, kPer = 100000;
    static std::vector<u8> arena(64ull << 20);
    alignas(64) static u8 object[FS::kObjectBytes];
    const u64 base = (reinterpret_cast<u64>(arena.data()) + 255) & ~255ull;
    const u64 primary_end = base + (16ull << 20), fallback_begin = base + (32ull << 20),
              fallback_end = base + (48ull << 20);
    std::memset(object, 0, sizeof(object));
    std::memcpy(object + 0x38, &base, 8);
    std::memcpy(object + 0x40, &primary_end, 8);
    std::memcpy(object + 0x68, &fallback_begin, 8);
    std::memcpy(object + 0x70, &fallback_end, 8);
    std::vector<std::vector<std::pair<u64, u32>>> got(kThreads);
    std::atomic<u32> start{0};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            std::mt19937 rng{t + 1};
            got[t].reserve(kPer);
            start.fetch_add(1);
            while (start.load() < kThreads) {
            }
            for (u32 i = 0; i < kPer; ++i) {
                const u32 size = 16 + rng() % 240;
                const u32 alignment = 1u << (rng() % 7);
                const u64 p = fn(reinterpret_cast<u64>(object), size, alignment);
                got[t].push_back({p, size | (alignment << 16)});
            }
        });
    }
    for (auto& th : threads) {
        th.join();
    }
    std::vector<std::pair<u64, u64>> ranges;
    u64 failures = 0, misaligned = 0, outside = 0;
    for (const auto& v : got) {
        for (const auto& [p, packed] : v) {
            const u32 size = packed & 0xffff, alignment = packed >> 16;
            if (p == 0) {
                ++failures;
                continue;
            }
            misaligned += (p & (alignment - 1)) != 0 ? 1 : 0;
            const bool in_primary = p >= base && p + size <= primary_end;
            const bool in_fallback = p >= fallback_begin && p + size <= fallback_end;
            outside += in_primary || in_fallback ? 0 : 1;
            ranges.push_back({p, p + size});
        }
    }
    std::sort(ranges.begin(), ranges.end());
    u64 overlaps = 0;
    for (size_t i = 1; i < ranges.size(); ++i) {
        overlaps += ranges[i].first < ranges[i - 1].second ? 1 : 0;
    }
    std::printf("  %s: %u threads x %u allocations: %llu ok, %llu failed (arenas full), overlaps %llu, misaligned "
                "%llu, outside %llu\n",
                what, kThreads, kPer, static_cast<unsigned long long>(ranges.size()),
                static_cast<unsigned long long>(failures), static_cast<unsigned long long>(overlaps),
                static_cast<unsigned long long>(misaligned), static_cast<unsigned long long>(outside));
    CHECK(overlaps == 0 && misaligned == 0 && outside == 0);
    CHECK(ranges.size() + failures == u64{kThreads} * kPer);
}

/// Installs a reconstruction on the mapped image and drives it through the patched entry in every mode.
void TestInstalledModes(const Image& image) {
    std::printf("registry on the mapped eboot (install, dispatch, verify, guarded, bench, fallback):\n");
    const FunctionSpec& oc = Reconstructed::kObjectConstantsSpec;
    const FunctionSpec& fs = Reconstructed::kFrameScratchAllocSpec;
    // a wrong hash refuses the install and leaves the code untouched
    FunctionSpec wrong = oc;
    wrong.function.fnv1a64 ^= 1;
    std::string error;
    u8 before[16];
    std::memcpy(before, reinterpret_cast<const void*>(image.base + oc.function.rva), 16);
    CHECK(!InstallFunction(image.base, wrong, Mode::Native, error));
    CHECK(error.find("hash") != std::string::npos);
    CHECK(std::memcmp(before, reinterpret_cast<const void*>(image.base + oc.function.rva), 16) == 0);

    void* reference = PristineCopy(image, oc.function);
    void* reference_fs = PristineCopy(image, fs.function);
    CHECK(reference != nullptr && reference_fs != nullptr);
    CHECK(InstallFunction(image.base, oc, Mode::Native, error));
    if (!error.empty() && g_failures != 0) {
        std::printf("  install: %s\n", error.c_str());
    }
    error.clear();
    CHECK(InstallFunction(image.base, fs, Mode::Native, error));
    if (!error.empty()) {
        std::printf("  install: %s\n", error.c_str());
    }
    // a second install on the same entry is refused (the entry no longer hashes)
    CHECK(!InstallFunction(image.base, oc, Mode::Native, error));

    const auto entry = reinterpret_cast<OC::Fn>(image.base + oc.function.rva);
    const auto ref = reinterpret_cast<OC::Fn>(reference);
    std::mt19937_64 rng{7};
    Matrices m{};
    alignas(16) u8 b1[OC::kBlockBytes] = {}, b2[OC::kBlockBytes] = {};
    const u32 i_oc = oc.slot->index;
    for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
        oc.slot->mode.store(static_cast<u8>(mode));
        oc.slot->sample_period = 4;
        const u64 n0 = Total(i_oc, kCallsNative) + Total(i_oc, kCallsOriginal) + Total(i_oc, kCallsChecked);
        u64 differ = 0;
        for (int k = 0; k < 2000; ++k) {
            Plausible(rng, m.world, false);
            Plausible(rng, m.previous, false);
            Plausible(rng, m.vp, true);
            Plausible(rng, m.extra, false);
            const u32 flags = static_cast<u32>(rng());
            entry(m.world, m.previous, m.vp, reinterpret_cast<u64>(m.object), m.extra, 3, flags, b1);
            ref(m.world, m.previous, m.vp, reinterpret_cast<u64>(m.object), m.extra, 3, flags, b2);
            differ += std::memcmp(b1, b2, sizeof(b1)) != 0 ? 1 : 0;
        }
        const u64 n1 = Total(i_oc, kCallsNative) + Total(i_oc, kCallsOriginal) + Total(i_oc, kCallsChecked);
        std::printf("  object_constants %-8s: 2000 calls through the jump, counted %llu, different from the "
                    "original %llu\n",
                    ModeName(mode), static_cast<unsigned long long>(n1 - n0), static_cast<unsigned long long>(differ));
        CHECK(differ == 0);
        CHECK(n1 > n0);
    }
    std::printf("  verified %llu, mismatches %llu, bench samples %llu, live-diff %llu\n",
                static_cast<unsigned long long>(oc.slot->verified.load()),
                static_cast<unsigned long long>(oc.slot->mismatches.load()),
                static_cast<unsigned long long>(oc.slot->bench_samples.load()),
                static_cast<unsigned long long>(oc.slot->live_diff.load()));
    CHECK(oc.slot->verified.load() > 0 && oc.slot->mismatches.load() == 0 && oc.slot->bench_samples.load() > 0);
    CHECK(oc.slot->live_diff.load() == 0);

    // fallback: a guarded function switched back runs the clone of the original
    oc.slot->mode.store(static_cast<u8>(Mode::Native));
    Fallback(*oc.slot);
    CHECK(static_cast<Mode>(oc.slot->mode.load()) == Mode::Original && oc.slot->fallbacks.load() == 1);
    const u64 orig0 = Total(i_oc, kCallsOriginal);
    entry(m.world, m.previous, m.vp, reinterpret_cast<u64>(m.object), m.extra, 3, 0xff, b1);
    CHECK(Total(i_oc, kCallsOriginal) == orig0 + 1);

    // the allocator through its jump, all modes, against the reference on a copy
    const auto alloc = reinterpret_cast<FS::Fn>(image.base + fs.function.rva);
    const auto alloc_ref = reinterpret_cast<FS::Fn>(reference_fs);
    alignas(64) static u8 arena[1 << 16];
    alignas(16) u8 obj1[FS::kObjectBytes] = {}, obj2[FS::kObjectBytes] = {};
    const u64 lo = reinterpret_cast<u64>(arena), hi = lo + sizeof(arena) / 2, lo2 = hi, hi2 = lo + sizeof(arena);
    for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench}) {
        fs.slot->mode.store(static_cast<u8>(mode));
        fs.slot->sample_period = 3;
        for (u8* o : {obj1, obj2}) {
            std::memcpy(o + 0x38, &lo, 8);
            std::memcpy(o + 0x40, &hi, 8);
            std::memcpy(o + 0x68, &lo2, 8);
            std::memcpy(o + 0x70, &hi2, 8);
        }
        u64 differ = 0;
        for (int k = 0; k < 3000; ++k) {
            const u32 size = static_cast<u32>(rng() % 64), alignment = 1u << (rng() % 6);
            differ += alloc(reinterpret_cast<u64>(obj1), size, alignment) !=
                              alloc_ref(reinterpret_cast<u64>(obj2), size, alignment)
                          ? 1
                          : 0;
        }
        differ += std::memcmp(obj1, obj2, sizeof(obj1)) != 0 ? 1 : 0;
        std::printf("  frame_alloc %-8s: 3000 allocations through the jump (arenas exhausted on the way), different "
                    "%llu\n",
                    ModeName(mode), static_cast<unsigned long long>(differ));
        CHECK(differ == 0);
    }
    CHECK(fs.slot->mismatches.load() == 0 && fs.slot->verified.load() > 0);

    // the dispatchers through the guest-like caller: the boundary holds in every mode
    for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
        oc.slot->mode.store(static_cast<u8>(mode));
        CallFrame f{};
        f.gpr[0] = reinterpret_cast<u64>(m.world);
        f.gpr[1] = reinterpret_cast<u64>(m.previous);
        f.gpr[2] = reinterpret_cast<u64>(m.vp);
        f.gpr[3] = reinterpret_cast<u64>(m.object);
        f.gpr[4] = reinterpret_cast<u64>(m.extra);
        f.gpr[5] = 1;
        f.stack[0] = 0xff;
        f.stack[1] = reinterpret_cast<u64>(b1);
        CHECK(CallAsGuest(reinterpret_cast<const void*>(entry), f) == 0);
        fs.slot->mode.store(static_cast<u8>(mode));
        f = CallFrame{};
        f.gpr[0] = reinterpret_cast<u64>(obj1);
        f.gpr[1] = 8;
        f.gpr[2] = 8;
        CHECK(CallAsGuest(reinterpret_cast<const void*>(alloc), f) == 0);
    }
    oc.slot->mode.store(static_cast<u8>(Mode::Native));
    fs.slot->mode.store(static_cast<u8>(Mode::Native));
}

// --- cost per call -----------------------------------------------------------------------------------------------

template <typename F>
double NsPerCall(F&& body, u64 n) {
    const auto t0 = std::chrono::steady_clock::now();
    for (u64 i = 0; i < n; ++i) {
        body(i);
    }
    return std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
           static_cast<double>(n);
}

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}

void Bench(const Image& image, OC::Fn reference, FS::Fn reference_fs) {
    std::printf("cost per call (median of 7 alternating rounds):\n");
    const FunctionSpec& oc = Reconstructed::kObjectConstantsSpec;
    const FunctionSpec& fs = Reconstructed::kFrameScratchAllocSpec;
    const auto entry = reinterpret_cast<OC::Fn>(image.base + oc.function.rva);
    std::mt19937_64 rng{11};
    constexpr int kSets = 64;
    std::vector<Matrices> sets(kSets);
    for (Matrices& m : sets) {
        Plausible(rng, m.world, false);
        Plausible(rng, m.previous, false);
        Plausible(rng, m.vp, true);
        Plausible(rng, m.extra, false);
    }
    alignas(16) static u8 blocks[kSets][OC::kBlockBytes];
    for (const u32 flags : {0x07u, 0xffu, 0x01u}) {
        std::vector<double> t_ref, t_nat, t_jmp_nat, t_jmp_orig;
        const u64 n = 2'000'000;
        for (int round = 0; round < 7; ++round) {
            const auto run = [&](OC::Fn fn) {
                return NsPerCall(
                    [&](u64 i) {
                        Matrices& m = sets[i % kSets];
                        fn(m.world, m.previous, m.vp, reinterpret_cast<u64>(m.object), m.extra, 1, flags,
                           blocks[i % kSets]);
                    },
                    n);
            };
            t_ref.push_back(run(reference));
            t_nat.push_back(run(&OC::Native));
            oc.slot->mode.store(static_cast<u8>(Mode::Native));
            t_jmp_nat.push_back(run(entry));
            oc.slot->mode.store(static_cast<u8>(Mode::Original));
            t_jmp_orig.push_back(run(entry));
        }
        std::printf("  object_constants flags %#04x: original %.2f ns, reconstruction %.2f ns, through the jump: "
                    "reconstruction %.2f ns, original clone %.2f ns\n",
                    flags, Median(t_ref), Median(t_nat), Median(t_jmp_nat), Median(t_jmp_orig));
    }
    oc.slot->mode.store(static_cast<u8>(Mode::Native));

    const auto alloc = reinterpret_cast<FS::Fn>(image.base + fs.function.rva);
    static std::vector<u8> arena(1 << 20);
    alignas(64) static u8 object[FS::kObjectBytes];
    const auto reset = [&] {
        const u64 lo = reinterpret_cast<u64>(arena.data()), hi = lo + arena.size() / 2, lo2 = hi,
                  hi2 = lo + arena.size();
        std::memcpy(object + 0x38, &lo, 8);
        std::memcpy(object + 0x40, &hi, 8);
        std::memcpy(object + 0x68, &lo2, 8);
        std::memcpy(object + 0x70, &hi2, 8);
    };
    std::vector<double> a_ref, a_nat, a_jmp_nat, a_jmp_orig;
    const u64 n = 4'000'000;
    for (int round = 0; round < 7; ++round) {
        const auto run = [&](FS::Fn fn) {
            return NsPerCall(
                [&](u64 i) {
                    if ((i & 1023) == 0) {
                        reset();
                    }
                    fn(reinterpret_cast<u64>(object), 0x48, 16);
                },
                n);
        };
        a_ref.push_back(run(reference_fs));
        a_nat.push_back(run(&FS::Native));
        fs.slot->mode.store(static_cast<u8>(Mode::Native));
        a_jmp_nat.push_back(run(alloc));
        fs.slot->mode.store(static_cast<u8>(Mode::Original));
        a_jmp_orig.push_back(run(alloc));
    }
    std::printf("  frame_alloc (0x48 bytes, 16-aligned, one thread): original %.2f ns, reconstruction %.2f ns, "
                "through the jump: reconstruction %.2f ns, original clone %.2f ns\n",
                Median(a_ref), Median(a_nat), Median(a_jmp_nat), Median(a_jmp_orig));
    fs.slot->mode.store(static_cast<u8>(Mode::Native));
}

void Replay(const Image& image, const std::string& path) {
    std::printf("replay %s:\n", path.c_str());
    std::ifstream in(path);
    std::string line;
    const auto oc_ref = reinterpret_cast<OC::Fn>(PristineCopy(image, Reconstructed::kObjectConstantsSpec.function));
    const auto fs_ref = reinterpret_cast<FS::Fn>(PristineCopy(image, Reconstructed::kFrameScratchAllocSpec.function));
    u32 n = 0;
    while (std::getline(in, line)) {
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) {
            continue;
        }
        std::string bytes, report;
        if (!FromHex(line.substr(sp + 1), bytes)) {
            continue;
        }
        const std::string key = line.substr(0, sp);
        const bool ok = key == "object_constants" ? OC::Replay(oc_ref, bytes, report)
                                                  : FS::Replay(fs_ref, bytes, report);
        std::printf("  case %u (%s): %s\n", ++n, key.c_str(), report.c_str());
        CHECK(ok);
    }
}

/// The static recompilation PoC: functions machine-translated from Ghidra P-code (tools/native-reconstruction/recomp)
/// against the eboot's own code, and the recompiled allocator through the registry.
void TestRecompiled(const Image& image, const std::string& dll, u64 cases, FS::Fn fs_original) {
    std::printf("static recompilation PoC (%s):\n", dll.c_str());
    HMODULE m = LoadLibraryA(dll.c_str());
    CHECK(m != nullptr);
    if (m == nullptr) {
        return;
    }
    auto* base = reinterpret_cast<u64*>(GetProcAddress(m, "recomp_image_base"));
    CHECK(base != nullptr);
    if (base == nullptr) {
        return;
    }
    *base = image.base;
    std::mt19937_64 rng{0x5ec0};
    using Fn6 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64);
    // 0x61f430 (Gnm DataFormat packing: pure, six integer arguments)
    if (auto* r = reinterpret_cast<Fn6>(GetProcAddress(m, "recomp_61f430"))) {
        const auto o = reinterpret_cast<Fn6>(PristineCopy(image, {0x61f430, 0x33, 0x75d0a6829a95f352ull}));
        u64 bad = 0;
        for (u64 n = 0; n < cases; ++n) {
            const u64 a[6] = {rng(), rng(), rng(), rng(), rng(), rng()};
            bad += o(a[0], a[1], a[2], a[3], a[4], a[5]) != r(a[0], a[1], a[2], a[3], a[4], a[5]) ? 1 : 0;
        }
        std::printf("  recomp_61f430 vs eboot 0x61f430: %llu random cases, %llu different\n",
                    static_cast<unsigned long long>(cases), static_cast<unsigned long long>(bad));
        CHECK(bad == 0);
    }
    // 0x61e260 (Gnm initAsRegularBuffer: writes a 16-byte V#, calls 0x61f430 in the eboot)
    if (auto* r = reinterpret_cast<Fn6>(GetProcAddress(m, "recomp_61e260"))) {
        const auto o = reinterpret_cast<Fn6>(image.base + 0x61e260); // calls the image's own 0x61f430 (rel32)
        u64 bad = 0;
        alignas(16) u8 va[16], vb[16];
        for (u64 n = 0; n < cases; ++n) {
            const u64 addr = rng(), stride = rng(), count = rng();
            std::memset(va, 0xcd, 16);
            std::memset(vb, 0xcd, 16);
            o(reinterpret_cast<u64>(va), addr, stride, count, 0, 0);
            r(reinterpret_cast<u64>(vb), addr, stride, count, 0, 0);
            bad += std::memcmp(va, vb, 16) != 0 ? 1 : 0;
        }
        std::printf("  recomp_61e260 vs eboot 0x61e260: %llu random cases, %llu different V#\n",
                    static_cast<unsigned long long>(cases), static_cast<unsigned long long>(bad));
        CHECK(bad == 0);
    }
    // 0x1f0870 (FrameScratch::Alloc: lock cmpxchg) - the same differential and contention tests as the hand-written one
    if (auto* r = reinterpret_cast<FS::Fn>(GetProcAddress(m, "recomp_1f0870"))) {
        const auto o = fs_original; // cloned before the registry tests patched the image
        const FS::Fn before = FS::Implementation();
        FS::UseImplementation(r, "recompiled");
        TestFrameScratchDifferential(o, cases);
        StressAllocator("recompiled", r);
        std::string error;
        CHECK(Reconstructed::kFrameScratchAllocSpec.self_check(reinterpret_cast<void*>(o), error));
        FS::UseImplementation(before, "hand-written reconstruction");
    }
    // 0x11c890 is AVX code: the PoC refuses it (pcode2c report), nothing to load
    CHECK(GetProcAddress(m, "recomp_11c890") == nullptr);
}

void TestModes() {
    std::printf("modes and switches:\n");
    Mode m = Mode::Original;
    CHECK(ParseMode("1", m) && m == Mode::Native);
    CHECK(ParseMode("verify", m) && m == Mode::Verify);
    CHECK(ParseMode("guarded", m) && m == Mode::Guarded);
    CHECK(ParseMode("bench", m) && m == Mode::Bench);
    CHECK(ParseMode("off", m) && m == Mode::Original);
    CHECK(!ParseMode("fast", m));
    CHECK(FunctionCount() == 2);
    u32 hits = 0;
    for (int i = 0; i < 1000; ++i) {
        hits += SampleThisCall(1, 10) ? 1 : 0;
    }
    CHECK(hits == 100);
}

} // namespace

int main(int argc, char** argv) {
    std::string eboot, replay, recomp;
    u64 cases = 1'000'000;
    bool bench = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--eboot" && i + 1 < argc) {
            eboot = argv[++i];
        } else if (a == "--cases" && i + 1 < argc) {
            cases = std::strtoull(argv[++i], nullptr, 10);
        } else if (a == "--bench") {
            bench = true;
        } else if (a == "--replay" && i + 1 < argc) {
            replay = argv[++i];
        } else if (a == "--recomp" && i + 1 < argc) {
            recomp = argv[++i];
        }
    }
    TestModes();
    TestAbiBoundary();
    TestClone();
    if (!eboot.empty()) {
        Image image;
        CHECK(MapEboot(eboot, image));
        if (image.base != 0) {
            const auto oc_ref =
                reinterpret_cast<OC::Fn>(PristineCopy(image, Reconstructed::kObjectConstantsSpec.function));
            const auto fs_ref =
                reinterpret_cast<FS::Fn>(PristineCopy(image, Reconstructed::kFrameScratchAllocSpec.function));
            CHECK(oc_ref != nullptr && fs_ref != nullptr);
            if (oc_ref != nullptr && fs_ref != nullptr) {
                std::string error;
                CHECK(Reconstructed::kObjectConstantsSpec.self_check(reinterpret_cast<void*>(oc_ref), error));
                CHECK(Reconstructed::kFrameScratchAllocSpec.self_check(reinterpret_cast<void*>(fs_ref), error));
                if (!error.empty()) {
                    std::printf("  self-check: %s\n", error.c_str());
                }
                TestObjectConstantsDifferential(oc_ref, cases);
                TestFrameScratchDifferential(fs_ref, cases);
                std::printf("allocator under contention:\n");
                StressAllocator("original (eboot code)", fs_ref);
                StressAllocator("reconstruction", &FS::Native);
                TestInstalledModes(image);
                if (bench) {
                    Bench(image, oc_ref, fs_ref);
                }
                if (!replay.empty()) {
                    Replay(image, replay);
                }
                if (!recomp.empty()) {
                    TestRecompiled(image, recomp, cases, fs_ref);
                }
            }
        }
    } else {
        std::printf("(no --eboot: the comparisons with the game's own code are skipped)\n");
    }
    std::printf("TOTAL %d checks, %d failures - %s\n", g_checks, g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures;
}
