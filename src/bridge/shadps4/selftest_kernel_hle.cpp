// SecondSonPC — self-test of P3.5 Native Kernel HLE (docs/PHASE3_P35_KERNEL_HLE.md).
//   NativeErrno      the guest errno (P3.5.1). Runs in --selftest (the bridge owns the guest errno) and
//                    --selftest --native errno (SecondSonPC owns it); the owner's checks differ, the rest is the same.
//                    "Guest" calls go through the exports the game binds (System V); threads are made the bridge's way
//                    (Core::NativeThread: CreateThread) — the manager's own threads: NativeThreads(errno).
//   NativeKernelMem  libkernel's memory functions of SecondSonPC (P3.5.2, Kernel::MemoryApi::Api) on a manager of the
//                    self-test above 2 TiB: argument checks against the bridge's functions, GPU notifications, D17, D19,
//                    D20, repeated frees, refusals, threads, leaks. On the game's manager, step by step against the
//                    bridge's functions: NativeKernelMem(game) in --selftest --native memory (selftest_native_start.cpp).
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "app/config.h"
#include "bridge/shadps4/loader_host.h"
#include "bridge/shadps4/selftest_threads.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/time.h"
#include "core/thread.h"
#include "runtime/exports.h"
#include "runtime/filesystem/fs_host.h"
#include "runtime/kernel/guest_errno.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/kernel_memory.h"
#include "runtime/memory/virtual_memory.h"

namespace Libraries::Kernel { // not in core/libraries/kernel/memory.h
void* PS4_SYSV_ABI posix_mmap(void* addr, u64 len, s32 prot, s32 flags, s32 fd, s64 phys_addr);
} // namespace Libraries::Kernel

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

// ------------------------------------------------------------------------- P3.5.2: libkernel's memory functions

namespace MemApi = Kernel::MemoryApi;
using Memory::VirtualMemory;
using Memory::VmaType;

constexpr u64 kMiB = 1ull << 20;
constexpr u32 kGameSdk = 0x4008061; // the game's eboot.bin
constexpr u32 kOldSdk = 0x1000000;  // before 1.70: Fixed at 0 is dropped, direct memory may alias
constexpr s32 kOk = 0;
constexpr s32 kErrEinval = static_cast<s32>(0x80020016);
constexpr s32 kErrEfault = static_cast<s32>(0x8002000E);
constexpr s32 kErrEagain = static_cast<s32>(0x80020023);
constexpr s32 kErrEbusy = static_cast<s32>(0x80020010);
void* const kMapFailed = reinterpret_cast<void*>(~uintptr_t{0});

void* P(u64 addr) {
    return reinterpret_cast<void*>(addr);
}

std::string Hex(s32 r) {
    return r == 0 ? std::string("ok") : fmt::format("{:#x}", static_cast<u32>(r));
}

/// The manager's GPU notifications as text ("U+0x0+0x10000 ...", window-relative).
class NoteRecorder final : public Memory::GpuListener {
public:
    explicit NoteRecorder(u64 w) : w_{w} {}
    void OnGpuUnmap(u64 addr, u64 size) override {
        Add('U', addr, size);
    }
    void OnGpuRegister(u64 addr, u64 size) override {
        Add('R', addr, size);
    }
    void OnGpuMap(u64 addr, u64 size) override {
        Add('M', addr, size);
    }
    void OnGuestWrite(u64 addr, u64 size) override {
        Add('W', addr, size);
    }
    std::string Take() {
        std::scoped_lock lock{mutex_};
        return std::exchange(text_, {});
    }
    std::atomic<u64> count{0};

private:
    void Add(char kind, u64 addr, u64 size) {
        ++count;
        std::scoped_lock lock{mutex_};
        text_ += fmt::format("{}+{:#x}+{:#x} ", kind, addr - w_, size);
    }
    u64 w_;
    std::mutex mutex_;
    std::string text_;
};

