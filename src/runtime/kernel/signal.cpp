// SecondSonPC — guest signals (see signal.h). The functions' argument checks and results are those of the
// Phase 0 oracle (third_party/shadPS4/src/core/libraries/kernel/threads/exception.cpp) where it is right;
// delivery differs on purpose: a blocked or ignored synchronous fault is the default action (the oracle's
// SIG_IGN returned and re-executed the faulting instruction forever).
#include "runtime/kernel/signal.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>

#include <intrin.h>
#include <windows.h>

#include "base/log.h"
#include "runtime/cpu/exceptions.h"
#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"

namespace SecondSon::Kernel::Signals {

namespace {

constexpr s32 kSigMax = 128;
constexpr s32 kSigKill = 9, kSigStop = 17, kSigThr = 32;
constexpr u64 kSigDfl = 0, kSigIgn = 1, kSigErr = ~0ull;
constexpr s32 kSigBlock = 1, kSigUnblock = 2, kSigSetMask = 3;
constexpr s32 kEINVAL = 22;
constexpr s32 kOrbisEINVAL = static_cast<s32>(0x80020016);

struct Action {
    u64 handler = kSigDfl;
    s32 flags = 0;
    Sigset mask{};
};

std::array<Action, kSigMax> g_actions{};
SRWLOCK g_actions_lock = SRWLOCK_INIT; // writers: signal/sigaction; the dispatcher only tries to read
std::atomic<u64> g_delivered{0};
thread_local Sigset t_mask{};

void SetErrno(s32 e) {
    if (Host* host = GetHost()) {
        *host->Errno() = e;
    }
}

bool Valid(s32 sig) {
    return sig >= 1 && sig <= kSigMax;
}

bool Member(const Sigset& s, s32 sig) {
    return ((s.bits[(sig - 1) >> 5] >> ((sig - 1) & 31)) & 1) != 0;
}

void Add(Sigset& s, s32 sig) {
    s.bits[(sig - 1) >> 5] |= 1u << ((sig - 1) & 31);
}

void Remove(Sigset& s, s32 sig) {
    s.bits[(sig - 1) >> 5] &= ~(1u << ((sig - 1) & 31));
}

/// The action of `sig` for the dispatcher: a try-lock, so a fault inside signal() itself cannot deadlock.
bool ReadAction(s32 sig, Action& out) {
    for (int i = 0; i < 1000; ++i) {
        if (TryAcquireSRWLockShared(&g_actions_lock)) {
            out = g_actions[sig - 1];
            ReleaseSRWLockShared(&g_actions_lock);
            return true;
        }
        YieldProcessor();
    }
    return false;
}

void ToMcontext(const CONTEXT& c, u64 address, Mcontext& m) {
    m = {};
    m.mc_rdi = c.Rdi;
    m.mc_rsi = c.Rsi;
    m.mc_rdx = c.Rdx;
    m.mc_rcx = c.Rcx;
    m.mc_r8 = c.R8;
    m.mc_r9 = c.R9;
    m.mc_rax = c.Rax;
    m.mc_rbx = c.Rbx;
    m.mc_rbp = c.Rbp;
    m.mc_r10 = c.R10;
    m.mc_r11 = c.R11;
    m.mc_r12 = c.R12;
    m.mc_r13 = c.R13;
    m.mc_r14 = c.R14;
    m.mc_r15 = c.R15;
    m.mc_addr = address;
    m.mc_rip = c.Rip;
    m.mc_rflags = c.EFlags;
    m.mc_rsp = c.Rsp;
    m.mc_len = sizeof(Mcontext);
}

void FromMcontext(const Mcontext& m, CONTEXT& c) {
    c.Rdi = m.mc_rdi;
    c.Rsi = m.mc_rsi;
    c.Rdx = m.mc_rdx;
    c.Rcx = m.mc_rcx;
    c.R8 = m.mc_r8;
    c.R9 = m.mc_r9;
    c.Rax = m.mc_rax;
    c.Rbx = m.mc_rbx;
    c.Rbp = m.mc_rbp;
    c.R10 = m.mc_r10;
    c.R11 = m.mc_r11;
    c.R12 = m.mc_r12;
    c.R13 = m.mc_r13;
    c.R14 = m.mc_r14;
    c.R15 = m.mc_r15;
    c.Rip = m.mc_rip;
    c.Rsp = m.mc_rsp;
    // The handler may change the arithmetic flags and the direction flag, not the system ones.
    constexpr DWORD kUser = 0xCD5; // CF PF AF ZF SF DF OF
    c.EFlags = (c.EFlags & ~kUser) | (static_cast<DWORD>(m.mc_rflags) & kUser);
}

using SignalHandler = void(SSPC_SYSV_ABI*)(s32);
using SigactionHandler = void(SSPC_SYSV_ABI*)(s32, Siginfo*, Ucontext*);

/// The dispatcher's delivery of a synchronous fault in guest code (on the faulting thread).
bool Deliver(_EXCEPTION_POINTERS* info, s32 sig, s32 code, u64 address) {
    Action a;
    if (!Valid(sig) || !ReadAction(sig, a) || a.handler == kSigDfl || a.handler == kSigIgn || Member(t_mask, sig)) {
        return false; // the default action: the dispatcher's fatal report
    }
    CONTEXT& c = *info->ContextRecord;
    Siginfo si{};
    si.si_signo = sig;
    si.si_code = code;
    si.si_addr = address;
    Ucontext uc{};
    uc.uc_sigmask = t_mask;
    ToMcontext(c, address, uc.uc_mcontext);
    Sigset during = t_mask;
    for (int i = 0; i < 4; ++i) {
        during.bits[i] |= a.mask.bits[i];
    }
    if ((a.flags & kSaNoDefer) == 0) {
        Add(during, sig);
    }
    if ((a.flags & kSaResetHand) != 0) {
        AcquireSRWLockExclusive(&g_actions_lock);
        g_actions[sig - 1] = {};
        ReleaseSRWLockExclusive(&g_actions_lock);
    }
    t_mask = during;
    g_delivered.fetch_add(1, std::memory_order_relaxed);
    if ((a.flags & kSaSigInfo) != 0) {
        reinterpret_cast<SigactionHandler>(a.handler)(sig, &si, &uc);
    } else {
        reinterpret_cast<SignalHandler>(a.handler)(sig);
    }
    t_mask = uc.uc_sigmask; // as sigreturn: the mask the context carries
    Remove(t_mask, kSigKill);
    Remove(t_mask, kSigStop);
    FromMcontext(uc.uc_mcontext, c);
    return true;
}

// ---------------------------------------------------------------------------------------- exports

SSPC_SYSV_ABI u64 PosixSignal(s32 sig, u64 handler) {
    if (!Valid(sig) || sig == kSigKill || sig == kSigStop || sig == kSigThr) {
        SetErrno(kEINVAL);
        return kSigErr;
    }
    AcquireSRWLockExclusive(&g_actions_lock);
    const u64 old = g_actions[sig - 1].handler;
    g_actions[sig - 1] = {handler, kSaRestart, {}};
    ReleaseSRWLockExclusive(&g_actions_lock);
    SSPC_LOG(Kernel, Info, "signal({}, {:#x}): previous {:#x}", sig, handler, old);
    return old;
}

s32 Sigprocmask(s32 how, const Sigset* set, Sigset* oset) {
    const Sigset old = t_mask;
    if (set != nullptr) {
        Sigset next = old;
        switch (how) {
        case kSigBlock:
            for (int i = 0; i < 4; ++i) {
                next.bits[i] |= set->bits[i];
            }
            break;
        case kSigUnblock:
            for (int i = 0; i < 4; ++i) {
                next.bits[i] &= ~set->bits[i];
            }
            break;
        case kSigSetMask:
            next = *set;
            break;
        default:
            SetErrno(kEINVAL);
            return -1;
        }
        Remove(next, kSigKill); // never blocked
        Remove(next, kSigStop);
        t_mask = next;
    }
    if (oset != nullptr) {
        *oset = old;
    }
    return 0;
}

SSPC_SYSV_ABI s32 PosixSigprocmask(s32 how, const Sigset* set, Sigset* oset) {
    return Sigprocmask(how, set, oset);
}

SSPC_SYSV_ABI s32 SysSigprocmask(s32 how, const Sigset* set, Sigset* oset) {
    return Sigprocmask(how, set, oset); // libc's own entry to the same system call
}

SSPC_SYSV_ABI s32 PosixSigfillset(Sigset* set) {
    if (set == nullptr) {
        SetErrno(kEINVAL);
        return -1;
    }
    std::memset(set, 0xFF, sizeof(*set));
    return 0;
}

[[noreturn]] void RaiseFatal(u32 error, const char* function, void* caller) {
    CONTEXT ctx{};
    RtlCaptureContext(&ctx);
    ctx.Rip = reinterpret_cast<u64>(caller); // the guest code that called it
    EXCEPTION_RECORD rec{};
    rec.ExceptionCode = 0xE0535344; // SecondSonPC: the guest raised a debug exception
    rec.ExceptionAddress = caller;
    EXCEPTION_POINTERS pointers{&rec, &ctx};
    char reason[128];
    std::snprintf(reason, sizeof(reason), "%s(error %#x): the game reported a fatal error", function, error);
    Cpu::Exceptions::Fatal(&pointers, reason);
}

SSPC_SYSV_ABI s32 DebugRaiseException(u32 error, s64 unk) {
    if (unk != 0) {
        return kOrbisEINVAL;
    }
    RaiseFatal(error, "sceKernelDebugRaiseException", _ReturnAddress());
}

SSPC_SYSV_ABI s32 DebugRaiseExceptionOnReleaseMode(u32 error, s64 unk) {
    if (unk != 0) {
        return kOrbisEINVAL;
    }
    RaiseFatal(error, "sceKernelDebugRaiseExceptionOnReleaseMode", _ReturnAddress());
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"VADc3MNQ3cM", "libkernel", "libkernel", "signal", Fn(&PosixSignal), nullptr, "exceptions", false, true},
    {"aPcyptbOiZs", "libkernel", "libkernel", "sigprocmask", Fn(&PosixSigprocmask), nullptr, "exceptions", false, true},
    {"6xVpy0Fdq+I", "libkernel", "libkernel", "_sigprocmask", Fn(&SysSigprocmask), nullptr, "exceptions", false, true},
    {"VkTAsrZDcJ0", "libkernel", "libkernel", "sigfillset", Fn(&PosixSigfillset), nullptr, "exceptions", false, true},
    {"OMDRKKAZ8I4", "libkernel", "libkernel", "sceKernelDebugRaiseException", Fn(&DebugRaiseException), nullptr,
     "exceptions", false, true},
    {"zE-wXIZjLoM", "libkernel", "libkernel", "sceKernelDebugRaiseExceptionOnReleaseMode",
     Fn(&DebugRaiseExceptionOnReleaseMode), nullptr, "exceptions", false, true},
};

} // namespace

std::span<const Runtime::Export> SignalExports() {
    return kExports;
}

void Install() {
    Cpu::Exceptions::SetGuestSignalDelivery(&Deliver);
}

namespace Test {

void SetAction(s32 signal, u64 handler, s32 flags) {
    if (!Valid(signal)) {
        return;
    }
    AcquireSRWLockExclusive(&g_actions_lock);
    g_actions[signal - 1] = {handler, flags, {}};
    ReleaseSRWLockExclusive(&g_actions_lock);
}

Sigset CurrentMask() {
    return t_mask;
}

u64 Delivered() {
    return g_delivered.load();
}

} // namespace Test

} // namespace SecondSon::Kernel::Signals
