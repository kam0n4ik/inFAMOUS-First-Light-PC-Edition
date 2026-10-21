// SecondSonPC — the tests of R7-G (native G-buffer / geometry engine), owned by that branch alone
// (docs/NATIVE_ENGINE_R7_CORE.md §3). main.cpp calls R7GBufferTests with the eboot mapped (before the CUE setters'
// jumps go into the image); it returns the number of failures and adds its checks to the total through `checks`.
//
// 0x1a2030 (the geometry pass): the native pass (r7/gbuffer/geometry_pass.h) against the eboot's own code as a
// recording clone, on generated cases of both variants (gbuffer_mesh_pass.h SelfCheck); two wrong implementations
// must be caught.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include "base/abi.h"
#include <Zydis/Zydis.h>

#include "loader/image_file.h"
#include "native_engine/r7/gbuffer/arg_liveness.h"
#include "native_engine/reconstructed/gbuffer_mesh_pass.h"
#include "native_engine/reconstructed/gbuffer_trace.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

namespace MP = Reconstructed::GBufferMeshPass;
namespace R7G = NativeEngine::R7G;

int g_fail = 0;
int* g_checks = nullptr;

#define R7G_CHECK(cond)                                                                                                \
    do {                                                                                                               \
        ++*g_checks;                                                                                                   \
        if (!(cond)) {                                                                                                 \
            ++g_fail;                                                                                                  \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
        }                                                                                                              \
    } while (0)

/// The --eboot argument of this process (the tests' command line).
std::string EbootPath() {
    const std::string cmd = GetCommandLineA();
    const size_t at = cmd.find("--eboot");
    if (at == std::string::npos) {
        return {};
    }
    size_t i = at + 7;
    while (i < cmd.size() && cmd[i] == ' ') {
        ++i;
    }
    if (i < cmd.size() && cmd[i] == '"') {
        const size_t end = cmd.find('"', i + 1);
        return cmd.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1);
    }
    const size_t end = cmd.find(' ', i);
    return cmd.substr(i, end == std::string::npos ? std::string::npos : end - i);
}

/// A second copy of the eboot's loaded segments, untouched by the installs of earlier tests (0 on failure).
u64 PristineImage() {
    Loader::ImageFile file;
    std::string error;
    const std::string path = EbootPath();
    if (path.empty() || !file.Open(path, error)) {
        return 0;
    }
    u64 end = 0;
    for (const auto& p : file.ProgramHeaders()) {
        if (p.type == Loader::Elf::PT_LOAD) {
            end = std::max<u64>(end, p.vaddr + p.memsz);
        }
    }
    auto* mem = static_cast<u8*>(VirtualAlloc(nullptr, (end + 0xffff) & ~0xffffull, MEM_COMMIT | MEM_RESERVE,
                                              PAGE_READWRITE));
    if (mem == nullptr) {
        return 0;
    }
    for (const auto& p : file.ProgramHeaders()) {
        if (p.type == Loader::Elf::PT_LOAD) {
            const std::vector<u8> bytes = file.ReadSegment(p);
            std::memcpy(mem + p.vaddr, bytes.data(), bytes.size());
        }
    }
    return reinterpret_cast<u64>(mem);
}

/// One instruction as text (the site of an undeclared read; no listings).
std::string Insn(u64 address, u64 base) {
    ZydisDecoder d;
    ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisDecodedInstruction in;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&d, reinterpret_cast<const void*>(address), 15, &in, ops))) {
        return "?";
    }
    ZydisFormatter f;
    ZydisFormatterInit(&f, ZYDIS_FORMATTER_STYLE_INTEL);
    char text[128];
    if (!ZYAN_SUCCESS(ZydisFormatterFormatInstruction(&f, &in, ops, in.operand_count_visible, text, sizeof(text),
                                                      address - base, nullptr))) {
        return "?";
    }
    return text;
}

std::string Where(const R7G::ArgUse& u, u8 gpr_extra, u8 xmm_extra, u64 base) {
    std::string s;
    for (u32 b = 0; b < 14; ++b) {
        const bool extra = b < 6 ? ((gpr_extra >> b) & 1u) != 0 : ((xmm_extra >> (b - 6)) & 1u) != 0;
        if (extra) {
            char t[64];
            std::snprintf(t, sizeof(t), " %s%u@%#llx [", b < 6 ? "arg" : "xmm", b < 6 ? b : b - 6,
                          static_cast<unsigned long long>(u.where[b] - base));
            s += t;
            s += Insn(u.where[b], base);
            s += "]";
        }
    }
    return s;
}

} // namespace