/// A manager of the self-test on a free window above 2 TiB (direct 64 MiB, flexible 32 MiB).
std::unique_ptr<VirtualMemory> TestVm(u32 sdk, u64* base, std::string* error) {
    constexpr u64 kSize = 256 * kMiB;
    const u64 w = FindFreeNativeWindow(kSize);
    if (w == 0) {
        *error = "no free window above 2 TiB";
        return nullptr;
    }
    Memory::Layout l;
    l.ranges = {{w, kSize}};
    l.lowest = w;
    l.search_base = w;
    l.search_limit = w + kSize;
    l.system_reserved_base = w;
    l.system_reserved_end = w + kSize;
    l.gpu = {w, kSize};
    l.direct_size = 64 * kMiB;
    l.flexible_base = 64 * kMiB;
    l.flexible_size = 32 * kMiB;
    l.commit = Memory::CommitMode::Upfront;
    l.sdk_version = sdk;
    *base = w;
    return VirtualMemory::Create(l, error);
}

std::string AreasText(const VirtualMemory& vm, u64 w) {
    std::string out;
    for (const auto& a : vm.Areas(false)) {
        out += fmt::format("[+{:#x} +{:#x} t{} p{:#x}] ", a.base - w, a.size, static_cast<int>(a.type), a.prot);
    }
    return out;
}

bool AllDirectFree(const VirtualMemory& vm) {
    for (const auto& a : vm.Direct().Snapshot()) {
        if (a.state != Memory::AreaState::Free) {
            return false;
        }
    }
    return true;
}

