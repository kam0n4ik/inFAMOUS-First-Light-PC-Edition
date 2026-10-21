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
//     --recomp    the static recompiler's module (recomp.dll) against the eboot's code (needs --eboot), recomp_11c890 also
//                 under FTZ/DAZ and the directed roundings
//     --lift-check <recomp_lift.dll> [--lift-trials N]  only: every lifted instruction against the CPU running its own
//                 bytes (lift_check.cpp), N random states per instruction and MXCSR mode (default 200)
// Exit code = failures.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <fmt/format.h>
#include <immintrin.h>
#include <windows.h>
#include <xbyak/xbyak.h>

#include "engine/guest_hooks.h"
#include "engine/guest_probe.h"
#include "loader/image_file.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"
#include "native_engine/native_function_registry.h"
#include "native_engine/native_measure.h"
#include "native_engine/reconstructed/cue_commit.h"
#include "native_engine/reconstructed/cue_set_buffers.h"
#include "native_engine/reconstructed/frame_scratch.h"
#include "native_engine/reconstructed/material_bind.h"
#include "native_engine/reconstructed/object_constants.h"

namespace SecondSon::NativeEngine::Tests {
int LiftCheck(u64 image_base, u64 image_size, const std::string& dll, u64 trials); // lift_check.cpp
int GBufferTests(u64 image_base, const std::string& recomp_dll, u64 cases);       // gbuffer_tests.cpp (R5-E)
int ShadowTests(u64 image_base, u64 cases, void* recomp_module);                   // shadow_tests.cpp (R5-F)
// R6: one entry per subsystem, each in its branch's own file (r6_<subsystem>_tests.cpp)
int R6GBufferTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                   int& checks);
int R6ShadowsTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                   int& checks);
int R6CoreTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                int& checks);
}

using namespace SecondSon;
using namespace SecondSon::NativeEngine;
namespace OC = SecondSon::NativeEngine::Reconstructed::ObjectConstants;
namespace FS = SecondSon::NativeEngine::Reconstructed::FrameScratch;
namespace CSB = SecondSon::NativeEngine::Reconstructed::CueSetBuffers;
namespace MB = SecondSon::NativeEngine::Reconstructed::MaterialBind;

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
    std::string error;
    for (u32 k = 0; k < CSB::kKinds; ++k) {
        CHECK(CSB::Prepare(static_cast<CSB::Kind>(k), image.base, error));
    }
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
        s32 cue_kind = -1;
        for (u32 k = 0; k < CSB::kKinds; ++k) {
            if (key == CSB::Spec(static_cast<CSB::Kind>(k)).key) {
                cue_kind = static_cast<s32>(k);
            }
        }
        const bool ok = key == "object_constants" ? OC::Replay(oc_ref, bytes, report)
                        : key == "frame_alloc"    ? FS::Replay(fs_ref, bytes, report)
                        : cue_kind >= 0           ? CSB::Replay(static_cast<CSB::Kind>(cue_kind), bytes, report)
                                                  : (report = "unknown key", false);
        std::printf("  case %u (%s): %s\n", ++n, key.c_str(), report.c_str());
        CHECK(ok);
    }
}

void TestRecompiledEngine(const Image& image, HMODULE m, u64 cases);
void TestCueCommitRecompiled(const Image& image, HMODULE m, u64 cases);

/// The static recompilation PoC: functions machine-translated from Ghidra P-code (tools/native-reconstruction/recomp)
/// against the eboot's own code, and the recompiled allocator through the registry.
void TestRecompiled(const Image& image, const std::string& dll, u64 cases, FS::Fn fs_original, OC::Fn oc_original) {
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
    // 0x11c890 (ObjectConstants::Update: AVX, 8 arguments, two on the stack) - the same differential as the
    // hand-written one, the System V boundary through the guest-like caller, the install's self-check
    if (auto* r = reinterpret_cast<OC::Fn>(GetProcAddress(m, "recomp_11c890"))) {
        const OC::Fn before = OC::Implementation();
        OC::UseImplementation(r, "recompiled");
        TestObjectConstantsDifferential(oc_original, cases);
        // the guest thread's MXCSR is the hardware's for both: flush-to-zero/denormals-are-zero and the directed
        // roundings must give the same bits too
        const u32 host_csr = _mm_getcsr();
        for (const u32 mode : {0x9fc0u, 0x3f80u, 0x5f80u, 0x7f80u}) {
            std::printf("  MXCSR %#x:\n", mode);
            _mm_setcsr(mode);
            TestObjectConstantsDifferential(oc_original, cases / 4);
            _mm_setcsr(host_csr);
        }
        Matrices mx{};
        std::mt19937_64 rng2{3};
        Plausible(rng2, mx.world, false);
        Plausible(rng2, mx.previous, false);
        Plausible(rng2, mx.vp, true);
        Plausible(rng2, mx.extra, false);
        alignas(16) u8 block[OC::kBlockBytes] = {}, expect[OC::kBlockBytes] = {};
        oc_original(mx.world, mx.previous, mx.vp, reinterpret_cast<u64>(mx.object), mx.extra, 77, 0xff, expect);
        CallFrame f{};
        f.gpr[0] = reinterpret_cast<u64>(mx.world);
        f.gpr[1] = reinterpret_cast<u64>(mx.previous);
        f.gpr[2] = reinterpret_cast<u64>(mx.vp);
        f.gpr[3] = reinterpret_cast<u64>(mx.object);
        f.gpr[4] = reinterpret_cast<u64>(mx.extra);
        f.gpr[5] = 77;
        f.stack[0] = 0xffffffff000000ffull;
        f.stack[1] = reinterpret_cast<u64>(block);
        const u64 violations = CallAsGuest(reinterpret_cast<const void*>(r), f);
        std::printf("  recomp_11c890 through the guest-like caller: violations %#llx, block %s\n",
                    static_cast<unsigned long long>(violations),
                    std::memcmp(block, expect, sizeof(block)) == 0 ? "identical" : "DIFFERENT");
        CHECK(violations == 0);
        CHECK(std::memcmp(block, expect, sizeof(block)) == 0);
        std::string error;
        CHECK(Reconstructed::kObjectConstantsSpec.self_check(reinterpret_cast<void*>(oc_original), error));
        if (!error.empty()) {
            std::printf("  self-check: %s\n", error.c_str());
        }
        OC::UseImplementation(before, "hand-written reconstruction");
    }
    TestRecompiledEngine(image, m, std::max<u64>(cases / 10, 1000));
}

// --- the related engine functions the recompiler translates (R5-B/C): original vs recompiled ----------------------
//
// Both run on the same buffers (restored between the two) through the guest-like caller; the functions they call are
// replaced in the mapped image by recorders, so the calls (target, argument registers, the 16 bytes a V# source points
// at) are compared as well as every byte of memory.

struct CallRecord {
    u32 target;
    u64 a[6];
    u8 src[32];
    bool operator==(const CallRecord& o) const {
        return target == o.target && std::memcmp(a, o.a, sizeof(a)) == 0 && std::memcmp(src, o.src, 32) == 0;
    }
};
std::vector<CallRecord> g_calls;

/// The argument registers each recorded callee is given [A: every call site sets exactly these]; the others are
/// don't-care: after an earlier
/// call they hold whatever that callee left in caller-saved registers, which the recompiled code (like any compiler)
/// does not reproduce.
constexpr u32 Arity(u32 target) {
    // 0x626520 and the CUE setters 0x169d0 / 0x16eb0 / 0x16d10 (the material setup's callees): 5
    return target == 0x626520 || target == 0x169d0 || target == 0x16eb0 || target == 0x16d10 ? 5
           : target == 0x11bed0                                                               ? 4
                                                                                              : 3; // 0x11be90, 0x9843a0
}
/// The descriptor a CUE setter's src points at (R5-D: S# / constant-buffer V# 16 bytes, T# 32).
constexpr u32 SetterSourceBytes(u32 target) {
    return target == 0x16d10 ? 32 : target == 0x169d0 || target == 0x16eb0 ? 16 : 0;
}

