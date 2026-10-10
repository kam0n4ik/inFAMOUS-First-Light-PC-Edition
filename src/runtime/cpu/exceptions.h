// SecondSonPC — the process's exception dispatcher (Phase 3, P3.4; docs/PHASE3_P34_EXCEPTIONS_CPU.md).
//
// With --native exceptions SecondSonPC owns the one vectored exception handler that guest-related
// exceptions reach first (first in the list), and decides by owner:
//   access violation  -> SecondSonPC's CPU patcher (an FS access: address near 0 or above the user space,
//                        in a registered module) -> an execute fault is recorded (SSPC-002 recorder) ->
//                        the graphics bridge (GPU write tracking of protected guest pages, the SRT walker's
//                        reads) through an explicit contract (BridgeHandlers, patch 0022) -> unresolved:
//                        guest code gets its signal (SIGSEGV/SIGBUS) or a fatal report; host code is passed
//                        on to its own SEH/C++ handlers and the unhandled exception filter;
//   int3 / privileged -> the CPU patcher's sites being written (and the kernel's misreport of them) -> the
//                        bridge's patcher (only when it owns the guest's code) -> guest SIGTRAP/SIGILL or
//                        fatal; host breakpoints (an assert of the bridge, a debugger) are passed on;
//   illegal instr.    -> SecondSonPC's CPU emulation (SSE4a on CPUs without it) -> the bridge's -> guest
//                        SIGILL or fatal; host passed on;
//   FPU/integer traps, in-page errors, stack overflow -> guest signal or fatal; host passed on;
//   anything else (C++ exceptions, thread naming, debugger output) -> passed on unchanged.
//
// The hot path (the bridge's GPU write tracking: tens of thousands of access violations a second) makes
// no system call, takes no lock, allocates nothing and writes no log: compares, the bridge's handler, a
// few relaxed atomics and two time stamps. Statistics are read with GetStats/LogSummary.
//
// A fatal guest exception is reported by a reporter thread created at Install (the faulting thread may
// hold the log's or the heap's lock): registers, stack, module + offset, thread, backends, build; then the
// process ends with TerminateProcess and the exception code (no shutdown run from inside the handler).
// If the reporter does not finish in time the faulting thread writes a minimal report without locks and
// ends the process itself.
#pragma once

#include <string_view>

#include "base/abi.h"

struct _EXCEPTION_POINTERS;

