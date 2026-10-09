// SecondSonPC — `--selftest --native memory` (Phase 2, P2.3.4; docs/PHASE2_P234_NATIVE_MEMORY.md): SecondSonPC's
// memory manager started the way the game starts it (InstallNativeMemory in RunGame, before anything reaches the
// guest memory; the bridge's libraries, Vulkan and the rasterizer up), then driven through what the game and the
// bridge call: the bridge's libkernel memory functions, the loader's own calls, Core::GpuMemory. No bridge manager
// exists in this process (creating one would assert), so nothing here compares with it: the expected values are
// the bridge's own, recorded from the game (docs/PHASE2_P23_MEMORY.md §2, scenario G).
//   NativeStart        the selection, the reservation (host regions against VirtualQuery), the layout, the
//                      rasterizer as the listener, the commit mode
//   NativeStart(game)  the game's direct memory scenario G with the bridge's results, the GnmDriver memory and the
//                      primary TLS through the loader, mprotect, queries, flexible memory, notifications, release
//   NativeStart(prot)  the guest's protection and the GPU's page tracking on the host (the access violation
//                      handler's call), D19, the handler's call latency
//   NativeStart(mt)    threads through libkernel (flexible, direct, mprotect, queries) with a GPU reader; watchdog
//   NativeStart(none)  what the manager does not have fails loudly (pools, file mappings)
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include <fmt/format.h>

#include "app/config.h"
#include "base/log.h"
#include "bridge/shadps4/loader_host.h"
#include "bridge/shadps4/native_memory.h"
#include "bridge/shadps4/selftest_vmm.h"
#include "core/libraries/kernel/memory.h"

namespace Libraries::Kernel { // not in core/libraries/kernel/memory.h
void* PS4_SYSV_ABI posix_mmap(void* addr, u64 len, s32 prot, s32 flags, s32 fd, s64 phys_addr);
s32 PS4_SYSV_ABI sceKernelConfiguredFlexibleMemorySize(u64* sizeOut);
} // namespace Libraries::Kernel

namespace SecondSon::Bridge {
namespace {

namespace K = Libraries::Kernel;

constexpr u64 kMiB = 1ull << 20;
constexpr s32 kAnonPrivate = 0x1002; // MAP_ANON | MAP_PRIVATE: flexible memory

void* Ptr(u64 addr) {
    return reinterpret_cast<void*>(addr);
}

std::string Code(s64 r) {
    return r == 0 ? std::string("ok") : fmt::format("{:#x}", static_cast<u32>(r));
}

DWORD HostProtect(u64 addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(Ptr(addr), &mbi, sizeof(mbi));
    return mbi.Protect;
}

/// The system's commit charge and what is left of it, in bytes.
std::pair<u64, u64> SystemCommit() {
    PERFORMANCE_INFORMATION info{};
    info.cb = sizeof(info);
    if (!GetPerformanceInfo(&info, sizeof(info))) {
        return {0, 0};
    }
    return {static_cast<u64>(info.CommitTotal) * info.PageSize,
            static_cast<u64>(info.CommitLimit - info.CommitTotal) * info.PageSize};
}

/// Between the manager and the rasterizer: records the notifications (while asked to) and hands each on.
class Recorder final : public Core::GpuMemoryListener {
public:
    void OnGpuUnmap(VAddr addr, u64 size) override {
        Add('U', addr, size);
        next->OnGpuUnmap(addr, size);
    }
    void OnGpuRegister(VAddr addr, u64 size) override {
        Add('R', addr, size);
        next->OnGpuRegister(addr, size);
    }
    void OnGpuMap(VAddr addr, u64 size) override {
        Add('M', addr, size);
        next->OnGpuMap(addr, size);
    }
    void OnGuestWrite(VAddr addr, u64 size) override {
        Add('W', addr, size);
        next->OnGuestWrite(addr, size);
    }
    std::string Take() {
        std::scoped_lock lock{mutex_};
        return std::exchange(text_, {});
    }

