// SecondSonPC — self-test of SecondSonPC's TCB slot and FS patcher (Phase 3, P3.3;
// docs/PHASE3_INDEPENDENT_RUNTIME.md §5). Groups:
//   NativeFs(decode) the decoder on every form the patcher rewrites, forms it refuses, non-FS code;
//   NativeFs         a test module in host memory (read/write/execute) with "guest" functions: every
//                    form executed through its trampoline — value, flags of cmp/xor, store, negative
//                    displacement, r12/r13 bases, the red zone — patched at load time (one set) and on
//                    first use through the vectored handler (another set); a function whose range ends
//                    inside an instruction is refused by the load-time pass and patched on use; which
//                    traps the handler claims;
//   NativeFs(game)   the load-time pass over a copy of the game's eboot.bin in host memory (never the
//                    game's files or its mapped modules): every FS instruction found, none refused, no
//                    byte changed outside the patched instructions.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include "app/config.h"
#include "bridge/shadps4/selftest_threads.h"
#include "bridge/shadps4/selftest_vmm.h"
#include "loader/eh_frame.h"
#include "loader/elf.h"
#include "loader/image_file.h"
#include "runtime/cpu/exceptions.h"
#include "runtime/cpu/fs_patcher.h"
#include "runtime/cpu/tcb_slot.h"

