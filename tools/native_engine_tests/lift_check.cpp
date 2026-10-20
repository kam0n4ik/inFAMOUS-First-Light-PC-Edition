// SecondSonPC — instruction-level conformance of the static recompiler (docs/NATIVE_RECOMP_COMPILER.md).
//
// Every guest instruction pcode2c.py lifted alone (recomp_lift.dll: lift_<addr>(R, U) over Ghidra's register layout) is
// run twice on the same random machine state: once as the CPU executing the instruction's own bytes (copied into a
// stub next to the mapped eboot, RIP-relative displacements re-aimed at the same targets), once as the lifted C++.
// Compared: the 15 general registers (RSP is never an operand of a lifted instruction), CF PF AF ZF SF OF minus the
// flags the instruction leaves undefined (Zydis), YMM0-15 (32 bytes each), the memory the instruction addresses
// (base/index registers aimed at a scratch buffer; RIP-relative windows in the image) - under five MXCSR settings:
// default, FTZ+DAZ, and the three directed roundings. A fault (#DE, #GP of an aligned access) must happen in both or
// in neither. MXCSR status flags are counted separately (the lift raises them through the same instructions).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <immintrin.h>
#include <windows.h>
#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

using LiftFn = SSPC_SYSV_ABI void (*)(u8*, u8*);
struct LiftEntry {
    u64 addr;
    u32 len;
    u32 flags; ///< bit 0: the function has no reader of AF (SLEIGH leaves AF of ADD/SUB/CMP/... unchanged)
    LiftFn fn;
};
struct LiftReg {
    char name[8];
    u32 off;
    u32 size;
};

constexpr u32 kFlagMask = 0x8d5; // CF PF AF ZF SF OF
constexpr u32 kModes[] = {0x1f80, 0x9fc0, 0x3f80, 0x5f80, 0x7f80};
constexpr const char* kGpr[16] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
                                  "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15"};
constexpr struct {
    const char* name;
    u32 bit;
} kFlags[] = {{"CF", 0}, {"PF", 2}, {"AF", 4}, {"ZF", 6}, {"SF", 7}, {"DF", 10}, {"OF", 11}};

struct alignas(64) CpuState {
    u64 gpr[16]; ///< x86 encoding order
    u64 rflags;
    u32 mxcsr;
    u32 pad;
    u8 pad2[48];
    alignas(32) u8 ymm[16][32];
};

/// One block within +-2 GiB of the image: the stub's code, both states and the host's saved rsp.
struct Near {
    u8 code[0x2000];
    CpuState in;
    CpuState out;
    u64 host_rsp;
    u32 fault;
    u32 fault_code;
};

Near* g_near = nullptr;
u64 g_landing = 0;

LONG CALLBACK StubFault(EXCEPTION_POINTERS* e) {
    if (g_near == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 rip = e->ContextRecord->Rip;
    const u64 code = reinterpret_cast<u64>(g_near->code);
    if (rip < code || rip >= code + sizeof(g_near->code)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    g_near->fault = 1;
    g_near->fault_code = e->ExceptionRecord->ExceptionCode;
    e->ContextRecord->Rip = g_landing;
    return EXCEPTION_CONTINUE_EXECUTION;
}

Near* AllocateNear(u64 image_base, u64 image_size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    const u64 gran = si.dwAllocationGranularity;
    for (u64 at = (image_base + image_size + gran - 1) & ~(gran - 1); at < image_base + (1ull << 30); at += gran) {
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), sizeof(Near), MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE)) {
            return static_cast<Near*>(p);
        }
    }
    for (u64 at = (image_base - (1ull << 30)) & ~(gran - 1); at < image_base; at += gran) {
        if (void* p = VirtualAlloc(reinterpret_cast<void*>(at), sizeof(Near), MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE)) {
            return static_cast<Near*>(p);
        }
    }
    return nullptr;
}