int R7GBufferTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                   int& checks) {
    (void)image_size, (void)recomp_module, (void)recomp_dll;
    g_fail = 0;
    g_checks = &checks;
    std::printf("R7-G geometry pass 0x1a2030 (gbuffer_mesh_pass.h):\n");
    if (image_base == 0) {
        std::printf("  (no eboot: skipped)\n");
        return 0;
    }
    // the recorded callees' argument contracts against their own code: a register / stack slot a callee reads that its
    // spec does not compare would go unverified (0x451440's R9, found by a crash in the game)
    if (const u64 pristine = PristineImage(); pristine == 0) {
        std::printf("  contracts: no second copy of the eboot (--eboot)\n");
        R7G_CHECK(false);
    } else {
        u32 bad = 0;
        for (u32 i = 0; i < MP::CalleeCount(); ++i) {
            const MP::CalleeInfo c = MP::CalleeAt(i);
            const R7G::ArgUse u = R7G::ArgumentsRead(pristine + c.rva);
            const u8 gpr_decl = static_cast<u8>(((1u << c.gpr) - 1u) & ~c.ignore);
            const u8 xmm_decl = static_cast<u8>((1u << c.xmm) - 1u);
            const u8 stk_decl = static_cast<u8>((1u << c.stack) - 1u);
            const u8 gpr_extra = static_cast<u8>(u.gpr & ~gpr_decl);
            const u8 xmm_extra = static_cast<u8>(u.xmm & ~xmm_decl);
            const u8 stk_extra = static_cast<u8>(u.stack & ~stk_decl);
            const bool fine = u.ok && gpr_extra == 0 && xmm_extra == 0 && stk_extra == 0;
            bad += fine ? 0 : 1;
            std::printf("  contract %-28s reads gpr %02x xmm %02x stack %02x, declared %02x %02x %02x (%u instr, %u "
                        "indirect)%s%s\n",
                        c.name, u.gpr, u.xmm, u.stack, gpr_decl, xmm_decl, stk_decl, u.instructions, u.unknown,
                        fine ? "" : u.ok ? "  UNDECLARED" : "  UNDECODABLE",
                        Where(u, gpr_extra, xmm_extra, pristine).c_str());
        }
        R7G_CHECK(bad == 0);
        // the DCB allocator 0x11be90 calls (0x11be90 sets only RDI / ESI / EDX for it)
        const R7G::ArgUse alloc = R7G::ArgumentsRead(pristine + 0x626150);
        std::printf("  contract 0x626150 DCB alloc: gpr %02x xmm %02x stack %02x%s\n", alloc.gpr, alloc.xmm,
                    alloc.stack, Where(alloc, static_cast<u8>(alloc.gpr & ~7u), alloc.xmm, pristine).c_str());
        const R7G::ArgUse pass = R7G::ArgumentsRead(pristine + 0x1a2030);
        std::printf("  contract 0x1a2030 itself: gpr %02x xmm %02x stack %02x%s\n", pass.gpr, pass.xmm, pass.stack,
                    Where(pass, static_cast<u8>(pass.gpr & ~3u), pass.xmm, pristine).c_str());
        R7G_CHECK(pass.ok && pass.gpr == 0x3 && pass.xmm == 0 && pass.stack == 0);
    }
    std::string error;
    const bool prepared = MP::Prepare(image_base, error);
    R7G_CHECK(prepared);
    if (!prepared) {
        std::printf("  prepare: %s\n", error.c_str());
        return g_fail;
    }
    const u32 n = cases > 0 ? static_cast<u32>(cases) : 2000;
    const std::string a = MP::SelfCheck(n, 0x1a2030 ^ cases);
    std::printf("  native pass vs eboot 0x1a2030 (recording clone): %u cases%s%s\n", n, a.empty() ? ", equal" : ": ",
                a.c_str());
    R7G_CHECK(a.empty());
    MP::EnableStats(true);
    const std::string s = MP::SelfCheck(200, 11);
    MP::EnableStats(false);
    std::printf("  with the branch counters on: %s\n", s.empty() ? "equal" : s.c_str());
    R7G_CHECK(s.empty());
    std::printf("  branch coverage of the generated cases (per case):%s\n", MP::StatsText(200, true).c_str());
    MP::InjectFault(1);
    const std::string f1 = MP::SelfCheck(300, 7);
    MP::InjectFault(2);
    const std::string f2 = MP::SelfCheck(300, 7);
    MP::InjectFault(0);
    std::printf("  wrong implementations seen: no stencil cache \"%s\", a wrong record field \"%s\"\n", f1.c_str(),
                f2.c_str());
    R7G_CHECK(!f1.empty());
    R7G_CHECK(!f2.empty());
    std::printf("  orphan thunk calls: %llu\n",
                static_cast<unsigned long long>(Reconstructed::GTrace::OrphanCalls()));
    return g_fail;
}

} // namespace SecondSon::NativeEngine::Tests