/// The checks libkernel makes before it reaches a manager, on the bridge's functions (its own manager, untouched: every
/// case is refused before it) and on SecondSonPC's: the same codes.
void MemArguments(TestGroup& c, MemApi::Api& api) {
    namespace K = Libraries::Kernel;
    std::string diff;
    int cases = 0;
    const auto Same = [&](std::string_view what, s32 bridge, s32 own) {
        ++cases;
        if (bridge != own) {
            diff += fmt::format("{}: bridge {} own {}; ", what, Hex(bridge), Hex(own));
        }
    };
    s64 phys = 0;
    Same("allocate, negative start", K::sceKernelAllocateDirectMemory(-0x4000, 0x1000000, 0x4000, 0, 0, &phys),
         api.AllocateDirectMemory(-0x4000, 0x1000000, 0x4000, 0, 0, &phys));
    Same("allocate, negative end", K::sceKernelAllocateDirectMemory(0, -1, 0x4000, 0, 0, &phys),
         api.AllocateDirectMemory(0, -1, 0x4000, 0, 0, &phys));
    Same("allocate, length 0", K::sceKernelAllocateDirectMemory(0, 0x1000000, 0, 0, 0, &phys),
         api.AllocateDirectMemory(0, 0x1000000, 0, 0, 0, &phys));
    Same("allocate, length not page aligned", K::sceKernelAllocateDirectMemory(0, 0x1000000, 0x2000, 0, 0, &phys),
         api.AllocateDirectMemory(0, 0x1000000, 0x2000, 0, 0, &phys));
    Same("allocate, alignment not page aligned", K::sceKernelAllocateDirectMemory(0, 0x1000000, 0x4000, 0x2000, 0, &phys),
         api.AllocateDirectMemory(0, 0x1000000, 0x4000, 0x2000, 0, &phys));
    Same("allocate, memory type 11", K::sceKernelAllocateDirectMemory(0, 0x1000000, 0x4000, 0, 11, &phys),
         api.AllocateDirectMemory(0, 0x1000000, 0x4000, 0, 11, &phys));
    Same("allocate, no result pointer", K::sceKernelAllocateDirectMemory(0, 0x1000000, 0x4000, 0, 0, nullptr),
         api.AllocateDirectMemory(0, 0x1000000, 0x4000, 0, 0, nullptr));
    Same("allocate, end <= start", K::sceKernelAllocateDirectMemory(0x10000, 0x10000, 0x4000, 0, 0, &phys),
         api.AllocateDirectMemory(0x10000, 0x10000, 0x4000, 0, 0, &phys));
    Same("allocate, end < length", K::sceKernelAllocateDirectMemory(0, 0x4000, 0x8000, 0, 0, &phys),
         api.AllocateDirectMemory(0, 0x4000, 0x8000, 0, 0, &phys));
    const auto Direct = [&](std::string_view what, u64 len, s32 prot, s32 flags, s64 ph, u64 align) {
        void* a = nullptr;
        void* b = nullptr;
        Same(what, K::sceKernelMapDirectMemory(&a, len, prot, flags, ph, align),
             api.MapDirectMemory(&b, len, prot, flags, ph, align));
    };
    Direct("map direct, length 0", 0, 3, 0, 0, 0);
    Direct("map direct, length not page aligned", 0x2000, 3, 0, 0, 0);
    Direct("map direct, physical address not page aligned", 0x4000, 3, 0, 0x2000, 0);
    Direct("map direct, alignment 3 (neither a power of two nor page aligned)", 0x4000, 3, 0, 0, 3);
    Direct("map direct, CPU execute", 0x4000, 7, 0, 0, 0);
    Direct("map direct, fixed at address 0", 0x4000, 3, 0x10, 0, 0);
    const auto Flexible = [&](std::string_view what, u64 len, s32 flags, const char* name) {
        void* a = nullptr;
        void* b = nullptr;
        Same(what, K::sceKernelMapNamedFlexibleMemory(&a, len, 3, flags, name),
             api.MapNamedFlexibleMemory(&b, len, 3, flags, name));
    };
    Flexible("flexible, length 0", 0, 0, "x");
    Flexible("flexible, length not page aligned", 0x2000, 0, "x");
    Flexible("flexible, a 32-character name", 0x4000, 0, "0123456789abcdef0123456789abcdef");
    Flexible("flexible, fixed at address 0", 0x4000, 0x10, "x");
    Same("munmap, length 0", K::sceKernelMunmap(P(0x4000), 0), api.Munmap(P(0x4000), 0));
    Same("release, start not page aligned", K::sceKernelReleaseDirectMemory(0x2000, 0x4000),
         api.ReleaseDirectMemory(0x2000, 0x4000));
    Same("release, length not page aligned", K::sceKernelReleaseDirectMemory(0x4000, 0x2000),
         api.ReleaseDirectMemory(0x4000, 0x2000));
    Same("release, length 0", K::sceKernelReleaseDirectMemory(0x4000, 0), api.ReleaseDirectMemory(0x4000, 0));
    Same("mprotect, size 0 at a page", K::sceKernelMprotect(P(0x4000), 0, 1), api.Mprotect(P(0x4000), 0, 1));
    const auto Mmap = [&](std::string_view what, u64 addr, u64 len, s32 flags) {
        void* const b = K::posix_mmap(P(addr), len, 3, flags, -1, 0);
        const s32 be = *K::__Error();
        s32 oe = 0;
        void* const o = api.Mmap(P(addr), len, 3, flags, -1, 0, &oe);
        Same(what, b == kMapFailed ? be : -1, o == kMapFailed ? oe : -1);
    };
    Mmap("mmap, length 0 (errno)", 0, 0, 0x1002);
    Mmap("mmap, fixed at an unaligned address (errno)", 0x4100, 0x4000, 0x1012);
    c.Check(fmt::format("{} argument errors: SecondSonPC's functions give the bridge's codes (EINVAL, EAGAIN, EACCES, "
                        "ENAMETOOLONG; mmap's errno 22)",
                        cases),
            diff.empty(), diff);

    // Where SecondSonPC is safer than the oracle (which reads *addr, faults on a null name in its log line, wraps).
    void* none = nullptr;
    s32 err = 0;
    const s32 a = api.MapDirectMemory(nullptr, 0x4000, 3, 0, 0, 0);
    const s32 b = api.MapNamedFlexibleMemory(nullptr, 0x4000, 3, 0, "x");
    const s32 d = api.MapNamedFlexibleMemory(&none, 0x4000, 3, 0, nullptr);
    const s32 e = api.Mprotect(P(0x4004), ~0ull - 2, 1);
    const bool f = api.Mmap(P(~0ull - 0x100), 0x4000, 3, 0x1002, -1, 0, &err) == kMapFailed && err == 22;
    err = 0;
    const bool g = api.Mmap(nullptr, 0x4000, 1, 0, 3, 0, &err) == kMapFailed && err == 22; // a file: not supported
    c.Check("no address pointer EINVAL (map direct, flexible), no name EFAULT, a wrapping mprotect size EINVAL, a "
            "wrapping mmap address errno 22, a file mapping errno 22 (no file mappings in the manager)",
            a == kErrEinval && b == kErrEinval && d == kErrEfault && e == kErrEinval && f && g,
            fmt::format("{} {} {} {} {} {}", Hex(a), Hex(b), Hex(d), Hex(e), f, g));
}