template <u32 Target>
SSPC_SYSV_ABI u64 Recorder(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    CallRecord r{Target, {a0, a1, a2, a3, a4, a5}, {}};
    for (u32 k = Arity(Target); k < 6; ++k) {
        r.a[k] = 0;
    }
    if (Target == 0x626520) { // (gnm context, stage, slot, const values*, dwords: 4, or 8 for a T#)
        std::memcpy(r.src, reinterpret_cast<const void*>(a3), std::min<u64>(static_cast<u32>(a4) * 4ull, 32));
    }
    if constexpr (SetterSourceBytes(Target) != 0) { // (cue, stage, slot, count 1, descriptor*)
        std::memcpy(r.src, reinterpret_cast<const void*>(a4), SetterSourceBytes(Target));
    }
    if (Target == 0x11be90) { // (buffer slot, owner, count): the original allocates; the test hands out a fake address
        const u64 fake = 0x5a5a000000ull + (a2 << 4);
        std::memcpy(reinterpret_cast<void*>(a0), &fake, 8);
    }
    g_calls.push_back(r);
    return 0;
}

/// Replaces an image function by `mov rax, fn; jmp rax`; restores on destruction.
struct ImagePatch {
    u8* at;
    u8 saved[12];
    ImagePatch(const Image& image, u64 rva, const void* fn) : at(reinterpret_cast<u8*>(image.base + rva)) {
        std::memcpy(saved, at, 12);
        u8 code[12] = {0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0};
        const u64 target = reinterpret_cast<u64>(fn);
        std::memcpy(code + 2, &target, 8);
        std::memcpy(at, code, 12);
    }
    ~ImagePatch() {
        std::memcpy(at, saved, 12);
    }
};

/// Runs `original` and `recompiled` on one argument set; memory = the buffers both may write. Returns "" or the
/// first difference. `compare_rax` false for a function without a result (RAX is what its last callee left).
std::string RunBoth(const void* original, const void* recompiled, CallFrame f, std::vector<std::pair<u8*, size_t>> memory,
                    bool compare_rax = true) {
    std::vector<std::vector<u8>> before, after_original;
    for (auto& [p, n] : memory) {
        before.emplace_back(p, p + n);
    }
    g_calls.clear();
    CallFrame fo = f;
    const u64 vo = CallAsGuest(original, fo);
    const std::vector<CallRecord> calls_original = g_calls;
    for (auto& [p, n] : memory) {
        after_original.emplace_back(p, p + n);
    }
    for (size_t i = 0; i < memory.size(); ++i) {
        std::memcpy(memory[i].first, before[i].data(), memory[i].second);
    }
    g_calls.clear();
    CallFrame fr = f;
    const u64 vr = CallAsGuest(recompiled, fr);
    if (vo != 0 || vr != 0) {
        return fmt::format("ABI violations: original {:#x}, recompiled {:#x}", vo, vr);
    }
    if (compare_rax && fo.rax_out != fr.rax_out) {
        return fmt::format("rax: original {:#x}, recompiled {:#x}", fo.rax_out, fr.rax_out);
    }
    if (g_calls.size() != calls_original.size()) {
        return fmt::format("calls: original {}, recompiled {}", calls_original.size(), g_calls.size());
    }
    for (size_t i = 0; i < g_calls.size(); ++i) {
        if (!(g_calls[i] == calls_original[i])) {
            return fmt::format("call {} (to {:#x}) differs", i, calls_original[i].target);
        }
    }
    for (size_t i = 0; i < memory.size(); ++i) {
        const u8* p = memory[i].first;
        for (size_t k = 0; k < memory[i].second; ++k) {
            if (p[k] != after_original[i][k]) {
                return fmt::format("memory {} +{:#x}: original {:#04x}, recompiled {:#04x}", i, k, after_original[i][k],
                                   p[k]);
            }
        }
    }
    return {};
}

void TestRecompiledEngine(const Image& image, HMODULE m, u64 cases) {
    std::mt19937_64 rng{0xe9e};
    const auto report = [](const char* what, u64 n, u64 bad, const std::string& first) {
        std::printf("  %s: %llu cases, %llu different%s%s\n", what, static_cast<unsigned long long>(n),
                    static_cast<unsigned long long>(bad), first.empty() ? "" : "; first: ", first.c_str());
        CHECK(bad == 0);
    };
    // 0x11cde0: the second object-constants writer (rdi, rsi matrices; edx flags; rcx block; xmm0, xmm1)
    if (const void* r = reinterpret_cast<const void*>(GetProcAddress(m, "recomp_11cde0"))) {
        const void* o = PristineCopy(image, {0x11cde0, 0x459, 0x7195e831ed1fc41cull});
        alignas(16) float a[16], b[16];
        alignas(16) u8 block[0xc0];
        u64 bad = 0;
        std::string first;
        for (u64 n = 0; n < cases; ++n) {
            const bool plausible = (n % 4) != 0;
            for (float* p : {a, b}) {
                if (plausible) {
                    Plausible(rng, p, p == b);
                } else {
                    for (int i = 0; i < 16; ++i) {
                        p[i] = Special(rng);
                    }
                }
            }
            for (auto& x : block) {
                x = static_cast<u8>(rng());
            }
            if (n % 8 == 1) { // unchanged values: the "no change" paths
                std::memcpy(block, a, 48);
            }
            CallFrame f{};
            f.gpr[0] = reinterpret_cast<u64>(a);
            f.gpr[1] = reinterpret_cast<u64>(b);
            f.gpr[2] = rng() & ((n % 3 == 0) ? 0xffffffffull : 0xfull);
            f.gpr[3] = reinterpret_cast<u64>(block);
            for (int k = 0; k < 8; ++k) {
                for (int l = 0; l < 4; ++l) {
                    const float v = Special(rng);
                    std::memcpy(f.xmm[k] + 4 * l, &v, 4);
                }
            }
            const std::string d = RunBoth(o, r, f, {{block, sizeof(block)}, {reinterpret_cast<u8*>(a), 64},
                                                    {reinterpret_cast<u8*>(b), 64}});
            if (!d.empty() && bad++ == 0) {
                first = fmt::format("case {}: {}", n, d);
            }
        }
        report("recomp_11cde0 vs eboot 0x11cde0", cases, bad, first);
    }
    // the CUE setters 0x16aa0 / 0x16b70 (R5-C), 0x169d0 / 0x16d10 / 0x16eb0 (R5-D): (rdi CUE, esi stage, edx first
    // slot, ecx count, r8 descriptors)
    const ImagePatch gnm_set(image, 0x626520, reinterpret_cast<const void*>(&Recorder<0x626520>));
    for (u32 k = 0; k < CSB::kKinds; ++k) {
        const Engine::GuestFunction& fn = CSB::Spec(static_cast<CSB::Kind>(k)).function;
        const u64 rva = fn.rva;
        const std::string name = fmt::format("recomp_{:x}", rva);
        const void* r = reinterpret_cast<const void*>(GetProcAddress(m, name.c_str()));
        if (r == nullptr) {
            continue;
        }
        const void* o = PristineCopy(image, fn);
        static std::vector<u8> cue(0x20000);
        static std::vector<u8> table(0x400);
        static std::vector<u8> src(64 * 32);
        u64 bad = 0;
        std::string first;
        for (u64 n = 0; n < cases; ++n) {
            const u32 stage = static_cast<u32>(rng() % 7), start = static_cast<u32>(rng() % 64),
                      count = static_cast<u32>(rng() % 33);
            for (size_t i = 0; i < table.size(); i += 2) {
                u16 w;
                switch (rng() % 4) {
                case 0: w = 0xffff; break;
                case 1: w = static_cast<u16>(0x8000 | (rng() & 0x3fff)); break;
                default: w = static_cast<u16>(rng() & 0x3fff); break;
                }
                std::memcpy(table.data() + i, &w, 2);
            }
            for (auto& x : src) {
                x = static_cast<u8>(rng());
            }
            const u64 t = reinterpret_cast<u64>(table.data());
            std::memcpy(cue.data() + 0xc180 + 8 * stage, &t, 8);
            const u64 gnm = rng();
            std::memcpy(cue.data() + 0xc090, &gnm, 8);
            cue[0xc1d0 + stage] = static_cast<u8>(rng());
            CallFrame f{};
            f.gpr[0] = reinterpret_cast<u64>(cue.data());
            f.gpr[1] = stage | (rng() << 32);
            f.gpr[2] = start;
            f.gpr[3] = count;
            f.gpr[4] = reinterpret_cast<u64>(src.data());
            f.gpr[5] = rng();
            const std::string d = RunBoth(o, r, f, {{cue.data(), cue.size()}});
            if (!d.empty() && bad++ == 0) {
                first = fmt::format("case {}: {}", n, d);
            }
        }
        report(fmt::format("{} vs eboot {:#x} (calls to 0x626520 recorded)", name, rva).c_str(), cases, bad, first);
    }
    TestCueCommitRecompiled(image, m, std::max<u64>(cases / 5, 1000));
    // 0x120280: draw constants (rdi object): 0x11be90, 0x9843a0 (copy), 0x11bed0 twice, the second as a tail call
    if (const void* r = reinterpret_cast<const void*>(GetProcAddress(m, "recomp_120280"))) {
        const void* o = PristineCopy(image, {0x120280, 0x9d, 0xad95be1456114d2aull});
        const ImagePatch p1(image, 0x11be90, reinterpret_cast<const void*>(&Recorder<0x11be90>));
        const ImagePatch p2(image, 0x9843a0, reinterpret_cast<const void*>(&Recorder<0x9843a0>));
        const ImagePatch p3(image, 0x11bed0, reinterpret_cast<const void*>(&Recorder<0x11bed0>));
        static std::vector<u8> object(0xe000);
        u64 bad = 0;
        std::string first;
        for (u64 n = 0; n < cases; ++n) {
            for (size_t i = 0xda00; i < object.size(); ++i) {
                object[i] = static_cast<u8>(rng());
            }
            const s32 count = (n % 4 == 0) ? 0 : static_cast<s32>(rng() % 64) - (n % 9 == 0 ? 70 : 0);
            std::memcpy(object.data() + 0xde18, &count, 4);
            object[0xde1c] = static_cast<u8>(n % 3 == 0 ? 0 : rng());
            const u64 slot = (n % 5 == 0) ? 0 : rng();
            std::memcpy(object.data() + 0xde20, &slot, 8);
            CallFrame f{};
            f.gpr[0] = reinterpret_cast<u64>(object.data());
            for (int k = 1; k < 6; ++k) {
                f.gpr[k] = rng();
            }
            const std::string d = RunBoth(o, r, f, {{object.data(), object.size()}});
            if (!d.empty() && bad++ == 0) {
                first = fmt::format("case {}: {}", n, d);
            }
        }
        report("recomp_120280 vs eboot 0x120280 (calls recorded, tail call included)", cases, bad, first);
    }
}

