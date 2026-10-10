// SecondSonPC — the guest's process end (Phase 3, P3.6; docs/PHASE3_P36_BRIDGE_ISOLATION.md §4). EXPERIMENTAL,
// group "exit" (--native exit): the bridge's _exit is UNREACHABLE_MSG ("Exiting with status code"), i.e. an
// assert and an int3 — the game's normal exit would end as a crash.
//   _exit — libc's exit() ends here (imported by libc.prx). The process ends through SecondSonPC's one
//           orderly exit (base/lifecycle.h): the exit steps (reports, the bridge's save data backup and shader
//           cache, the logs) run once, then the process exits with `status` as its exit code. The guest's
//           atexit handlers are libc's business and already ran in exit(); _exit runs none (POSIX).
// Not called in any recorded run or session: the game does not end itself (its window is closed).
#include "base/lifecycle.h"
#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"

namespace SecondSon::Kernel {

namespace {

[[noreturn]] SSPC_SYSV_ABI void Exit(s32 status) {
    SSPC_LOG(Kernel, Info, "the guest called _exit({})", status);
    Lifecycle::Exit(status, "the guest's _exit");
}

const Runtime::Export kExports[] = {
    {"6Z83sYWFlA8", "libkernel", "libkernel", "_exit", reinterpret_cast<void*>(&Exit), nullptr, "exit", false,
     true},
};

} // namespace

std::span<const Runtime::Export> ProcessExports() {
    return kExports;
}

} // namespace SecondSon::Kernel