void MemScenario(TestGroup& c) {
    u64 w = 0;
    std::string error;
    auto vm = TestVm(kGameSdk, &w, &error);
    if (vm == nullptr) {
        c.Check("a manager of the self-test", false, error);
        return;
    }
    NoteRecorder rec(w);
    vm->SetListener(&rec);
    MemApi::Api api(*vm, kGameSdk);
    MemArguments(c, api);

    // Notifications: as on the bridge's path, the manager tells the GPU; mprotect adds the host write over exactly the
    // aligned range.
    void* p = P(w);
    const s32 m = api.MapNamedFlexibleMemory(&p, 0x10000, 3, 0x10, "flex");
    rec.Take();
    const s32 pr = api.Mprotect(P(w + 0x4000), 0x4000, 1);
    const std::string n1 = rec.Take();
    const s32 pr2 = api.Mprotect(P(w + 0x8100), 0x100, 3);
    const std::string n2 = rec.Take();
    void* qs = nullptr;
    void* qe = nullptr;
    u32 qp = 0;
    const s32 q = api.QueryMemoryProtection(P(w + 0x4000), &qs, &qe, &qp);
    c.Check("mprotect: protection by whole pages (an unaligned start covers its page), the GPU told of a host write "
            "over exactly that range; the query sees the page's own area",
            m == kOk && p == P(w) && pr == kOk && n1.find("W+0x4000+0x4000") != std::string::npos && pr2 == kOk &&
                n2.find("W+0x8000+0x4000") != std::string::npos && q == kOk && qs == P(w + 0x4000) &&
                qe == P(w + 0x8000) && qp == 1,
            fmt::format("{} | {} | {} {} {:#x}", n1, n2, qs, qe, qp));
    const s32 u = api.Munmap(P(w), 0x10000);
    const std::string n3 = rec.Take();
    c.Check("munmap: the GPU hears of the unmap before the address space changes", u == kOk && n3.starts_with("U+0x0+0x10000"),
            n3);

    // D17: private memory (a stack) cannot be unmapped in part; a stack with its guard goes in one call.
    s32 err = 0;
    void* const stack = api.Mmap(P(w + 0x200000), 0x10000, 3, 0x410, -1, 0, &err);
    const s32 part = api.Munmap(P(w + 0x204000), 0x4000);
    const std::string kept = AreasText(*vm, w);
    void* const guard = api.Mmap(P(w + 0x300000), 0x4000, 0, 0x110, -1, 0, &err); // MAP_VOID | MAP_FIXED
    void* const stack2 = api.Mmap(P(w + 0x304000), 0x10000, 3, 0x410, -1, 0, &err);
    const s32 whole = api.Munmap(P(w + 0x300000), 0x14000);
    c.Check("D17: part of a stack (private memory) is refused with EINVAL and the stack stays whole; a guard + stack "
            "pair goes in one munmap",
            stack == P(w + 0x200000) && part == kErrEinval && kept.find("[+0x200000 +0x10000 t6") != std::string::npos &&
                guard == P(w + 0x300000) && stack2 == P(w + 0x304000) && whole == kOk &&
                AreasText(*vm, w).find("+0x30") == std::string::npos,
            fmt::format("{} {} | {}", Hex(part), Hex(whole), kept));
    api.Munmap(P(w + 0x200000), 0x10000);

    // D19: what stays of a partly unmapped direct view keeps the guest's protection and the host's (the bridge maps the
    // rest again read/write: a no-access page — the guest's, or the GPU's page tracking — becomes writable).
    s64 phys = -1;
    void* d = P(w + 0x400000);
    const auto HostProtect = [](u64 addr) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery(P(addr), &mbi, sizeof(mbi));
        return mbi.Protect;
    };
    const s32 al = api.AllocateDirectMemory(0, 64 * kMiB, 0x10000, 0, 0, &phys);
    const s32 mp = api.MapDirectMemory(&d, 0x10000, 3, 0x10, phys, 0);
    const s32 na = api.Mprotect(P(w + 0x408000), 0x4000, 0);
    const DWORD host_before = HostProtect(w + 0x408000);
    const s32 cut = api.Munmap(P(w + 0x400000), 0x4000);
    const DWORD host_after = HostProtect(w + 0x408000);
    u32 p2 = 9;
    u32 p1 = 9;
    api.QueryMemoryProtection(P(w + 0x408000), nullptr, nullptr, &p2);
    api.QueryMemoryProtection(P(w + 0x404000), nullptr, nullptr, &p1);
    c.Check("D19: after unmapping the first page of a direct view, a no-access page keeps protection 0 for the guest and "
            "PAGE_NOACCESS on the host, the read/write page keeps 3",
            al == kOk && mp == kOk && na == kOk && cut == kOk && p2 == 0 && p1 == 3 && host_before == PAGE_NOACCESS &&
                host_after == PAGE_NOACCESS && HostProtect(w + 0x404000) == PAGE_READWRITE,
            fmt::format("{} {} {} {} guest {} {} host {:#x} -> {:#x}", Hex(al), Hex(mp), Hex(na), Hex(cut), p2, p1,
                        host_before, host_after));

    // Exclusive direct memory from SDK 2.50; repeated frees.
    void* alias = P(w + 0x500000);
    const s32 ex = api.MapDirectMemory(&alias, 0x4000, 3, 0x10, phys + 0x8000, 0);
    const s32 un1 = api.Munmap(P(w + 0x404000), 0xC000);
    const s32 un2 = api.Munmap(P(w + 0x404000), 0xC000);
    const s32 r1 = api.ReleaseDirectMemory(static_cast<u64>(phys), 0x10000);
    const s32 r2 = api.ReleaseDirectMemory(static_cast<u64>(phys), 0x10000);
    c.Check("SDK 4.0: a second view of mapped direct memory is EBUSY; munmap twice and release twice answer 0 and leave "
            "no area and no allocated direct memory",
            ex == kErrEbusy && un1 == kOk && un2 == kOk && r1 == kOk && r2 == kOk && vm->Areas(false).empty() &&
                AllDirectFree(*vm),
            fmt::format("{} {} {} {} {} | {}", Hex(ex), Hex(un1), Hex(un2), Hex(r1), Hex(r2), AreasText(*vm, w)));

    // Refusals: no memory left, nothing mapped by the failure.
    s64 big = -1;
    const s32 nodirect = api.AllocateDirectMemory(0, 64 * kMiB, 128 * kMiB, 0, 0, &big);
    void* f = nullptr;
    const s32 noflex = api.MapNamedFlexibleMemory(&f, 64 * kMiB, 3, 0, "too much");
    c.Check("more direct memory than exists EAGAIN, more flexible memory than the budget refused; nothing is left mapped "
            "or allocated",
            nodirect == kErrEagain && noflex != kOk && vm->Areas(false).empty() && AllDirectFree(*vm) &&
                vm->FlexibleUsed() == 0,
            fmt::format("{} {}", Hex(nodirect), Hex(noflex)));
    const std::string inv = vm->CheckInvariants();
    c.Check("the manager's areas, host regions, backing table and physical maps agree after all of it", inv.empty(), inv);
    vm->SetListener(nullptr);
}

