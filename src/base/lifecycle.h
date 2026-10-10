// SecondSonPC — the one owner of the process's end (Phase 3, P3.6; docs/PHASE3_P36_BRIDGE_ISOLATION.md §4).
//
// Two ways out, never mixed:
//   orderly — Exit(code, reason) (window closed, a start-up failure, the guest's _exit) or std::quick_exit by
//             anyone: the first caller wins and runs the exit steps once, in Stage order, on its own thread
//             while the other threads still run; then the process exits (ExitProcess) with the code. A later or
//             concurrent caller waits for the winner to end the process. A watchdog ends the process with the
//             code if the steps do not finish in time (a step that hangs on a thread that never returns).
//   fatal   — EnterFatal (SecondSonPC's fatal exception reporter, the unhandled-exception filter): from then on
//             no orderly exit runs its steps; the fatal path writes its report and ends the process itself
//             (TerminateProcess). Nothing is shut down from inside exception handling.
// The bridge starts no shutdown of its own (patch 0026: its Emulator::Shutdown, reached from its asserts and its
// vectored handler, only flushes the logs through the hook SecondSonPC installs).
#pragma once

#include "base/abi.h"

namespace SecondSon::Lifecycle {

enum class State : u8 { Running, Exiting, Fatal };

/// Order of the exit steps: reports first (they read live state), then the services that write files (the save
/// data backup), then storage (the shader cache), then devices (controller lights), the logs last.
enum class Stage : u8 { Reports, Services, Storage, Devices, Logs, Count };

/// Adds a step (at start-up, before any exit; up to 32). Steps run in Stage order, in the order added within one.
void AddExitStep(Stage stage, const char* name, void (*step)());

/// Takes over std::quick_exit (it runs the steps too). Once, early. `watchdog_ms`: the steps' time limit.
void Install(u32 watchdog_ms);

/// The orderly end of the process (see above). Called again from inside an exit step, it ends the process at once
/// with the first exit's code (the remaining steps are skipped).
[[noreturn]] void Exit(int code, const char* reason);

/// The fatal path is ending the process: no orderly exit starts its steps from now on. Lock-free, allocates
/// nothing; any thread, inside exception handling. true: the first call.
bool EnterFatal(const char* reason);

State Current();

namespace Test {
/// The steps added so far (self-test).
u32 StepCount();
} // namespace Test

} // namespace SecondSon::Lifecycle
