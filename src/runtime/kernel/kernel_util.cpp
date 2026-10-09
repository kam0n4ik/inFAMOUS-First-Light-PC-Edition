// SecondSonPC — libkernel utility functions (NATIVE since Phase 2.2.3, Tier 1 of
// docs/PHASE2_NATIVE_MIGRATION_MATRIX.md): pure functions that share no state with the bridge.
// Group "kernel-util": `--bridge kernel-util` sends all five back to the oracle
// (third_party/shadPS4/src/core/libraries/kernel/{threads/pthread.cpp,kernel.cpp,threads/exception.cpp}).
//   scePthreadEqual  — pthread_t is a pointer: equal handles = equal values (oracle: posix_pthread_equal)
//   scePthreadYield  — void, gives up the rest of the time slice (oracle: posix_pthread_yield)
//   sched_yield      — the same, returns 0 as POSIX/FreeBSD do (the oracle's is declared void: the
//                      caller reads whatever was in eax; libc.prx is its only importer)
//   getpagesize      — 16 KiB, the PS4 page (oracle: posix_getpagesize)
//   sigfillset       — all 128 signal bits set, returns 0; like the oracle and FreeBSD it does not
//                      check the pointer (oracle: posix_sigfillset)
#include <thread>

#include "runtime/exports.h"
#include "runtime/kernel/kernel.h"

namespace SecondSon::Kernel {

namespace {

constexpr s32 kPageSize = 16 * 1024;

struct SigSet {
    u32 bits[4];
};

SSPC_SYSV_ABI s32 PthreadEqual(const void* a, const void* b) {
    return a == b ? 1 : 0;
}

SSPC_SYSV_ABI void PthreadYield() {
    std::this_thread::yield();
}

SSPC_SYSV_ABI s32 SchedYield() {
    std::this_thread::yield();
    return 0;
}

SSPC_SYSV_ABI s32 GetPageSize() {
    return kPageSize;
}

SSPC_SYSV_ABI s32 SigFillSet(SigSet* set) {
    for (u32& word : set->bits) {
        word = ~0u;
    }
    return 0;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"3PtV6p3QNX4", "libkernel", "libkernel", "scePthreadEqual", Fn(&PthreadEqual), nullptr, "kernel-util"},
    {"T72hz6ffq08", "libkernel", "libkernel", "scePthreadYield", Fn(&PthreadYield), nullptr, "kernel-util"},
    {"6XG4B33N09g", "libkernel", "libkernel", "sched_yield", Fn(&SchedYield), nullptr, "kernel-util"},
    {"k+AXqu2-eBc", "libkernel", "libkernel", "getpagesize", Fn(&GetPageSize), nullptr, "kernel-util"},
    {"VkTAsrZDcJ0", "libkernel", "libkernel", "sigfillset", Fn(&SigFillSet), nullptr, "kernel-util"},
};

} // namespace

std::span<const Runtime::Export> KernelUtilExports() {
    return kExports;
}

} // namespace SecondSon::Kernel