void MemAliases(TestGroup& c) {
    // D20 (the bridge's defect: two overlapping views of one direct allocation, the smaller unmapped first, assert in
    // its map) through SecondSonPC's functions with an SDK before 2.50, where views may alias.
    u64 w = 0;
    std::string error;
    auto vm = TestVm(kOldSdk, &w, &error);
    if (vm == nullptr) {
        c.Check("a manager of the self-test (SDK 1.00)", false, error);
        return;
    }
    MemApi::Api api(*vm, kOldSdk);
    s64 phys = -1;
    void* a = P(w);
    void* b = P(w + 0x100000);
    const s32 al = api.AllocateDirectMemory(0, 64 * kMiB, 0x30000, 0, 0, &phys);
    const s32 ma = api.MapDirectMemory(&a, 0x20000, 3, 0x10, phys, 0);
    const s32 mb = api.MapDirectMemory(&b, 0x20000, 3, 0x10, phys + 0x10000, 0);
    *static_cast<volatile u32*>(P(w + 0x10000)) = 0x5EC0;
    const bool shared = *static_cast<volatile u32*>(P(w + 0x100000)) == 0x5EC0; // the same physical page
    const s32 ub = api.Munmap(P(w + 0x100000), 0x20000);
    const s32 ua = api.Munmap(P(w), 0x20000);
    const s32 rl = api.ReleaseDirectMemory(static_cast<u64>(phys), 0x30000);
    void* z = nullptr;
    const s32 fz = api.MapNamedFlexibleMemory(&z, 0x4000, 3, 0x10, "fixed at 0");
    const bool dropped = fz == kOk && z == P(w); // before 1.70 Fixed at 0 is dropped: a search from the base
    api.Munmap(z, 0x4000);
    const std::string inv = vm->CheckInvariants();
    c.Check("D20 with SDK 1.00: two overlapping views of one allocation share their pages; the smaller unmapped first, "
            "then the larger, then the release: no failure, the maps agree; Fixed at 0 is dropped before SDK 1.70",
            al == kOk && ma == kOk && mb == kOk && shared && ub == kOk && ua == kOk && rl == kOk && dropped &&
                AllDirectFree(*vm) && inv.empty(),
            fmt::format("{} {} {} {} {} {} {} {}", Hex(al), Hex(ma), Hex(mb), shared, Hex(ub), Hex(ua), Hex(rl), inv));
}

