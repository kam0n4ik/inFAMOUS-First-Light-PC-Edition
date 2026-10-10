// SecondSonPC — the guest errno (P3.5.1, EXPERIMENTAL: --native errno). Why a thread-local of the executable is the
// guest ABI's cell: runtime/kernel/guest_errno.h.
#include "runtime/kernel/guest_errno.h"

#include <atomic>

#include "runtime/exports.h"

namespace SecondSon::Kernel::GuestErrno {

namespace {

thread_local s32 t_errno = 0; // .tbss: zero in every new thread, no initialiser to run
std::atomic<bool> g_enabled{false};

/// libkernel `int* __error(void)`.
SSPC_SYSV_ABI s32* Error() {
    return &t_errno;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    // Imported by eboot.bin, libc.prx and libSceFios2.prx.
    {"9BcDykPmo1I", "libkernel", "libkernel", "__error", Fn(&Error), nullptr, "errno", false, true},
};

} // namespace

s32* OwnLocation() {
    return &t_errno;
}

void Enable() {
    g_enabled.store(true, std::memory_order_release);
}

bool Enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

std::span<const Runtime::Export> Exports() {
    return kExports;
}

} // namespace SecondSon::Kernel::GuestErrno
