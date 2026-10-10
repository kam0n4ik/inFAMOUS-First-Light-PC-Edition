// SecondSonPC — guest signals (Phase 3, P3.4; docs/PHASE3_P34_EXCEPTIONS_CPU.md).
//
// What the game imports (libc.prx): signal, sigprocmask, _sigprocmask, sigfillset,
// sceKernelDebugRaiseException(OnReleaseMode); none of them was called in any recorded run. Group
// "exceptions": bound with --native exceptions only, because delivery belongs to SecondSonPC's exception
// dispatcher (the bridge's functions keep their own table for its own handler).
//
// Model (FreeBSD): one action table per process, one blocked mask per thread (thread_local: every guest thread
// is one host thread, in both thread managers). Synchronous signals only — the faults the dispatcher does not
// resolve in guest code (SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP): with a handler installed and the signal
// not blocked the handler runs on the faulting thread with the PS4's siginfo/ucontext, and the thread resumes
// from the context the handler left; default, ignored or blocked: the default action — a fatal report (an
// ignored or blocked fault cannot resume: FreeBSD resets it to the default and kills the process).
#pragma once

#include <cstddef>
#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Kernel::Signals {

std::span<const Runtime::Export> SignalExports();

/// Delivery through the exception dispatcher (Exceptions::SetGuestSignalDelivery). With --native exceptions.
void Install();

/// FreeBSD values.
constexpr s32 kSigIll = 4, kSigTrap = 5, kSigFpe = 8, kSigBus = 10, kSigSegv = 11;
constexpr s32 kSaRestart = 0x2, kSaResetHand = 0x4, kSaNoDefer = 0x10, kSaSigInfo = 0x40;

/// The PS4's signal structures (reverse-engineered layout of the bridge, core/libraries/kernel/threads/
/// exception.h): uc_mcontext at 0x40.
struct Sigset {
    u32 bits[4];
};
struct Mcontext {
    u64 mc_onstack, mc_rdi, mc_rsi, mc_rdx, mc_rcx, mc_r8, mc_r9, mc_rax, mc_rbx, mc_rbp, mc_r10, mc_r11, mc_r12,
        mc_r13, mc_r14, mc_r15;
    s32 mc_trapno;
    u16 mc_fs, mc_gs;
    u64 mc_addr;
    s32 mc_flags;
    u16 mc_es, mc_ds;
    u64 mc_err, mc_rip, mc_cs, mc_rflags, mc_rsp, mc_ss;
    u64 mc_len, mc_fpformat, mc_ownedfp, mc_lbrfrom, mc_lbrto, mc_aux1, mc_aux2;
    u64 mc_fpstate[104];
    u64 mc_fsbase, mc_gsbase;
    u64 mc_spare[6];
};
struct Siginfo {
    s32 si_signo, si_errno, si_code, si_pid;
    u32 si_uid;
    s32 si_status;
    u64 si_addr;
    u64 si_value;
    u8 reason[40];
};
struct Ucontext {
    Sigset uc_sigmask;
    s32 field1_0x10[12];
    Mcontext uc_mcontext;
    Ucontext* uc_link;
    struct {
        void* ss_sp;
        u64 ss_size;
        s32 ss_flags;
        s32 align;
    } uc_stack;
    s32 uc_flags;
    s32 spare[4];
    s32 field7_0x4f4[3];
};
static_assert(offsetof(Ucontext, uc_mcontext) == 0x40);

namespace Test {
/// sigaction (the game does not import it): `handler` 0 = SIG_DFL, 1 = SIG_IGN, else a System V function.
void SetAction(s32 signal, u64 handler, s32 flags);
/// The calling thread's blocked mask.
Sigset CurrentMask();
u64 Delivered(); // handlers run since start
} // namespace Test

} // namespace SecondSon::Kernel::Signals
