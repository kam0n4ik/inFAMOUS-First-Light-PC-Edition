// SecondSonPC — SSPC-002 regression test (--patch-race-test, docs/PHASE2_P25_PERFORMANCE.md).
//
// On Windows the bridge patches every guest `mov reg, fs:[disp]` the first time it runs (access
// violation -> core/cpu_patches.cpp TryPatchJit): the instruction becomes `jmp rel32` to a trampoline
// that reads the guest TCB. That jump is written into code other guest threads may be executing at
// the same moment. Written byte by byte, a thread can fetch a torn `jmp`: the new opcode with old
// displacement bytes, i.e. a jump to module + 0x25xxxxxx (0x25 = the SIB byte of the original
// instruction) - the unmapped address of every SSPC-002 crash.
//
// The test builds a module of its own (RWX code + trampoline area, registered with the patcher):
// kSites sites `mov rax, qword ptr fs:[0]; ret`. For each site kThreads threads are released
// together, wait a random 0..kSpreadUs microseconds each and call it kCallsPerThread times with random
// pauses: the first call faults and patches the site, the others arrive at random moments of that
// patch - some while its bytes are being written, as guest threads do in the game. Without a safe
// patch protocol a torn jump eventually kills the process (an execute fault in unmapped memory); with
// it every site ends patched. No game, no GPU, no window; the bridge's handler and patcher are real.
#include <atomic>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

#include <intrin.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "app/config.h"
#include "base/log.h"
#include "bridge/shadps4/loader_host.h"

#include "core/cpu_patches.h"
#include "core/signals.h"
#include "core/tls.h"
#include "runtime/cpu/fs_patcher.h"
#include "runtime/cpu/tcb_slot.h"

namespace SecondSon::Bridge {

namespace {

constexpr u32 kSites = 32768;
constexpr u32 kThreads = 8;
constexpr u32 kCallsPerThread = 3;
constexpr u32 kSpreadUs = 20;
constexpr u8 kSite[10] = {0x64, 0x48, 0x8b, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00, 0xC3}; // mov rax, fs:[0]; ret
constexpr u64 kSiteStride = 16;
constexpr u64 kCodeSize = kSites * kSiteStride;
constexpr u64 kTrampolineSize = 4ull << 20; // ~14 bytes per site

thread_local u64 t_dummy_tcb[4];

u64 TicksPerUs() {
    LARGE_INTEGER f, a, b;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&a);
    const u64 t0 = __rdtsc();
    Sleep(20);
    QueryPerformanceCounter(&b);
    const u64 t1 = __rdtsc();
    return static_cast<u64>(double(t1 - t0) / (double(b.QuadPart - a.QuadPart) * 1e6 / double(f.QuadPart)));
}

void SpinTicks(u64 ticks) {
    const u64 end = __rdtsc() + ticks;
    while (__rdtsc() < end) {
        _mm_pause();
    }
}

} // namespace