// --- the CUE buffer setters 0x16aa0 / 0x16b70 (R5-C): reconstruction, recording clone, registry ----------------------

/// A CUE as the setters see it: the stage's table pointer, the context, the changed byte, the descriptors; entries of
/// every kind (unused, direct setUserDataRegion, in the stage's window, anywhere the 14 bits reach).
struct CueCase {
    std::vector<u8> cue = std::vector<u8>(0x20000);
    std::vector<u8> table = std::vector<u8>(0x200);
    std::vector<u8> src = std::vector<u8>(64 * 32); // up to 64 T# (32 bytes)
    u32 stage = 0, slot = 0, count = 0;

    void Generate(std::mt19937_64& rng) {
        stage = static_cast<u32>(rng() % 8);
        slot = static_cast<u32>(rng() % 32);
        count = static_cast<u32>(rng() % 33);
        for (size_t i = 0; i < table.size(); i += 2) {
            u16 w;
            switch (rng() % 4) {
            case 0: w = 0xffff; break;
            case 1: w = static_cast<u16>(0x8000 | (rng() & 0x7fff)); break;
            case 2: w = static_cast<u16>(rng() & 0x7fff); break;
            default: w = static_cast<u16>((rng() % 0x5fc) & ~3ull); break;
            }
            std::memcpy(table.data() + i, &w, 2);
        }
        for (auto& x : src) {
            x = static_cast<u8>(rng());
        }
        const u64 t = reinterpret_cast<u64>(table.data());
        std::memcpy(cue.data() + 0xc180 + 8 * stage, &t, 8);
        const u64 context = rng();
        std::memcpy(cue.data() + 0xc090, &context, 8);
        cue[0xc1d0 + stage] = static_cast<u8>(rng());
    }
    /// The guest's registers: the 32-bit arguments with garbage above (the original reads ESI/EDX/ECX).
    CallFrame Frame(std::mt19937_64& rng) {
        CallFrame f{};
        f.gpr[0] = reinterpret_cast<u64>(cue.data());
        f.gpr[1] = stage | (rng() << 32);
        f.gpr[2] = slot | (rng() << 32);
        f.gpr[3] = count | (rng() << 32);
        f.gpr[4] = reinterpret_cast<u64>(src.data());
        f.gpr[5] = rng();
        return f;
    }
};

/// A wrong implementation (sets the changed byte to 2): verification must see it.
SSPC_SYSV_ABI u64 BrokenSetBuffers(u64 cue, u32 stage, u32 slot, u32 count, u64 src) {
    const u64 r = CSB::NativeBuffers(cue, stage, slot, count, src);
    *reinterpret_cast<u8*>(cue + 0xc1d0 + static_cast<u64>(static_cast<s64>(static_cast<s32>(stage)))) = 2;
    return r;
}
void NoRedirect(u64, u64) {}

