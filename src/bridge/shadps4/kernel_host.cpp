// SecondSonPC — TEMPORARY bridge for the native libkernel time functions (Phase 2, P2.2): the guest
// errno and the process clock are still shadPS4's (shared with BRIDGE functions and with the
// VideoOut/AudioOut timestamps).
#include "app/config.h"
#include "base/log.h"
#include "bridge/shadps4/loader_host.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/time.h"
#include "runtime/kernel/kernel.h"

namespace SecondSon::Bridge {

namespace {

class KernelHostImpl final : public Kernel::Host {
public:
    s32* Errno() override {
        return Libraries::Kernel::__Error();
    }
    u64 ProcessTimeUs() override {
        return Libraries::Kernel::sceKernelGetProcessTime();
    }
};

} // namespace

void InstallKernelHost(const App::Config& cfg, u32 psf_attributes) {
    static KernelHostImpl host;
    Kernel::SetHost(&host);
    const bool title_neo = ((psf_attributes >> 23) & 1) != 0; // param.sfo ATTRIBUTE: supports PS4 Pro
    Kernel::Configure(cfg.neo_mode, title_neo);
    Kernel::Start();
    SSPC_LOG(Kernel, Info, "neo mode {} (config {}, title supports it {})", cfg.neo_mode && title_neo,
             cfg.neo_mode, title_neo);
}

} // namespace SecondSon::Bridge
