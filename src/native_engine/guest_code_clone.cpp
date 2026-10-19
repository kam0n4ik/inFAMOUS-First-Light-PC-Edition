// SecondSonPC — a callable copy of a guest function (see guest_code_clone.h).
#include "native_engine/guest_code_clone.h"

#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>

#include <Zydis/Zydis.h>
#include <fmt/format.h>
#include <windows.h>

namespace SecondSon::NativeEngine {

namespace {

ZydisDecoder MakeDecoder() {
    ZydisDecoder d;
    ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    return d;
}

/// Executable memory near `hint` (within ±2 GiB when `near` is set): a 64 KiB region per request is plenty for
/// the few functions the registry installs; the region stays for the process lifetime.
u8* AllocateNear(u64 hint, u32 bytes, bool within_rel32, std::string& error) {
    const u64 gran = 0x10000;
    const u64 size = (bytes + gran - 1) & ~(gran - 1);
    if (!within_rel32) {
        void* p = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p == nullptr) {
            error = fmt::format("VirtualAlloc failed ({})", GetLastError());
        }
        return static_cast<u8*>(p);
    }
    const u64 span = 0x7FF00000ull; // stay inside the rel32 reach with room for the body
    const u64 lo = hint > span ? hint - span : gran;
    const u64 hi = hint + span;
    // free regions in [lo, hi): walk the address space, closest above first, then below
    for (int pass = 0; pass < 2; ++pass) {
        u64 at = pass == 0 ? ((hint + gran) & ~(gran - 1)) : (lo + gran - 1) & ~(gran - 1);
        const u64 end = pass == 0 ? hi : hint;
        while (at + size <= end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == 0) {
                break;
            }
            const u64 region_end = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
            if (mbi.State == MEM_FREE && region_end >= at + size) {
                void* p = VirtualAlloc(reinterpret_cast<void*>(at), size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
                if (p != nullptr) {
                    return static_cast<u8*>(p);
                }
            }
            at = (region_end + gran - 1) & ~(gran - 1);
        }
    }
    error = fmt::format("no free memory within 2 GiB of {:#x}", hint);
    return nullptr;
}

} // namespace

bool RelocateCode(std::vector<u8>& code, u64 from, u64 to, u32& relocated, u32& instructions, std::string& error) {
    static const ZydisDecoder decoder = MakeDecoder();
    relocated = 0;
    instructions = 0;
    const u64 size = code.size();
    u64 off = 0;
    while (off < size) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code.data() + off, size - off, &insn, ops))) {
            error = fmt::format("+{:#x}: undecodable instruction", off);
            return false;
        }
        ++instructions;
        if ((insn.attributes & ZYDIS_ATTRIB_IS_RELATIVE) != 0) {
            for (u32 i = 0; i < insn.operand_count_visible; ++i) {
                const ZydisDecodedOperand& op = ops[i];
                const bool rip_mem = op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.base == ZYDIS_REGISTER_RIP;
                const bool rel_imm = op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE && op.imm.is_relative;
                if (!rip_mem && !rel_imm) {
                    continue;
                }
                ZyanU64 target = 0;
                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn, &op, from + off, &target))) {
                    error = fmt::format("+{:#x}: relative operand without a target", off);
                    return false;
                }
                if (target >= from && target < from + size) {
                    continue; // inside the body: moves with it
                }
                u32 field = 0, width = 0;
                if (rip_mem) {
                    field = insn.raw.disp.offset;
                    width = insn.raw.disp.size;
                } else {
                    for (const auto& imm : insn.raw.imm) {
                        if (imm.is_relative) {
                            field = imm.offset;
                            width = imm.size;
                        }
                    }
                }
                if (width != 32) {
                    error = fmt::format("+{:#x}: a {}-bit relative operand leaves the body (to {:#x})", off, width,
                                        target);
                    return false;
                }
                const s64 disp = static_cast<s64>(target) - static_cast<s64>(to + off + insn.length);
                if (disp < std::numeric_limits<s32>::min() || disp > std::numeric_limits<s32>::max()) {
                    error = fmt::format("+{:#x}: target {:#x} out of rel32 reach of the copy", off, target);
                    return false;
                }
                const s32 d32 = static_cast<s32>(disp);
                std::memcpy(code.data() + off + field, &d32, sizeof(d32));
                ++relocated;
            }
        }
        off += insn.length;
    }
    if (off != size) {
        error = "the body does not end on an instruction boundary";
        return false;
    }
    return true;
}

namespace {
std::atomic<NearCodeAllocator> g_near_code{nullptr};
} // namespace

void SetNearCodeAllocator(NearCodeAllocator allocator) {
    g_near_code.store(allocator);
}

CloneResult CloneGuestFunction(u64 address, const u8* source, u32 size, std::string& error) {
    CloneResult r;
    std::vector<u8> code(source, source + size);
    // a dry run decides whether the copy must be near (any outside reference)
    u32 outside = 0, count = 0;
    {
        std::vector<u8> probe = code;
        if (!RelocateCode(probe, address, address, outside, count, error)) {
            return r;
        }
    }
    static std::mutex lock;
    std::lock_guard guard(lock);
    // near code first from the allocator the runtime set (memory that stays RWX and is never freed)
    u8* mem = nullptr;
    bool shared = false;
    if (const NearCodeAllocator near_code = g_near_code.load(); outside != 0 && near_code != nullptr) {
        mem = reinterpret_cast<u8*>(near_code(address, size));
        shared = mem != nullptr;
    }
    if (mem == nullptr) {
        mem = AllocateNear(address, size, outside != 0, error);
    }
    if (mem == nullptr) {
        return r;
    }
    const u64 to = reinterpret_cast<u64>(mem);
    if (!RelocateCode(code, address, to, r.relocated, r.instructions, error)) {
        if (!shared) {
            VirtualFree(mem, 0, MEM_RELEASE);
        }
        return r;
    }
    std::memcpy(mem, code.data(), code.size());
    DWORD old = 0;
    if (!shared && !VirtualProtect(mem, code.size(), PAGE_EXECUTE_READ, &old)) {
        error = fmt::format("VirtualProtect failed ({})", GetLastError());
        VirtualFree(mem, 0, MEM_RELEASE);
        return r;
    }
    FlushInstructionCache(GetCurrentProcess(), mem, code.size());
    r.code = mem;
    return r;
}

} // namespace SecondSon::NativeEngine