void TestCueSetters(const Image& image, HMODULE m, u64 cases) {
    std::printf("CUE setters 0x16aa0 / 0x16b70 (R5-C), 0x169d0 / 0x16d10 / 0x16eb0 (R5-D):\n");
    std::mt19937_64 rng{0xc5e};
    const FunctionSpec* spec[CSB::kKinds];
    CSB::Fn hand[CSB::kKinds];
    void* reference[CSB::kKinds] = {};
    std::string error;
    bool all = true;
    for (u32 k = 0; k < CSB::kKinds; ++k) {
        spec[k] = &CSB::Spec(static_cast<CSB::Kind>(k));
        hand[k] = CSB::HandWritten(static_cast<CSB::Kind>(k));
        reference[k] = PristineCopy(image, spec[k]->function);
        CHECK(reference[k] != nullptr);
        CHECK(CSB::Prepare(static_cast<CSB::Kind>(k), image.base, error));
        CHECK(CSB::RecordingOriginal(static_cast<CSB::Kind>(k)) != nullptr);
        all = all && reference[k] != nullptr;
    }
    if (!all) {
        return;
    }
    auto* redirect = m ? reinterpret_cast<CSB::RedirectFn>(GetProcAddress(m, "recomp_set_redirect")) : nullptr;
    struct Variant {
        const char* name;
        CSB::Fn fn[CSB::kKinds];
        CSB::RedirectFn redirect;
    };
    Variant hand_written{"hand-written", {}, nullptr};
    std::copy(std::begin(hand), std::end(hand), hand_written.fn);
    std::vector<Variant> variants{hand_written};
    if (redirect != nullptr) {
        Variant v{"recompiled", {}, redirect};
        for (u32 k = 0; k < CSB::kKinds; ++k) {
            v.fn[k] = reinterpret_cast<CSB::Fn>(GetProcAddress(m, fmt::format("recomp_{:x}", CSB::kRva[k]).c_str()));
            CHECK(v.fn[k] != nullptr);
            if (v.fn[k] == nullptr) {
                v.fn[k] = hand[k]; // (reported above; the rest still runs)
            }
        }
        variants.push_back(v);
    }
    const auto use = [&](const Variant& v) {
        for (u32 k = 0; k < CSB::kKinds; ++k) {
            CSB::UseImplementation(static_cast<CSB::Kind>(k), v.fn[k], v.name, v.redirect);
        }
    };

    // the install self-check (1000 generated cases on whole shadow buffers, recording clone vs implementation)
    for (const Variant& v : variants) {
        use(v);
        for (u32 k = 0; k < CSB::kKinds; ++k) {
            error.clear();
            const bool ok = spec[k]->self_check(nullptr, error);
            std::printf("  %s %s: install self-check %s%s%s\n", v.name, spec[k]->key, ok ? "passed" : "FAILED",
                        ok ? "" : ": ", error.c_str());
            CHECK(ok);
        }
    }
    // each implementation against the eboot through the guest-like caller, setUserDataRegion recorded
    {
        const ImagePatch gnm_set(image, CSB::kCalleeRva, reinterpret_cast<const void*>(&Recorder<0x626520>));
        CueCase c;
        for (const Variant& v : variants) {
            for (u32 k = 0; k < CSB::kKinds; ++k) {
                u64 bad = 0;
                std::string first;
                for (u64 n = 0; n < cases; ++n) {
                    c.Generate(rng);
                    const std::string d = RunBoth(reference[k], reinterpret_cast<const void*>(v.fn[k]), c.Frame(rng),
                                                  {{c.cue.data(), c.cue.size()}});
                    if (!d.empty() && bad++ == 0) {
                        first = fmt::format("case {}: {}", n, d);
                    }
                }
                std::printf("  %s %s vs eboot %#llx: %llu cases, %llu different%s%s\n", v.name, spec[k]->key,
                            static_cast<unsigned long long>(spec[k]->function.rva),
                            static_cast<unsigned long long>(cases), static_cast<unsigned long long>(bad),
                            first.empty() ? "" : "; first: ", first.c_str());
                CHECK(bad == 0);
            }
        }
    }

    // installed on the mapped image: every mode through the jump against the reference on a copy
    use(variants[0]);
    for (u32 k = 0; k < CSB::kKinds; ++k) {
        error.clear();
        const bool ok = InstallFunction(image.base, *spec[k], Mode::Native, error);
        if (!ok) {
            std::printf("  install %s: %s\n", spec[k]->key, error.c_str());
        }
        CHECK(ok);
    }
    const ImagePatch gnm_set(image, CSB::kCalleeRva, reinterpret_cast<const void*>(&Recorder<0x626520>));
    CueCase c, r;
    for (const Variant& v : variants) {
        use(v);
        for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
            for (u32 k = 0; k < CSB::kKinds; ++k) {
                spec[k]->slot->mode.store(static_cast<u8>(mode));
                spec[k]->slot->sample_period = 3;
                const auto entry = reinterpret_cast<CSB::Fn>(image.base + spec[k]->function.rva);
                const auto ref = reinterpret_cast<CSB::Fn>(reference[k]);
                u64 differ = 0;
                for (int n = 0; n < 2000; ++n) {
                    c.Generate(rng);
                    r.cue = c.cue; // the same table and descriptors (pointers into c)
                    g_calls.clear();
                    entry(reinterpret_cast<u64>(c.cue.data()), c.stage, c.slot, c.count,
                          reinterpret_cast<u64>(c.src.data()));
                    const std::vector<CallRecord> calls = g_calls;
                    g_calls.clear();
                    ref(reinterpret_cast<u64>(r.cue.data()), c.stage, c.slot, c.count,
                        reinterpret_cast<u64>(c.src.data()));
                    bool same_calls = calls.size() == g_calls.size();
                    for (size_t i = 0; same_calls && i < calls.size(); ++i) {
                        same_calls = calls[i] == g_calls[i];
                    }
                    differ += (c.cue != r.cue || !same_calls) ? 1 : 0;
                }
                CallFrame f = c.Frame(rng);
                const u64 abi = CallAsGuest(reinterpret_cast<const void*>(entry), f);
                std::printf("  %s %s %-8s: 2000 calls through the jump, different from the original %llu; guest-like "
                            "caller: violations %#llx\n",
                            v.name, spec[k]->key, ModeName(mode), static_cast<unsigned long long>(differ),
                            static_cast<unsigned long long>(abi));
                CHECK(differ == 0 && abi == 0);
            }
        }
    }
    for (u32 k = 0; k < CSB::kKinds; ++k) {
        const Slot& s = *spec[k]->slot;
        std::printf("  %s: verified %llu, mismatches %llu, skipped %llu, live-diff %llu, bench samples %llu\n",
                    spec[k]->key, static_cast<unsigned long long>(s.verified.load()),
                    static_cast<unsigned long long>(s.mismatches.load()),
                    static_cast<unsigned long long>(s.skipped.load()),
                    static_cast<unsigned long long>(s.live_diff.load()),
                    static_cast<unsigned long long>(s.bench_samples.load()));
        CHECK(s.verified.load() > 0 && s.mismatches.load() == 0 && s.live_diff.load() == 0 &&
              s.bench_samples.load() > 0);
    }

    // a wrong implementation: guarded sees it on its first sample, falls back and the result is the original's
    {
        Slot& s = *spec[CSB::kBuffers]->slot;
        CSB::UseImplementation(CSB::kBuffers, &BrokenSetBuffers, "broken (test)", &NoRedirect);
        s.mode.store(static_cast<u8>(Mode::Guarded));
        s.sample_period = 1;
        const u64 mismatches = s.mismatches.load(), fallbacks = s.fallbacks.load(), recorded = CasesRecorded();
        const auto entry = reinterpret_cast<CSB::Fn>(image.base + spec[CSB::kBuffers]->function.rva);
        c.Generate(rng);
        r.cue = c.cue;
        entry(reinterpret_cast<u64>(c.cue.data()), c.stage, c.slot, c.count, reinterpret_cast<u64>(c.src.data()));
        reinterpret_cast<CSB::Fn>(reference[CSB::kBuffers])(reinterpret_cast<u64>(r.cue.data()), c.stage, c.slot,
                                                            c.count, reinterpret_cast<u64>(c.src.data()));
        const bool caught = s.mismatches.load() == mismatches + 1 && s.fallbacks.load() == fallbacks + 1 &&
                            static_cast<Mode>(s.mode.load()) == Mode::Original && CasesRecorded() == recorded + 1;
        std::printf("  a wrong implementation under guarded: mismatch %s, fallback %s, result the original's %s\n",
                    s.mismatches.load() > mismatches ? "seen" : "MISSED",
                    s.fallbacks.load() > fallbacks ? "done" : "MISSING", c.cue == r.cue ? "yes" : "NO");
        CHECK(caught && c.cue == r.cue);
    }
    use(variants[0]);
    for (u32 k = 0; k < CSB::kKinds; ++k) {
        spec[k]->slot->mode.store(static_cast<u8>(Mode::Native));
    }
}

// --- the CUE commit 0x14ad0 (R5-D): the recompiled function against the eboot's, every side effect recorded ------
//
// 0x14ad0 turns the CUE's stage shadows into user data: it allocates snapshots (0x123d0), copies the shadows into them
// (memcpy 0x9843a0), writes SET_SH_REG / user-data packets into the DCB (0x626400, 0x626520, 0x623630, 0x624cb0) and
// binds shaders through Gnm (0x625100 ... 0x625aa0); 0x638540 builds a descriptor through a function that calls the
// HLE. All of these are replaced in the mapped image by recorders: the allocator hands out the test's arena, the
// memcpy copies into it, the rest only record. The pure builders (0x61e0c0, 0x61e330, 0x639250, 0x6393a0, 0x639450,
// 0x6394e0: no calls, no absolute stores [A: P-code]) run for real. Recorded per call: the argument registers the
// callee reads [A: read-before-write over its P-code, E:\research\native-r5\r5d\arity.py], a stack pointer as the
// 32 bytes it points at (the original and the recompiled frames lie at different depths; the stack is zeroed before
// each run, so uninitialised locals are equal). 0x14ad0 itself writes no absolute address [A: P-code]; it reads one,
// the stack guard's GOT slot (eboot+0xd9de00), which the test points at its own guard.

struct CommitCall {
    u32 target;
    u64 a[6];
    u8 content[6][32];
    bool operator==(const CommitCall& o) const {
        return target == o.target && std::memcmp(a, o.a, sizeof(a)) == 0 &&
               std::memcmp(content, o.content, sizeof(content)) == 0;
    }
};
std::vector<CommitCall> g_commit_calls;
const bool g_trace_commit = std::getenv("SSPC_TEST_TRACE_COMMIT") != nullptr; ///< each case printed (a crash)
u8* g_commit_arena = nullptr; ///< the snapshots the recorded allocator hands out
u64 g_commit_arena_used = 0, g_commit_arena_size = 0;
ULONG_PTR g_stack_lo = 0, g_stack_hi = 0;