void MemThreads(TestGroup& c) {
    u64 w = 0;
    std::string error;
    auto vm = TestVm(kGameSdk, &w, &error);
    if (vm == nullptr) {
        c.Check("a manager of the self-test", false, error);
        return;
    }
    NoteRecorder rec(w);
    vm->SetListener(&rec);
    MemApi::Api api(*vm, kGameSdk);
    const HANDLE process = GetCurrentProcess();
    DWORD handles_before = 0;
    GetProcessHandleCount(process, &handles_before);
    PROCESS_MEMORY_COUNTERS_EX before{};
    GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof(before));

    constexpr int kFlexThreads = 6;
    constexpr int kDirectThreads = 2;
    constexpr int kRounds = 300;
    std::atomic<u64> failures{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kFlexThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kRounds; ++i) {
                s32 err = 0;
                void* const p = api.Mmap(nullptr, 0x10000, 3, 0x1002, -1, 0, &err);
                if (p == kMapFailed) {
                    ++failures;
                    continue;
                }
                static_cast<volatile u8*>(p)[0x8000] = 1;
                u32 prot = 0;
                if (api.Mprotect(static_cast<u8*>(p) + 0x8000, 0x8000, 1) != kOk ||
                    api.QueryMemoryProtection(static_cast<u8*>(p) + 0x8000, nullptr, nullptr, &prot) != kOk || prot != 1 ||
                    api.Munmap(p, 0x10000) != kOk) {
                    ++failures;
                }
            }
        });
    }
    for (int t = 0; t < kDirectThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kRounds; ++i) {
                s64 phys = -1;
                void* d = nullptr;
                if (api.AllocateDirectMemory(0, 64 * kMiB, 0x10000, 0, 3, &phys) != kOk) {
                    ++failures;
                    continue;
                }
                if (api.MapDirectMemory(&d, 0x10000, 0x33, 0, phys, 0) != kOk || api.Mprotect(d, 0x4000, 0x11) != kOk ||
                    api.Munmap(d, 0x10000) != kOk) {
                    ++failures;
                }
                api.ReleaseDirectMemory(static_cast<u64>(phys), 0x10000);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    DWORD handles_after = 0;
    GetProcessHandleCount(process, &handles_after);
    PROCESS_MEMORY_COUNTERS_EX after{};
    GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof(after));
    const s64 private_delta = static_cast<s64>(after.PrivateUsage) - static_cast<s64>(before.PrivateUsage);
    const std::string inv = vm->CheckInvariants();
    c.Check(fmt::format("{} threads x {} rounds (flexible mmap/mprotect/query/munmap; direct allocate/map/mprotect/"
                        "munmap/release) at once: no failure, nothing left mapped or allocated, the maps agree",
                        kFlexThreads + kDirectThreads, kRounds),
            failures == 0 && vm->Areas(false).empty() && AllDirectFree(*vm) && vm->FlexibleUsed() == 0 && inv.empty() &&
                rec.count > 0,
            fmt::format("failures {}, {}", failures.load(), inv));
    c.Check("no HANDLE and no private memory left behind by 2400 rounds (handles +-4, private < 8 MiB)",
            static_cast<s64>(handles_after) - static_cast<s64>(handles_before) <= 4 && private_delta < 8 * static_cast<s64>(kMiB),
            fmt::format("handles {} -> {}, private {:+} KiB", handles_before, handles_after, private_delta / 1024));
    vm->SetListener(nullptr);
}