namespace SecondSon::Cpu::Exceptions {

/// The graphics bridge's side of the contract (until Phase 4). Each handler returns true when it resolved
/// the exception; the dispatcher then resumes the context the handler left. A handler runs on the faulting
/// thread inside exception dispatch: it must not raise an exception itself (a nested one is fatal).
struct BridgeHandlers {
    /// GPU write/read tracking (protected guest pages) and the SRT walker's faulting reads.
    bool (*access_violation)(_EXCEPTION_POINTERS* info, u64 fault_address) = nullptr;
    /// The bridge's CPU patcher, only while it owns the guest's code (no --native cpu): the int3 of a patch
    /// being written (misreported: reported by the kernel as a privileged instruction) ...
    bool (*patch_trap)(_EXCEPTION_POINTERS* info, bool misreported) = nullptr;
    /// ... and its illegal-instruction emulation (SSE4a on CPUs without it).
    bool (*illegal_instruction)(_EXCEPTION_POINTERS* info) = nullptr;
};

struct Options {
    BridgeHandlers bridge;
    bool cpu_patcher = false;    ///< SecondSonPC's CPU patcher owns the guest's code (its faults come first)
    bool write_minidump = false; ///< a minidump of the first fatal exception (logs\crash)
    const char* backends = "";   ///< the active backends, for fatal reports (static string)
    const char* logs_dir = "";   ///< UTF-8; fatal reports go to <logs_dir>\crash too
};

/// Registers the dispatcher (first in the vectored handler list) and starts the reporter thread. Once,
/// before any guest code runs. false: the handler or the thread could not be created.
bool Install(const Options& options);
bool Installed();
/// The CPU patcher's exceptions go through the dispatcher (Options::cpu_patcher).
bool CpuPatcherDelegated();

/// The bridge asserted (patch 0022: RemoveHandlers): from now on every exception is passed on, so its
/// own int3 reaches the unhandled exception filter instead of looping through the dispatcher.
void PassEverything();

/// A guest module's code or a trampoline area: exceptions there are the guest's (signals, fatal reports
/// with module + offset). Before the module runs; up to 64 ranges.
void RegisterGuestCode(u64 begin, u64 end, std::string_view name);
bool IsGuestCode(u64 address);
/// The registered range containing `address`: its name and the offset into it. false: none.
bool FindGuestCode(u64 address, const char** name, u64* offset);

/// The CPU patcher's load-time pass marks functions that keep data below rsp (the System V red zone):
/// a guest exception there, with the kernel's extended-state area over [rsp-128, rsp), may have destroyed
/// it (P3.4 §6). Function granularity, 16-byte granules: an upper bound for the exposure counters.
void MarkRedZoneFunction(u64 begin, u64 end);
bool InRedZoneFunction(u64 address);

/// Called on the faulting thread for a guest exception no owner resolved: deliver the guest signal (true:
/// the guest's handler ran; resume the context it left). Set by the guest signal runtime.
using GuestSignalDelivery = bool (*)(_EXCEPTION_POINTERS* info, s32 signal, s32 code, u64 address);
void SetGuestSignalDelivery(GuestSignalDelivery delivery);

/// Writes a one-line description of the calling thread (guest thread id and name of either thread
/// manager) for fatal reports; runs on the faulting thread, must not lock or allocate.
using ThreadDescriber = void (*)(char* out, u64 size);
void SetThreadDescriber(ThreadDescriber describer);
/// Called by the reporter thread after the report, before the process ends (the bridge's log flush).
void SetFatalHook(void (*hook)());

/// Ends the process with a fatal report for this exception (the dispatcher's path; also the guest
/// signal runtime's default action and sceKernelDebugRaiseException). Never returns.
[[noreturn]] void Fatal(_EXCEPTION_POINTERS* info, const char* reason);

enum class Owner : u8 { CpuPatcher, Bridge, GuestSignal, Fatal, PassedOn, Count };

struct Stats {
    u64 access_violations;   ///< every access violation the dispatcher saw
    u64 av_by[static_cast<u32>(Owner::Count)]; ///< ... by who resolved it
    u64 traps;               ///< int3 / privileged instruction
    u64 traps_by[static_cast<u32>(Owner::Count)];
    u64 illegal;             ///< illegal instruction
    u64 illegal_by[static_cast<u32>(Owner::Count)];
    u64 other_guest;         ///< FPU/integer traps, in-page errors, stack overflow in guest code
    u64 passed_other;        ///< everything else passed on (C++ exceptions, thread names, ...)
    u64 nested;              ///< exceptions raised inside an owner's handler (fatal)
    u64 execute_faults;      ///< recorded execute faults
    // GPU tracking (access violations the bridge resolved): dispatcher + bridge handler time.
    u64 bridge_cycles;       ///< sum, TSC cycles
    u64 bridge_p50, bridge_p95, bridge_p99, bridge_max; ///< cycles (histogram resolution ~19 %)
    double tsc_mhz;          ///< measured since Install
    // Red zone (P3.4 §6), exceptions resolved in guest code.
    u64 guest_resolved;      ///< resolved exceptions whose rip was in guest code
    u64 xstate_over_red_zone;///< ... the kernel wrote extended state (AVX) over [rsp-128, rsp)
    u64 in_red_zone_function;///< ... rip in a function that keeps data below rsp
    u64 exposed;             ///< both: the red zone of a function that uses it may have been destroyed
    u64 first_exposed[8];    ///< the first rips of `exposed` (0: none)
};
Stats GetStats();
/// One line in SecondSonPC.log (periodically and at exit).
void LogSummary(const char* reason);

namespace Test {
/// Resets the counters and the histogram (self-test).
void ResetStats();
/// Replaces the bridge's handlers (a test double of the GPU write tracking); returns the previous ones.
/// Only while no other thread raises exceptions.
BridgeHandlers SetBridgeHandlers(const BridgeHandlers& handlers);
/// How long a faulting thread waits for the reporter before its own minimal report (default 45 s).
void SetReportTimeout(u32 ms);
} // namespace Test

} // namespace SecondSon::Cpu::Exceptions