constexpr u32 CommitArity(u32 target) {
    switch (target) {
    case 0x626520: case 0x638540: return 5;
    case 0x626400: return 4;
    case 0x9843a0: case 0x623630: case 0x624cb0: case 0x625220: case 0x625300: case 0x625370: case 0x6253e0:
    case 0x625510: case 0x6256a0: return 3;
    case 0x123d0: case 0x625100: case 0x6251c0: case 0x625a40: case 0x625aa0: return 2;
    default: return 0; // 0x984360 __stack_chk_fail
    }
}

template <u32 Target>
SSPC_SYSV_ABI u64 CommitRecorder(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    CommitCall r{Target, {a0, a1, a2, a3, a4, a5}, {}};
    for (u32 k = 0; k < 6; ++k) {
        if (k >= CommitArity(Target)) {
            r.a[k] = 0;
        } else if (r.a[k] >= g_stack_lo && r.a[k] < g_stack_hi) {
            // setUserDataRegion's values: a4 dwords; any other stack pointer: the 32 bytes at it
            const u64 n = Target == 0x626520 && k == 3 ? std::min<u64>(static_cast<u32>(a4) * 4ull, 32) : 32;
            std::memcpy(r.content[k], reinterpret_cast<const void*>(r.a[k]), std::min<u64>(n, g_stack_hi - r.a[k]));
            r.a[k] = 0x57ac000000000000ull | k;
        }
    }
    u64 result = 0;
    if constexpr (Target == 0x123d0) { // (cue, dwords): the next snapshot of the arena, nullptr when full
        const u64 bytes = (static_cast<u64>(static_cast<u32>(a1)) * 4 + 15) & ~15ull;
        if (g_commit_arena_used + bytes <= g_commit_arena_size) {
            result = reinterpret_cast<u64>(g_commit_arena + g_commit_arena_used);
            g_commit_arena_used += bytes;
        }
    }
    if constexpr (Target == 0x9843a0) { // memcpy(dst, src, n): made, into the arena only
        if (a0 >= reinterpret_cast<u64>(g_commit_arena) &&
            a0 + a2 <= reinterpret_cast<u64>(g_commit_arena) + g_commit_arena_size) {
            std::memcpy(reinterpret_cast<void*>(a0), reinterpret_cast<const void*>(a1), a2);
        }
        result = a0;
    }
    g_commit_calls.push_back(r);
    return result;
}

/// Zeroes the stack below the caller (the frames of the next run and their uninitialised locals).
__declspec(noinline) void WipeStackBelow() {
    volatile u8 area[0x10000];
    for (u32 i = 0; i < sizeof(area); i += 8) {
        *reinterpret_cast<volatile u64*>(&area[i]) = 0;
    }
}

/// A CUE as 0x14ad0 reads it: random contents; the dirty flags 0 or 1; the stage layouts, the shader structures and
/// the prebuilt descriptor pointing into the world; counts and snapshot offsets inside the stage shadows.
struct CommitWorld {
    static constexpr u64 kCue = 0, kCueBytes = 0x20000, kShaders = 0x20000, kShaderBytes = 0x1000,
                         kLayouts = 0x27000, kLayoutBytes = 0x200, kArena = 0x28000, kArenaBytes = 0x18000,
                         kBytes = 0x40000;
    std::vector<u8> mem = std::vector<u8>(kBytes);
    u8* cue() {
        return mem.data() + kCue;
    }
    void Put64(u64 off, u64 v) {
        std::memcpy(mem.data() + off, &v, 8);
    }
    void Generate(std::mt19937_64& rng) {
        for (u64 i = 0; i < kArena; i += 8) {
            Put64(i, rng());
        }
        std::memset(mem.data() + kArena, 0, kArenaBytes);
        const u64 base = reinterpret_cast<u64>(mem.data());
        // the six shader structures +0xc0a8..+0xc0d0 (+0xc0a8 may be null) and the prebuilt descriptor +0xc1c0
        for (u32 i = 0; i < 6; ++i) {
            Put64(0xc0a8 + 8 * i, base + kShaders + kShaderBytes * i);
        }
        if (rng() % 10 == 0) {
            Put64(0xc0a8, 0);
        }
        Put64(0xc1c0, rng() % 2 ? 0 : base + kShaders); // only passed on (0x624cb0 is recorded)
        // the six stage layouts +0xc188..+0xc1b0: snapshot dwords, user-data slots, offsets inside the snapshot
        for (u32 i = 0; i < 6; ++i) {
            const u64 at = kLayouts - 6 * kLayoutBytes + kLayoutBytes * i; // 0x26400..
            u8* t = mem.data() + at;
            const u16 count = static_cast<u16>(1 + rng() % 0x600);
            std::memcpy(t, &count, 2);
            for (u32 b = 4; b < 0x10; ++b) {
                t[b] = rng() % 3 == 0 ? 0xff : static_cast<u8>(rng() % 16);
            }
            for (u32 o = 0x16; o <= 0x20; o += 2) {
                const u16 v = static_cast<u16>(rng() % count);
                std::memcpy(t + o, &v, 2);
            }
            // the 16-byte entry it writes for a shadow slot: inside the stage's window (0x600 dwords) as in the game
            // - an index beyond it would overwrite the CUE's own pointers (case 1984 of an earlier run)
            u16 w;
            switch (rng() % 4) {
            case 0: w = 0xffff; break;
            case 1: w = static_cast<u16>(0x8000 | (rng() % 0x5fc)); break;
            default: w = static_cast<u16>(rng() % 0x5fc); break;
            }
            std::memcpy(t + 0x108, &w, 2);
            Put64(0xc188 + 8 * i, base + at);
        }
        // the dirty flags and switches 0 / 1, the stage masks
        for (u64 f = 0xc1c9; f <= 0xc1de; ++f) {
            mem[f] = static_cast<u8>(rng() % 2);
        }
        mem[0x88] = static_cast<u8>(rng() % 2);
        mem[0xc70e] = static_cast<u8>(rng() % 2);
        const u32 stages = static_cast<u32>(rng() % 4 == 0 ? rng() : (rng() % 2 ? 0x20u : 0u) | (rng() % 2 ? 1u : 0u));
        std::memcpy(mem.data() + 0xc1e0, &stages, 4);
        const u32 alt = rng() % 2 ? 0 : static_cast<u32>(rng());
        std::memcpy(mem.data() + 0xc6e4, &alt, 4);
    }
};

/// A wrong implementation: the recompiled commit, then a dirty flag it clears set again (verification must see it).
Reconstructed::CueCommit::Fn g_broken_commit_inner = nullptr;
SSPC_SYSV_ABI void BrokenCommit(u64 cue) {
    g_broken_commit_inner(cue);
    *reinterpret_cast<u8*>(cue + 0xc1d1) = 1;
}