/// stub(): saves the host (Windows x64), loads `in`, runs the instruction, stores `out`, restores the host. A fault
/// inside the stub lands on the epilogue (StubFault) with `fault` set.
struct Stub : Xbyak::CodeGenerator {
    u64 landing = 0;
    Stub(Near* n, const std::vector<u8>& bytes, size_t disp_offset, u64 rip_target)
        : Xbyak::CodeGenerator(sizeof(n->code), n->code) {
        using namespace Xbyak::util;
        const Xbyak::Reg64 saved[8] = {rbx, rbp, rdi, rsi, r12, r13, r14, r15};
        for (const auto& r : saved) {
            push(r);
        }
        sub(rsp, 0xa8);
        for (int i = 0; i < 10; ++i) {
            movdqu(ptr[rsp + 16 * i], Xbyak::Xmm(6 + i));
        }
        stmxcsr(ptr[rsp + 0xa0]);
        mov(ptr[rip + &n->host_rsp], rsp);
        ldmxcsr(ptr[rip + &n->in.mxcsr]);
        for (int i = 0; i < 16; ++i) {
            vmovdqu(Xbyak::Ymm(i), ptr[rip + &n->in.ymm[i][0]]);
        }
        push(qword[rip + &n->in.rflags]);
        popfq();
        for (int i = 0; i < 16; ++i) {
            if (i != 4) {
                mov(Xbyak::Reg64(i), ptr[rip + &n->in.gpr[i]]);
            }
        }
        const u64 insn_at = reinterpret_cast<u64>(getCurr());
        std::vector<u8> b = bytes;
        if (disp_offset != 0) {
            const s64 disp = static_cast<s64>(rip_target) - static_cast<s64>(insn_at + b.size());
            const s32 d32 = static_cast<s32>(disp);
            std::memcpy(b.data() + disp_offset, &d32, 4);
        }
        for (u8 x : b) {
            db(x);
        }
        for (int i = 0; i < 16; ++i) {
            if (i != 4) {
                mov(ptr[rip + &n->out.gpr[i]], Xbyak::Reg64(i));
            }
        }
        pushfq();
        pop(qword[rip + &n->out.rflags]);
        for (int i = 0; i < 16; ++i) {
            vmovdqu(ptr[rip + &n->out.ymm[i][0]], Xbyak::Ymm(i));
        }
        stmxcsr(ptr[rip + &n->out.mxcsr]);
        Xbyak::Label land;
        L(land);
        mov(rsp, ptr[rip + &n->host_rsp]);
        cld();
        ldmxcsr(ptr[rsp + 0xa0]);
        for (int i = 0; i < 10; ++i) {
            movdqu(Xbyak::Xmm(6 + i), ptr[rsp + 16 * i]);
        }
        add(rsp, 0xa8);
        for (int i = 7; i >= 0; --i) {
            pop(saved[i]);
        }
        vzeroupper();
        ret();
        ready();
        landing = reinterpret_cast<u64>(land.getAddress());
    }
};

