// SecondSonPC — TEMPORARY bridge for the native libkernel functions (Phase 2, P2.2): the guest
// errno and the process clock are shadPS4's by default (shared with BRIDGE functions and with the
// VideoOut/AudioOut timestamps) and SecondSonPC's with --native errno / --native clock (P3.5, the
// bridge reads them through patches 0023/0024); so are the guest threads whose priority orders waiters —
// unless --native threads (P2.4) runs them on SecondSonPC's thread manager, whose host is here too:
// guest stacks from the selected guest memory manager, TCBs from SecondSonPC's loader, the FS base in
// SecondSonPC's TCB slot (P3.3, runtime/cpu/tcb_slot.h), the bridge's debug thread list.
#include <algorithm>
#include <limits>
#include <mutex>
#include <string>

#include "app/config.h"
#include "base/log.h"
#include "runtime/kernel/event_queues.h"
#include "bridge/shadps4/guest_stack.h"
#include "bridge/shadps4/loader_host.h"
#include "core/debug_state.h"
#include "core/guest_memory.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/tls.h"
#include "loader/linker.h"
#include "runtime/cpu/tcb_slot.h"
#include "runtime/kernel/clock.h"
#include "runtime/kernel/guest_errno.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/thread.h"

namespace SecondSon::Bridge {

namespace {

class KernelHostImpl final : public Kernel::Host {
public:
    s32* Errno() override {
        // The guest errno's one owner: the bridge's cell, or with --native errno SecondSonPC's, which the bridge's
        // __Error() then returns (patch 0023).
        return Libraries::Kernel::__Error();
    }
    u64 ProcessTimeUs() override {
        return Libraries::Kernel::sceKernelGetProcessTime();
    }
    s32 CurrentThreadPriority() override {
        // A host thread (no guest thread) waits behind every guest thread; the oracle would crash.
        if (Kernel::Threads::Enabled()) {
            return Kernel::Threads::CurrentPriority().value_or(std::numeric_limits<s32>::max());
        }
        const auto* thread = Libraries::Kernel::g_curthread;
        return thread != nullptr ? thread->attr.prio : std::numeric_limits<s32>::max();
    }
};

class ThreadHostImpl final : public Kernel::Threads::ThreadHost {
public:
    void* MapStack(u64 size, u64 guard) override {
        // The search goes up from a fixed address 1 GiB below where the bridge's own stacks start
        // (threads/stack.cpp: usrstack - ThrStackInitial - ThrGuardDefault, going down; AudioOut's
        // two): the lowest free range that fits, so the ranges of unmapped stacks are used again
        // (P3.2) and the two kinds do not meet unless a GiB of stacks is mapped. The address is a
        // hint; the memory manager decides.
        constexpr VAddr kStackArena = 0x7B0000000ULL;
        return MapGuestStack(kStackArena, size, guard);
    }
    bool UnmapStack(void* stack, u64 size, u64 guard) override {
        return UnmapGuestStack(stack, size, guard);
    }
    void* CreateTcb(void* thread, bool initial) override {
        auto* loader = Loader::Linker::Active();
        return loader != nullptr ? loader->CreateTcb(thread, initial) : nullptr;
    }
    void DestroyTcb(void* tcb) override {
        if (auto* loader = Loader::Linker::Active()) {
            loader->DestroyTcb(static_cast<Loader::GuestTcb*>(tcb));
        }
    }
    void SetTcbBase(void* tcb) override {
        Cpu::SetCurrentTcb(tcb); // SecondSonPC's slot: every FS trampoline reads it (P3.3)
    }
    bool IsReadable(const void* p, u64 size) override {
        return Core::GetGuestVirtualMemory().IsValid(reinterpret_cast<VAddr>(p), size);
    }
    void ThreadStarted() override {
        DebugState.AddCurrentThreadToGuestList();
    }
    void ThreadExiting() override {
        DebugState.RemoveCurrentThreadFromGuestList();
    }
};

} // namespace

void InstallKernelHost(const App::Config& cfg, u32 psf_attributes) {
    static KernelHostImpl host;
    Kernel::SetHost(&host);
    if (std::ranges::find(cfg.bridged_groups, "kernel-equeue") == cfg.bridged_groups.end()) {
        Kernel::EventQueues::InstallNativeOwner(); // the game's queues are SecondSonPC's (group kernel-equeue)
    } else {
        InstallEventQueueHost(); // P3.6: the event queue contract over the bridge's libkernel
    }
    const bool title_neo = ((psf_attributes >> 23) & 1) != 0; // param.sfo ATTRIBUTE: supports PS4 Pro
    Kernel::Configure(cfg.neo_mode, title_neo);
    Kernel::Start();
    SSPC_LOG(Kernel, Info, "neo mode {} (config {}, title supports it {})", cfg.neo_mode && title_neo,
             cfg.neo_mode, title_neo);
    if (std::ranges::find(cfg.native_groups, "threads") != cfg.native_groups.end() && !cfg.selftest) {
        // P2.4: chosen once, before the main thread exists; every guest thread is then native.
        static ThreadHostImpl thread_host;
        Kernel::Threads::SetThreadHost(&thread_host);
        Kernel::Threads::Enable();
        SSPC_LOG(Kernel, Warning, "EXPERIMENTAL: guest threads on SecondSonPC's thread manager");
    }
}

bool InstallKernelRuntime(const App::Config& cfg, std::string* error) {
    const auto native = [&cfg](std::string_view g) { return std::ranges::find(cfg.native_groups, g) != cfg.native_groups.end(); };
    if (native("errno")) {
        // P3.5.1: one guest errno, SecondSonPC's: the game's __error is its export (group "errno"), the bridge's
        // __Error() — every BRIDGE function, Kernel::Host::Errno, the file system's host — returns the same cell.
        if (!Libraries::Kernel::UseExternalErrno(&Kernel::GuestErrno::OwnLocation)) {
            *error = "--native errno: the bridge's errno is already redirected";
            return false;
        }
        Kernel::GuestErrno::Enable();
        SSPC_LOG(Kernel, Warning, "EXPERIMENTAL: the guest errno is SecondSonPC's (--native errno): __error and every "
                                  "BRIDGE function use its per-thread cell");
    }
    if (native("clock")) {
        // P3.5.3: one process clock, SecondSonPC's: its TSC measurement and epoch for the game (TSC frequency,
        // CLOCK_PROCTIME, sceKernelGetProcessTime) and the bridge (VideoOut stamps, AudioOut pacing, GPU timestamps,
        // input), which then makes no NativeClock of its own (patch 0024). Before libkernel's RegisterLib.
        Kernel::Clock::Start();
        static const Libraries::Kernel::ExternalClock external{
            .tsc_frequency = &Kernel::Clock::TscFrequency,
            .process_time_us = &Kernel::Clock::ProcessTimeUs,
            .process_time_counter = &Kernel::Clock::ProcessTimeCounter,
            .advance_epoch = &Kernel::Clock::AdvanceEpoch,
        };
        if (!Libraries::Kernel::UseExternalClock(&external)) {
            *error = "--native clock: the bridge's process clock has already started";
            return false;
        }
        Kernel::Clock::Enable();
        SSPC_LOG(Kernel, Warning, "EXPERIMENTAL: the process clock is SecondSonPC's (--native clock): one TSC measurement "
                                  "and one epoch for the game and the bridge");
    }
    return true;
}

} // namespace SecondSon::Bridge