void TestCueCommitRecompiled(const Image& image, HMODULE m, u64 cases) {
    const void* recompiled = m ? reinterpret_cast<const void*>(GetProcAddress(m, "recomp_14ad0")) : nullptr;
    if (recompiled == nullptr) {
        std::printf("CUE commit 0x14ad0: no recomp_14ad0 in the module, skipped\n");
        return;
    }
    std::printf("CUE commit 0x14ad0 (R5-D), recompiled vs the eboot, every side effect recorded:\n");
    const void* original = PristineCopy(image, {0x14ad0, 0x1184, 0x8bb6e7985e565584ull});
    CHECK(original != nullptr);
    if (original == nullptr) {
        return;
    }
    GetCurrentThreadStackLimits(&g_stack_lo, &g_stack_hi);
    if (g_trace_commit) {
        std::printf("    [image %#llx, original clone %p]\n", static_cast<unsigned long long>(image.base), original);
    }
    // the stack guard: eboot+0xd9de00 holds its address (unrelocated in the mapped image)
    static u64 guard = 0x5ec0de5ec0de5ec0ull;
    u64* got = reinterpret_cast<u64*>(image.base + 0xd9de00);
    const u64 got_saved = *got;
    *got = reinterpret_cast<u64>(&guard);
    std::vector<std::unique_ptr<ImagePatch>> patches;
    const auto patch = [&](u64 rva, const void* fn) { patches.push_back(std::make_unique<ImagePatch>(image, rva, fn)); };
#define SSPC_COMMIT_RECORD(rva) patch(rva, reinterpret_cast<const void*>(&CommitRecorder<rva>))
    SSPC_COMMIT_RECORD(0x123d0);
    SSPC_COMMIT_RECORD(0x9843a0);
    SSPC_COMMIT_RECORD(0x984360);
    SSPC_COMMIT_RECORD(0x626400);
    SSPC_COMMIT_RECORD(0x626520);
    SSPC_COMMIT_RECORD(0x623630);
    SSPC_COMMIT_RECORD(0x624cb0);
    SSPC_COMMIT_RECORD(0x625100);
    SSPC_COMMIT_RECORD(0x6251c0);
    SSPC_COMMIT_RECORD(0x625220);
    SSPC_COMMIT_RECORD(0x625300);
    SSPC_COMMIT_RECORD(0x625370);
    SSPC_COMMIT_RECORD(0x6253e0);
    SSPC_COMMIT_RECORD(0x625510);
    SSPC_COMMIT_RECORD(0x6256a0);
    SSPC_COMMIT_RECORD(0x625a40);
    SSPC_COMMIT_RECORD(0x625aa0);
    SSPC_COMMIT_RECORD(0x638540);
#undef SSPC_COMMIT_RECORD
    std::mt19937_64 rng{0x14ad0};
    CommitWorld w;
    g_commit_arena = w.mem.data() + CommitWorld::kArena;
    g_commit_arena_size = CommitWorld::kArenaBytes;
    std::vector<u8> before, after_original;
    u64 bad = 0, calls = 0, allocations = 0;
    std::string first;
    for (u64 n = 0; n < cases; ++n) {
        w.Generate(rng);
        before = w.mem;
        CallFrame f{};
        f.gpr[0] = reinterpret_cast<u64>(w.cue());
        for (int k = 1; k < 6; ++k) {
            f.gpr[k] = rng();
        }
        const auto run = [&](const void* fn, CallFrame& frame) {
            g_commit_calls.clear();
            g_commit_arena_used = 0;
            WipeStackBelow();
            return CallAsGuest(fn, frame);
        };
        CallFrame fo = f, fr = f;
        if (g_trace_commit) {
            std::printf("    [case %llu original]\n", static_cast<unsigned long long>(n));
        }
        const u64 vo = run(original, fo);
        if (g_trace_commit) {
            std::printf("    [case %llu recompiled]\n", static_cast<unsigned long long>(n));
        }
        const std::vector<CommitCall> calls_original = g_commit_calls;
        after_original = w.mem;
        w.mem = before;
        const u64 vr = run(recompiled, fr);
        std::string d;
        if (vo != 0 || vr != 0) {
            d = fmt::format("ABI violations: original {:#x}, recompiled {:#x}", vo, vr);
        } else if (std::memcmp(after_original.data() + CommitWorld::kShaders, before.data() + CommitWorld::kShaders,
                               CommitWorld::kArena - CommitWorld::kShaders) != 0) {
            d = "the original wrote the shader structures or the layouts (outside the CUE and the arena)";
        } else if (calls_original.size() != g_commit_calls.size()) {
            d = fmt::format("calls: original {}, recompiled {}", calls_original.size(), g_commit_calls.size());
        } else {
            for (size_t i = 0; d.empty() && i < calls_original.size(); ++i) {
                if (!(calls_original[i] == g_commit_calls[i])) {
                    d = fmt::format("call {} (to {:#x}) differs", i, calls_original[i].target);
                }
            }
            for (size_t k = 0; d.empty() && k < w.mem.size(); ++k) {
                if (w.mem[k] != after_original[k]) {
                    d = fmt::format("memory +{:#x}: original {:#04x}, recompiled {:#04x}", k, after_original[k], w.mem[k]);
                }
            }
        }
        calls += calls_original.size();
        for (const CommitCall& c : calls_original) {
            allocations += c.target == 0x123d0 ? 1 : 0;
        }
        if (!d.empty() && bad++ == 0) {
            first = fmt::format("case {}: {}", n, d);
        }
    }
    std::printf("  recomp_14ad0 vs eboot 0x14ad0: %llu cases (%llu recorded calls, %llu snapshots), %llu different%s%s\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(calls),
                static_cast<unsigned long long>(allocations), static_cast<unsigned long long>(bad),
                first.empty() ? "" : "; first: ", first.c_str());
    CHECK(bad == 0 && calls > cases && allocations > 0);

    // installed on the mapped image (the recompiled implementation; there is no hand-written one): the install's
    // self-check, then every mode through the jump against the reference on a copy of the world
    namespace CC = Reconstructed::CueCommit;
    const FunctionSpec& spec = Reconstructed::kCueCommitSpec;
    auto* redirect = reinterpret_cast<void (*)(u64, u64)>(GetProcAddress(m, "recomp_set_redirect"));
    CHECK(redirect != nullptr);
    std::string error;
    CC::UseImplementation(nullptr, "none", nullptr);
    CHECK(!InstallFunction(image.base, spec, Mode::Native, error)); // no implementation: refused
    std::printf("  without an implementation: refused (%s)\n", error.c_str());
    CC::UseImplementation(reinterpret_cast<CC::Fn>(const_cast<void*>(recompiled)), "recompiled (test)", redirect);
    error.clear();
    // as in the game: the install runs before the linker's relocation, the stack guard's GOT slot is still 0 (the
    // first game run crashed on it in the self-check); the self-check must not depend on it and must restore it
    *got = 0;
    const auto t0 = std::chrono::steady_clock::now();
    const bool installed = InstallFunction(image.base, spec, Mode::Native, error);
    std::printf("  install with recomp_14ad0 (GOT slot of the stack guard unrelocated): %s%s (self-check of 300 "
                "generated CUEs, %.1f ms), slot after it %#llx\n",
                installed ? "passed" : "FAILED: ", error.c_str(),
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(),
                static_cast<unsigned long long>(*got));
    CHECK(installed);
    CHECK(*got == 0);
    *got = reinterpret_cast<u64>(&guard); // "relocated" for the calls below
    if (installed) {
        const auto entry = reinterpret_cast<CC::Fn>(image.base + spec.function.rva);
        // both on the same world (restored in between): the snapshots' addresses the builders put into descriptors
        // are then the same
        const auto run_live = [&](CC::Fn fn, std::vector<CommitCall>& out) {
            g_commit_calls.clear();
            g_commit_arena = w.mem.data() + CommitWorld::kArena;
            g_commit_arena_used = 0;
            WipeStackBelow();
            fn(reinterpret_cast<u64>(w.cue()));
            out = g_commit_calls;
        };
        for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
            spec.slot->mode.store(static_cast<u8>(mode));
            spec.slot->sample_period = 3;
            u64 differ = 0;
            for (int n = 0; n < 1000; ++n) {
                w.Generate(rng);
                before = w.mem;
                std::vector<CommitCall> a, b;
                run_live(entry, a);
                after_original = w.mem; // (the installed path's result)
                w.mem = before;
                run_live(reinterpret_cast<CC::Fn>(const_cast<void*>(original)), b);
                std::string why;
                if (a.size() != b.size()) {
                    why = fmt::format("calls {} / {}", a.size(), b.size());
                }
                for (size_t i = 0; why.empty() && i < a.size(); ++i) {
                    if (!(a[i] == b[i])) {
                        why = fmt::format("call {} to {:#x} differs", i, a[i].target);
                    }
                }
                for (u64 k = 0; why.empty() && k < w.mem.size(); ++k) {
                    if (after_original[k] != w.mem[k]) {
                        why = fmt::format("memory +{:#x}: {:#04x} / {:#04x}", k, after_original[k], w.mem[k]);
                    }
                }
                if (!why.empty() && differ++ == 0) {
                    std::printf("    first difference (case %d): %s\n", n, why.c_str());
                }
            }
            std::printf("  cue_commit %-8s: 1000 calls through the jump, different from the original %llu\n",
                        ModeName(mode), static_cast<unsigned long long>(differ));
            CHECK(differ == 0);
        }
        const Slot& s = *spec.slot;
        std::printf("  cue_commit: verified %llu, mismatches %llu, skipped %llu, bench samples %llu\n",
                    static_cast<unsigned long long>(s.verified.load()),
                    static_cast<unsigned long long>(s.mismatches.load()),
                    static_cast<unsigned long long>(s.skipped.load()),
                    static_cast<unsigned long long>(s.bench_samples.load()));
        CHECK(s.verified.load() > 0 && s.mismatches.load() == 0 && s.skipped.load() == 0 &&
              s.bench_samples.load() > 0);
        // a wrong implementation (the recompiled one, then a dirty flag set again): guarded sees it and falls back
        g_broken_commit_inner = reinterpret_cast<CC::Fn>(const_cast<void*>(recompiled));
        CC::UseImplementation(&BrokenCommit, "broken (test)", redirect);
        Slot& g = *spec.slot;
        g.mode.store(static_cast<u8>(Mode::Guarded));
        g.sample_period = 1;
        const u64 mismatches = g.mismatches.load(), fallbacks = g.fallbacks.load();
        w.Generate(rng);
        std::vector<CommitCall> a;
        run_live(entry, a);
        const bool caught = g.mismatches.load() == mismatches + 1 && g.fallbacks.load() == fallbacks + 1 &&
                            static_cast<Mode>(g.mode.load()) == Mode::Original;
        std::printf("  a wrong implementation under guarded: mismatch %s, fallback %s\n",
                    g.mismatches.load() > mismatches ? "seen" : "MISSED",
                    g.fallbacks.load() > fallbacks ? "done" : "MISSING");
        CHECK(caught);
        CC::UseImplementation(reinterpret_cast<CC::Fn>(const_cast<void*>(recompiled)), "recompiled (test)", redirect);
        g.mode.store(static_cast<u8>(Mode::Original));
    }
    patches.clear();
    *got = got_saved;
}