void MemExports(TestGroup& c) {
    if (MemApi::Installed()) {
        c.Info("INFO  --native kernel-memory installed: the exports are checked on the game's manager (NativeKernelMem(game))");
        return;
    }
    using SizeFn = u64(SSPC_SYSV_ABI*)();
    using UnmapFn = s32(SSPC_SYSV_ABI*)(void*, u64);
    SizeFn size = nullptr;
    UnmapFn unmap = nullptr;
    int experimental = 0;
    for (const auto& e : MemApi::Exports()) {
        experimental += e.experimental && std::string_view(e.group) == "kernel-memory" ? 1 : 0;
        if (std::string_view(e.name) == "sceKernelGetDirectMemorySize") {
            size = reinterpret_cast<SizeFn>(e.function);
        } else if (std::string_view(e.name) == "sceKernelMunmap") {
            unmap = reinterpret_cast<UnmapFn>(e.function);
        }
    }
    c.Check("9 exports in the experimental group kernel-memory; not installed (no --native kernel-memory), they refuse "
            "instead of reaching any manager",
            experimental == 9 && size != nullptr && unmap != nullptr && size() == 0 && unmap(P(0x4000), 0x4000) == kErrEinval,
            fmt::format("{}", experimental));
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

    TestGroup mem{.name = "NativeKernelMem"};
    MemScenario(mem);
    MemAliases(mem);
    MemThreads(mem);
    MemExports(mem);
    out.push_back(std::move(mem));
    return out;
}

} // namespace SecondSon::Bridge
