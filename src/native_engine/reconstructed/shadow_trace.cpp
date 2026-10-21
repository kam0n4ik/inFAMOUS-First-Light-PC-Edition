// SecondSonPC — call-trace recording for the geometry pass reconstructions (see shadow_trace.h).
#include "native_engine/reconstructed/shadow_trace.h"

#include <atomic>
#include <cstring>

#include <fmt/format.h>
#include <xbyak/xbyak.h>

namespace SecondSon::NativeEngine::Reconstructed::STrace {

namespace {

thread_local Trace* t_active = nullptr;
std::atomic<u64> g_orphans{0};

constexpr u64 kStackWindow = 0x10000;
constexpr u64 kStackTag = 0x5354ac0000000000ull;

} // namespace

void SetActive(Trace* t) {
    t_active = t;
}

Trace* Active() {
    return t_active;
}

u64 OrphanCalls() {
    return g_orphans.load(std::memory_order_relaxed);
}

void g_orphans_add() {
    g_orphans.fetch_add(1, std::memory_order_relaxed);
}

void Trace::Begin(u64 seed, u64 stack_top) {
    ncalls = 0;
    snap_used = 0;
    arena_used = 0;
    sequence = seed;
    overflow = false;
    stack_check_failed = false;
    stack_hi = stack_top;
    stack_lo = stack_hi - kStackWindow;
}

u64 Trace::Allocate(u64 bytes, u64 alignment) {
    if (alignment < 16) {
        alignment = 16;
    }
    const u64 at = (arena_used + alignment - 1) & ~(alignment - 1);
    if (arena == nullptr || at + bytes > arena_cap) {
        overflow = true;
        return 0;
    }
    arena_used = at + bytes;
    return reinterpret_cast<u64>(arena) + at;
}

void Trace::AddRegion(u64 base, u64 size) {
    if (nregions < kMaxRegions) {
        regions[nregions++] = {base, size};
    } else {
        overflow = true;
    }
}

u64 Normal(const Trace& t, u64 v) {
    if (v >= t.stack_lo && v < t.stack_hi) {
        return kStackTag;
    }
    for (u32 i = 0; i < t.nregions; ++i) {
        if (v >= t.regions[i].base && v < t.regions[i].base + t.regions[i].size) {
            return (static_cast<u64>(0xee00 + i) << 48) | (v - t.regions[i].base);
        }
    }
    if (t.arena != nullptr && v >= reinterpret_cast<u64>(t.arena) && v < reinterpret_cast<u64>(t.arena) + t.arena_cap) {
        return (u64{0xeeff} << 48) | (v - reinterpret_cast<u64>(t.arena));
    }
    return v;
}

} // namespace SecondSon::NativeEngine::Reconstructed::STrace

