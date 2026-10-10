// SecondSonPC — self-test of P3.5 Native Kernel HLE (docs/PHASE3_P35_KERNEL_HLE.md): the guest errno (P3.5.1).
// Runs in --selftest (the bridge owns the guest errno) and --selftest --native errno (SecondSonPC owns it); the
// checks of the owner differ, everything else is the same in both. "Guest" calls go through the exports the game
// binds (System V); threads are made the bridge's way (Core::NativeThread: CreateThread) — the manager's own threads
// are checked in NativeThreads(errno) (selftest_threads.cpp).
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "app/config.h"
#include "bridge/shadps4/loader_host.h"
#include "bridge/shadps4/selftest_threads.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/time.h"
#include "core/thread.h"
#include "runtime/exports.h"
#include "runtime/filesystem/fs_host.h"
#include "runtime/kernel/guest_errno.h"
#include "runtime/kernel/kernel.h"

namespace SecondSon::Bridge {

namespace {

namespace GuestErrno = Kernel::GuestErrno;

using ErrorFn = s32*(SSPC_SYSV_ABI*)();

struct Timespec {
    s64 tv_sec;
    s64 tv_nsec;
};

// FreeBSD (the console's) errno values.
constexpr s32 kEintr = 4;
constexpr s32 kEbadf = 9;
constexpr s32 kEfault = 14;
constexpr s32 kEinval = 22;

ErrorFn g_error = nullptr;
s32(SSPC_SYSV_ABI* g_clock_gettime)(u32, Timespec*) = nullptr;
s32(SSPC_SYSV_ABI* g_sce_clock_gettime)(u32, Timespec*) = nullptr;
s32(SSPC_SYSV_ABI* g_nanosleep)(const Timespec*, Timespec*) = nullptr;

template <typename F>
bool Find(std::span<const Runtime::Export> exports, std::string_view name, std::string_view library, F& out) {
    for (const auto& e : exports) {
        if (name == e.name && library == e.library) {
            out = reinterpret_cast<F>(e.function);
            return true;
        }
    }
    return false;
}

/// Runs `body` on `count` threads made the bridge's way (Core::NativeThread, one CreateThread each) and waits for them.
template <typename Body>
bool OnBridgeThreads(int count, Body body) {
    struct Arg {
        Body* body;
        int index;
    };
    std::vector<std::unique_ptr<Core::NativeThread>> threads;
    std::vector<Arg> args(count);
    bool ok = true;
    for (int i = 0; i < count; ++i) {
        args[i] = {&body, i};
        auto t = std::make_unique<Core::NativeThread>();
        ok &= t->Create(
                  [](void* p) -> DWORD {
                      auto* a = static_cast<Arg*>(p);
                      (*a->body)(a->index);
                      return 0;
                  },
                  &args[i]) == 0;
        threads.push_back(std::move(t));
    }
    for (auto& t : threads) {
        const auto handle = reinterpret_cast<HANDLE>(t->GetHandle());
        if (handle == nullptr) {
            continue;
        }
        ok &= WaitForSingleObject(handle, 30'000) == WAIT_OBJECT_0;
        CloseHandle(handle);
    }
    return ok;
}

// ------------------------------------------------------------------------- the ABI

void ErrnoAbi(TestGroup& c) {
    const Runtime::Export* e = nullptr;
    for (const auto& x : GuestErrno::Exports()) {
        if (std::string_view(x.name) == "__error") {
            e = &x;
        }
    }
    c.Check("__error is exported with the game's key (9BcDykPmo1I, libkernel/libkernel), group errno, experimental",
            e != nullptr && std::string_view(e->nid) == "9BcDykPmo1I" && std::string_view(e->library) == "libkernel" &&
                std::string_view(e->module) == "libkernel" && std::string_view(e->group) == "errno" &&
                e->experimental && e->original == nullptr,
            "");
    s32* const p = g_error();
    bool stable = true;
    for (int i = 0; i < 1000; ++i) {
        stable &= g_error() == p;
    }
    c.Check("int* __error(void): a 4-byte aligned cell, this thread's own (OwnLocation), the same through 1000 calls",
            p != nullptr && reinterpret_cast<uintptr_t>(p) % alignof(s32) == 0 && p == GuestErrno::OwnLocation() &&
                stable,
            fmt::format("{}", fmt::ptr(p)));

    // The active owner's cell (the bridge's or SecondSonPC's): what NATIVE and BRIDGE functions write.
    s32* const active = Kernel::GetHost()->Errno();
    const s32 saved = *active;
    *active = 1234;
    FILE* f = nullptr;
    const errno_t open_error = _wfopen_s(&f, L"Z:\\sspc-selftest\\no such file", L"rb");
    SetLastError(ERROR_ACCESS_DENIED);
    c.Check("host code's own errno (the CRT's) and last error do not touch the guest errno",
            f == nullptr && open_error != 0 && *active == 1234, fmt::format("CRT {}, guest {}", open_error, *active));
    Timespec ts{};
    const Timespec zero{0, 0};
    const bool ok_calls = g_clock_gettime(4, &ts) == 0 && g_nanosleep(&zero, nullptr) == 0;
    c.Check("successful calls leave errno as it was (clock_gettime, nanosleep)", ok_calls && *active == 1234,
            fmt::format("{}", *active));
    const s32 r1 = g_clock_gettime(0, nullptr);
    const s32 e1 = *active;
    const Timespec bad{0, 1'000'000'000};
    const s32 r2 = g_nanosleep(&bad, nullptr);
    const s32 e2 = *active;
    const s32 r3 = g_sce_clock_gettime(0, nullptr);
    c.Check("failures set the console's (FreeBSD) values: clock_gettime(NULL) -1/EFAULT 14, nanosleep(1e9 ns) "
            "-1/EINVAL 22, sceKernelClockGettime(NULL) 0x8002000E",
            r1 == -1 && e1 == kEfault && r2 == -1 && e2 == kEinval && r3 == static_cast<s32>(0x8002000E),
            fmt::format("{}/{} {}/{} {:#x}", r1, e1, r2, e2, static_cast<u32>(r3)));
    *active = saved;
}

// ------------------------------------------------------------------------- threads made the bridge's way

void ErrnoBridgeThreads(TestGroup& c) {
    constexpr int kThreads = 8;
    std::atomic<int> written{0};
    std::atomic<u32> bad{0};
    std::vector<s32*> cells(kThreads, nullptr);
    const bool ran = OnBridgeThreads(kThreads, [&](int index) {
        const s32 mine = 2000 + index;
        s32* const cell = g_error();
        cells[index] = cell;
        u32 b = *cell != 0 ? 1 : 0;
        *cell = mine;
        written.fetch_add(1);
        while (written.load() < kThreads) {
            Sleep(0);
        }
        for (int i = 0; i < 20000 && b == 0; ++i) {
            s32* const again = g_error();
            b |= again != cell ? 2 : (*again != mine ? 4 : 0);
            *again = mine;
        }
        *cell = 0x5A5A;
        bad.fetch_or(b);
    });
    std::ranges::sort(cells);
    const bool distinct = std::ranges::adjacent_find(cells) == cells.end() &&
                          std::ranges::find(cells, nullptr) == cells.end() &&
                          std::ranges::find(cells, GuestErrno::OwnLocation()) == cells.end();
    c.Check("8 threads made the bridge's way at once: each starts at 0, keeps its own cell, sees only its own value",
            ran && bad == 0 && distinct, fmt::format("result bits {:#x}, distinct {}", bad.load(), distinct));

    u32 fresh = 0;
    for (int i = 0; i < 64; ++i) {
        OnBridgeThreads(1, [&](int) {
            s32* const cell = g_error();
            fresh += *cell == 0 ? 1 : 0;
            *cell = 0x5A5A;
        });
    }
    c.Check("64 such threads one after another, each after one that left 0x5A5A: each starts at 0", fresh == 64,
            fmt::format("{}/64", fresh));
}

// ------------------------------------------------------------------------- one owner

void ErrnoOwner(TestGroup& c) {
    namespace K = Libraries::Kernel;
    s32* const own = GuestErrno::OwnLocation();
    const auto fs_host = CreateFsHost();
    if (GuestErrno::Enabled()) {
        c.Info("INFO  --native errno: SecondSonPC owns the guest errno in this run");
        c.Check("the bridge's __Error(), Kernel::Host::Errno and the file system's host all give SecondSonPC's cell",
                K::__Error() == own && Kernel::GetHost()->Errno() == own && fs_host->Errno() == own, "");
        std::atomic<bool> same_elsewhere{false};
        OnBridgeThreads(1, [&](int) { same_elsewhere = K::__Error() == g_error() && g_error() != own; });
        c.Check("... on another thread too: the bridge's cell there is that thread's own SecondSonPC cell",
                same_elsewhere.load(), "");
        *own = 0;
        K::SetPosixErrno(EINVAL);
        const s32 a = *own;
        K::ErrSceToPosix(static_cast<s32>(0x80020009));
        const s32 b = *own;
        const s32 r = K::posix_clock_gettime(0, nullptr);
        const s32 d = *own;
        c.Check("BRIDGE functions write SecondSonPC's cell: SetPosixErrno(EINVAL) 22, ErrSceToPosix(EBADF) 9, the "
                "bridge's clock_gettime(NULL) -1/14",
                a == kEinval && b == kEbadf && r == -1 && d == kEfault, fmt::format("{} {} {}/{}", a, b, r, d));
        c.Check("the bridge cannot be redirected again (nullptr or a second location refused)",
                !K::UseExternalErrno(nullptr) && !K::UseExternalErrno(&GuestErrno::OwnLocation), "");
    } else {
        c.Info("INFO  the bridge owns the guest errno in this run (--selftest --native errno checks SecondSonPC's)");
        s32* const bridge = K::__Error();
        *own = 777;
        K::SetPosixErrno(EINVAL);
        const s32 a = *bridge;
        K::ErrSceToPosix(static_cast<s32>(0x80020004));
        const s32 b = *bridge;
        c.Check("one owner, the bridge's cell: Kernel::Host::Errno and the file system's host give it, BRIDGE writes "
                "(22, then EINTR 4) land there, SecondSonPC's own cell stays untouched",
                bridge != own && Kernel::GetHost()->Errno() == bridge && fs_host->Errno() == bridge && a == kEinval &&
                    b == kEintr && *own == 777,
                fmt::format("{} {} own {}", a, b, *own));
        *own = 0;
    }
}

// ------------------------------------------------------------------------- many threads at once

void ErrnoStress(TestGroup& c) {
    constexpr int kThreads = 16;
    constexpr int kRounds = 200'000;
    std::atomic<u64> mismatches{0};
    const bool native = GuestErrno::Enabled();
    const auto t0 = std::chrono::steady_clock::now();
    const bool ran = OnBridgeThreads(kThreads, [&](int index) {
        u64 bad = 0;
        for (int i = 0; i < kRounds; ++i) {
            s32* const cell = g_error();
            const s32 v = index * 1'000'000 + i;
            *cell = v;
            if (native && Libraries::Kernel::__Error() != cell) {
                ++bad; // with --native errno the bridge's accessor is the same cell
            }
            if (*g_error() != v) {
                ++bad;
            }
        }
        mismatches += bad;
    });
    const double ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count() /
                      (static_cast<double>(kThreads) * kRounds);
    c.Check(fmt::format("16 threads x 200000 set/read rounds through __error{}: no value crosses threads",
                        native ? " and the bridge's __Error()" : ""),
            ran && mismatches == 0, fmt::format("{} mismatches", mismatches.load()));
    c.Info(fmt::format("INFO  {:.1f} ns per round (wall time / rounds, 16 threads)", ns));
}

} // namespace

std::vector<TestGroup> TestKernelHle(const App::Config& cfg) {
    (void)cfg;
    std::vector<TestGroup> out(1);
    out[0].name = "NativeErrno";
    for (const auto& e : GuestErrno::Exports()) {
        if (std::string_view(e.name) == "__error") {
            g_error = reinterpret_cast<ErrorFn>(e.function);
        }
    }
    const auto k = Kernel::KernelExports();
    if (g_error == nullptr || !Find(k, "clock_gettime", "libkernel", g_clock_gettime) ||
        !Find(k, "sceKernelClockGettime", "libkernel", g_sce_clock_gettime) ||
        !Find(k, "nanosleep", "libkernel", g_nanosleep)) {
        out[0].Check("exports (__error, clock_gettime, sceKernelClockGettime, nanosleep)", false, "missing");
        return out;
    }
    ErrnoAbi(out[0]);
    ErrnoBridgeThreads(out[0]);
    ErrnoOwner(out[0]);
    ErrnoStress(out[0]);
    return out;
}

} // namespace SecondSon::Bridge
