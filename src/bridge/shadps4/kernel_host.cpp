// SecondSonPC — TEMPORARY bridge for the native libkernel functions (Phase 2, P2.2): the guest
// errno and the process clock are still shadPS4's (shared with BRIDGE functions and with the
// VideoOut/AudioOut timestamps), and so are the guest threads whose priority orders waiters —
// unless --native threads (P2.4) runs them on SecondSonPC's thread manager, whose host is here too:
// guest stacks from the selected guest memory manager, TCBs from SecondSonPC's loader, the FS base in
// SecondSonPC's TCB slot (P3.3, runtime/cpu/tcb_slot.h), the bridge's debug thread list.
#include <algorithm>
#include <limits>
#include <mutex>

#include "app/config.h"
#include "base/log.h"
#include "bridge/shadps4/loader_host.h"
#include "core/debug_state.h"
#include "core/guest_memory.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/threads/pthread.h"
#include "core/libraries/kernel/time.h"
#include "core/tls.h"
#include "loader/linker.h"
#include "runtime/cpu/tcb_slot.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/thread.h"

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
        // Below the area the bridge's own stacks start from (threads/stack.cpp: usrstack -
        // ThrStackInitial - ThrGuardDefault), going down; the address is a hint, the manager finds
        // a free range, so stacks of the bridge's own threads (AudioOut) never overlap these.
        constexpr VAddr kUsrStack = 0x7EFFF8000ULL;
        constexpr VAddr kFirst = kUsrStack - 0x200000 - 0x4000;
        VAddr hint;
        {
            std::scoped_lock lk{mutex_};
            if (cursor_ == 0) {
                cursor_ = kFirst;
            }
            cursor_ -= size + guard;
            hint = cursor_;
        }
        auto& memory = Core::GetGuestVirtualMemory();
        void* addr = nullptr;
        if (memory.Map(&addr, {.addr = hint,
                               .size = size + guard,
                               .prot = Core::MemoryProt::CpuReadWrite,
                               .flags = Core::MemoryMapFlags::NoFlags,
                               .type = Core::VMAType::Stack}) != 0) {
            SSPC_LOG(Kernel, Error, "guest stack of {:#x} bytes could not be mapped", size + guard);
            return nullptr;
        }
        const VAddr base = reinterpret_cast<VAddr>(addr);
        if (guard != 0 && memory.Protect(base, guard, Core::MemoryProt::NoAccess) != 0) {
            SSPC_LOG(Kernel, Error, "guard of the guest stack at {:#x} could not be protected", base);
            memory.Unmap(base, size + guard);
            return nullptr;
        }
        return reinterpret_cast<void*>(base + guard);
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

private:
    std::mutex mutex_;
    VAddr cursor_ = 0;
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
    if (std::ranges::find(cfg.native_groups, "threads") != cfg.native_groups.end() && !cfg.selftest) {
        // P2.4: chosen once, before the main thread exists; every guest thread is then native.
        static ThreadHostImpl thread_host;
        Kernel::Threads::SetThreadHost(&thread_host);
        Kernel::Threads::Enable();
        SSPC_LOG(Kernel, Warning, "EXPERIMENTAL: guest threads on SecondSonPC's thread manager");
    }
}

} // namespace SecondSon::Bridge
