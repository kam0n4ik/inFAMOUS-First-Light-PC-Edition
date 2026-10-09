// SecondSonPC — crash diagnostics in SecondSonPC.log.
#pragma once

#include <filesystem>

#include "base/abi.h"

namespace SecondSon::Base {

/// Installs both crash recorders:
///  - a first-chance vectored handler, called before the bridge's (SSPC-002, docs/KNOWN_ISSUES.md):
///    for an execute fault it logs the registers, the jump evidence and the stack, and writes a
///    minidump to dump_dir (once per process, when write_dump). Every other exception leaves it
///    without a system call (the bridge's GPU write tracking raises ~1000 access violations a
///    second). Guest code touching memory that is not committed is recorded the same way (no
///    minidump) by the data-fault half, which InstallLateFaultRecorder() puts behind the bridge's
///    handler. Neither ever handles the exception: the bridge and the game see it as before;
///  - an unhandled exception filter: code, address, registers and the return addresses found on
///    the stack (as exe+offset, for llvm-symbolizer with SecondSonPC.pdb).
/// first_chance = false (--no-fault-recorder, diagnostics) installs the unhandled filter only.
void InstallCrashLogger(const std::filesystem::path& dump_dir, bool write_dump, bool first_chance = true);

/// Adds the data-fault half of the recorder at the end of the vectored handler list. Call it once
/// the bridge's handler exists (bootstrap), so the faults that handler resolves never reach it; no
/// effect without the first-chance recorder.
void InstallLateFaultRecorder();

/// One line in SecondSonPC.log: access violations the recorder has seen (total and per second since
/// the previous line), how many reached the data-fault half (not handled by the bridge or the
/// game), how many cost a VirtualQuery and the time spent in those queries. Called with every
/// VRAM/commit sample.
void LogFirstChanceStats(const char* reason);

/// What the first-chance recorder does with an exception.
enum class FaultKind : u8 {
    Ignore,              // not an access violation, or one the bridge handles (protected pages)
    Execute,             // instruction fetch failed: a jump/call/return to a bad address
    GuestUnmappedAccess, // code outside any PE image read/wrote memory that is not committed
};

struct FaultFacts {
    u32 code;               // ExceptionCode
    u64 access;             // ExceptionInformation[0]: 0 read, 1 write, 8 execute
    u64 address;            // ExceptionInformation[1]: the inaccessible address
    u64 rip;
    bool rip_in_image;      // RIP lies in a PE image (host code: exe, system DLLs, drivers)
    bool address_committed; // the inaccessible address is committed memory
};

FaultKind ClassifyFault(const FaultFacts& facts);

/// The last fault the first-chance recorder recorded (for --fault-test).
struct FaultRecord {
    u32 serial = 0; // 1, 2, ...; 0 = nothing recorded yet
    FaultKind kind = FaultKind::Ignore;
    u64 rip = 0;
    u64 address = 0;
    u64 stack_top = 0;      // [rsp]: return address of a call to a bad target
    u64 popped = 0;         // [rsp-8]: equals rip after a ret to a bad address
    u32 regs_equal_rip = 0; // bit i: register i holds rip (x86 encoding order: rax rcx rdx rbx
                            // rsp rbp rsi rdi r8 ... r15)
    bool dump_written = false;
};

FaultRecord LastFaultRecord();

/// --fault-test: provokes faults in its own code (execute faults through call, ret and jmp reg,
/// a read of reserved memory from private code, host and protected-page faults that must be
/// ignored) under __try, and checks what the recorder logged. No game, no bridge.
int RunFaultSelfTest(const std::filesystem::path& logs_dir);

} // namespace SecondSon::Base