// --- the material setup 0x448640 (R5-D): reconstruction, recording clone, installed below an entry probe ----------

/// A pass and a material as 0x448640 reads them: 16 texture entries (+0x2c), 4 sampler entries (+0x70) and the
/// counts after them, a descriptor block whose first V# has an address or not, sometimes no material.
struct MaterialCase {
    std::vector<u8> pass = std::vector<u8>(0x100);
    std::vector<u8> material = std::vector<u8>(0x40);
    std::vector<u8> block = std::vector<u8>(0x4000);
    u64 context = 0;
    bool none = false;

    void Generate(std::mt19937_64& rng, u64 n) {
        for (auto& x : pass) {
            x = static_cast<u8>(rng());
        }
        for (auto& x : block) {
            x = static_cast<u8>(rng());
        }
        for (u32 i = 0; i < 16; ++i) {
            const u16 ti = static_cast<u16>(rng() % 0x100);
            pass[0x2c + 4 * i + 1] = static_cast<u8>(rng() % 8);
            std::memcpy(pass.data() + 0x2c + 4 * i + 2, &ti, 2);
            if (i < 4) {
                const u16 si = static_cast<u16>(rng() % 0x100);
                pass[0x70 + 4 * i + 1] = static_cast<u8>(rng() % 8);
                std::memcpy(pass.data() + 0x70 + 4 * i + 2, &si, 2);
            }
        }
        const u32 samplers = static_cast<u32>(rng() % 5), textures = static_cast<u32>(rng() % 17);
        const s32 base = static_cast<s32>(rng() % 9) - 4;
        std::memcpy(pass.data() + 0x80, &samplers, 4);
        std::memcpy(pass.data() + 0x84, &base, 4);
        std::memcpy(pass.data() + 0x6c, &textures, 4);
        if (n % 3 == 0) {
            std::memset(block.data() + 0x400, 0, 4);
            block[0x404] = 0;
            block[0x405] &= 0xf0;
        }
        const u64 d = reinterpret_cast<u64>(block.data()) + 0x400;
        std::memcpy(material.data() + 0x28, &d, 8);
        context = rng();
        none = n % 50 == 7;
    }
    u64 Material() const {
        return none ? 0 : reinterpret_cast<u64>(material.data());
    }
    CallFrame Frame(std::mt19937_64& rng) const {
        CallFrame f{};
        f.gpr[0] = reinterpret_cast<u64>(pass.data());
        f.gpr[1] = context;
        f.gpr[2] = Material();
        f.gpr[3] = rng();
        f.gpr[4] = rng();
        f.gpr[5] = rng();
        return f;
    }
};

// a probe on 0x448640 like CITY-OPT's draw_runs: counts the calls, calls the original through its slot
using Fn6 = SSPC_SYSV_ABI void (*)(u64, u64, u64, u64, u64, u64);
void* g_material_probe_original = nullptr;
u64 g_material_probe_calls = 0;
SSPC_SYSV_ABI void MaterialProbe(u64 a, u64 b, u64 c, u64 d, u64 e, u64 f) {
    ++g_material_probe_calls;
    reinterpret_cast<Fn6>(g_material_probe_original)(a, b, c, d, e, f);
}

// a wrong implementation for guarded: it binds the constants to slot 14, and only those (its setter call goes
// through the redirection the verification sets up, like a recompiled module's)
thread_local u64 t_mb_from[4], t_mb_to[4];
thread_local u32 t_mb_redirects = 0;
void TestRedirect(u64 from, u64 to) {
    if (from == 0) {
        t_mb_redirects = 0;
    } else if (t_mb_redirects < 4) {
        t_mb_from[t_mb_redirects] = from;
        t_mb_to[t_mb_redirects++] = to;
    }
}
u64 g_mb_image = 0;
SSPC_SYSV_ABI void BrokenMaterialBind(u64 pass, u64 context, u64 material) {
    (void)pass;
    if (material == 0) {
        return;
    }
    u64 target = g_mb_image + MB::kSetterRva[MB::kConstantBuffers];
    for (u32 i = 0; i < t_mb_redirects; ++i) {
        target = t_mb_from[i] == target ? t_mb_to[i] : target;
    }
    reinterpret_cast<MB::SetterFn>(target)(context + 0x128, 1, 0xe, 1, *reinterpret_cast<const u64*>(material + 0x28));
}

