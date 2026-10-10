// SecondSonPC — self-test of the CPU patcher's SSE4a emulation (Phase 3, P3.4;
// docs/PHASE3_P34_EXCEPTIONS_CPU.md). Groups:
//   NativeCpu(sse4a)  the decoder on every form; then every form — 4-byte and REX register forms, immediate
//                     forms of insertq/extrq, movntss/movntsd — built as a small function and run twice: on this
//                     CPU's own SSE4a and after the load-time pass with the emulation forced (a trampoline, the
//                     illegal-instruction path for the 4-byte forms, the in-place rewrite for movnt*). Random
//                     inputs; the 128-bit result, rax/rcx/rdx/r8 and CF must be the hardware's. Inputs the APM
//                     leaves undefined (length + index > 64) are compared too and only reported;
//   NativeCpu(game)   the load-time pass with the emulation forced over a copy of eboot.bin: every SSE4a site of
//                     the audit (96 extrq: 23 trampolines, 73 short) and every FS site, no other byte changed.
// Needs a CPU with SSE4a (the reference); without it the comparisons are skipped.
#include <algorithm>
#include <cstring>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include "app/config.h"
#include "bridge/shadps4/selftest_threads.h"
#include "bridge/shadps4/selftest_vmm.h"
#include "runtime/cpu/fs_patcher.h"
#include "runtime/cpu/sse4a.h"

