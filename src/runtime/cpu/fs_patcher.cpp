// SecondSonPC — guest FS-relative instructions on Windows (see fs_patcher.h).
//
// Audit this design rests on (docs/PHASE3_INDEPENDENT_RUNTIME.md §5): Second Son's eboot.bin has 3616
// FS-relative instructions, all `mov r64, qword ptr fs:[0]`, all inside .eh_frame function ranges;
// libc.prx and libSceFios2.prx have none. A raw byte scan also finds ~2900 false candidates in the
// middle of other instructions, so sites are only taken from decoding whole functions.
#include "runtime/cpu/fs_patcher.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <vector>

#include <intrin.h>
#include <windows.h>

#include <Zydis/Zydis.h>

#include "base/log.h"
#include "runtime/cpu/tcb_slot.h"

namespace SecondSon::Cpu {

namespace {

// ------------------------------------------------------------------------------------- decoding

ZydisDecoder g_decoder;
std::atomic<bool> g_decoder_ready{false};
SRWLOCK g_decoder_init = SRWLOCK_INIT;

const ZydisDecoder& Decoder() {
    if (!g_decoder_ready.load(std::memory_order_acquire)) {
        AcquireSRWLockExclusive(&g_decoder_init);
        if (!g_decoder_ready.load(std::memory_order_relaxed)) {
            ZydisDecoderInit(&g_decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
            g_decoder_ready.store(true, std::memory_order_release);
        }
        ReleaseSRWLockExclusive(&g_decoder_init);
    }
    return g_decoder;
}

/// General purpose register number and width (4/8) of a 32/64-bit GPR operand, false otherwise.
bool Gpr(ZydisRegister reg, u8& number, u8& width) {
    const ZydisRegisterClass cls = ZydisRegisterGetClass(reg);
    if (cls != ZYDIS_REGCLASS_GPR64 && cls != ZYDIS_REGCLASS_GPR32) {
        return false;
    }
    const ZyanI8 id = ZydisRegisterGetId(reg);
    if (id < 0 || id > 15) {
        return false;
    }
    number = static_cast<u8>(id);
    width = cls == ZYDIS_REGCLASS_GPR64 ? 8 : 4;
    return true;
}

bool IsFsMemory(const ZydisDecodedOperand& op) {
    return op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.segment == ZYDIS_REGISTER_FS;
}

// ------------------------------------------------------------------------------------- encoding

constexpr u8 kRax = 0;
constexpr u8 kRcx = 1;
constexpr u8 kRsp = 4;
constexpr u32 kTebTlsSlots = 0x1480;
constexpr u32 kTebTlsExpansionSlots = 0x1780;
constexpr u64 kNearJump = 5;

class Emitter {
public:
    Emitter(u8* out, u64 capacity, u64 at) : begin_{out}, p_{out}, end_{out + capacity}, at_{at} {}

    u64 Size() const {
        return ok_ ? static_cast<u64>(p_ - begin_) : 0;
    }

    void Byte(u8 v) {
        if (p_ < end_) {
            *p_++ = v;
        } else {
            ok_ = false;
        }
    }
    void Dword(u32 v) {
        for (int i = 0; i < 4; ++i) {
            Byte(static_cast<u8>(v >> (8 * i)));
        }
    }

    /// mov reg64, gs:[TEB slot of the guest TCB]. Slots from 64 on go through TlsExpansionSlots, which is
    /// null on a thread that never set a TLS value — as the TCB itself is on a thread that never set its
    /// TCB: guest code runs only on threads that did (both managers set it before entering the guest).
    void LoadTcb(u8 reg, u32 slot) {
        const bool expansion = slot >= 64;
        Byte(0x65);
        Byte(static_cast<u8>(0x48 | (reg >= 8 ? 0x04 : 0)));
        Byte(0x8B);
        Byte(static_cast<u8>(0x04 | ((reg & 7) << 3))); // mod 00, rm 100: SIB follows
        Byte(0x25);                                      // no base, no index: disp32
        Dword(expansion ? kTebTlsExpansionSlots : kTebTlsSlots + 8 * slot);
        if (expansion) {
            Memory(0x8B, reg, 8, reg, static_cast<s32>(8 * (slot - 64)));
        }
    }

    /// opcode reg, [base + disp] (0x8B load) or opcode [base + disp], reg (0x89 store), width 4/8.
    void Memory(u8 opcode, u8 reg, u8 width, u8 base, s32 disp) {
        const u8 rex =
            static_cast<u8>(0x40 | (width == 8 ? 0x08 : 0) | (reg >= 8 ? 0x04 : 0) | (base >= 8 ? 0x01 : 0));
        if (rex != 0x40) {
            Byte(rex);
        }
        Byte(opcode);
        u8 mod = 2;
        if (disp == 0 && (base & 7) != 5) {
            mod = 0;
        } else if (disp >= -128 && disp <= 127) {
            mod = 1;
        }
        Byte(static_cast<u8>((mod << 6) | ((reg & 7) << 3) | (base & 7)));
        if ((base & 7) == 4) {
            Byte(0x24); // SIB: base rsp/r12, no index
        }
        if (mod == 1) {
            Byte(static_cast<u8>(disp));
        } else if (mod == 2) {
            Dword(static_cast<u32>(disp));
        }
    }

    /// op dst, src (register form "r, r/m")
    void Alu(u8 opcode, u8 dst, u8 src, u8 width) {
        const u8 rex =
            static_cast<u8>(0x40 | (width == 8 ? 0x08 : 0) | (dst >= 8 ? 0x04 : 0) | (src >= 8 ? 0x01 : 0));
        if (rex != 0x40) {
            Byte(rex);
        }
        Byte(opcode);
        Byte(static_cast<u8>(0xC0 | ((dst & 7) << 3) | (src & 7)));
    }

    void Push(u8 reg) {
        if (reg >= 8) {
            Byte(0x41);
        }
        Byte(static_cast<u8>(0x50 | (reg & 7)));
    }
    void Pop(u8 reg) {
        if (reg >= 8) {
            Byte(0x41);
        }
        Byte(static_cast<u8>(0x58 | (reg & 7)));
    }

    /// lea rsp, [rsp + delta] (flags untouched)
    void AdjustRsp(s32 delta) {
        Byte(0x48);
        Byte(0x8D);
        if (delta >= -128 && delta <= 127) {
            Byte(0x64);
            Byte(0x24);
            Byte(static_cast<u8>(delta));
        } else {
            Byte(0xA4);
            Byte(0x24);
            Dword(static_cast<u32>(delta));
        }
    }

    void Jump(u64 target) {
        const u64 next = at_ + static_cast<u64>(p_ - begin_) + kNearJump;
        const s64 rel = static_cast<s64>(target - next);
        if (rel != static_cast<s32>(rel)) {
            ok_ = false;
            return;
        }
        Byte(0xE9);
        Dword(static_cast<u32>(static_cast<s32>(rel)));
    }

private:
    u8* begin_;
    u8* p_;
    u8* end_;
    u64 at_;
    bool ok_ = true;
};

u8 AluOpcode(FsOp op) {
    switch (op) {
    case FsOp::Xor:
        return 0x33;
    case FsOp::Cmp:
        return 0x3B;
    case FsOp::Add:
        return 0x03;
    case FsOp::Sub:
        return 0x2B;
    case FsOp::And:
        return 0x23;
    case FsOp::Or:
        return 0x0B;
    default:
        return 0;
    }
}

// --------------------------------------------------------------------------------------- modules

constexpr u32 kMaxModules = 64;
constexpr u64 kTrampolineMax = 64; // the longest trampoline is ~45 bytes

struct Module {
    u64 code = 0;
    u64 code_end = 0;
    u64 trampoline = 0;
    u64 trampoline_end = 0;
    u64 cursor = 0;                   // next free trampoline byte (under lock)
    std::atomic<u64> writing_site{0}; // the site whose jump is being written into live code
    SRWLOCK lock = SRWLOCK_INIT;      // one patch at a time in this module
    char name[48]{};
};

std::array<Module, kMaxModules> g_modules;
std::atomic<u32> g_module_count{0};
SRWLOCK g_register_lock = SRWLOCK_INIT;
std::atomic<PVOID> g_handler{nullptr};

std::atomic<u64> g_static_sites{0};
std::atomic<u64> g_runtime_sites{0};
std::atomic<u64> g_runtime_repeats{0};
std::atomic<u64> g_breakpoints{0};
std::atomic<u64> g_misreported{0};
std::atomic<u64> g_unsupported{0};
std::atomic<u64> g_trampoline_bytes{0};
std::atomic<u64> g_first_unsupported{0};

/// The registered module whose code contains `address` (bounds checked), or nullptr. Lock-free:
/// entries are complete before the count that publishes them.
Module* FindModule(u64 address) {
    const u32 count = g_module_count.load(std::memory_order_acquire);
    for (u32 i = 0; i < count; ++i) {
        Module& m = g_modules[i];
        if (address >= m.code && address < m.code_end) {
            return &m;
        }
    }
    return nullptr;
}

/// `site` holds a jump of this patcher: E9 rel32 into the module's trampoline area.
bool IsOurJump(const Module& m, u64 site) {
    if (site < m.code || site + kNearJump > m.code_end) {
        return false;
    }
    const auto* p = reinterpret_cast<const u8*>(site);
    if (p[0] != 0xE9) {
        return false;
    }
    s32 rel;
    std::memcpy(&rel, p + 1, sizeof(rel));
    const u64 target = site + kNearJump + static_cast<u64>(static_cast<s64>(rel));
    return target >= m.trampoline && target < m.trampoline_end;
}

/// The jump that replaces an instruction at `site`: E9 rel32, then int3 padding (never executed: the
/// jump comes first; int3 rather than nop so a stray jump into the middle traps at once).
std::array<u8, 16> JumpBytes(u64 site, u64 target) {
    std::array<u8, 16> bytes;
    bytes.fill(0xCC);
    bytes[0] = 0xE9;
    const s32 rel = static_cast<s32>(static_cast<s64>(target - (site + kNearJump)));
    std::memcpy(&bytes[1], &rel, sizeof(rel));
    return bytes;
}

/// Writes the trampoline of `insn` into the module's area (under the module's lock). 0 = no room.
u64 MakeTrampoline(Module& m, const FsInstruction& insn, u64 site) {
    if (m.trampoline_end - m.cursor < kTrampolineMax) {
        return 0;
    }
    const u64 at = m.cursor;
    const u64 size = EncodeTrampoline(insn, site, TcbSlot(), at, reinterpret_cast<u8*>(at), kTrampolineMax);
    if (size == 0) {
        return 0;
    }
    m.cursor += (size + 15) & ~u64{15};
    g_trampoline_bytes.fetch_add(size, std::memory_order_relaxed);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), size);
    return at;
}

/// Replaces the `size` bytes at `code` while other threads may execute them (as Linux text_poke_bp):
/// int3 into the first byte, the rest, then the first byte; FlushProcessWriteBuffers after each step
/// interrupts every processor running a thread of this process, which serializes its instruction
/// stream — a processor either completes the old instruction (fetched whole before the int3 became
/// visible) or fetches the int3 and traps. Never a mix of old and new bytes.
void WriteLiveCode(u8* code, const u8* bytes, u64 size) {
    std::atomic_ref<u8>(code[0]).store(0xCC, std::memory_order_seq_cst);
    FlushProcessWriteBuffers();
    std::memcpy(code + 1, bytes + 1, size - 1);
    FlushProcessWriteBuffers();
    std::atomic_ref<u8>(code[0]).store(bytes[0], std::memory_order_seq_cst);
    FlushProcessWriteBuffers();
    FlushInstructionCache(GetCurrentProcess(), code, size);
}

/// In the exception handler: counters only (no logging — the faulting thread may hold the log's lock); the
/// first address is reported at exit (GetStats).
void NoteUnsupported(u64 site) {
    g_unsupported.fetch_add(1, std::memory_order_relaxed);
    u64 none = 0;
    g_first_unsupported.compare_exchange_strong(none, site, std::memory_order_relaxed);
}

// --------------------------------------------------------------------------------------- handler

/// An access violation at an FS-relative instruction of a registered module: patch it (or find it
/// patched by another thread) and re-execute it.
LONG OnAccessViolation(EXCEPTION_POINTERS* info) {
    const auto* rec = info->ExceptionRecord;
    if (rec->NumberParameters < 2) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    // FS base 0: the address is the displacement — near 0, or above the user address space for a
    // negative one (the system then reports all ones). The GPU write tracking's faults (tens of
    // thousands a second, guest memory at 0x2'0000'0000 and above) leave here. A displacement of 64 KiB
    // or more would leave too (the TCB is 0x38 bytes; static TLS lies below it): never seen, and such
    // a site would still be patched at load time.
    const u64 address = rec->ExceptionInformation[1];
    if (address >= 0x10000 && address < 0xFFFF800000000000ull) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 rip = info->ContextRecord->Rip;
    Module* m = FindModule(rip);
    if (m == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    AcquireSRWLockExclusive(&m->lock);
    if (IsOurJump(*m, rip)) {
        // Another thread patched it after this one fetched the old instruction.
        ReleaseSRWLockExclusive(&m->lock);
        g_runtime_repeats.fetch_add(1, std::memory_order_relaxed);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    bool fs_relative = false;
    const auto insn = DecodeFs(reinterpret_cast<const u8*>(rip), m->code_end - rip, &fs_relative);
    if (!insn) {
        ReleaseSRWLockExclusive(&m->lock);
        if (fs_relative) {
            NoteUnsupported(rip);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const u64 trampoline = MakeTrampoline(*m, *insn, rip);
    if (trampoline == 0) {
        ReleaseSRWLockExclusive(&m->lock);
        NoteUnsupported(rip);
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const auto jump = JumpBytes(rip, trampoline);
    m->writing_site.store(rip, std::memory_order_seq_cst);
    WriteLiveCode(reinterpret_cast<u8*>(rip), jump.data(), insn->length);
    m->writing_site.store(0, std::memory_order_release);
    ReleaseSRWLockExclusive(&m->lock);
    g_runtime_sites.fetch_add(1, std::memory_order_relaxed);
    return EXCEPTION_CONTINUE_EXECUTION; // rip unchanged: the jump runs now
}

/// A thread reached the int3 of a site being patched (or the kernel reported that int3 as a
/// privileged instruction, having found the jump already there): wait for the patch, re-execute.
/// Claimed only for this patcher's own sites; any other breakpoint is someone else's.
/// The site of this patcher a trap at `address` belongs to (being written, or written), or 0.
u64 TrapSite(u64 address, bool misreported, Module** module) {
    const u64 candidates[2] = {address, address - 1}; // the int3 itself, or one past it
    for (u32 i = 0; i < (misreported ? 1u : 2u); ++i) {
        const u64 site = candidates[i];
        Module* m = FindModule(site);
        if (m != nullptr && (m->writing_site.load(std::memory_order_acquire) == site || IsOurJump(*m, site))) {
            *module = m;
            return site;
        }
    }
    return 0;
}

LONG OnPatchTrap(EXCEPTION_POINTERS* info, bool misreported) {
    Module* m = nullptr;
    const u64 site = TrapSite(reinterpret_cast<u64>(info->ExceptionRecord->ExceptionAddress), misreported, &m);
    if (site == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    AcquireSRWLockShared(&m->lock); // held by the writer until the jump is complete
    ReleaseSRWLockShared(&m->lock);
    if (!IsOurJump(*m, site)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    info->ContextRecord->Rip = site;
    g_breakpoints.fetch_add(1, std::memory_order_relaxed);
    if (misreported) {
        g_misreported.fetch_add(1, std::memory_order_relaxed);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

LONG WINAPI FsHandler(EXCEPTION_POINTERS* info) {
    switch (info->ExceptionRecord->ExceptionCode) {
    case EXCEPTION_ACCESS_VIOLATION:
        return OnAccessViolation(info);
    case EXCEPTION_BREAKPOINT:
        return OnPatchTrap(info, false);
    case EXCEPTION_PRIV_INSTRUCTION:
        return OnPatchTrap(info, true);
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
}

} // namespace

// ------------------------------------------------------------------------------------- interface

std::optional<FsInstruction> DecodeFs(const u8* code, u64 available, bool* fs_relative) {
    if (fs_relative != nullptr) {
        *fs_relative = false;
    }
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), code,
                                             std::min<u64>(available, ZYDIS_MAX_INSTRUCTION_LENGTH), &insn, ops))) {
        return std::nullopt;
    }
    int fs_op = -1;
    for (u8 i = 0; i < insn.operand_count_visible; ++i) {
        if (IsFsMemory(ops[i])) {
            fs_op = i;
        }
    }
    if (fs_op < 0) {
        return std::nullopt;
    }
    if (fs_relative != nullptr) {
        *fs_relative = true;
    }
    const auto& mem = ops[fs_op].mem;
    if (insn.operand_count_visible != 2 || mem.base != ZYDIS_REGISTER_NONE || mem.index != ZYDIS_REGISTER_NONE ||
        insn.length < kNearJump || mem.disp.value != static_cast<s32>(mem.disp.value)) {
        return std::nullopt;
    }
    FsInstruction out{};
    out.length = insn.length;
    out.disp = static_cast<s32>(mem.disp.value);
    const auto& other = ops[fs_op == 0 ? 1 : 0];
    if (other.type != ZYDIS_OPERAND_TYPE_REGISTER || !Gpr(other.reg.value, out.reg, out.width) ||
        ops[fs_op].size != out.width * 8u) {
        return std::nullopt;
    }
    if (fs_op == 0) {
        if (insn.mnemonic != ZYDIS_MNEMONIC_MOV || out.reg == kRsp) {
            return std::nullopt;
        }
        out.op = FsOp::Store;
        return out;
    }
    switch (insn.mnemonic) {
    case ZYDIS_MNEMONIC_MOV:
        out.op = FsOp::Load;
        return out;
    case ZYDIS_MNEMONIC_XOR:
        out.op = FsOp::Xor;
        break;
    case ZYDIS_MNEMONIC_CMP:
        out.op = FsOp::Cmp;
        break;
    case ZYDIS_MNEMONIC_ADD:
        out.op = FsOp::Add;
        break;
    case ZYDIS_MNEMONIC_SUB:
        out.op = FsOp::Sub;
        break;
    case ZYDIS_MNEMONIC_AND:
        out.op = FsOp::And;
        break;
    case ZYDIS_MNEMONIC_OR:
        out.op = FsOp::Or;
        break;
    default:
        return std::nullopt;
    }
    if (out.reg == kRsp) {
        return std::nullopt; // the trampoline borrows the stack
    }
    return out;
}

u64 EncodeTrampoline(const FsInstruction& insn, u64 site, u32 slot, u64 at, u8* out, u64 capacity) {
    Emitter e(out, capacity, at);
    if (insn.op == FsOp::Load) {
        // The TCB into the destination, then the guest's operand from it (fs:[0] is the TCB's self
        // pointer: read like any other field, as the console does).
        e.LoadTcb(insn.reg, slot);
        e.Memory(0x8B, insn.reg, insn.width, insn.reg, insn.disp);
    } else {
        // A scratch register on the stack below the red zone (System V leaf functions keep data in
        // the 128 bytes below rsp). lea/push/pop/mov leave the flags alone: the flags after the
        // trampoline are the ones the guest's own operation set (cmp/xor/... results survive).
        const u8 scratch = insn.reg != kRax ? kRax : kRcx;
        e.AdjustRsp(-128);
        e.Push(scratch);
        e.LoadTcb(scratch, slot);
        if (insn.op == FsOp::Store) {
            e.Memory(0x89, insn.reg, insn.width, scratch, insn.disp);
        } else {
            e.Memory(0x8B, scratch, insn.width, scratch, insn.disp);
            e.Alu(AluOpcode(insn.op), insn.reg, scratch, insn.width);
        }
        e.Pop(scratch);
        e.AdjustRsp(128);
    }
    e.Jump(site + insn.length);
    return e.Size();
}

bool CpuNeedsOnlyFsPatches(std::string* why_not) {
    int regs[4]{};
    __cpuid(regs, 0);
    char vendor[13]{};
    std::memcpy(vendor + 0, &regs[1], 4);
    std::memcpy(vendor + 4, &regs[3], 4);
    std::memcpy(vendor + 8, &regs[2], 4);
    __cpuid(regs, static_cast<int>(0x80000000u));
    bool sse4a = false;
    if (static_cast<u32>(regs[0]) >= 0x80000001u) {
        __cpuid(regs, static_cast<int>(0x80000001u));
        sse4a = (regs[2] & (1 << 6)) != 0;
    }
    const bool intel = std::strcmp(vendor, "GenuineIntel") == 0;
    if (sse4a && !intel) {
        return true;
    }
    if (why_not != nullptr) {
        *why_not = !sse4a ? "the CPU has no SSE4a (the bridge emulates extrq/insertq/movntss/movntsd)"
                          : "Intel CPU (the bridge emulates AMD's rcpps/rsqrtps precision)";
    }
    return false;
}

bool InstallFsPatcher() {
    AcquireSRWLockExclusive(&g_register_lock);
    if (g_handler.load() == nullptr) {
        (void)Decoder();
        g_handler.store(AddVectoredExceptionHandler(1, &FsHandler));
    }
    const bool ok = g_handler.load() != nullptr;
    ReleaseSRWLockExclusive(&g_register_lock);
    return ok;
}

bool FsPatcherInstalled() {
    return g_handler.load() != nullptr;
}

bool RegisterModule(u64 code, u64 code_size, u64 trampoline, u64 trampoline_size, std::string_view name) {
    const u64 code_end = code + code_size;
    const u64 trampoline_end = trampoline + trampoline_size;
    // Every jump from code into the trampoline area (and back) must reach with rel32.
    const u64 lo = std::min(code, trampoline);
    const u64 hi = std::max(code_end, trampoline_end);
    if (code_size == 0 || trampoline_size < kTrampolineMax || code_end < code || trampoline_end < trampoline ||
        hi - lo > 0x7FFF0000ull || (trampoline < code_end && code < trampoline_end)) {
        SSPC_LOG(Cpu, Error, "FS patcher: module {} code [{:#x}, {:#x}) / trampolines [{:#x}, {:#x}) refused", name,
                 code, code_end, trampoline, trampoline_end);
        return false;
    }
    AcquireSRWLockExclusive(&g_register_lock);
    const u32 index = g_module_count.load(std::memory_order_relaxed);
    if (index == kMaxModules) {
        ReleaseSRWLockExclusive(&g_register_lock);
        SSPC_LOG(Cpu, Error, "FS patcher: module table full ({}), {} not registered", kMaxModules, name);
        return false;
    }
    Module& m = g_modules[index];
    m.code = code;
    m.code_end = code_end;
    m.trampoline = trampoline;
    m.trampoline_end = trampoline_end;
    m.cursor = trampoline;
    const size_t n = std::min(name.size(), sizeof(m.name) - 1);
    std::memcpy(m.name, name.data(), n);
    m.name[n] = '\0';
    g_module_count.store(index + 1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_register_lock);
    SSPC_LOG(Cpu, Info, "FS patcher: module {} code [{:#x}, {:#x}), trampolines [{:#x}, {:#x})", name, code,
             code_end, trampoline, trampoline_end);
    return true;
}

StaticResult PatchModuleStatically(u64 segment, u64 size, std::span<const FunctionRange> functions) {
    StaticResult r;
    Module* m = FindModule(segment);
    if (m == nullptr) {
        SSPC_LOG(Cpu, Error, "FS patcher: segment {:#x} is not in a registered module", segment);
        return r;
    }
    const u64 segment_end = segment + size;
    struct Site {
        u64 address;
        FsInstruction insn;
    };
    std::vector<Site> sites; // of one function
    AcquireSRWLockExclusive(&m->lock);
    for (const auto& f : functions) {
        if (f.begin < segment || f.end > segment_end || f.end <= f.begin || f.end > m->code_end) {
            continue;
        }
        ++r.functions;
        sites.clear();
        bool clean = true;
        u64 unsupported = 0;
        u64 unsupported_before_end = 0; // ... before the last unconditional transfer (see below)
        u64 last_transfer_end = f.begin;
        u64 pc = f.begin;
        while (pc < f.end) {
            ZydisDecodedInstruction insn;
            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&Decoder(), reinterpret_cast<const void*>(pc), f.end - pc,
                                                     &insn, ops))) {
                clean = false;
                break;
            }
            if ((insn.attributes & ZYDIS_ATTRIB_HAS_SEGMENT_FS) != 0) {
                bool fs_relative = false;
                if (const auto fs = DecodeFs(reinterpret_cast<const u8*>(pc), f.end - pc, &fs_relative)) {
                    sites.push_back({pc, *fs});
                } else if (fs_relative) {
                    ++unsupported;
                }
            }
            pc += insn.length;
            switch (insn.mnemonic) {
            case ZYDIS_MNEMONIC_RET:
            case ZYDIS_MNEMONIC_JMP:
            case ZYDIS_MNEMONIC_UD2:
            case ZYDIS_MNEMONIC_INT3:
            case ZYDIS_MNEMONIC_HLT:
                last_transfer_end = pc;
                unsupported_before_end = unsupported;
                break;
            default:
                break;
            }
        }
        // Decoding that ends exactly at the function's end: every instruction boundary is trusted. Otherwise
        // something in the range is not code — in this game always a switch jump table placed after the code
        // (68 functions of eboot.bin) — and the decoder may have read some of it as instructions before it
        // stopped. The boundaries are then trusted only up to the last unconditional transfer (ret, jmp, ud2,
        // int3, hlt) before the stop: execution never falls past it, the table lies behind it. Sites after it
        // are left to the run-time path.
        u64 trusted_end = f.end;
        if (!clean || pc != f.end) {
            ++r.undecodable;
            trusted_end = last_transfer_end;
            unsupported = unsupported_before_end;
        }
        r.unsupported += unsupported;
        for (const auto& s : sites) {
            if (s.address + s.insn.length > trusted_end) {
                ++r.left_to_runtime;
                continue;
            }
            const u64 trampoline = MakeTrampoline(*m, s.insn, s.address);
            if (trampoline == 0) {
                ++r.unsupported;
                continue;
            }
            const auto jump = JumpBytes(s.address, trampoline);
            std::memcpy(reinterpret_cast<void*>(s.address), jump.data(), s.insn.length); // not running yet
            ++r.patched;
        }
    }
    ReleaseSRWLockExclusive(&m->lock);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(segment), size);
    g_static_sites.fetch_add(r.patched, std::memory_order_relaxed);
    if (r.unsupported != 0) {
        g_unsupported.fetch_add(r.unsupported, std::memory_order_relaxed);
    }
    return r;
}

Stats GetStats() {
    return {g_module_count.load(), g_static_sites.load(),      g_runtime_sites.load(), g_runtime_repeats.load(),
            g_breakpoints.load(),  g_misreported.load(),       g_unsupported.load(),   g_trampoline_bytes.load(),
            g_first_unsupported.load()};
}

namespace Test {

bool ClaimsTrap(u64 address, bool misreported) {
    Module* m = nullptr;
    return TrapSite(address, misreported, &m) != 0;
}

} // namespace Test

} // namespace SecondSon::Cpu
