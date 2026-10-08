// SecondSonPC — libkernel (Phase 2, P2.2). First NATIVE group: time — clocks, sleeps, TSC
// frequency — and sceKernelIsNeoMode. Synchronisation, threads and memory stay BRIDGE
// (docs/PHASE2_PLAN.md, docs/generated/KERNEL_AUDIT.md).
#pragma once

#include <span>

#include "base/abi.h"

namespace SecondSon::Runtime {
struct Export;
}

namespace SecondSon::Kernel {

/// What the native libkernel functions still need from the bridge.
class Host {
public:
    virtual ~Host() = default;
    /// The guest errno of the calling thread (one variable shared with every BRIDGE function).
    virtual s32* Errno() = 0;
    /// The bridge's process clock (CLOCK_PROCTIME): VideoOut/AudioOut timestamps the game compares
    /// with it are BRIDGE until Phase 4.
    virtual u64 ProcessTimeUs() = 0;
};
void SetHost(Host* host);

/// neo: [Console] neoMode; title_supports_neo: param.sfo ATTRIBUTE bit 23.
void Configure(bool neo, bool title_supports_neo);

/// Start-up: the TSC frequency is measured in the background (100 ms) from here on.
void Start();

std::span<const Runtime::Export> KernelExports();

} // namespace SecondSon::Kernel