void TestMaterialBind(const Image& image, HMODULE m, u64 cases) {
    std::printf("material setup 0x448640 (R5-D):\n");
    std::mt19937_64 rng{0x4486};
    const FunctionSpec& spec = Reconstructed::kMaterialBindSpec;
    void* reference = PristineCopy(image, spec.function);
    std::string error;
    CHECK(reference != nullptr);
    CHECK(MB::Prepare(image.base, error) && MB::RecordingOriginal() != nullptr);
    if (reference == nullptr || MB::RecordingOriginal() == nullptr) {
        std::printf("  prepare: %s\n", error.c_str());
        return;
    }
    g_mb_image = image.base;
    auto* redirect = m ? reinterpret_cast<void (*)(u64, u64)>(GetProcAddress(m, "recomp_set_redirect")) : nullptr;
    auto* recompiled = m ? reinterpret_cast<MB::Fn>(GetProcAddress(m, "recomp_448640")) : nullptr;
    struct Variant {
        const char* name;
        MB::Fn fn;
        void (*redirect)(u64, u64);
    };
    std::vector<Variant> variants{{"hand-written", &MB::Native, nullptr}};
    if (m != nullptr) {
        CHECK(redirect != nullptr && recompiled != nullptr);
        if (redirect != nullptr && recompiled != nullptr) {
            variants.push_back({"recompiled", recompiled, redirect});
        }
    }
    // the install self-check (2000 generated cases, recording clone vs implementation)
    for (const Variant& v : variants) {
        MB::UseImplementation(v.fn, v.name, v.redirect);
        error.clear();
        const bool ok = spec.self_check(nullptr, error);
        std::printf("  %s: install self-check %s%s%s\n", v.name, ok ? "passed" : "FAILED", ok ? "" : ": ",
                    error.c_str());
        CHECK(ok);
    }
    // against the eboot through the guest-like caller, the three setters recorded
    const ImagePatch p1(image, 0x169d0, reinterpret_cast<const void*>(&Recorder<0x169d0>));
    const ImagePatch p2(image, 0x16eb0, reinterpret_cast<const void*>(&Recorder<0x16eb0>));
    const ImagePatch p3(image, 0x16d10, reinterpret_cast<const void*>(&Recorder<0x16d10>));
    MaterialCase c;
    for (const Variant& v : variants) {
        u64 bad = 0, calls = 0;
        std::string first;
        for (u64 n = 0; n < cases; ++n) {
            c.Generate(rng, n);
            const std::string d = RunBoth(reference, reinterpret_cast<const void*>(v.fn), c.Frame(rng),
                                          {{c.pass.data(), c.pass.size()}, {c.block.data(), c.block.size()}}, false);
            calls += g_calls.size();
            if (!d.empty() && bad++ == 0) {
                first = fmt::format("case {}: {}", n, d);
            }
        }
        std::printf("  %s vs eboot 0x448640: %llu cases (%llu setter calls), %llu different%s%s\n", v.name,
                    static_cast<unsigned long long>(cases), static_cast<unsigned long long>(calls),
                    static_cast<unsigned long long>(bad), first.empty() ? "" : "; first: ", first.c_str());
        CHECK(bad == 0 && calls > cases);
    }

    // installed below an entry probe on the mapped image (as with CITY-OPT Phase 6 in the game)
    MB::UseImplementation(&MB::Native, "hand-written reconstruction", nullptr);
    const Engine::ProbeSite site{spec.function, 17};
    g_material_probe_original = Engine::InstallEntryProbe(image.base, site, reinterpret_cast<const void*>(&MaterialProbe),
                                                          error, &g_material_probe_original);
    CHECK(g_material_probe_original != nullptr);
    error.clear();
    const bool installed = InstallFunction(image.base, spec, Mode::Native, error);
    if (!installed) {
        std::printf("  install below the probe: %s\n", error.c_str());
    }
    CHECK(installed && InstalledBelowProbe(spec) && g_material_probe_original == spec.entry);
    if (!installed) {
        return;
    }
    const auto entry = reinterpret_cast<MB::Fn>(image.base + spec.function.rva);
    const auto ref = reinterpret_cast<MB::Fn>(reference);
    for (const Variant& v : variants) {
        MB::UseImplementation(v.fn, v.name, v.redirect);
        for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
            spec.slot->mode.store(static_cast<u8>(mode));
            spec.slot->sample_period = 3;
            const u64 probe_before = g_material_probe_calls;
            u64 differ = 0;
            for (int n = 0; n < 2000; ++n) {
                c.Generate(rng, n);
                g_calls.clear();
                entry(reinterpret_cast<u64>(c.pass.data()), c.context, c.Material());
                const std::vector<CallRecord> calls = g_calls;
                g_calls.clear();
                ref(reinterpret_cast<u64>(c.pass.data()), c.context, c.Material());
                bool same = calls.size() == g_calls.size();
                for (size_t i = 0; same && i < calls.size(); ++i) {
                    same = calls[i] == g_calls[i];
                }
                differ += same ? 0 : 1;
            }
            const u64 probed = g_material_probe_calls - probe_before;
            CallFrame f = c.Frame(rng);
            const u64 abi = CallAsGuest(reinterpret_cast<const void*>(entry), f);
            std::printf("  %s %-8s: 2000 calls through the probe, different from the original %llu, the probe saw "
                        "%llu; guest-like caller: violations %#llx\n",
                        v.name, ModeName(mode), static_cast<unsigned long long>(differ),
                        static_cast<unsigned long long>(probed), static_cast<unsigned long long>(abi));
            CHECK(differ == 0 && probed == 2000 && abi == 0);
        }
    }
    const Slot& s = *spec.slot;
    std::printf("  material_bind: verified %llu, mismatches %llu, skipped %llu, bench samples %llu\n",
                static_cast<unsigned long long>(s.verified.load()), static_cast<unsigned long long>(s.mismatches.load()),
                static_cast<unsigned long long>(s.skipped.load()),
                static_cast<unsigned long long>(s.bench_samples.load()));
    CHECK(s.verified.load() > 0 && s.mismatches.load() == 0 && s.skipped.load() == 0 && s.bench_samples.load() > 0);

    // a wrong implementation: guarded sees it on its first sample, falls back and the result is the original's
    {
        Slot& g = *spec.slot;
        MB::UseImplementation(&BrokenMaterialBind, "broken (test)", &TestRedirect);
        g.mode.store(static_cast<u8>(Mode::Guarded));
        g.sample_period = 1;
        const u64 mismatches = g.mismatches.load(), fallbacks = g.fallbacks.load(), recorded = CasesRecorded();
        c.Generate(rng, 1);
        g_calls.clear();
        entry(reinterpret_cast<u64>(c.pass.data()), c.context, c.Material());
        const std::vector<CallRecord> calls = g_calls;
        g_calls.clear();
        ref(reinterpret_cast<u64>(c.pass.data()), c.context, c.Material());
        const bool same = calls.size() == g_calls.size() && std::equal(calls.begin(), calls.end(), g_calls.begin());
        const bool caught = g.mismatches.load() == mismatches + 1 && g.fallbacks.load() == fallbacks + 1 &&
                            static_cast<Mode>(g.mode.load()) == Mode::Original && CasesRecorded() == recorded + 1;
        std::printf("  a wrong implementation under guarded: mismatch %s, fallback %s, result the original's %s\n",
                    g.mismatches.load() > mismatches ? "seen" : "MISSED",
                    g.fallbacks.load() > fallbacks ? "done" : "MISSING", same ? "yes" : "NO");
        CHECK(caught && same);
    }
    MB::UseImplementation(&MB::Native, "hand-written reconstruction", nullptr);
    spec.slot->mode.store(static_cast<u8>(Mode::Native));
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
    CHECK(FunctionCount() >= 11 && FunctionCount() <= kMaxFunctions);
    u32 hits = 0;
    for (int i = 0; i < 1000; ++i) {
        hits += SampleThisCall(1, 10) ? 1 : 0;
    }
    CHECK(hits == 100);
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a crash in a test keeps everything printed before it
    if (g_trace_commit) {
        AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS* e) -> LONG {
            if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
                std::printf("    access violation at %p (%s %#llx)\n", e->ExceptionRecord->ExceptionAddress,
                            e->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                            static_cast<unsigned long long>(e->ExceptionRecord->ExceptionInformation[1]));
            }
            return EXCEPTION_CONTINUE_SEARCH;
        });
    }
    std::string eboot, replay, recomp, lift;
    u64 cases = 1'000'000, lift_trials = 200;
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
        } else if (a == "--lift-check" && i + 1 < argc) {
            lift = argv[++i];
        } else if (a == "--lift-trials" && i + 1 < argc) {
            lift_trials = std::strtoull(argv[++i], nullptr, 10);
        }
    }
    if (!lift.empty()) { // only the instruction-level check, on its own pristine mapping of the eboot
        Image image;
        if (eboot.empty() || !MapEboot(eboot, image)) {
            std::printf("--lift-check needs --eboot\n");
            return 1;
        }
        const int bad = Tests::LiftCheck(image.base, image.size, lift, lift_trials);
        std::printf("LIFT CHECK %s\n", bad == 0 ? "PASS" : "FAIL");
        return bad;
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
                HMODULE recomp_module = nullptr;
                if (!recomp.empty()) {
                    TestRecompiled(image, recomp, cases, fs_ref, oc_ref);
                    recomp_module = LoadLibraryA(recomp.c_str());
                    if (auto* base = recomp_module ? reinterpret_cast<u64*>(GetProcAddress(recomp_module,
                                                                                         "recomp_image_base"))
                                                   : nullptr) {
                        *base = image.base;
                    }
                }
                g_failures += Tests::GBufferTests(image.base, recomp, cases); // R5-E
                // R5-F shadow subsystem (append-only): before TestCueSetters patches the image's setters
                g_failures += Tests::ShadowTests(image.base, std::max<u64>(cases / 10, 1000), recomp_module);
                g_checks += 1;
                // R6 subsystems (each its own file), before anything installs jumps on the mapped image
                g_failures += Tests::R6GBufferTests(image.base, image.size, recomp_module, recomp, cases, g_checks);
                g_failures += Tests::R6ShadowsTests(image.base, image.size, recomp_module, recomp, cases, g_checks);
                g_failures += Tests::R6CoreTests(image.base, image.size, recomp_module, recomp, cases, g_checks);
                // last: it installs the setters' jumps on the mapped image
                TestCueSetters(image, recomp_module, std::max<u64>(cases / 10, 1000));
                // after the setters: installs 0x448640 below an entry probe on the mapped image
                TestMaterialBind(image, recomp_module, std::max<u64>(cases / 10, 1000));
            }
        }
    } else {
        std::printf("(no --eboot: the comparisons with the game's own code are skipped)\n");
    }
    std::printf("TOTAL %d checks, %d failures - %s\n", g_checks, g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures;
}