float SpecialFloat(std::mt19937_64& rng) {
    static constexpr u32 kBits[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x00000001u, 0x807fffffu,
                                    0x00800000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc12345u,
                                    0x7fa00001u, 0x3effffffu, 0x4b000000u};
    u32 bits;
    const u32 pick = static_cast<u32>(rng() % 10);
    if (pick < 5) {
        const float f = static_cast<float>(static_cast<double>(static_cast<s64>(rng() % 2000001) - 1000000) / 977.0);
        std::memcpy(&bits, &f, 4);
    } else if (pick < 8) {
        bits = kBits[rng() % std::size(kBits)];
        if ((bits & 0x7f800000u) == 0x7f800000u && (bits & 0x7fffffu) != 0) {
            bits |= static_cast<u32>(rng() & 0x3fffff);
        }
    } else {
        bits = static_cast<u32>(rng());
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

u64 RandomGpr(std::mt19937_64& rng) {
    switch (rng() % 8) {
    case 0: return 0;
    case 1: return ~0ull;
    case 2: return rng() % 256;
    case 3: return static_cast<u32>(rng());
    case 4: return 1ull << (rng() % 64);
    default: return rng();
    }
}

void RandomState(std::mt19937_64& rng, CpuState& s, u32 mxcsr) {
    std::memset(&s, 0, sizeof(s));
    for (u64& g : s.gpr) {
        g = RandomGpr(rng);
    }
    u32 flags = 0x2;
    for (const auto& f : kFlags) {
        if (f.bit != 10 && (rng() & 1) != 0) {
            flags |= 1u << f.bit;
        }
    }
    s.rflags = flags;
    s.mxcsr = mxcsr;
    for (auto& y : s.ymm) {
        if (rng() % 4 == 0) {
            for (int k = 0; k < 4; ++k) { // doubles
                double d = static_cast<double>(SpecialFloat(rng));
                if (rng() % 3 == 0) {
                    const u64 b = rng();
                    std::memcpy(&d, &b, 8);
                }
                std::memcpy(y + 8 * k, &d, 8);
            }
        } else {
            for (int k = 0; k < 8; ++k) {
                const float f = SpecialFloat(rng);
                std::memcpy(y + 4 * k, &f, 4);
            }
        }
    }
}

struct MemWindow {
    u64 at;
    u32 size;
    std::vector<u8> before, native, lifted;
};

struct Aim {
    ZydisRegister base, index;
    u32 scale;
    s64 disp;
};

struct Result {
    u64 checked = 0, mismatched = 0, faults_both = 0, fault_disagree = 0, status_differ = 0;
    std::string first;
};

/// Runs the lift with the state's MXCSR; false when it faulted (recomp_fault, an access the CPU refused too).
bool RunLift(LiftFn fn, u8* r, u8* u, u32 mode, u32& csr_after) {
    const u32 host = _mm_getcsr();
    __try {
        _mm_setcsr(mode);
        fn(r, u);
        csr_after = _mm_getcsr();
        _mm_setcsr(host);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        _mm_setcsr(host);
        return false;
    }
    return true;
}

} // namespace

/// --lift-check: returns the number of instructions with a mismatch (each printed with its first difference).
int LiftCheck(u64 image_base, u64 image_size, const std::string& dll, u64 trials) {
    std::printf("instruction-level conformance (%s), %llu random states per instruction and MXCSR mode:\n",
                dll.c_str(), static_cast<unsigned long long>(trials));
    HMODULE m = LoadLibraryA(dll.c_str());
    if (m == nullptr) {
        std::printf("  cannot load %s\n", dll.c_str());
        return 1;
    }
    auto* base = reinterpret_cast<u64*>(GetProcAddress(m, "recomp_image_base"));
    auto* table = reinterpret_cast<const LiftEntry*>(GetProcAddress(m, "lift_table"));
    auto* count = reinterpret_cast<const u32*>(GetProcAddress(m, "lift_count"));
    auto* reg_bytes = reinterpret_cast<const u32*>(GetProcAddress(m, "lift_reg_bytes"));
    auto* unique_bytes = reinterpret_cast<const u32*>(GetProcAddress(m, "lift_unique_bytes"));
    auto* regfile = reinterpret_cast<const LiftReg*>(GetProcAddress(m, "lift_regfile"));
    auto* regfile_count = reinterpret_cast<const u32*>(GetProcAddress(m, "lift_regfile_count"));
    if (!base || !table || !count || !reg_bytes || !unique_bytes || !regfile || !regfile_count) {
        std::printf("  %s is not a lift module (lift_table, lift_regfile, ...)\n", dll.c_str());
        return 1;
    }
    *base = image_base;
    const auto reg = [&](const char* name) -> const LiftReg* {
        for (u32 i = 0; i < *regfile_count; ++i) {
            if (std::strcmp(regfile[i].name, name) == 0) {
                return &regfile[i];
            }
        }
        return nullptr;
    };
    const LiftReg* gpr[16];
    const LiftReg* flag[std::size(kFlags)];
    const LiftReg* ymm[16];
    for (int i = 0; i < 16; ++i) {
        gpr[i] = reg(kGpr[i]);
        char nm[8];
        std::snprintf(nm, sizeof(nm), "YMM%d", i);
        ymm[i] = reg(nm);
        if (gpr[i] == nullptr || ymm[i] == nullptr) {
            std::printf("  register file incomplete (%s / YMM%d)\n", kGpr[i], i);
            return 1;
        }
    }
    for (size_t i = 0; i < std::size(kFlags); ++i) {
        flag[i] = reg(kFlags[i].name);
        if (flag[i] == nullptr) {
            std::printf("  register file incomplete (%s)\n", kFlags[i].name);
            return 1;
        }
    }

    g_near = AllocateNear(image_base, image_size);
    if (g_near == nullptr) {
        std::printf("  no memory within 2 GiB of the image for the stub\n");
        return 1;
    }
    PVOID veh = AddVectoredExceptionHandler(1, &StubFault);
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    std::vector<u8> R(*reg_bytes), U(*unique_bytes);
    alignas(64) static u8 scratch[8192];
    alignas(64) static u8 scratch_before[8192], scratch_native[8192];
    std::mt19937_64 rng{0x11f7c4ec};
    int bad_instructions = 0;
    u64 total_runs = 0, refused = 0, af_unread = 0;

    for (u32 e = 0; e < *count; ++e) {
        const LiftEntry& le = table[e];
        const u8* code = reinterpret_cast<const u8*>(image_base + le.addr);
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, le.len, &insn, ops)) || insn.length != le.len) {
            std::printf("  %#llx: Zydis cannot decode the instruction\n", static_cast<unsigned long long>(le.addr));
            ++bad_instructions;
            continue;
        }
        char text[96] = {};
        ZydisFormatterFormatInstruction(&formatter, &insn, ops, insn.operand_count_visible, text, sizeof(text),
                                        image_base + le.addr, nullptr);
        // memory operands: RIP-relative ones keep their target (a window of the image), the others are aimed at the
        // scratch buffer through their base/index registers
        size_t disp_offset = 0;
        u64 rip_target = 0;
        std::vector<MemWindow> windows;
        std::vector<Aim> aims;
        bool skip = false;
        for (u8 i = 0; i < insn.operand_count; ++i) {
            const ZydisDecodedOperand& op = ops[i];
            if (op.type != ZYDIS_OPERAND_TYPE_MEMORY) {
                continue;
            }
            if (op.mem.type == ZYDIS_MEMOP_TYPE_AGEN) {
                // LEA reg, [rip + d]: no memory read, but the stub's copy must compute the same image address (R5-E)
                if (op.mem.base == ZYDIS_REGISTER_RIP) {
                    ZyanU64 t = 0;
                    ZydisCalcAbsoluteAddress(&insn, &op, image_base + le.addr, &t);
                    rip_target = t;
                    disp_offset = insn.raw.disp.offset;
                }
                continue;
            }
            if (op.mem.type != ZYDIS_MEMOP_TYPE_MEM || op.mem.segment == ZYDIS_REGISTER_FS ||
                op.mem.segment == ZYDIS_REGISTER_GS || insn.address_width != 64) {
                skip = true;
                break;
            }
            if (op.mem.base == ZYDIS_REGISTER_RIP) {
                ZyanU64 t = 0;
                ZydisCalcAbsoluteAddress(&insn, &op, image_base + le.addr, &t);
                rip_target = t;
                disp_offset = insn.raw.disp.offset;
                windows.push_back({(t & ~63ull) - 64, 192, {}, {}, {}});
            } else if (op.mem.base == ZYDIS_REGISTER_NONE) {
                skip = true; // an absolute address outside the image
                break;
            } else {
                aims.push_back({op.mem.base, op.mem.index, op.mem.scale, op.mem.disp.value});
            }
        }
        if (skip) {
            ++refused;
            continue;
        }
        const Stub stub(g_near, std::vector<u8>(code, code + le.len), disp_offset, rip_target);
        g_landing = stub.landing;
        u32 flag_mask = kFlagMask & ~static_cast<u32>(insn.cpu_flags ? insn.cpu_flags->undefined : 0);
        if ((le.flags & 1) != 0 && (flag_mask & 0x10) != 0) {
            flag_mask &= ~0x10u; // AF: no reader in the function, not part of the System V interface
            ++af_unread;
        }
        Result r;
        for (const u32 mode : kModes) {
            for (u64 t = 0; t < trials; ++t) {
                CpuState& in = g_near->in;
                RandomState(rng, in, mode);
                const u64 ea = reinterpret_cast<u64>(scratch) + 2048 + 64 * (rng() % 8);
                for (const Aim& a : aims) {
                    const int bi = ZydisRegisterGetId(a.base);
                    const int ii = a.index == ZYDIS_REGISTER_NONE ? -1 : ZydisRegisterGetId(a.index);
                    if (ii >= 0 && ii != bi) {
                        in.gpr[ii] = rng() % 4;
                        in.gpr[bi] = ea - static_cast<u64>(a.disp) - in.gpr[ii] * a.scale;
                    } else if (ii == bi) {
                        in.gpr[bi] = (ea - static_cast<u64>(a.disp)) / (1 + a.scale);
                    } else {
                        in.gpr[bi] = ea - static_cast<u64>(a.disp);
                    }
                }
                for (auto& x : scratch) {
                    x = static_cast<u8>(rng());
                }
                std::memcpy(scratch_before, scratch, sizeof(scratch));
                for (MemWindow& w : windows) {
                    w.before.assign(reinterpret_cast<u8*>(w.at), reinterpret_cast<u8*>(w.at) + w.size);
                }
                // the CPU
                g_near->fault = 0;
                reinterpret_cast<void (*)()>(const_cast<u8*>(stub.getCode()))();
                const bool native_fault = g_near->fault != 0;
                const CpuState out = g_near->out;
                std::memcpy(scratch_native, scratch, sizeof(scratch));
                std::memcpy(scratch, scratch_before, sizeof(scratch));
                for (MemWindow& w : windows) {
                    w.native.assign(reinterpret_cast<u8*>(w.at), reinterpret_cast<u8*>(w.at) + w.size);
                    std::memcpy(reinterpret_cast<void*>(w.at), w.before.data(), w.size);
                }
                // the lift
                std::fill(R.begin(), R.end(), u8{0});
                std::fill(U.begin(), U.end(), u8{0});
                for (int i = 0; i < 16; ++i) {
                    std::memcpy(R.data() + gpr[i]->off, &in.gpr[i], 8);
                    std::memcpy(R.data() + ymm[i]->off, in.ymm[i], 32);
                }
                for (size_t i = 0; i < std::size(kFlags); ++i) {
                    R[flag[i]->off] = static_cast<u8>((in.rflags >> kFlags[i].bit) & 1);
                }
                u32 lift_csr = 0;
                const bool lift_fault = !RunLift(le.fn, R.data(), U.data(), mode, lift_csr);
                for (MemWindow& w : windows) {
                    w.lifted.assign(reinterpret_cast<u8*>(w.at), reinterpret_cast<u8*>(w.at) + w.size);
                    std::memcpy(reinterpret_cast<void*>(w.at), w.before.data(), w.size);
                }
                ++total_runs;
                ++r.checked;
                char b[256];
                if (native_fault || lift_fault) {
                    if (native_fault && lift_fault) {
                        ++r.faults_both;
                    } else {
                        ++r.fault_disagree;
                        if (r.first.empty()) {
                            std::snprintf(b, sizeof(b), "mxcsr %#x: %s faults (%#x), the other does not", mode,
                                          native_fault ? "the CPU" : "the lift", g_near->fault_code);
                            r.first = b;
                        }
                    }
                    std::memcpy(scratch, scratch_before, sizeof(scratch));
                    continue;
                }
                std::string diff;
                for (int i = 0; i < 16 && diff.empty(); ++i) {
                    u64 v;
                    std::memcpy(&v, R.data() + gpr[i]->off, 8);
                    if (i != 4 && v != out.gpr[i]) {
                        std::snprintf(b, sizeof(b), "%s: in %#llx, CPU %#llx, lift %#llx", kGpr[i],
                                      static_cast<unsigned long long>(in.gpr[i]),
                                      static_cast<unsigned long long>(out.gpr[i]), static_cast<unsigned long long>(v));
                        diff = b;
                    }
                }
                for (size_t i = 0; i < std::size(kFlags) && diff.empty(); ++i) {
                    if (((flag_mask >> kFlags[i].bit) & 1) == 0) {
                        continue;
                    }
                    const u32 cpu = static_cast<u32>((out.rflags >> kFlags[i].bit) & 1), lift = R[flag[i]->off] & 1u;
                    if (cpu != lift) {
                        std::snprintf(b, sizeof(b), "%s: in %u, CPU %u, lift %u", kFlags[i].name,
                                      static_cast<u32>((in.rflags >> kFlags[i].bit) & 1), cpu, lift);
                        diff = b;
                    }
                }
                for (int i = 0; i < 16 && diff.empty(); ++i) {
                    if (std::memcmp(R.data() + ymm[i]->off, out.ymm[i], 32) != 0) {
                        u32 at = 0;
                        while (R[ymm[i]->off + at] == out.ymm[i][at]) {
                            ++at;
                        }
                        at &= ~3u;
                        u32 vi, vc, vl;
                        std::memcpy(&vi, in.ymm[i] + at, 4);
                        std::memcpy(&vc, out.ymm[i] + at, 4);
                        std::memcpy(&vl, R.data() + ymm[i]->off + at, 4);
                        std::snprintf(b, sizeof(b), "YMM%d byte %u: in %08x, CPU %08x, lift %08x", i, at, vi, vc, vl);
                        diff = b;
                    }
                }
                if (diff.empty() && std::memcmp(scratch, scratch_native, sizeof(scratch)) != 0) {
                    u32 at = 0;
                    while (scratch[at] == scratch_native[at]) {
                        ++at;
                    }
                    std::snprintf(b, sizeof(b), "memory +%#x: CPU %02x, lift %02x", at, scratch_native[at], scratch[at]);
                    diff = b;
                }
                for (const MemWindow& w : windows) {
                    if (diff.empty() && w.native != w.lifted) {
                        std::snprintf(b, sizeof(b), "image memory at %#llx differs",
                                      static_cast<unsigned long long>(w.at - image_base));
                        diff = b;
                    }
                }
                std::memcpy(scratch, scratch_before, sizeof(scratch));
                if ((out.mxcsr & 0x3f) != (lift_csr & 0x3f)) {
                    ++r.status_differ;
                }
                if (!diff.empty()) {
                    ++r.mismatched;
                    if (r.first.empty()) {
                        std::snprintf(b, sizeof(b), "mxcsr %#x: ", mode);
                        r.first = b + diff;
                    }
                }
            }
        }
        if (r.mismatched != 0 || r.fault_disagree != 0) {
            ++bad_instructions;
            std::printf("  MISMATCH %#llx %-40s %llu of %llu runs (%llu fault disagreements); first: %s\n",
                        static_cast<unsigned long long>(le.addr), text, static_cast<unsigned long long>(r.mismatched),
                        static_cast<unsigned long long>(r.checked), static_cast<unsigned long long>(r.fault_disagree),
                        r.first.c_str());
        } else if (r.status_differ != 0) {
            std::printf("  status   %#llx %-40s MXCSR status flags differ in %llu of %llu runs (results identical)\n",
                        static_cast<unsigned long long>(le.addr), text,
                        static_cast<unsigned long long>(r.status_differ), static_cast<unsigned long long>(r.checked));
        }
    }
    RemoveVectoredExceptionHandler(veh);
    std::printf("  %u lifted instructions, %llu not checked (segment/absolute memory), %llu runs: %d with a mismatch "
                "(AF not compared in %llu instructions of functions without an AF reader)\n",
                *count, static_cast<unsigned long long>(refused), static_cast<unsigned long long>(total_runs),
                bad_instructions, static_cast<unsigned long long>(af_unread));
    return bad_instructions;
}

} // namespace SecondSon::NativeEngine::Tests