namespace SecondSon::NativeEngine::Reconstructed::STrace {

namespace GT = STrace;

namespace {

/// The thunks' handler (System V): records the call into the active trace, returns RAX.
SSPC_SYSV_ABI u64 Record(u64 index, GT::CallIn* in, const u64* stack) {
    GT::Trace* t = GT::Active();
    if (t == nullptr || index >= t->ncallees) {
        GT::g_orphans_add();
        return 0;
    }
    const GT::CalleeSpec& spec = t->callees[index];
    if (spec.record) {
        if (t->ncalls >= t->max_calls) {
            t->overflow = true;
        } else {
            GT::Call& c = t->calls[t->ncalls++];
            c.callee = static_cast<u32>(index);
            c.flags = 0;
            std::memcpy(c.gpr, in->gpr, sizeof(c.gpr));
            std::memcpy(c.xmm, in->xmm, sizeof(c.xmm));
            for (u32 k = 0; k < 4; ++k) {
                c.stack[k] = k < spec.stack ? stack[k] : 0;
            }
            for (u32 s = 0; s < 2; ++s) {
                c.snap_off[s] = 0;
                c.snap_len[s] = 0;
                const GT::Snap& sn = spec.snap[s];
                if (sn.ptr < 0) {
                    continue;
                }
                const u64 p = sn.ptr < 6 ? in->gpr[sn.ptr] : stack[sn.ptr - 6];
                u64 len = sn.unit;
                if (sn.count >= 0) {
                    len *= static_cast<u32>(in->gpr[sn.count]);
                }
                if (len > sn.max || t->snap_used + len > t->snap_cap) {
                    c.flags |= 1;
                    t->overflow = true;
                    continue;
                }
                if (len != 0 && p != 0) {
                    std::memcpy(t->snap + t->snap_used, reinterpret_cast<const void*>(p), len);
                }
                c.snap_off[s] = t->snap_used;
                c.snap_len[s] = static_cast<u32>(len);
                t->snap_used += static_cast<u32>(len);
            }
        }
    }
    return spec.effect != nullptr ? spec.effect(*t, spec, *in, stack) : 0;
}

/// The thunks: thunk i = push i; jmp common. common saves RDI..R11 and XMM0-15 as a CallIn, calls Record (System V),
/// restores them all (RAX = the result) and returns to the caller with the stack as the call left it.
struct Thunks : Xbyak::CodeGenerator {
    Thunks() : Xbyak::CodeGenerator(4096 + GT::kMaxCallees * 16) {
        using namespace Xbyak::util;
        Xbyak::Label common;
        for (u32 i = 0; i < GT::kMaxCallees; ++i) {
            align(16);
            entry[i] = getCurr();
            push(static_cast<u32>(i));
            jmp(common, T_NEAR);
        }
        align(16);
        L(common);
        // [rsp] = index, [rsp + 8] = return address, [rsp + 16] = the stack arguments
        push(rbp);
        mov(rbp, rsp);
        sub(rsp, 0x148); // 16-aligned (entry: 8 mod 16; + index + rbp)
        const Xbyak::Reg64 gpr[8] = {rdi, rsi, rdx, rcx, r8, r9, r10, r11};
        for (u32 k = 0; k < 8; ++k) {
            mov(ptr[rsp + 8 * k], gpr[k]);
        }
        for (u32 k = 0; k < 16; ++k) {
            movdqu(ptr[rsp + 0x40 + 16 * k], Xbyak::Xmm(k));
        }
        mov(rdi, ptr[rbp + 8]);
        mov(rsi, rsp);
        lea(rdx, ptr[rbp + 24]);
        mov(rax, reinterpret_cast<size_t>(&Record));
        call(rax);
        for (u32 k = 0; k < 8; ++k) {
            mov(gpr[k], ptr[rsp + 8 * k]);
        }
        for (u32 k = 0; k < 16; ++k) {
            movdqu(Xbyak::Xmm(k), ptr[rsp + 0x40 + 16 * k]);
        }
        leave();
        lea(rsp, ptr[rsp + 8]); // the index
        ret();
        ready();
    }
    const u8* entry[GT::kMaxCallees] = {};
};

} // namespace

u64 ThunkAddress(u32 index) {
    static Thunks thunks;
    return index < kMaxCallees ? reinterpret_cast<u64>(thunks.entry[index]) : 0;
}

std::string Compare(const Trace& a, const Trace& b, bool exact) {
    if (a.overflow || b.overflow) {
        return "trace overflow";
    }
    if (a.stack_check_failed || b.stack_check_failed) {
        return "stack check failed";
    }
    const auto name = [](const Trace& t, u32 k) { return k < t.ncalls ? t.callees[t.calls[k].callee].name : "(end)"; };
    if (a.ncalls != b.ncalls) {
        u32 i = 0;
        while (i < a.ncalls && i < b.ncalls && a.calls[i].callee == b.calls[i].callee) {
            ++i;
        }
        return fmt::format("{} calls vs {} (call {}: {} vs {})", a.ncalls, b.ncalls, i, name(a, i), name(b, i));
    }
    for (u32 i = 0; i < a.ncalls; ++i) {
        const Call& x = a.calls[i];
        const Call& y = b.calls[i];
        if (x.callee != y.callee) {
            return fmt::format("call {}: {} vs {}", i, name(a, i), name(b, i));
        }
        const CalleeSpec& s = a.callees[x.callee];
        for (u32 r = 0; r < s.gpr; ++r) {
            u64 u = Normal(a, x.gpr[r]), v = Normal(b, y.gpr[r]);
            if (!exact && (s.widths >> r & 1) != 0) {
                u &= 0xffffffffull;
                v &= 0xffffffffull;
            }
            if (u != v) {
                return fmt::format("call {} ({}): argument register {}: {:#x} vs {:#x}", i, s.name, r, x.gpr[r],
                                   y.gpr[r]);
            }
        }
        for (u32 r = 0; r < s.xmm; ++r) {
            if (std::memcmp(x.xmm[r], y.xmm[r], 16) != 0) {
                return fmt::format("call {} ({}): XMM{} differs", i, s.name, r);
            }
        }
        for (u32 k = 0; k < s.stack && k < 4; ++k) {
            if (Normal(a, x.stack[k]) != Normal(b, y.stack[k])) {
                return fmt::format("call {} ({}): stack argument {}: {:#x} vs {:#x}", i, s.name, k, x.stack[k],
                                   y.stack[k]);
            }
        }
        for (u32 n = 0; n < 2; ++n) {
            if (x.snap_len[n] != y.snap_len[n] ||
                std::memcmp(a.snap + x.snap_off[n], b.snap + y.snap_off[n], x.snap_len[n]) != 0) {
                u32 k = 0;
                while (k < x.snap_len[n] && k < y.snap_len[n] &&
                       a.snap[x.snap_off[n] + k] == b.snap[y.snap_off[n] + k]) {
                    ++k;
                }
                return fmt::format("call {} ({}): the {} / {} bytes argument {} points at differ at +{:#x}", i, s.name,
                                   x.snap_len[n], y.snap_len[n], s.snap[n].ptr, k);
            }
        }
    }
    if (a.arena_used != b.arena_used || (a.arena_used != 0 && std::memcmp(a.arena, b.arena, a.arena_used) != 0)) {
        u64 k = 0;
        while (k < a.arena_used && k < b.arena_used && a.arena[k] == b.arena[k]) {
            ++k;
        }
        return fmt::format("allocations: {} vs {} bytes, first difference at +{:#x}", a.arena_used, b.arena_used, k);
    }
    return {};
}

} // namespace SecondSon::NativeEngine::Reconstructed::STrace