    Core::GpuMemoryListener* next = nullptr;
    std::atomic<bool> recording{true};
    std::atomic<u64> events{0};

private:
    void Add(char kind, VAddr addr, u64 size) {
        ++events;
        if (!recording) {
            return;
        }
        std::scoped_lock lock{mutex_};
        text_ += fmt::format("{}{:#x}+{:#x} ", kind, addr, size);
    }

    std::mutex mutex_;
    std::string text_;
};

// ---------------------------------------------------------------------------- NativeStart

void TestStart(TestGroup& g, const App::Config& cfg, const NativeGame& game, Recorder& rec) {
    auto& v = Core::GetGuestVirtualMemory();
    auto& p = Core::GetGuestPhysicalMemory();
    auto& n = Core::GetGuestWriteNotifier();
    auto& gpu = Core::GetGpuMemory();
    g.Check("--native memory selected SecondSonPC's manager for all four interfaces before anything reached them; the "
            "bridge's manager is not the guest memory and nothing else can be selected now",
            game.vm != nullptr && &v == static_cast<Core::GuestVirtualMemory*>(game.guest) &&
                &p == static_cast<Core::GuestPhysicalMemory*>(game.guest) &&
                &n == static_cast<Core::GuestWriteNotifier*>(game.guest) && &gpu == game.gpu &&
                !Core::BridgeGuestMemorySelected() &&
                !Core::SelectGuestMemory({game.guest, game.guest, game.guest, game.gpu}),
            "");
    if (game.vm == nullptr) {
        return;
    }
    const auto& layout = game.vm->GetLayout();
    const std::string inv = game.vm->CheckInvariants();
    g.Check("the reservation: every range of the layout taken from the host (placeholders and mappings agree with "
            "VirtualQuery region by region), the areas, the backing table and the physical maps agree",
            inv.empty(), inv);
    u64 total = 0;
    bool inside = !layout.ranges.empty();
    for (const auto& r : layout.ranges) {
        total += r.size;
        inside = inside && r.base >= 0x400000 && r.End() <= (1ull << 40) && r.size > 0x1000000;
    }
    g.Check("the game's layout as the bridge computes it: free host regions above 16 MiB in [0x400000, 1 TiB) (this "
            "Windows build), searches from 0x200000000, system reserved [0x7FFFFC000, 0x1000000000), GPU 40 bits, "
            "direct 7936 MiB and flexible 512 MiB in one section (+ [Console] extras)",
            inside && layout.lowest == layout.ranges.front().base && layout.search_base == 0x200000000ull &&
                layout.system_reserved_base == 0x7FFFFC000ull && layout.system_reserved_end == 0x1000000000ull &&
                layout.gpu.base == 0 && layout.gpu.size == (1ull << 40) &&
                layout.direct_size == (7936 + u64{cfg.extra_dmem_mib} + cfg.extra_fmem_mib) * kMiB &&
                layout.flexible_size == (512 + u64{cfg.extra_fmem_mib}) * kMiB &&
                game.vm->Host().BackingSize() == layout.flexible_base + layout.flexible_size,
            fmt::format("{} ranges, {:#x} bytes, lowest {:#x}, section {:#x}", layout.ranges.size(), total,
                        layout.lowest, game.vm->Host().BackingSize()));
    g.Check("the range where the game maps its first direct memory, [0x200000000, +0x48000000), is the manager's",
            game.vm->IsValid(0x200000000ull, 0x48000000ull), "");
    g.Check("the rasterizer listens to the manager (it registered itself when the bridge's GnmDriver started)",
            rec.next != nullptr, "");
    const bool on_demand = cfg.memory_commit_on_demand;
    g.Check(fmt::format("commit {} as asked", on_demand ? "on demand" : "upfront"),
            game.vm->Host().Commit() == (on_demand ? Memory::CommitMode::OnDemand : Memory::CommitMode::Upfront), "");
    g.Info(fmt::format("layout: {} ranges, {:#x} bytes from {:#x} to {:#x}; section {} MiB, commit {}; system commit "
                       "{} MiB used, {} MiB free",
                       layout.ranges.size(), total, layout.ranges.front().base, layout.ranges.back().End(),
                       game.vm->Host().BackingSize() >> 20, on_demand ? "on demand" : "upfront",
                       SystemCommit().first >> 20, SystemCommit().second >> 20));
}

// ---------------------------------------------------------------------------- NativeStart(game)

void TestGame(TestGroup& g, const App::Config& cfg, const NativeGame& game, Recorder& rec) {
    if (cfg.memory_commit_on_demand && SystemCommit().second < 6144 * kMiB) {
        g.Check("free system commit >= 6 GiB for the game's 4608 MiB of direct memory on demand", false,
                fmt::format("{} MiB", SystemCommit().second >> 20));
        return;
    }
    const auto loader = CreateLoaderHost();
    std::string bad;
    const auto Expect = [&bad](const std::string& what, bool ok) {
        if (!ok) {
            bad += what + "; ";
        }
    };
    // The loader's budgets from the game's SceKernelMemParam (flexible 0x20000000, no extended memory).
    loader->SetupMemoryRegions(0x20000000, false, false);
    const u64 direct_size = K::sceKernelGetDirectMemorySize();
    const Memory::Budget budget{.neo = false,
                                .dev_kit = cfg.dev_kit,
                                .extra_dmem_mib = cfg.extra_dmem_mib,
                                .extra_fmem_mib = cfg.extra_fmem_mib,
                                .flexible_size = 0x20000000,
                                .extended1 = false,
                                .extended2 = false};
    const bool console_default = !cfg.neo_mode && !cfg.dev_kit && cfg.extra_dmem_mib == 0 && cfg.extra_fmem_mib == 0;
    u64 configured = 0;
    K::sceKernelConfiguredFlexibleMemorySize(&configured);
    Expect(fmt::format("budgets: direct {:#x} (the bridge's 0x120000000 for this game), flexible {:#x} (0x1C000000)",
                       direct_size, configured),
           !console_default || (direct_size == 0x120000000ull && configured == 0x1C000000ull &&
                                direct_size == Memory::DirectMemorySize(budget)));

    rec.Take();
    struct Block {
        u64 len;
        s32 type;
        s32 prot;
        u64 phys;
        u64 addr;
    };
    // Scenario G: the game's three blocks and, between the first two, libSceGnmDriver's start-up (the loader's
    // InitGnmDriverMemory); the bridge gave these physical and virtual addresses.
    const Block blocks[] = {{0x48000000, 0, 0x03, 0x0, 0x200000000ull},
                            {0x20000000, 0, 0x33, 0x48010000, 0x248000000ull},
                            {0xb7ff0000, 3, 0x33, 0x68010000, 0x268000000ull}};
    std::string events;
    for (int i = 0; i < 3; ++i) {
        const Block& b = blocks[i];
        s64 phys = -1;
        const s32 a = K::sceKernelAllocateDirectMemory(0, static_cast<s64>(direct_size), b.len, 0x10000, b.type, &phys);
        void* addr = nullptr;
        const s32 m = a == 0 ? K::sceKernelMapDirectMemory(&addr, b.len, b.prot, 0, phys, 0x10000) : a;
        Expect(fmt::format("block {}: allocate {} -> {:#x} (expected {:#x}), map {} -> {} (expected {:#x})", i + 1,
                           Code(a), phys, b.phys, Code(m), addr, b.addr),
               a == 0 && m == 0 && static_cast<u64>(phys) == b.phys && reinterpret_cast<u64>(addr) == b.addr);
        events += fmt::format("block {}: {}\n", i + 1, rec.Take());
        if (i == 0) {
            Expect("GnmDriver start-up through the loader: 64 KiB of type 3 at 0x48000000, mapped at 0xfe0000000",
                   loader->InitGnmDriverMemory());
            Memory::QueryInfo q;
            Expect("GnmDriver's mapping", game.vm->Query(0xfe0000000ull, false, &q) == 0 && q.is_direct &&
                                              q.offset == 0x48000000 && q.memory_type == 3 && q.name == "SceGnmDriver");
            events += fmt::format("GnmDriver: {}\n", rec.Take());
        }
    }
    const std::string expected_events =
        "block 1: U0x200000000+0x48000000 R0x200000000+0x48000000 M0x200000000+0x48000000 \n"
        "GnmDriver: U0xfe0000000+0x10000 R0xfe0000000+0x10000 M0xfe0000000+0x10000 \n"
        "block 2: U0x248000000+0x20000000 R0x248000000+0x20000000 M0x248000000+0x20000000 \n"
        "block 3: U0x268000000+0xb7ff0000 R0x268000000+0xb7ff0000 M0x268000000+0xb7ff0000 \n";
    g.Check("the game's direct memory (scenario G) through the bridge's libkernel functions and the loader: the "
            "bridge's physical and virtual addresses, all of the direct memory used; each mapping notified to the "
            "rasterizer as the bridge does (GPU unmap, register, map)",
            bad.empty() && events == expected_events, bad + events);
    bad.clear();

    // What the game does with it: mprotect (its helper), queries, writes through the backing.
    Expect("Mprotect of the first page read-only", K::sceKernelMprotect(Ptr(0x200000000ull), 0x4000, 1) == 0);
    void* start = nullptr;
    void* end = nullptr;
    u32 prot = 0;
    Expect("QueryMemoryProtection after it",
           K::sceKernelQueryMemoryProtection(Ptr(0x200000000ull), &start, &end, &prot) == 0 &&
               start == Ptr(0x200000000ull) && end == Ptr(0x200004000ull) && prot == 1);
    Expect("the host keeps read/write for a read-only guest page (any read/write bit, as the bridge: H11)",
           HostProtect(0x200000000ull) == PAGE_READWRITE);
    Expect("Mprotect of the second page no-access", K::sceKernelMprotect(Ptr(0x200004000ull), 0x4000, 0) == 0 &&
                                                        HostProtect(0x200004000ull) == PAGE_NOACCESS);
    Expect("Mprotect back", K::sceKernelMprotect(Ptr(0x200000000ull), 0x8000, 3) == 0 &&
                                HostProtect(0x200004000ull) == PAGE_READWRITE);
    const std::string mprotect_events = rec.Take();
    Expect("Mprotect announces the host write to the GPU (W per call): " + mprotect_events,
           mprotect_events == "W0x200000000+0x4000 W0x200004000+0x4000 W0x200000000+0x8000 ");
    K::OrbisVirtualQueryInfo info{};
    Expect("VirtualQuery of block 2", K::sceKernelVirtualQuery(Ptr(0x248000000ull), 0, &info, sizeof(info)) == 0 &&
                                          info.start == 0x248000000ull && info.end == 0x268000000ull &&
                                          info.offset == 0x48010000 && info.is_direct == 1 && info.protection == 0x33);
    std::memset(Ptr(0x267fffff0ull), 0xA5, 32); // across blocks 2 and 3 (two physical areas)
    std::array<u8, 32> back{};
    Core::GetGpuMemory().CopySparseMemory(0x267fffff0ull, back.data(), back.size());
    Expect("a CPU write across two blocks reads back through the backing (H1/H2)",
           std::all_of(back.begin(), back.end(), [](u8 b) { return b == 0xA5; }));
    const std::array<u8, 16> data{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    Expect("TryWriteBacking across the blocks (D11)",
           Core::GetGpuMemory().TryWriteBacking(Ptr(0x267fffff8ull), data.data(), data.size()) &&
               std::memcmp(Ptr(0x267fffff8ull), data.data(), data.size()) == 0);
    const u64 clamped = Core::GetGpuMemory().ClampRangeSize(0x200000000ull, 8ull << 30);
    Expect(fmt::format("ClampRangeSize of 8 GiB from block 1: the three contiguous blocks ({:#x})", clamped),
           clamped == 0x48000000ull + 0x20000000ull + 0xb7ff0000ull &&
               Core::GetGpuMemory().ClampRangeSize(0x200000000ull, 4ull << 30) == (4ull << 30));

    // libc.prx's heaps: mmap/munmap and named flexible memory against the flexible budget.
    u64 available = 0;
    K::sceKernelAvailableFlexibleMemorySize(&available);
    void* heap = K::posix_mmap(nullptr, 0x100000, 3, kAnonPrivate, -1, 0);
    u64 after = 0;
    K::sceKernelAvailableFlexibleMemorySize(&after);
    const bool heap_ok = heap != Ptr(~0ull) && reinterpret_cast<u64>(heap) >= 0x200000000ull;
    if (heap_ok) {
        std::memset(heap, 0x3C, 0x100000);
    }
    Expect(fmt::format("mmap of 1 MiB of flexible memory ({}), the budget down by it", heap),
           heap_ok && available - after == 0x100000);
    void* named = nullptr;
    Expect("MapNamedFlexibleMemory", K::sceKernelMapNamedFlexibleMemory(&named, 0x10000, 3, 0, "libc heap") == 0);
    const bool unmapped = (!heap_ok || K::sceKernelMunmap(heap, 0x100000) == 0) &&
                          (named == nullptr || K::sceKernelMunmap(named, 0x10000) == 0);
    K::sceKernelAvailableFlexibleMemorySize(&after);
    Expect("munmap of both, the budget back", unmapped && after == available);
    void* tls = loader->MapPrimaryTls(0x8000);
    Expect(fmt::format("the primary thread's TLS through the loader: flexible memory from 0x881000000 ({})", tls),
           tls != nullptr && reinterpret_cast<u64>(tls) >= 0x881000000ull);
    if (tls != nullptr) {
        K::sceKernelMunmap(tls, 0x8000);
    }
    rec.Take();

    // The game's release at its end: the views go first (the GPU told), then the direct memory is free.
    Expect("ReleaseDirectMemory of all of it (the game's blocks and GnmDriver's)",
           K::sceKernelReleaseDirectMemory(0, direct_size) == 0);
    const std::string release_events = rec.Take();
    K::OrbisQueryInfo q{};
    u64 phys = 0;
    u64 size = 0;
    Expect("nothing allocated or mapped afterwards",
           K::sceKernelDirectMemoryQuery(0, 1, &q, sizeof(q)) != 0 &&
               K::sceKernelAvailableDirectMemorySize(0, direct_size, 0, &phys, &size) == 0 && size == direct_size &&
               K::sceKernelVirtualQuery(Ptr(0x200000000ull), 0, &info, sizeof(info)) != 0);
    // One GPU unmap per physical piece of each view (as the bridge: NativeMem(diff) compares this), all of them
    // before the first register, together covering the three blocks and GnmDriver's 64 KiB.
    u64 unmapped_bytes = 0;
    bool unmap_first = true;
    bool seen_register = false;
    for (size_t at = 0; at < release_events.size();) {
        const size_t next_space = release_events.find(' ', at);
        const std::string token = release_events.substr(at, next_space - at);
        at = next_space == std::string::npos ? release_events.size() : next_space + 1;
        if (token.empty()) {
            continue;
        }
        if (token[0] == 'R') {
            seen_register = true;
        } else if (token[0] == 'U') {
            unmap_first = unmap_first && !seen_register;
            unmapped_bytes += std::stoull(token.substr(token.find('+') + 1), nullptr, 16);
        }
    }
    Expect(fmt::format("the release told the GPU about every view before any of it went ({})", release_events),
           unmap_first && seen_register && unmapped_bytes == 0x48000000ull + 0x20000000ull + 0xb7ff0000ull + 0x10000);
    const std::string inv = game.vm->CheckInvariants();
    Expect("invariants: " + inv, inv.empty());
    g.Check("what the game does with its memory: mprotect and the protection query, VirtualQuery, writes across the "
            "blocks through the CPU and the backing, the GPU's clamp, libc's flexible heaps against the budget, the "
            "primary TLS, the release at the end",
            bad.empty(), bad);
}

// ---------------------------------------------------------------------------- NativeStart(prot)

void TestProtection(TestGroup& g, const NativeGame& game) {
    // A direct view somewhere free; the guest takes a page away, the GPU's tracking (the call the access violation
    // handler makes) protects another; unmapping the middle keeps both (D19).
    s64 phys = -1;
    const u64 direct_size = K::sceKernelGetDirectMemorySize();
    void* addr = nullptr;
    const bool mapped =
        K::sceKernelAllocateDirectMemory(0, static_cast<s64>(direct_size), 0x40000, 0x10000, 0, &phys) == 0 &&
        K::sceKernelMapDirectMemory(&addr, 0x40000, 0x33, 0, phys, 0x10000) == 0;
    const u64 w = reinterpret_cast<u64>(addr);
    auto& gpu = Core::GetGpuMemory();
    if (!mapped) {
        g.Check("a direct view to protect", false, "");
        return;
    }
    K::sceKernelMprotect(Ptr(w), 0x4000, 0);
    gpu.ProtectTracking(w + 0x3C000, 0x4000, Core::MemoryPermission::Read);
    const bool before = HostProtect(w) == PAGE_NOACCESS && HostProtect(w + 0x3C000) == PAGE_READONLY;
    const s32 cut = K::sceKernelMunmap(Ptr(w + 0x10000), 0x20000);
    const DWORD first = HostProtect(w);
    const DWORD last = HostProtect(w + 0x3C000);
    const DWORD tail = HostProtect(w + 0x30000);
    g.Check("D19 in the game's layout: after munmap of a view's middle the guest's no-access page and the GPU's "
            "read-only tracking page keep their host protection (the bridge would make both read/write)",
            before && cut == 0 && first == PAGE_NOACCESS && last == PAGE_READONLY && tail == PAGE_READWRITE,
            fmt::format("{} {:#x} {:#x} {:#x} {:#x}", before, static_cast<u32>(cut), first, last, tail));

    // The handler's call: ProtectTracking none / read-write on a page, many times (no allocation, one lock).
    std::vector<double> ns;
    ns.reserve(20000);
    for (int i = 0; i < 20000; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        gpu.ProtectTracking(w + 0x30000, 0x4000,
                            i % 2 == 0 ? Core::MemoryPermission::None : Core::MemoryPermission::ReadWrite);
        ns.push_back(std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(ns.begin(), ns.end());
    g.Check("ProtectTracking (the access violation handler's call) leaves the page as asked",
            HostProtect(w + 0x30000) == PAGE_READWRITE, "");
    g.Info(fmt::format("ProtectTracking through Core::GetGpuMemory() on the game's manager, 20000 calls: P50 {:.0f} ns, "
                       "P95 {:.0f} ns, P99 {:.0f} ns (P2.3.3 on a test window: bridge 496, native 493)",
                       ns[ns.size() / 2], ns[ns.size() * 95 / 100], ns[ns.size() * 99 / 100]));
    gpu.ProtectTracking(w + 0x3C000, 0x4000, Core::MemoryPermission::ReadWrite);
    K::sceKernelMunmap(Ptr(w), 0x40000);
    K::sceKernelReleaseDirectMemory(static_cast<u64>(phys), 0x40000);
    const std::string inv = game.vm->CheckInvariants();
    g.Check("invariants after", inv.empty(), inv);
}

// ---------------------------------------------------------------------------- NativeStart(mt)

void TestThreads(TestGroup& g, const NativeGame& game, Recorder& rec) {
    rec.recording = false;
    const u64 events_before = rec.events.load();
    u64 available = 0;
    K::sceKernelAvailableFlexibleMemorySize(&available);
    const u64 direct_size = K::sceKernelGetDirectMemorySize();
    // A stable view the GPU reader works on.
    s64 stable_phys = -1;
    void* stable = nullptr;
    const bool stable_ok =
        K::sceKernelAllocateDirectMemory(0, static_cast<s64>(direct_size), 0x100000, 0x10000, 0, &stable_phys) == 0 &&
        K::sceKernelMapDirectMemory(&stable, 0x100000, 0x33, 0, stable_phys, 0x10000) == 0;
    if (!stable_ok) {
        g.Check("a direct view for the GPU reader", false, "");
        rec.recording = true;
        return;
    }
    std::atomic<bool> stop{false};
    std::atomic<bool> done{false};
    std::atomic<int> errors{0};
    std::atomic<u64> operations{0};
    std::thread watchdog([&] {
        for (int i = 0; i < 900 && !done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (!done) {
            SSPC_LOG(App, Critical, "NativeStart(mt): no progress for 90 s (deadlock?), stopping");
            Log::Flush();
            std::quick_exit(5);
        }
    });
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) { // libc's heaps: flexible memory
        threads.emplace_back([&, t] {
            for (int i = 0; i < 300; ++i) {
                const u64 len = 0x10000ull * (1 + (i + t) % 4);
                void* p = K::posix_mmap(nullptr, len, 3, kAnonPrivate, -1, 0);
                if (p == Ptr(~0ull)) {
                    ++errors;
                    continue;
                }
                static_cast<u8*>(p)[0] = static_cast<u8>(i);
                errors += K::sceKernelMprotect(p, 0x4000, (i & 1) != 0 ? 1 : 3) == 0 ? 0 : 1;
                K::OrbisVirtualQueryInfo info{};
                errors += K::sceKernelVirtualQuery(p, 0, &info, sizeof(info)) == 0 && info.is_flexible == 1 ? 0 : 1;
                errors += K::sceKernelMunmap(p, len) == 0 ? 0 : 1;
                ++operations;
            }
        });
    }
    for (int t = 0; t < 2; ++t) { // direct memory: allocate, map, protect, release with its view
        threads.emplace_back([&] {
            for (int i = 0; i < 200; ++i) {
                s64 phys = -1;
                if (K::sceKernelAllocateDirectMemory(0, static_cast<s64>(direct_size), 0x20000, 0x10000,
                                                     i % 2 == 0 ? 0 : 3, &phys) != 0) {
                    ++errors;
                    continue;
                }
                void* addr = nullptr;
                errors += K::sceKernelMapDirectMemory(&addr, 0x20000, 0x33, 0, phys, 0x10000) == 0 ? 0 : 1;
                errors += K::sceKernelMprotect(addr, 0x4000, 0x13) == 0 ? 0 : 1;
                errors += K::sceKernelReleaseDirectMemory(static_cast<u64>(phys), 0x20000) == 0 ? 0 : 1;
                ++operations;
            }
        });
    }
    std::thread reader([&] { // the GPU's side meanwhile
        auto& gpu = Core::GetGpuMemory();
        std::array<u8, 256> buffer{};
        const u64 base = reinterpret_cast<u64>(stable);
        while (!stop) {
            gpu.CopySparseMemory(base + 0x1000, buffer.data(), buffer.size());
            (void)gpu.ClampRangeSize(base, 2ull << 30);
            gpu.ProtectTracking(base + 0x80000, 0x4000, Core::MemoryPermission::Read);
            gpu.ProtectTracking(base + 0x80000, 0x4000, Core::MemoryPermission::ReadWrite);
            (void)gpu.TryWriteBacking(Ptr(base + 0x2000), buffer.data(), 64);
            ++operations;
        }
    });
    for (auto& t : threads) {
        t.join();
    }
    stop = true;
    reader.join();
    done = true;
    watchdog.join();
    K::sceKernelMunmap(stable, 0x100000);
    K::sceKernelReleaseDirectMemory(static_cast<u64>(stable_phys), 0x100000);
    u64 after = 0;
    K::sceKernelAvailableFlexibleMemorySize(&after);
    u64 free_phys = 0;
    u64 free_size = 0;
    K::sceKernelAvailableDirectMemorySize(0, direct_size, 0, &free_phys, &free_size);
    const std::string inv = game.vm->CheckInvariants();
    g.Check("4 threads on libc-style flexible heaps (mmap, mprotect, VirtualQuery, munmap) and 2 on direct memory "
            "(allocate, map, mprotect, release with its views) through the bridge's libkernel functions, a GPU reader "
            "(copy, clamp, tracking protection, backing writes) meanwhile: no error, no deadlock (90 s watchdog), "
            "the flexible budget and the direct memory back to before, invariants hold",
            errors == 0 && after == available && free_size == direct_size && inv.empty(),
            fmt::format("errors {}, flexible {:#x} -> {:#x}, free direct {:#x} of {:#x}; {}", errors.load(), available,
                        after, free_size, direct_size, inv));
    g.Info(fmt::format("NativeStart(mt): {} operations, {} GPU notifications to the rasterizer", operations.load(),
                       rec.events.load() - events_before));
    rec.recording = true;
}

// ---------------------------------------------------------------------------- NativeStart(none)

void TestUnsupported(TestGroup& g) {
    // Pools and file mappings: no user in this game (docs/PHASE2_P23_MEMORY.md §2); refused with a critical log line,
    // nothing mapped.
    void* pool = nullptr;
    const s32 reserve = K::sceKernelMemoryPoolReserve(nullptr, 0x200000, 0, 0, &pool);
    u64 phys = 0;
    const s32 expand = K::sceKernelMemoryPoolExpand(0, K::sceKernelGetDirectMemorySize(), 0x10000, 0, &phys);
    void* file = K::posix_mmap(nullptr, 0x10000, 1, 0, 3, 0);
    // The arguments pass libkernel's checks (2 MiB aligned reservation, 64 KiB expansion in range, a file mmap of
    // 64 KiB), so the codes are the manager's: EINVAL for the pool reservation (Map refuses pool types), ENOMEM for
    // the expansion (libkernel's answer to PoolExpand's -1), and mmap's -1 from MapFile's EINVAL.
    g.Check("memory pools (reserve, expand) and file mappings reach the manager and are refused with an error, not "
            "answered silently",
            reserve == Memory::kErrEinval && expand == Memory::kErrEnomem && file == Ptr(~0ull),
            fmt::format("{} {} {}", Code(reserve), Code(expand), file));
}

} // namespace

int RunNativeMemorySelfTest(const App::Config& cfg) {
    const NativeGame game = GetNativeGame();
    Recorder rec;
    std::vector<TestGroup> groups;
    const auto Run = [&](const char* name, auto&& test) {
        TestGroup g;
        g.name = name;
        test(g);
        groups.push_back(std::move(g));
    };
    if (game.vm != nullptr) {
        rec.next = game.gpu->SetListener(&rec);
    }
    Run("NativeStart", [&](TestGroup& g) { TestStart(g, cfg, game, rec); });
    if (game.vm != nullptr && rec.next != nullptr) {
        Run("NativeStart(game)", [&](TestGroup& g) { TestGame(g, cfg, game, rec); });
        Run("NativeStart(prot)", [&](TestGroup& g) { TestProtection(g, game); });
        Run("NativeStart(mt)", [&](TestGroup& g) { TestThreads(g, game, rec); });
        Run("NativeStart(none)", [&](TestGroup& g) { TestUnsupported(g); });
    }
    if (game.vm != nullptr) {
        game.gpu->SetListener(rec.next);
    }
    int checks = 0;
    int failures = 0;
    std::string text = fmt::format("SecondSonPC self-test --native memory (commit {}): SecondSonPC's memory manager "
                                   "started as for the game\n",
                                   cfg.memory_commit_on_demand ? "on demand" : "upfront");
    for (const auto& g : groups) {
        for (const auto& line : g.lines) {
            text += line + "\n";
        }
        text += fmt::format("{:18} {:4} checks, {} different - {}\n", g.name, g.checks, g.failures,
                            g.failures == 0 ? "PASS" : "FAIL");
        checks += g.checks;
        failures += g.failures;
    }
    text += fmt::format("TOTAL {} checks, {} failures - {}\n", checks, failures, failures == 0 ? "PASS" : "FAIL");
    const auto path = cfg.logs_dir / "selftest_native_memory.txt";
    std::ofstream(path, std::ios::binary) << text;
    std::fwrite(text.data(), 1, text.size(), stdout);
    SSPC_LOG(App, Info, "native memory self-test: {} checks, {} failures, report {}", checks, failures, path.string());
    Log::Flush();
    return failures == 0 && checks > 0 ? 0 : 1;
}

} // namespace SecondSon::Bridge