namespace SecondSon::Bridge {

namespace {

using Cpu::Sse4a::Op;

// ---- instruction encodings (Intel order: dst, src, len, idx)

std::vector<u8> Encode(Op op, u8 dst, u8 src, u8 len, u8 idx) {
    std::vector<u8> b;
    const auto rex = [&b](bool r, bool x_b) {
        if (r || x_b) {
            b.push_back(static_cast<u8>(0x40 | (r ? 4 : 0) | (x_b ? 1 : 0)));
        }
    };
    switch (op) {
    case Op::ExtrqReg:
        b.push_back(0x66);
        rex(dst >= 8, src >= 8);
        b.insert(b.end(), {0x0F, 0x79, static_cast<u8>(0xC0 | ((dst & 7) << 3) | (src & 7))});
        break;
    case Op::ExtrqImm:
        b.push_back(0x66);
        rex(false, dst >= 8);
        b.insert(b.end(), {0x0F, 0x78, static_cast<u8>(0xC0 | (dst & 7)), len, idx});
        break;
    case Op::InsertqReg:
        b.push_back(0xF2);
        rex(dst >= 8, src >= 8);
        b.insert(b.end(), {0x0F, 0x79, static_cast<u8>(0xC0 | ((dst & 7) << 3) | (src & 7))});
        break;
    case Op::InsertqImm:
        b.push_back(0xF2);
        rex(dst >= 8, src >= 8);
        b.insert(b.end(), {0x0F, 0x78, static_cast<u8>(0xC0 | ((dst & 7) << 3) | (src & 7)), len, idx});
        break;
    case Op::Movntss:
    case Op::Movntsd: // movnt* [r11 + 0x50], xmm(dst)
        b.push_back(op == Op::Movntss ? 0xF3 : 0xF2);
        b.push_back(static_cast<u8>(0x41 | (dst >= 8 ? 4 : 0)));
        b.insert(b.end(), {0x0F, 0x2B, static_cast<u8>(0x43 | ((dst & 7) << 3)), 0x50});
        break;
    }
    return b;
}

/// movdqu xmm <-> [r11 + disp8] (load: 0F 6F, store: 0F 7F)
void Movdqu(std::vector<u8>& b, bool store, u8 xmm, u8 disp) {
    b.insert(b.end(), {0xF3, static_cast<u8>(0x41 | (xmm >= 8 ? 4 : 0)), 0x0F, static_cast<u8>(store ? 0x7F : 0x6F),
                       static_cast<u8>(0x43 | ((xmm & 7) << 3)), disp});
}

constexpr u64 kRaxIn = 0x1111222233334444ull, kRcxIn = 0x5555666677778888ull, kRdxIn = 0x9999AAAABBBBCCCCull,
              kR8In = 0xDDDDEEEEFFFF0000ull;

/// A function (Microsoft ABI, rcx = io) around `insn`: io[8..9] -> xmm(dst), io[10..11] -> xmm(src), known
/// rax/rcx/rdx/r8 and CF set; after: io[0..3] = rax/rcx/rdx/r8, io[5] = rflags, io[8..9] = xmm(dst). The
/// caller's xmm(dst/src) are kept (xmm6-15 are callee-saved). Returns the code and the offset of `insn`.
std::pair<std::vector<u8>, u32> Stub(const std::vector<u8>& insn, u8 dst, u8 src) {
    std::vector<u8> b{0x49, 0x89, 0xCB}; // mov r11, rcx
    const bool two = src != dst;
    Movdqu(b, true, dst, 0x60);
    if (two) {
        Movdqu(b, true, src, 0x70);
    }
    Movdqu(b, false, dst, 0x40);
    if (two) {
        Movdqu(b, false, src, 0x50);
    }
    const auto imm = [&b](std::initializer_list<u8> op, u64 v) {
        b.insert(b.end(), op);
        for (int i = 0; i < 8; ++i) {
            b.push_back(static_cast<u8>(v >> (8 * i)));
        }
    };
    imm({0x48, 0xB8}, kRaxIn);
    imm({0x48, 0xB9}, kRcxIn);
    imm({0x48, 0xBA}, kRdxIn);
    imm({0x49, 0xB8}, kR8In);
    b.push_back(0xF9); // stc
    const u32 at = static_cast<u32>(b.size());
    b.insert(b.end(), insn.begin(), insn.end());
    b.insert(b.end(), {0x9C, 0x41, 0x8F, 0x43, 0x28});                   // pushfq; pop [r11+0x28]
    b.insert(b.end(), {0x49, 0x89, 0x03, 0x49, 0x89, 0x4B, 0x08});       // mov [r11], rax; [r11+8], rcx
    b.insert(b.end(), {0x49, 0x89, 0x53, 0x10, 0x4D, 0x89, 0x43, 0x18}); // [r11+0x10], rdx; [r11+0x18], r8
    Movdqu(b, true, dst, 0x40);
    if (two) {
        Movdqu(b, false, src, 0x70);
    }
    Movdqu(b, false, dst, 0x60);
    b.push_back(0xC3);
    return {b, at};
}

struct Case {
    const char* what;
    Op op;
    u8 dst, src, len, idx;
};

using StubFn = void (*)(u64* io);

/// Inputs: random values, a control field that is defined (length + index <= 64) or, `undefined`, not.
void RandomInput(std::mt19937_64& rng, const Case& c, bool undefined, u64 io[16]) {
    std::memset(io, 0, 16 * sizeof(u64));
    io[8] = rng();
    io[9] = rng();
    io[10] = rng();
    io[11] = rng();
    if (c.op != Op::ExtrqReg && c.op != Op::InsertqReg) {
        return;
    }
    u64 len = rng() % 64;
    u64 idx = rng() % 64;
    const u64 eff = len == 0 ? 64 : len;
    if (!undefined && eff + idx > 64) {
        idx = 64 - eff;
    } else if (undefined && eff + idx <= 64) {
        len = 40;
        idx = 40;
    }
    const u64 control = (rng() & ~0x3F3Full) | len | (idx << 8);
    (c.op == Op::ExtrqReg ? io[10] : io[11]) = control;
}

void TestForms(TestGroup& g) {
    // The decoder.
    const struct {
        const char* what;
        std::vector<u8> code;
        Op op;
        u8 dst, src, len, idx, length;
    } decodes[] = {
        {"extrq xmm0, xmm1 (4 bytes, the game's short form)", {0x66, 0x0F, 0x79, 0xC1}, Op::ExtrqReg, 0, 1, 0, 0, 4},
        {"extrq xmm0, xmm10 (REX.B, 5 bytes)", {0x66, 0x41, 0x0F, 0x79, 0xC2}, Op::ExtrqReg, 0, 10, 0, 0, 5},
        {"extrq xmm3, 16, 8", {0x66, 0x0F, 0x78, 0xC3, 16, 8}, Op::ExtrqImm, 3, 0, 16, 8, 6},
        {"insertq xmm2, xmm3", {0xF2, 0x0F, 0x79, 0xD3}, Op::InsertqReg, 2, 3, 0, 0, 4},
        {"insertq xmm6, xmm7, 12, 20", {0xF2, 0x0F, 0x78, 0xF7, 12, 20}, Op::InsertqImm, 6, 7, 12, 20, 6},
        {"movntsd [r11+0x50], xmm9", {0xF2, 0x45, 0x0F, 0x2B, 0x4B, 0x50}, Op::Movntsd, 9, 0, 0, 0, 6},
    };
    for (const auto& d : decodes) {
        const auto r = Cpu::Sse4a::Decode(d.code.data(), d.code.size());
        g.Check(fmt::format("decode: {}", d.what),
                r && r->op == d.op && r->dst == d.dst && (r->src == d.src || d.op == Op::Movntsd) && r->len == d.len &&
                    r->idx == d.idx && r->length == d.length,
                r ? fmt::format("op {} dst {} src {} len {} idx {} length {}", static_cast<int>(r->op), r->dst, r->src,
                                r->len, r->idx, r->length)
                  : std::string("not decoded"));
    }
    const u8 not_sse4a[] = {0x48, 0x8B, 0x04, 0x24};
    g.Check("decode: mov rax, [rsp] is not SSE4a", !Cpu::Sse4a::Decode(not_sse4a, sizeof(not_sse4a)), "");

    if (!Cpu::Sse4a::HostHasSse4a()) {
        g.Info("INFO  this CPU has no SSE4a: no hardware reference, the comparisons are skipped");
        return;
    }
    if (!Cpu::FsPatcherInstalled()) {
        g.Check("the CPU patcher is installed (NativeFs runs first)", false, "");
        return;
    }
    constexpr u64 kCode = 0x10000, kTramp = 0xF0000;
    auto* region = static_cast<u8*>(VirtualAlloc(nullptr, kCode + kTramp, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    auto* hw = static_cast<u8*>(VirtualAlloc(nullptr, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (region == nullptr || hw == nullptr) {
        g.Check("test memory", false, fmt::format("VirtualAlloc {}", GetLastError()));
        return;
    }
    const u64 base = reinterpret_cast<u64>(region);
    if (!Cpu::RegisterModule(base, kCode, base + kCode, kTramp, "selftest sse4a")) {
        g.Check("test module registered", false, "");
        return;
    }
    Cpu::SetSse4aEmulation(true); // forced: this CPU has SSE4a, the short forms are made to trap

    const Case cases[] = {
        {"extrq xmm0, xmm1 (4 bytes: illegal-instruction path)", Op::ExtrqReg, 0, 1, 0, 0},
        {"extrq xmm1, xmm10 (REX.B: trampoline)", Op::ExtrqReg, 1, 10, 0, 0},
        {"extrq xmm10, xmm2 (REX.R: trampoline)", Op::ExtrqReg, 10, 2, 0, 0},
        {"extrq xmm14, xmm15 (trampoline)", Op::ExtrqReg, 14, 15, 0, 0},
        {"extrq xmm3, 16, 8", Op::ExtrqImm, 3, 3, 16, 8},
        {"extrq xmm9, 0, 0 (length 64)", Op::ExtrqImm, 9, 9, 0, 0},
        {"extrq xmm5, 1, 63", Op::ExtrqImm, 5, 5, 1, 63},
        {"insertq xmm2, xmm3 (4 bytes: illegal-instruction path)", Op::InsertqReg, 2, 3, 0, 0},
        {"insertq xmm11, xmm4 (REX: trampoline)", Op::InsertqReg, 11, 4, 0, 0},
        {"insertq xmm6, xmm7, 12, 20", Op::InsertqImm, 6, 7, 12, 20},
        {"insertq xmm12, xmm13, 0, 0 (length 64)", Op::InsertqImm, 12, 13, 0, 0},
        {"movntss [r11+0x50], xmm4 (rewritten to movss)", Op::Movntss, 4, 4, 0, 0},
        {"movntsd [r11+0x50], xmm12 (rewritten to movsd)", Op::Movntsd, 12, 12, 0, 0},
    };
    std::mt19937_64 rng(0x5EC07D504A00ull);
    u64 hw_at = 0, next = 0, short_cases = 0;
    const u64 emulated_before = Cpu::GetStats().sse4a_emulated;
    for (const auto& c : cases) {
        const auto insn = Encode(c.op, c.dst, c.src, c.len, c.idx);
        const auto [code, at] = Stub(insn, c.dst, c.src);
        std::memcpy(hw + hw_at, code.data(), code.size());
        const auto hw_fn = reinterpret_cast<StubFn>(hw + hw_at);
        hw_at = (hw_at + code.size() + 63) & ~u64{63};
        const u64 slot = base + next;
        std::memcpy(reinterpret_cast<void*>(slot), code.data(), code.size());
        next = (next + code.size() + 63) & ~u64{63};
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        const Cpu::FunctionRange range{slot, slot + code.size()};
        const auto r = Cpu::PatchModuleStatically(slot, code.size(), {&range, 1});
        const bool movnt = c.op == Op::Movntss || c.op == Op::Movntsd;
        const bool is_short = insn.size() < 5 && !movnt;
        short_cases += is_short ? 1 : 0;
        const auto* site = reinterpret_cast<const u8*>(slot + at);
        const bool rewritten = movnt ? r.sse4a_inplace == 1
                               : is_short ? r.sse4a_short == 1 && site[0] == 0x0F && site[1] == 0x0B
                                          : r.sse4a_trampolines == 1 && site[0] == 0xE9;
        const auto fn = reinterpret_cast<StubFn>(slot);
        u32 bad = 0, undefined_differ = 0, regs_bad = 0;
        std::string first;
        for (u32 i = 0; i < 400; ++i) {
            const bool undefined = (c.op == Op::ExtrqReg || c.op == Op::InsertqReg) && i % 8 == 7;
            alignas(16) u64 a[16];
            alignas(16) u64 b[16];
            RandomInput(rng, c, undefined, a);
            std::memcpy(b, a, sizeof(a));
            hw_fn(a);
            fn(b);
            const bool same = std::memcmp(a, b, sizeof(a)) == 0;
            const bool regs = b[0] == kRaxIn && b[1] == kRcxIn && b[2] == kRdxIn && b[3] == kR8In && (b[5] & 1) == 1;
            regs_bad += regs ? 0 : 1;
            if (!same && undefined) {
                ++undefined_differ;
            } else if (!same) {
                if (bad++ == 0) {
                    first = fmt::format("dst {:016x}:{:016x} vs hardware {:016x}:{:016x}", b[9], b[8], a[9], a[8]);
                }
            }
        }
        g.Check(fmt::format("{}: rewritten, 400 random inputs equal the hardware (result, rax/rcx/rdx/r8, CF)", c.what),
                rewritten && bad == 0 && regs_bad == 0,
                fmt::format("rewritten {}, {} different, {} with registers/flags changed{}", rewritten, bad, regs_bad,
                            first.empty() ? std::string{} : "; first: " + first));
        if (undefined_differ != 0) {
            g.Info(fmt::format("INFO  {}: {} of 50 undefined inputs (length + index > 64) differ from this CPU", c.what,
                               undefined_differ));
        }
    }
    const u64 emulated = Cpu::GetStats().sse4a_emulated - emulated_before;
    g.Check("the 4-byte forms ran through the illegal-instruction exception every time",
            emulated == short_cases * 400, fmt::format("{} executions for {} forms", emulated, short_cases));
    Cpu::SetSse4aEmulation(false);
}

void TestGame(TestGroup& g, const App::Config& cfg) {
    EbootCopy e;
    std::string error;
    if (!CopyEbootCode(cfg, e, error)) {
        g.Check("eboot.bin: executable segment and functions in host memory", false, error);
        return;
    }
    const u64 c = reinterpret_cast<u64>(e.copy);
    if (!Cpu::RegisterModule(c, e.code_size, c + e.code_size, e.trampoline_size, "eboot.bin (SSE4a self-test copy)")) {
        g.Check("copy registered", false, "");
        return;
    }
    Cpu::SetSse4aEmulation(true);
    const auto r = Cpu::PatchModuleStatically(c, e.filesz, e.ranges);
    Cpu::SetSse4aEmulation(false);
    const bool audited = e.fdes == 42483;
    g.Info(fmt::format("INFO  eboot.bin copy, SSE4a emulated: {} trampolines, {} executed from their exception, {} movnt* "
                       "rewritten, {} left to run time; FS {} sites; {} functions keep data below rsp",
                       r.sse4a_trampolines, r.sse4a_short, r.sse4a_inplace, r.sse4a_left_to_runtime, r.patched,
                       r.red_zone_functions));
    g.Check("every SSE4a site of the game rewritten at load time (audit: 96 extrq, 23 of 5 bytes, 73 of 4)",
            r.sse4a_left_to_runtime == 0 && r.unsupported == 0 &&
                (!audited || (r.sse4a_trampolines == 23 && r.sse4a_short == 73 && r.sse4a_inplace == 0 &&
                              r.patched == 3616)),
            fmt::format("trampolines {} short {} left {} unsupported {} FS {}", r.sse4a_trampolines, r.sse4a_short,
                        r.sse4a_left_to_runtime, r.unsupported, r.patched));
    // Every changed byte lies in a rewritten instruction: an FS site (9 bytes, now a jump), an extrq of 5 bytes
    // (now a jump) or of 4 (now ud2 + int3).
    u64 stray = 0, fs = 0, jumps = 0, traps = 0;
    const auto& o = e.original;
    for (u64 i = 0; i < e.filesz;) {
        const u8* p = e.copy + i;
        if (p[0] == o[i]) {
            ++i;
            continue;
        }
        if (p[0] == 0xE9 && i + 9 <= e.filesz && o[i] == 0x64 && o[i + 4] == 0x25) {
            ++fs;
            i += 9;
        } else if (p[0] == 0xE9 && i + 5 <= e.filesz && o[i] == 0x66 && o[i + 2] == 0x0F && o[i + 3] == 0x79) {
            ++jumps;
            i += 5;
        } else if (p[0] == 0x0F && p[1] == 0x0B && i + 4 <= e.filesz && o[i] == 0x66 && o[i + 1] == 0x0F &&
                   o[i + 2] == 0x79) {
            ++traps;
            i += 4;
        } else {
            ++stray;
            ++i;
        }
    }
    g.Check("no byte changed outside the rewritten instructions",
            stray == 0 && fs == r.patched && jumps == r.sse4a_trampolines && traps == r.sse4a_short,
            fmt::format("FS {} extrq jumps {} extrq traps {} stray {}", fs, jumps, traps, stray));
}

} // namespace

std::vector<TestGroup> TestNativeCpu(const App::Config& cfg) {
    std::vector<TestGroup> out(2);
    out[0].name = "NativeCpu(sse4a)";
    TestForms(out[0]);
    out[1].name = "NativeCpu(game)";
    TestGame(out[1], cfg);
    return out;
}

} // namespace SecondSon::Bridge