int RunPatchRaceTest(const App::Config& cfg) {
    std::ofstream out(cfg.logs_dir / "patchrace.txt", std::ios::trunc);
    const auto line = [&out](const std::string& text) {
        out << text << '\n';
        out.flush();
        SSPC_LOG(App, Info, "patch race test: {}", text);
    };
    (void)Core::Signals::Instance(); // the bridge's vectored handler (patcher, priority last)

    auto* base = static_cast<u8*>(VirtualAlloc(nullptr, kCodeSize + kTrampolineSize, MEM_RESERVE | MEM_COMMIT,
                                               PAGE_EXECUTE_READWRITE));
    if (base == nullptr) {
        line(fmt::format("FAIL: cannot allocate {:#x} bytes of RWX memory ({})", kCodeSize + kTrampolineSize,
                         GetLastError()));
        return 1;
    }
    for (u32 s = 0; s < kSites; ++s) {
        std::memcpy(base + s * kSiteStride, kSite, sizeof(kSite));
    }
    FlushInstructionCache(GetCurrentProcess(), base, kCodeSize);
    // P3.3: the patcher InstallGuestCpu chose (--native fs-patch = SecondSonPC's; its run-time path is
    // tested here: the module is registered but not patched at load time).
    const bool native = NativeFsPatcher();
    if (native) {
        if (!Cpu::RegisterModule(reinterpret_cast<u64>(base), kCodeSize, reinterpret_cast<u64>(base) + kCodeSize,
                                 kTrampolineSize, "patch-race-test")) {
            line("FAIL: SecondSonPC's FS patcher refused the test module");
            return 1;
        }
    } else {
        Core::RegisterPatchModule(base, kCodeSize, base + kCodeSize, kTrampolineSize);
    }
    // The TCB goes where the chosen patcher's trampolines read it (one slot with --loader sspc, P3.3; the
    // bridge's own on its Phase 0 path, where SecondSonPC's slot is not set up).
    const auto set_tcb = [native](void* tcb) {
        if (native) {
            Cpu::SetCurrentTcb(tcb);
        } else {
            Core::SetTcbBase(tcb);
        }
    };
    const u64 ticks_per_us = TicksPerUs();
    line(fmt::format("module {:#x}: {} sites, {} threads x {} calls each at random moments within {} us; patcher: {}",
                     reinterpret_cast<uintptr_t>(base), kSites, kThreads, kCallsPerThread, kSpreadUs,
                     native ? "SecondSonPC (run-time path)" : "the bridge's"));

    std::atomic<u32> ready{0};
    std::atomic<u32> round{0};
    std::atomic<u32> done{0};
    std::vector<std::thread> threads;
    for (u32 t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            t_dummy_tcb[0] = reinterpret_cast<u64>(&t_dummy_tcb[0]);
            set_tcb(t_dummy_tcb);
            u64 rng = 0x9E3779B97F4A7C15ull * (t + 1);
            const auto next = [&rng] {
                rng ^= rng << 13;
                rng ^= rng >> 7;
                rng ^= rng << 17;
                return rng;
            };
            for (u32 s = 0; s < kSites; ++s) {
                ready.fetch_add(1);
                while (round.load(std::memory_order_acquire) != s + 1) {
                    _mm_pause();
                }
                const auto site = reinterpret_cast<void (*)()>(base + s * kSiteStride);
                for (u32 c = 0; c < kCallsPerThread; ++c) {
                    SpinTicks(next() % (kSpreadUs * ticks_per_us));
                    site();
                }
                done.fetch_add(1);
            }
        });
    }
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    for (u32 s = 0; s < kSites; ++s) {
        while (ready.load() != (s + 1) * kThreads) {
            _mm_pause();
        }
        round.store(s + 1, std::memory_order_release);
        while (done.load() != (s + 1) * kThreads) {
            _mm_pause();
        }
    }
    for (auto& t : threads) {
        t.join();
    }
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);

    u32 patched = 0;
    for (u32 s = 0; s < kSites; ++s) {
        patched += base[s * kSiteStride] == 0xE9 ? 1 : 0;
    }
    // Every call read its thread's TCB through the trampoline: the value it returned must be checked too,
    // or a wrong slot would pass. Four threads on TCBs of their own call every site once more (each a quarter
    // of them) and compare.
    std::atomic<u32> wrong{0};
    std::vector<std::thread> checkers;
    for (u32 t = 0; t < 4; ++t) {
        checkers.emplace_back([&, t] {
            alignas(16) static thread_local u64 tcb[4];
            tcb[0] = reinterpret_cast<u64>(&tcb[0]); // a TCB starts with its self pointer (per thread)
            set_tcb(tcb);
            for (u32 s = t; s < kSites; s += 4) {
                const auto site = reinterpret_cast<u64 (*)()>(base + s * kSiteStride);
                if (site() != tcb[0]) {
                    wrong.fetch_add(1);
                }
            }
        });
    }
    for (auto& t : checkers) {
        t.join();
    }
    const u64 breakpoints = native ? Cpu::GetStats().breakpoints : Core::PatchBreakpointsHandled();
    const u64 misreported = native ? Cpu::GetStats().misreported : Core::PatchBreakpointsMisreported();
    const bool ok = patched == kSites && wrong.load() == 0;
    line(fmt::format("{} of {} sites patched (jmp to a trampoline), {} calls, {:.0f} ms; {} threads reached a "
                     "patch while it was written and were sent back to it ({} of them reported as a privileged "
                     "instruction); fs:[0] read back wrong {} times; {}",
                     patched, kSites, u64{kSites} * kThreads * kCallsPerThread,
                     double(t1.QuadPart - t0.QuadPart) * 1e3 / double(f.QuadPart), breakpoints, misreported,
                     wrong.load(), ok ? "PASS" : "FAIL"));
    if (native) {
        const auto st = Cpu::GetStats();
        line(fmt::format("SecondSonPC FS patcher: {} run-time sites, {} repeats, {} unsupported, {} trampoline bytes",
                         st.runtime_sites, st.runtime_repeats, st.unsupported, st.trampoline_bytes));
    }
    return ok ? 0 : 1;
}

} // namespace SecondSon::Bridge