namespace SecondSon::Bridge {

namespace {

using Cpu::FsInstruction;
using Cpu::FsOp;
namespace Elf = Loader::Elf;

struct DecodeCase {
    const char* what;
    std::vector<u8> code;
    bool fs_relative;
    bool supported;
    FsOp op;
    u8 reg, width, length;
    s32 disp;
};

void TestDecode(TestGroup& g) {
    const DecodeCase cases[] = {
        {"mov rax, fs:[0] (the game's only form)", {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0}, true, true, FsOp::Load, 0, 8, 9, 0},
        {"mov r14, fs:[0]", {0x64, 0x4C, 0x8B, 0x34, 0x25, 0, 0, 0, 0}, true, true, FsOp::Load, 14, 8, 9, 0},
        {"mov eax, fs:[-0x10] (static TLS below the TCB)", {0x64, 0x8B, 0x04, 0x25, 0xF0, 0xFF, 0xFF, 0xFF}, true, true, FsOp::Load, 0, 4, 8, -16},
        {"xor rax, fs:[0x28]", {0x64, 0x48, 0x33, 0x04, 0x25, 0x28, 0, 0, 0}, true, true, FsOp::Xor, 0, 8, 9, 0x28},
        {"cmp rcx, fs:[0x28]", {0x64, 0x48, 0x3B, 0x0C, 0x25, 0x28, 0, 0, 0}, true, true, FsOp::Cmp, 1, 8, 9, 0x28},
        {"add r9, fs:[8]", {0x64, 0x4C, 0x03, 0x0C, 0x25, 0x08, 0, 0, 0}, true, true, FsOp::Add, 9, 8, 9, 8},
        {"mov fs:[0x10], rax", {0x64, 0x48, 0x89, 0x04, 0x25, 0x10, 0, 0, 0}, true, true, FsOp::Store, 0, 8, 9, 0x10},
        {"mov rax, fs:[rax] (base register: refused)", {0x64, 0x48, 0x8B, 0x00}, true, false, FsOp::Load, 0, 0, 0, 0},
        {"mov rax, fs:[rcx*8+0x10] (index: refused)", {0x64, 0x48, 0x8B, 0x04, 0xCD, 0x10, 0, 0, 0}, true, false, FsOp::Load, 0, 0, 0, 0},
        {"mov ax, fs:[0] (16-bit: refused)", {0x64, 0x66, 0x8B, 0x04, 0x25, 0, 0, 0, 0}, true, false, FsOp::Load, 0, 0, 0, 0},
        {"xor rsp, fs:[0] (rsp beside a scratch on the stack: refused)", {0x64, 0x48, 0x33, 0x24, 0x25, 0, 0, 0, 0}, true, false, FsOp::Load, 0, 0, 0, 0},
        {"mov rax, [0] (no FS)", {0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0}, false, false, FsOp::Load, 0, 0, 0, 0},
        {"mov rax, gs:[0] (GS, not FS)", {0x65, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0}, false, false, FsOp::Load, 0, 0, 0, 0},
        {"sub eax, imm32 with an fs prefix (no memory operand)", {0x64, 0x2D, 0x65, 0x6C, 0x66, 0x2E}, false, false, FsOp::Load, 0, 0, 0, 0},
    };
    for (const auto& c : cases) {
        bool fs = false;
        const auto d = Cpu::DecodeFs(c.code.data(), c.code.size(), &fs);
        bool ok = fs == c.fs_relative && d.has_value() == c.supported;
        if (ok && d) {
            ok = d->op == c.op && d->reg == c.reg && d->width == c.width && d->length == c.length && d->disp == c.disp;
        }
        g.Check(c.what, ok,
                d ? fmt::format("fs {} op {} reg {} width {} length {} disp {}", fs, static_cast<int>(d->op), d->reg,
                                d->width, d->length, d->disp)
                  : fmt::format("fs {}, not supported", fs));
    }
    // Every supported form and register encodes within the trampoline budget; slots >= 64 too.
    bool sizes_ok = true;
    u64 longest = 0;
    alignas(16) u8 out[64];
    for (u8 op = 0; op <= static_cast<u8>(FsOp::Or); ++op) {
        for (u8 reg = 0; reg < 16; ++reg) {
            if (reg == 4 && op != static_cast<u8>(FsOp::Load)) {
                continue;
            }
            for (u32 slot : {5u, 63u, 64u, 1000u}) {
                const FsInstruction insn{static_cast<FsOp>(op), reg, 8, 9, -0x1000};
                const u64 size = Cpu::EncodeTrampoline(insn, 0x100000, slot, 0x200000, out, sizeof(out));
                sizes_ok = sizes_ok && size != 0;
                longest = std::max(longest, size);
            }
        }
    }
    g.Check("every form/register/slot encodes in <= 64 bytes", sizes_ok, fmt::format("longest {}", longest));
}

// ----------------------------------------------------------------------------------- execution

/// One "guest" function (called from the host with the Microsoft ABI: argument in rcx, result in rax):
/// its bytes and the offset of its FS instruction.
struct Fn {
    const char* what;
    std::vector<u8> code;
    u64 site;
};

const Fn kFns[] = {
    {"mov rax, fs:[0]", {0x64, 0x48, 0x8B, 0x04, 0x25, 0, 0, 0, 0, 0xC3}, 0},
    {"mov rax, fs:[8]", {0x64, 0x48, 0x8B, 0x04, 0x25, 0x08, 0, 0, 0, 0xC3}, 0},
    {"mov eax, fs:[-0x10]", {0x64, 0x8B, 0x04, 0x25, 0xF0, 0xFF, 0xFF, 0xFF, 0xC3}, 0},
    {"mov r12, fs:[0x10]", {0x41, 0x54, 0x64, 0x4C, 0x8B, 0x24, 0x25, 0x10, 0, 0, 0, 0x4C, 0x89, 0xE0, 0x41, 0x5C, 0xC3}, 2},
    {"mov r13, fs:[0]", {0x41, 0x55, 0x64, 0x4C, 0x8B, 0x2C, 0x25, 0, 0, 0, 0, 0x4C, 0x89, 0xE8, 0x41, 0x5D, 0xC3}, 2},
    {"xor rax, fs:[0x28] (value)", {0x48, 0x89, 0xC8, 0x64, 0x48, 0x33, 0x04, 0x25, 0x28, 0, 0, 0, 0xC3}, 3},
    {"cmp rcx, fs:[0x28] (flags)", {0x64, 0x48, 0x3B, 0x0C, 0x25, 0x28, 0, 0, 0, 0x0F, 0x94, 0xC0, 0x0F, 0xB6, 0xC0, 0xC3}, 0},
    {"xor rax, fs:[0x28] (flags, rcx as scratch)",
     {0x48, 0x89, 0xC8, 0x64, 0x48, 0x33, 0x04, 0x25, 0x28, 0, 0, 0, 0x0F, 0x94, 0xC0, 0x0F, 0xB6, 0xC0, 0xC3}, 3},
    {"mov fs:[0x18], rcx", {0x64, 0x48, 0x89, 0x0C, 0x25, 0x18, 0, 0, 0, 0x48, 0x31, 0xC0, 0xC3}, 0},
    {"add rax, fs:[8]", {0x48, 0x89, 0xC8, 0x64, 0x48, 0x03, 0x04, 0x25, 0x08, 0, 0, 0, 0xC3}, 3},
    {"red zone kept across xor rdx, fs:[0x28]",
     {0x48, 0x89, 0x4C, 0x24, 0xF8, 0x64, 0x48, 0x33, 0x14, 0x25, 0x28, 0, 0, 0, 0x48, 0x8B, 0x44, 0x24, 0xF8, 0xC3}, 5},
};
constexpr size_t kRedZoneFn = 10;
constexpr u64 kSlotStride = 64;

struct TestTcb {
    alignas(16) u64 block[16];
    u64* tcb() {
        return &block[4];
    }
};

/// What function `i` must return with `arg` on TCB `t`.
u64 Expected(size_t i, u64 arg, TestTcb& t) {
    u64* tcb = t.tcb();
    switch (i) {
    case 0:
    case 4:
        return tcb[0];
    case 1:
        return tcb[1];
    case 2:
        return static_cast<u32>(t.block[2]);
    case 3:
        return tcb[2];
    case 5:
        return arg ^ tcb[5];
    case 6:
    case 7:
        return arg == tcb[5] ? 1 : 0;
    case 8:
        return 0;
    case 9:
        return arg + tcb[1];
    case 10:
        return arg;
    default:
        return ~0ull;
    }
}

using GuestFn = u64 (*)(u64);

struct OtherThread {
    TestTcb* tcb;
    u8* fn;
    u64 result;
};

DWORD WINAPI OtherThreadBody(void* p) {
    auto* o = static_cast<OtherThread*>(p);
    Cpu::SetCurrentTcb(o->tcb->tcb());
    o->result = reinterpret_cast<GuestFn>(o->fn)(0);
    return 0;
}

void TestExecution(TestGroup& g) {
    // With SecondSonPC's dispatcher owning the CPU patcher's exceptions (--native cpu --native exceptions)
    // the patcher's faults and traps here go through the dispatcher; otherwise through its own handler.
    if (!Cpu::InstallFsPatcher(!(Cpu::Exceptions::Installed() && Cpu::Exceptions::CpuPatcherDelegated()))) {
        g.Check("the FS patcher's exception handler is installed", false, "AddVectoredExceptionHandler failed");
        return;
    }
    constexpr u64 kCode = 0x10000;
    constexpr u64 kTramp = 0x100000;
    auto* base =
        static_cast<u8*>(VirtualAlloc(nullptr, kCode + kTramp, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
    if (base == nullptr) {
        g.Check("test module memory", false, fmt::format("VirtualAlloc {}", GetLastError()));
        return;
    }
    // Never freed: a registered module stays registered.
    std::memset(base, 0xCC, kCode);
    const size_t n = std::size(kFns);
    const auto at = [&](size_t set, size_t i) { return base + (set * n + i) * kSlotStride; };
    for (size_t set = 0; set < 2; ++set) {
        for (size_t i = 0; i < n; ++i) {
            std::memcpy(at(set, i), kFns[i].code.data(), kFns[i].code.size());
        }
    }
    // Set 2: a function whose (claimed) range ends inside its FS instruction.
    u8* const odd = base + 2 * n * kSlotStride;
    std::memcpy(odd, kFns[0].code.data(), kFns[0].code.size());
    // A function with data after its code, as a switch jump table: `mov rax, fs:[8]; ret`, then bytes that
    // decode as another FS instruction, then a truncated one (decoding stops). The site before the ret is
    // patched at load time; the "site" in the data must stay untouched.
    u8* const tabled = odd + kSlotStride;
    const std::vector<u8> tabled_code = {0x64, 0x48, 0x8B, 0x04, 0x25, 0x08, 0, 0, 0, 0xC3,
                                         0x64, 0x48, 0x8B, 0x04, 0x25, 0x00, 0, 0, 0, 0xC4};
    std::memcpy(tabled, tabled_code.data(), tabled_code.size());
    FlushInstructionCache(GetCurrentProcess(), base, kCode);
    const u64 b = reinterpret_cast<u64>(base);
    const bool registered = Cpu::RegisterModule(b, kCode, b + kCode, kTramp, "selftest");
    g.Check("test module registered", registered, "");
    if (!registered) {
        return;
    }

    std::vector<Cpu::FunctionRange> ranges;
    for (size_t i = 0; i < n; ++i) {
        const u64 f = reinterpret_cast<u64>(at(0, i));
        ranges.push_back({f, f + kFns[i].code.size()});
    }
    ranges.push_back({reinterpret_cast<u64>(odd), reinterpret_cast<u64>(odd) + 5});
    ranges.push_back({reinterpret_cast<u64>(tabled), reinterpret_cast<u64>(tabled) + tabled_code.size()});
    const auto before = Cpu::GetStats();
    const auto r = Cpu::PatchModuleStatically(b, kCode, ranges);
    g.Check("load-time pass: every FS instruction of code patched; the odd range and the data after a ret refused",
            r.functions == n + 2 && r.patched == n + 1 && r.undecodable == 2 && r.left_to_runtime == 1 &&
                r.unsupported == 0,
            fmt::format("functions {} patched {} undecodable {} left to run time {} unsupported {}", r.functions,
                        r.patched, r.undecodable, r.left_to_runtime, r.unsupported));
    g.Check("data after the last ret of a function is not touched, the code before it is patched",
            tabled[0] == 0xE9 && std::memcmp(tabled + 9, tabled_code.data() + 9, tabled_code.size() - 9) == 0, "");
    bool jumps = true;
    for (size_t i = 0; i < n; ++i) {
        jumps = jumps && at(0, i)[kFns[i].site] == 0xE9 && at(1, i)[kFns[i].site] == 0x64;
    }
    g.Check("load-time pass wrote jumps only into the functions it was given", jumps && odd[0] == 0x64, "");

    TestTcb t{};
    u64* tcb = t.tcb();
    tcb[0] = reinterpret_cast<u64>(tcb); // self
    tcb[1] = 0x1111222233334444ull;
    tcb[2] = 0x5555666677778888ull;
    tcb[5] = 0xC0FFEE00DEADBEEFull; // the stack protector canary's place
    t.block[2] = 0x12345678ABCDEF01ull;
    void* const saved = Cpu::CurrentTcb();
    Cpu::SetCurrentTcb(tcb);
    const u64 args[] = {0xC0FFEE00DEADBEEFull, 7};
    for (size_t set = 0; set < 2; ++set) {
        for (size_t i = 0; i < n; ++i) {
            for (int call = 0; call < 2; ++call) {
                for (u64 arg : args) {
                    tcb[3] = 0;
                    const u64 got = reinterpret_cast<GuestFn>(at(set, i))(arg);
                    const u64 want = Expected(i, arg, t);
                    bool ok = got == want;
                    if (i == 8) {
                        ok = ok && tcb[3] == arg;
                    }
                    if (i == kRedZoneFn && set == 1 && call == 0 && arg == args[0]) {
                        // First use faults: Windows builds the exception frame right below rsp, over the System V
                        // red zone (one reason for the load-time pass). Reported, not required.
                        g.Info(fmt::format("INFO  red zone across the run-time patch's own fault: {}",
                                           ok ? "kept" : "overwritten by the exception frame (as expected on Windows)"));
                        continue;
                    }
                    g.Check(fmt::format("{} ({}, call {}, arg {:#x})", kFns[i].what,
                                        set == 0 ? "load time" : "run time", call + 1, arg),
                            ok, fmt::format("got {:#x} want {:#x} stored {:#x}", got, want, tcb[3]));
                }
            }
        }
    }
    const u64 odd_got = reinterpret_cast<GuestFn>(odd)(0);
    const u64 tabled_got = reinterpret_cast<GuestFn>(tabled)(0);
    g.Check("the function with data after its ret runs through its load-time trampoline", tabled_got == tcb[1],
            fmt::format("{:#x}", tabled_got));
    const auto after = Cpu::GetStats();
    g.Check("run time: every set-2 function and the odd one patched on first use, each once",
            after.runtime_sites - before.runtime_sites == n + 1 && odd[0] == 0xE9 && odd_got == tcb[0],
            fmt::format("run-time sites +{}", after.runtime_sites - before.runtime_sites));
    // A thread on another TCB reads its own through the same trampoline.
    TestTcb t2{};
    t2.tcb()[0] = reinterpret_cast<u64>(t2.tcb());
    OtherThread other{&t2, at(0, 0), 0};
    if (HANDLE h = CreateThread(nullptr, 0, &OtherThreadBody, &other, 0, nullptr)) {
        WaitForSingleObject(h, 10000);
        CloseHandle(h);
    }
    g.Check("another thread reads its own TCB through the same trampoline", other.result == t2.tcb()[0],
            fmt::format("{:#x} vs {:#x}", other.result, t2.tcb()[0]));
    Cpu::SetCurrentTcb(saved);

    // Traps: only this patcher's own sites.
    u8* const genuine = base + kCode - 16; // a plain int3 (the fill) inside the module
    const u64 site = reinterpret_cast<u64>(at(1, 0));
    g.Check("a breakpoint at a patched site, at it or one past it, is the patcher's",
            Cpu::Test::ClaimsTrap(site, false) && Cpu::Test::ClaimsTrap(site + 1, false), "");
    g.Check("a privileged instruction reported at a patched site is the patcher's (the kernel's misreport)",
            Cpu::Test::ClaimsTrap(site, true), "");
    g.Check("a privileged instruction one past a site is not", !Cpu::Test::ClaimsTrap(site + 1, true), "");
    g.Check("an int3 of the module that is no site is not the patcher's",
            !Cpu::Test::ClaimsTrap(reinterpret_cast<u64>(genuine), false) && genuine[0] == 0xCC, "");
    g.Check("a trap outside every registered module is not the patcher's",
            !Cpu::Test::ClaimsTrap(reinterpret_cast<u64>(&OtherThreadBody), false), "");
}

// ------------------------------------------------------------------------------- the game's code

} // namespace

bool CopyEbootCode(const App::Config& cfg, EbootCopy& out, std::string& error) {
    Loader::ImageFile file;
    if (!file.Open(cfg.game_dir / "eboot.bin", error)) {
        error = "eboot.bin not readable: " + error;
        return false;
    }
    const Elf::ProgramHeader* exec = nullptr;
    const Elf::ProgramHeader* eh = nullptr;
    for (const auto& p : file.ProgramHeaders()) {
        if (p.type == Elf::PT_LOAD && (p.flags & Elf::PF_EXEC) != 0 && exec == nullptr) {
            exec = &p;
        } else if (p.type == Elf::PT_GNU_EH_FRAME) {
            eh = &p;
        }
    }
    if (exec == nullptr || eh == nullptr || eh->vaddr < exec->vaddr ||
        eh->vaddr + eh->memsz > exec->vaddr + exec->filesz) {
        error = "eboot.bin: no executable segment holding .eh_frame_hdr";
        return false;
    }
    const u64 code_size = (exec->memsz + 0xFFFF) & ~u64{0xFFFF};
    constexpr u64 kTramp = 4ull << 20;
    auto* copy = static_cast<u8*>(VirtualAlloc(nullptr, code_size + kTramp, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    out.original.resize(exec->filesz);
    if (copy == nullptr || !file.Read(exec->offset, exec->filesz, copy) ||
        !file.Read(exec->offset, exec->filesz, out.original.data())) {
        error = "eboot.bin: executable segment not copied into host memory";
        return false;
    }
    const u64 base = reinterpret_cast<u64>(copy) - exec->vaddr; // the segment at its place relative to the base
    const u64 hdr = base + eh->vaddr;
    const u64 hdr_end = hdr + eh->memsz;
    Loader::EhFrameHeader header;
    std::vector<Loader::CodeRange> fdes;
    if (!Loader::DecodeEhFrameHeader(hdr, hdr_end, header, error)) {
        error = "eboot.bin: .eh_frame_hdr: " + error;
        return false;
    }
    const u64 frame_end = header.eh_frame_ptr < hdr ? hdr : base + exec->vaddr + exec->filesz;
    if (!Loader::DecodeEhFrameFunctions(header, hdr_end, frame_end, fdes, error)) {
        error = "eboot.bin: .eh_frame functions: " + error;
        return false;
    }
    const u64 data_lo = std::min(header.eh_frame_ptr, hdr);
    const u64 data_hi = std::max(frame_end, hdr_end);
    out.ranges.clear();
    out.dead = 0;
    for (const auto& f : fdes) {
        if (f.begin < data_hi && data_lo < f.end) {
            ++out.dead;
        } else {
            out.ranges.push_back({f.begin, f.end});
        }
    }
    out.copy = copy;
    out.filesz = exec->filesz;
    out.code_size = code_size;
    out.trampoline_size = kTramp;
    out.fdes = fdes.size();
    return true;
}

namespace {

void TestGame(TestGroup& g, const App::Config& cfg) {
    EbootCopy e;
    std::string error;
    if (!CopyEbootCode(cfg, e, error)) {
        g.Check("eboot.bin: executable segment and functions in host memory", false, error);
        return;
    }
    u8* const copy = e.copy;
    const auto& original = e.original;
    // The exact counts below are those of the audited eboot.bin (CUSA00004 EU, App 01.07: 42483 FDEs); another
    // build of the game is checked for the properties only.
    const bool audited = e.fdes == 42483;
    if (!audited) {
        g.Info(fmt::format("INFO  eboot.bin has {} FDEs, not the audited 42483: exact counts not checked", e.fdes));
    }
    g.Info(fmt::format("INFO  eboot.bin: {} FDEs, {} of them describe .eh_frame itself (dead, skipped)", e.fdes,
                       e.dead));
    const u64 c = reinterpret_cast<u64>(copy);
    if (!Cpu::RegisterModule(c, e.code_size, c + e.code_size, e.trampoline_size, "eboot.bin (self-test copy)")) {
        g.Check("copy registered", false, "");
        return;
    }
    const auto r = Cpu::PatchModuleStatically(c, e.filesz, e.ranges);
    g.Info(fmt::format("INFO  eboot.bin copy: {} functions, {} FS instructions patched at load time, {} functions not "
                       "decoded to their end (jump tables), {} sites left to run time, {} unsupported",
                       r.functions, r.patched, r.undecodable, r.left_to_runtime, r.unsupported));
    // The audit found 3616 FS instructions (all `mov r64, fs:[0]`, all inside functions). 68 functions end
    // in a switch jump table inside their FDE range; their 14 sites lie before the last ret/jmp, so all
    // 3616 are patched at load time (docs/PHASE3_INDEPENDENT_RUNTIME.md §5.3).
    g.Check("every FS instruction of the game is patched at load time (the audit's 3616), none refused",
            r.unsupported == 0 && r.left_to_runtime == 0 && (!audited || (r.patched == 3616 && r.undecodable == 68)),
            fmt::format("patched {} undecodable {} left {} unsupported {}", r.patched, r.undecodable, r.left_to_runtime,
                        r.unsupported));
    // No byte changed outside a patched instruction: each differing byte lies in a 9-byte site that now
    // starts with the jump.
    u64 stray = 0, sites = 0;
    for (u64 i = 0; i < e.filesz;) {
        if (copy[i] == original[i]) {
            ++i;
            continue;
        }
        if (copy[i] == 0xE9 && i + 9 <= e.filesz && original[i] == 0x64 && original[i + 4] == 0x25) {
            ++sites;
            i += 9;
        } else {
            ++stray;
            ++i;
        }
    }
    g.Check("no byte changed outside the patched instructions", stray == 0 && sites == r.patched,
            fmt::format("{} sites, {} stray bytes", sites, stray));
}

} // namespace

std::vector<TestGroup> TestNativeFs(const App::Config& cfg) {
    std::vector<TestGroup> out(3);
    out[0].name = "NativeFs(decode)";
    TestDecode(out[0]);
    out[1].name = "NativeFs";
    TestExecution(out[1]);
    out[2].name = "NativeFs(game)";
    TestGame(out[2], cfg);
    return out;
}

} // namespace SecondSon::Bridge
