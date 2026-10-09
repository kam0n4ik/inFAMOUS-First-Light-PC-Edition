// SecondSonPC — self-test of SecondSonPC's own virtual memory manager (Phase 2, P2.3.3b; docs/PHASE2_P233_NATIVE_VMM.md).
// Every manager here lives on its own address window above 1 TiB, found free on the host first (nothing is
// assumed free); the bridge's memory and the game's addresses are not touched. Groups:
//   NativeVmm         operations one by one against the host's state (VirtualQuery), H1/H2/H3, aliases, a view
//                     unmapped in the middle, a release with views, the stack of D17;
//   NativeVmm(D)      the bridge's defects fixed (D5, D6, D10, D11, D12, D16) and the parameter checks (alignment,
//                     overflow, outside the space);
//   NativeVmm(model)  thousands of random operations against an independent page model (areas, physical pages,
//                     host pages and protection, contents), the manager's own invariants after each;
//   NativeVmm(mt)     threads mapping, protecting, unmapping, reading and writing at once, with a watchdog;
//   NativeVmm(game)   the game's layout computed like the bridge (against the bridge's regions), its budgets;
//   NativeVmm(commit) experiment E1: what the system commit does with a SEC_RESERVE section, on demand.
#include "bridge/shadps4/selftest_vmm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <psapi.h>

#include <fmt/format.h>

#include "base/log.h"
#include "core/memory.h"
#include "runtime/memory/virtual_memory.h"

namespace SecondSon::Bridge {

void TestGroup::Check(const std::string& what, bool ok, const std::string& detail) {
    ++checks;
    if (!ok) {
        ++failures;
        lines.push_back(fmt::format("  DIFF  {}: {}: {}", name, what, detail));
    }
}

void TestGroup::Info(const std::string& text) {
    lines.push_back(fmt::format("  INFO  {}: {}", name, text));
}

namespace {

using Memory::AreaInfo;
using Memory::CommitMode;
using Memory::HostAccess;
using Memory::kNoPhys;
using Memory::Layout;
using Memory::VirtualMemory;
using Memory::VmaType;
namespace Prot = Memory::Prot;
namespace MapFlag = Memory::MapFlag;

constexpr u64 kPage = Memory::kPageSize;
constexpr u64 kMiB = 1ull << 20;
constexpr s32 kOk = Memory::kOk;
constexpr s32 kEinval = Memory::kErrEinval;
constexpr s32 kEnomem = Memory::kErrEnomem;
constexpr s32 kEbusy = Memory::kErrEbusy;
constexpr s32 kEacces = Memory::kErrEacces;

/// A free stretch of the host's address space at or above 2 TiB (nothing of the bridge or the game is there).
u64 FindFreeWindow(u64 size, u64 from = 0x20000000000ull) {
    for (u64 base = from; base + size < 0x7F0000000000ull; base += 0x4000000000ull) {
        bool free = true;
        for (u64 addr = base; addr < base + size && free;) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi)) == 0) {
                free = false;
                break;
            }
            free = mbi.State == MEM_FREE;
            addr = reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize;
        }
        if (free) {
            return base;
        }
    }
    return 0;
}

Layout WindowLayout(u64 base, u64 size, u64 direct, u64 flexible, CommitMode mode, u32 sdk) {
    Layout l;
    l.ranges = {{base, size}};
    l.lowest = base;
    l.search_base = base;
    l.search_limit = base + size;
    l.system_reserved_base = base;
    l.system_reserved_end = base + size;
    l.gpu = {base, size};
    l.direct_size = direct;
    l.flexible_base = direct;
    l.flexible_size = flexible;
    l.commit = mode;
    l.sdk_version = sdk;
    return l;
}

std::unique_ptr<VirtualMemory> MakeVm(TestGroup& g, u64 window, u64 direct, u64 flexible, CommitMode mode, u32 sdk,
                                      u64* base_out) {
    const u64 base = FindFreeWindow(window);
    std::string error = "no free window";
    std::unique_ptr<VirtualMemory> vm;
    if (base != 0) {
        vm = VirtualMemory::Create(WindowLayout(base, window, direct, flexible, mode, sdk), &error);
    }
    if (vm == nullptr) {
        g.Check("a free window above 1 TiB and a manager on it", false, fmt::format("{:#x}: {}", base, error));
    }
    *base_out = base;
    return vm;
}

struct HostPage {
    DWORD state = 0;
    DWORD type = 0;
    DWORD protect = 0;
};

HostPage Host(u64 addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<void*>(addr), &mbi, sizeof(mbi));
    return {mbi.State, mbi.Type, mbi.Protect};
}

DWORD Win(HostAccess a) {
    switch (a) {
    case HostAccess::None:
        return PAGE_NOACCESS;
    case HostAccess::Read:
        return PAGE_READONLY;
    case HostAccess::ReadWrite:
        return PAGE_READWRITE;
    case HostAccess::Execute:
        return PAGE_EXECUTE;
    case HostAccess::ExecuteRead:
        return PAGE_EXECUTE_READ;
    case HostAccess::ExecuteReadWrite:
        return PAGE_EXECUTE_READWRITE;
    }
    return 0;
}

/// The bridge's host protection for a guest protection (ProtectBytes; see virtual_memory.cpp).
HostAccess GuestToHost(u32 prot) {
    const bool rw = (prot & (Prot::kCpuReadWrite | Prot::kGpuReadWrite)) != 0;
    const bool exec = (prot & Prot::kCpuExec) != 0;
    return rw ? (exec ? HostAccess::ExecuteReadWrite : HostAccess::ReadWrite)
              : (exec ? HostAccess::Execute : HostAccess::None);
}

s32 Map(VirtualMemory& vm, u64* out, u64 addr, u64 size, u32 prot, u32 flags, VmaType type,
        std::string_view name = "anon", u64 phys = kNoPhys, bool exclusive = false, u64 alignment = 0) {
    return vm.Map({.addr = addr,
                   .size = size,
                   .prot = prot,
                   .flags = flags,
                   .type = type,
                   .name = name,
                   .phys = phys,
                   .exclusive = exclusive,
                   .alignment = alignment},
                  out);
}

std::string AreasText(const VirtualMemory& vm, u64 w) {
    std::string out;
    for (const auto& a : vm.Areas(false)) {
        out += fmt::format("[{:#x}+{:#x} t{} p{:#x} {:#x}/{} \"{}\"] ", a.base - w, a.size, static_cast<int>(a.type),
                           a.prot, a.phys, a.phys_slices, a.name);
    }
    return out;
}

/// Collects the first failed invariant check of a sequence of steps.
struct Invariants {
    std::string first;
    void After(const VirtualMemory& vm, std::string_view step) {
        if (!first.empty()) {
            return;
        }
        const std::string e = vm.CheckInvariants();
        if (!e.empty()) {
            first = fmt::format("after {}: {}", step, e);
        }
    }
};

// ---------------------------------------------------------------------------- NativeVmm: operations

void TestBasics(TestGroup& g, u32 sdk) {
    u64 w = 0;
    auto vm = MakeVm(g, 64 * kMiB, 16 * kMiB, 4 * kMiB, CommitMode::Upfront, sdk, &w);
    if (vm == nullptr) {
        return;
    }
    Invariants inv;
    inv.After(*vm, "creation");
    {
        std::string error;
        auto twin = VirtualMemory::Create(WindowLayout(w, 64 * kMiB, 16 * kMiB, 4 * kMiB, CommitMode::Upfront, sdk),
                                          &error);
        g.Check("a second manager on the same window is refused (the range is not free on the host)",
                twin == nullptr && error.find("not free") != std::string::npos, error);
    }
    g.Check("a new manager: one free area, one placeholder over the window",
            vm->Areas(true).size() == 1 && vm->Host().Snapshot().size() == 1 && Host(w).state == MEM_RESERVE, "");

    // The loader's pattern: a module reserved, its trampoline, a game segment (flexible), a system one (code).
    u64 out = 0;
    const s32 reserve = Map(*vm, &out, w, 0x40000, Prot::kNone, 0, VmaType::Reserved, "module");
    const bool reserved_ok = reserve == kOk && out == w && Host(w).state == MEM_RESERVE;
    inv.After(*vm, "reserve");
    const s32 tramp = Map(*vm, &out, w + 0x30000, 0x10000, Prot::kCpuReadWrite | Prot::kCpuExec, MapFlag::kFixed,
                          VmaType::Code, "module");
    const HostPage tramp_host = Host(w + 0x30000);
    inv.After(*vm, "trampoline");
    const s32 flex = Map(*vm, &out, w, 0x20000, Prot::kCpuReadWrite, MapFlag::kFixed, VmaType::Flexible, "module");
    const HostPage flex_host = Host(w);
    inv.After(*vm, "flexible segment");
    const s32 code = Map(*vm, &out, w + 0x20000, 0x10000, Prot::kCpuRead | Prot::kCpuExec, MapFlag::kFixed,
                         VmaType::Code, "module");
    const HostPage code_host = Host(w + 0x20000);
    inv.After(*vm, "code segment");
    g.Check("module: reserve (a placeholder), trampoline (private RWX), game segment (a view of the section, "
            "flexible), system segment (private; the host keeps read/write/execute as in the bridge)",
            reserved_ok && tramp == kOk && tramp_host.type == MEM_PRIVATE &&
                tramp_host.protect == PAGE_EXECUTE_READWRITE && flex == kOk && flex_host.type == MEM_MAPPED &&
                flex_host.protect == PAGE_READWRITE && code == kOk && code_host.type == MEM_PRIVATE &&
                code_host.protect == PAGE_EXECUTE_READWRITE && vm->FlexibleUsed() == 0x20000 &&
                vm->PhysicalAddress(w) == 16 * kMiB,
            AreasText(*vm, w));

    // Direct memory: H1 (guest address = host address) and H2 (the section's permanent view).
    u64 phys = 0;
    const s32 alloc = vm->AllocateDirect(0, vm->DirectSize(), 0x20000, 0, 0, &phys);
    u64 v1 = 0;
    const s32 m1 = Map(*vm, &v1, w + 0x100000, 0x20000, 0x33, 0, VmaType::Direct, "anon", phys);
    if (alloc != kOk || m1 != kOk) {
        g.Check("direct memory allocated and mapped", false,
                fmt::format("{:#x} {:#x}", static_cast<u32>(alloc), static_cast<u32>(m1)));
        return;
    }
    for (u64 o = 0; o < 0x20000; o += 8) {
        *reinterpret_cast<u64*>(v1 + o) = 0xD1D1000000000000ull | o;
    }
    std::array<u8, 64> buf{};
    vm->CopySparse(v1 + 0x1000, buf.data(), buf.size());
    const bool h1 = phys == 0 &&
                    *reinterpret_cast<const u64*>(vm->BackingBase() + phys + 0x1000) == (0xD1D1000000000000ull | 0x1000) &&
                    std::memcmp(buf.data(), reinterpret_cast<const void*>(v1 + 0x1000), buf.size()) == 0;
    g.Check("H1/H2: what the guest writes at its address is in the section at the physical address, and "
            "CopySparse (lock-free, through the section) reads it",
            h1, fmt::format("phys {:#x} view {:#x}", phys, v1));
    inv.After(*vm, "direct view");

    u64 v2 = 0;
    const s32 alias = Map(*vm, &v2, w + 0x200000, 0x20000, 0x33, 0, VmaType::Direct, "anon", phys);
    *reinterpret_cast<u64*>(v2 + 0x40) = 0xA11A5ull;
    u64 busy_out = 0;
    const s32 busy = Map(*vm, &busy_out, w + 0x300000, 0x4000, 0x33, 0, VmaType::Direct, "anon", phys, true);
    g.Check("aliases: a second view of the same physical memory shares its data; an exclusive map of it is EBUSY "
            "and leaves nothing behind",
            alias == kOk && v2 != v1 && *reinterpret_cast<const u64*>(v1 + 0x40) == 0xA11A5ull && busy == kEbusy &&
                vm->Direct().Snapshot().front().mappings == 2,
            fmt::format("{:#x} {:#x} {:#x}", static_cast<u32>(alias), v2, static_cast<u32>(busy)));
    inv.After(*vm, "alias");

    Memory::QueryInfo qi;
    const s32 q1 = vm->Query(v1, false, &qi);
    const bool q_direct = q1 == kOk && qi.start == v1 && qi.end == v1 + 0x20000 && qi.is_direct && qi.offset == phys &&
                          qi.protection == 0x33 && qi.is_committed;
    const s32 q2 = vm->Query(w + 0x80000, false, &qi); // free
    const s32 q3 = vm->Query(w + 0x80000, true, &qi);  // the next area in use
    const bool q_next = q2 == kEacces && q3 == kOk && qi.start == v1;
    u64 qs = 0, qe = 0;
    u32 qp = 0;
    const s32 qprot = vm->QueryProtection(v1 + 0x4000, &qs, &qe, &qp);
    const bool qprot_ok = qprot == kOk && qs == v1 && qe == v1 + 0x20000 && qp == 0x33;
    const s32 qfree = vm->QueryProtection(w + 0x80000, &qs, &qe, &qp);
    g.Check("Query: direct view (offset = physical address, committed), a free address EACCES, find_next skips "
            "free areas; QueryProtection",
            q_direct && q_next && qprot_ok && qfree == kEacces, fmt::format("{} {} {} {}", q1, q2, q3, qprot));

    // Protection: the guest's and the host's (the bridge's rule: any read/write bit = host read/write).
    const s32 p1 = vm->Protect(v1, 0x4000, Prot::kCpuRead);
    const HostPage after_read = Host(v1);
    const size_t areas_read = vm->Areas(false).size();
    const s32 p2 = vm->Protect(v1, 0x4000, Prot::kNone);
    const HostPage after_none = Host(v1);
    const s32 p3 = vm->Protect(v1, 0x4000, 0x33);
    inv.After(*vm, "protect");
    g.Check("Protect: CPU read only -> the area says 1, the host stays read/write (H11, as the bridge); no access "
            "-> PAGE_NOACCESS; back to 0x33 -> read/write and one area again",
            p1 == kOk && after_read.protect == PAGE_READWRITE && p2 == kOk && after_none.protect == PAGE_NOACCESS &&
                p3 == kOk && Host(v1).protect == PAGE_READWRITE && areas_read == vm->Areas(false).size() + 1,
            fmt::format("{:#x} {:#x} {}", after_read.protect, after_none.protect, AreasText(*vm, w)));

    // H2: writes past the host protection; H3: anonymous memory has no backing.
    vm->ProtectTracking(v1, 0x4000, HostAccess::None);
    const u64 secret = 0x5EC12E7ull;
    const bool written = vm->TryWriteBacking(v1 + 0x100, &secret, 8);
    const DWORD tracked = Host(v1).protect;
    vm->ProtectTracking(v1, 0x4000, HostAccess::ReadWrite);
    *reinterpret_cast<u64*>(w + 0x30000) = 0x7777; // the trampoline (private)
    vm->CopySparse(w + 0x30000, buf.data(), 16);
    const bool zeros = std::all_of(buf.begin(), buf.begin() + 16, [](u8 b) { return b == 0; });
    const bool anon_write = vm->TryWriteBacking(w + 0x30000, &secret, 8);
    g.Check("H2: TryWriteBacking writes past a no-access tracking protection (it stays); H3: anonymous memory reads "
            "as zeros through the section and cannot be written there",
            written && tracked == PAGE_NOACCESS && *reinterpret_cast<const u64*>(v1 + 0x100) == secret && zeros &&
                !anon_write,
            fmt::format("{} {:#x} {} {}", written, tracked, zeros, anon_write));

    const size_t areas_before_name = vm->Areas(false).size();
    const s32 n1 = vm->NameRange(v1 + 0x4000, 0x8000, "named");
    const size_t named = vm->Areas(false).size();
    const s32 n2 = vm->NameRange(v1 + 0x4000, 0x8000, "anon");
    inv.After(*vm, "name");
    g.Check("NameRange splits the area in three; the old name back merges them again",
            n1 == kOk && n2 == kOk && named == areas_before_name + 2 && vm->Areas(false).size() == areas_before_name,
            fmt::format("{} {} {}", areas_before_name, named, vm->Areas(false).size()));

    // A view unmapped in the middle: the parts around are mapped again with their data.
    const u64 remaps_before = vm->GetStats().host.remaps;
    const s32 mid = vm->Unmap(v1 + 0x8000, 0x4000);
    inv.After(*vm, "unmap in the middle");
    const bool kept = *reinterpret_cast<const u64*>(v1 + 0x7FF8) == (0xD1D1000000000000ull | 0x7FF8) &&
                      *reinterpret_cast<const u64*>(v1 + 0xC000) == (0xD1D1000000000000ull | 0xC000);
    g.Check("a view unmapped in the middle: both sides keep their data (mapped again), the middle is a placeholder",
            mid == kOk && kept && Host(v1 + 0x8000).state == MEM_RESERVE && Host(v1).type == MEM_MAPPED &&
                Host(v1 + 0xC000).type == MEM_MAPPED && vm->GetStats().host.remaps == remaps_before + 2,
            fmt::format("{:#x} {} {}", static_cast<u32>(mid), kept, vm->GetStats().host.remaps));

    // Releasing the physical memory unmaps every view of it first.
    const s32 rel = vm->ReleaseDirect(phys, 0x20000, false);
    inv.After(*vm, "release with views");
    g.Check("ReleaseDirect with views: every view (both sides of the first, the alias) unmapped, then free",
            rel == kOk && vm->Direct().Pristine() && Host(v1).state == MEM_RESERVE && Host(v2).state == MEM_RESERVE &&
                vm->Areas(false).size() == 3,
            fmt::format("{:#x} {}", static_cast<u32>(rel), AreasText(*vm, w)));

    // The module goes (flexible + private + private), the flexible memory is cleared.
    *reinterpret_cast<u64*>(w) = 0xF1F1;
    const s32 um = vm->Unmap(w, 0x40000);
    inv.After(*vm, "unmap module");
    g.Check("unmapping the module: everything back to a placeholder, the flexible memory free and cleared",
            um == kOk && vm->Areas(false).empty() && vm->FlexibleUsed() == 0 &&
                *reinterpret_cast<const u64*>(vm->BackingBase() + 16 * kMiB) == 0 && Host(w).state == MEM_RESERVE,
            fmt::format("{:#x} {}", static_cast<u32>(um), AreasText(*vm, w)));

    // D17: a stack with its guard page and a name (three areas, one host allocation) unmaps whole; a part of it
    // is refused without any change.
    u64 stack = 0;
    Map(*vm, &stack, w + 0x400000, 0x24000, Prot::kCpuReadWrite, 0, VmaType::Stack);
    vm->Protect(stack, 0x4000, Prot::kNone);
    vm->NameRange(stack + 0x4000, 0x10000, "thread");
    const size_t stack_areas = vm->Areas(false).size();
    const s32 part = vm->Unmap(stack + 0x4000, 0x4000);
    const size_t after_part = vm->Areas(false).size();
    const s32 whole = vm->Unmap(stack, 0x24000);
    inv.After(*vm, "stack");
    g.Check("D17: a stack with a guard page and a name (3 areas, 1 private allocation) unmaps whole; unmapping part "
            "of it is EINVAL and changes nothing (the bridge asserts)",
            stack_areas == 3 && part == kEinval && after_part == 3 && whole == kOk && vm->Areas(false).empty() &&
                Host(stack).state == MEM_RESERVE,
            fmt::format("{} {:#x} {} {:#x}", stack_areas, static_cast<u32>(part), after_part, static_cast<u32>(whole)));

    const auto stats = vm->GetStats();
    g.Check("the invariants (areas, host regions, backing table, direct and flexible maps agree) after every step",
            inv.first.empty(), inv.first);
    g.Info(fmt::format("counters: maps {} unmaps {} protects {} failures {}; areas split {} merged {}; host views {} "
                       "private {} unmaps {} remaps {} splits {} coalesces {} protects {}",
                       stats.maps, stats.unmaps, stats.protects, stats.failures, stats.vma.splits, stats.vma.merges,
                       stats.host.views, stats.host.privates, stats.host.unmaps, stats.host.remaps, stats.host.splits,
                       stats.host.coalesces, stats.host.protects));
}

// ---------------------------------------------------------------------------- NativeVmm(D): defects and bounds

void TestDefects(TestGroup& g, u32 sdk) {
    u64 w = 0;
    constexpr u64 kWin = 64 * kMiB;
    auto vm = MakeVm(g, kWin, 16 * kMiB, 1 * kMiB, CommitMode::Upfront, sdk, &w);
    if (vm == nullptr) {
        return;
    }
    Invariants inv;
    const u64 end = w + kWin;
    g.Check("D10: the GPU range check does not wrap and takes a range ending exactly at the top",
            vm->IsGpuVisible(end - 16, 16) && !vm->IsGpuVisible(end - 16, 17) && !vm->IsGpuVisible(end, 0) &&
                vm->IsGpuVisible(w, 0) && !vm->IsGpuVisible(~0ull, 2) && !vm->IsGpuVisible(1ull << 63, 1ull << 63) &&
                !vm->IsGpuVisible(w - 1, 2),
            "");

    // D11: a write across two physical areas (two direct areas of different memory types, not merged).
    u64 pa = 0, pb = 0, va = 0, vb = 0;
    vm->AllocateDirect(0, vm->DirectSize(), 0x10000, 0, 0, &pa);
    vm->AllocateDirect(0, vm->DirectSize(), 0x10000, 0, 3, &pb);
    Map(*vm, &va, w, 0x10000, 0x33, MapFlag::kFixed, VmaType::Direct, "anon", pa);
    Map(*vm, &vb, w + 0x10000, 0x10000, 0x33, MapFlag::kFixed, VmaType::Direct, "anon", pb);
    std::array<u8, 16> data{};
    for (size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<u8>(0xA0 + i);
    }
    std::memset(reinterpret_cast<void*>(w), 0, 0x20000);
    const bool across = vm->TryWriteBacking(w + 0xFFF8, data.data(), 16);
    const bool d11 = across && std::memcmp(reinterpret_cast<const void*>(w + 0xFFF8), data.data(), 16) == 0 &&
                     *reinterpret_cast<const u8*>(w + 0x10008) == 0 && *reinterpret_cast<const u8*>(w + 0xFFF7) == 0 &&
                     vm->Areas(false).size() == 2;
    g.Check("D11: TryWriteBacking across two physical areas writes the continuation of the data into the second one "
            "(the bridge repeats the start); the bytes around are untouched",
            d11, fmt::format("{} {:#x} {:#x} {}", across, pa, pb, AreasText(*vm, w)));
    vm->Unmap(w, 0x20000);
    vm->ReleaseDirect(0, 0x20000, false);
    // The same inside one flexible area made of two physical pieces.
    u64 f1 = 0, f2 = 0, f3 = 0;
    Map(*vm, &f1, w, 0x8000, Prot::kCpuReadWrite, MapFlag::kFixed, VmaType::Flexible);
    Map(*vm, &f2, w + 0x8000, 0x8000, Prot::kCpuReadWrite, MapFlag::kFixed, VmaType::Flexible, "other");
    vm->Unmap(f1, 0x8000);
    Map(*vm, &f3, w + 0x100000, 0x10000, Prot::kCpuReadWrite, MapFlag::kFixed, VmaType::Flexible);
    const auto f3_area = vm->Areas(false);
    const bool two_pieces = std::any_of(f3_area.begin(), f3_area.end(), [&](const AreaInfo& a) {
        return a.base == w + 0x100000 && a.phys_slices == 2;
    });
    const bool across_pieces = vm->TryWriteBacking(w + 0x100000 + 0x7FF8, data.data(), 16) &&
                               std::memcmp(reinterpret_cast<const void*>(w + 0x100000 + 0x7FF8), data.data(), 16) == 0;
    g.Check("D11 inside one area: a flexible area of two physical pieces, a write across them is continuous",
            two_pieces && across_pieces, AreasText(*vm, w));
    vm->Unmap(w, 0x200000);
    inv.After(*vm, "D11");

    // D12: reads outside the GPU-visible range are zeros, also wrapping past 2^64.
    std::array<u8, 32> z{};
    z.fill(0xEE);
    vm->CopySparse(~0ull - 15, z.data(), z.size());
    const bool wrap_zero = std::all_of(z.begin(), z.end(), [](u8 b) { return b == 0; });
    z.fill(0xEE);
    vm->CopySparse(end - 8, z.data(), 16);
    const bool past_zero = std::all_of(z.begin(), z.begin() + 16, [](u8 b) { return b == 0; });
    g.Check("D12: CopySparse beyond the GPU-visible range (and wrapping past 2^64) reads zeros, nothing out of bounds",
            wrap_zero && past_zero, "");

    // D5 and the parameter checks.
    u64 out = 0;
    const std::vector<std::pair<std::string, bool>> params = {
        {"size 0", Map(*vm, &out, w, 0, 3, 0, VmaType::Code) == kEinval},
        {"size not a page multiple", Map(*vm, &out, w, 0x1000, 3, 0, VmaType::Code) == kEinval},
        {"fixed address not page aligned",
         Map(*vm, &out, w + 0x1000, 0x4000, 3, MapFlag::kFixed, VmaType::Code) == kEinval},
        {"alignment not a page multiple",
         Map(*vm, &out, w, 0x4000, 3, 0, VmaType::Code, "anon", kNoPhys, false, 0x1000) == kEinval},
        {"fixed past the end", Map(*vm, &out, end - 0x4000, 0x8000, 3, MapFlag::kFixed, VmaType::Code) == kEnomem},
        {"fixed wrapping 2^64", Map(*vm, &out, ~0ull - 0x3FFF, 0x8000, 3, MapFlag::kFixed, VmaType::Code) == kEnomem},
        {"search hint outside", Map(*vm, &out, ~0ull - 0x3FFF, 0x4000, 3, 0, VmaType::Code) == kEnomem},
        {"huge alignment",
         Map(*vm, &out, w, 0x4000, 3, 0, VmaType::Code, "anon", kNoPhys, false, 1ull << 62) == kEnomem},
        {"bigger than the window", Map(*vm, &out, 0, kWin + kPage, 3, 0, VmaType::Code) == kEnomem},
        {"direct without a physical address", Map(*vm, &out, w, 0x4000, 3, 0, VmaType::Direct) == kEinval},
        {"a physical address without direct", Map(*vm, &out, w, 0x4000, 3, 0, VmaType::Code, "anon", 0) == kEinval},
        {"direct not allocated", Map(*vm, &out, w, 0x4000, 3, 0, VmaType::Direct, "anon", 0) == kEnomem},
        {"direct beyond the size", Map(*vm, &out, w, 0x4000, 3, 0, VmaType::Direct, "anon", 64 * kMiB) == kEnomem},
        {"direct physical address not aligned",
         Map(*vm, &out, w, 0x4000, 3, 0, VmaType::Direct, "anon", 0x1000) == kEinval},
        {"memory pools", Map(*vm, &out, w, 0x4000, 3, 0, VmaType::PoolReserved) == kEinval},
        {"unmap size wraps", vm->Unmap(w, ~0ull) == kEinval},
        {"unmap past 2^64", vm->Unmap(~0ull - 0x3FFF, 0x8000) == kEinval},
        {"unmap outside", vm->Unmap(w - 0x10000, 0x4000) == kEinval},
        {"unmap size 0", vm->Unmap(w, 0) == kOk},
        {"D5: protect outside", vm->Protect(end, 0x4000, 3) == kEinval},
        {"protect not aligned", vm->Protect(w + 0x1000, 0x4000, 3) == kEinval},
        {"protect size 0", vm->Protect(end, 0, 3) == kOk},
        {"name outside", vm->NameRange(end, 0x4000, "x") == kEinval},
        {"release not aligned", vm->ReleaseDirect(0x1000, 0x4000, false) == kEinval},
        {"release wraps", vm->ReleaseDirect(0x4000, ~0ull - 0x3FFF, false) == kEinval},
        {"query below the space", vm->QueryProtection(w - 1, nullptr, nullptr, nullptr) == kEacces},
        {"valid: the window", vm->IsValid(w, kWin) && !vm->IsValid(w, kWin + 1) && !vm->IsValid(end, 0)},
    };
    std::string bad;
    for (const auto& [what, ok] : params) {
        if (!ok) {
            bad += what + "; ";
        }
    }
    g.Check("parameters: alignment, overflow, outside the space, wrong combinations - refused with the bridge's "
            "codes, nothing changed (D5: an invalid protect is EINVAL, the bridge asserts)",
            bad.empty() && vm->Areas(false).empty(), bad + AreasText(*vm, w));
    inv.After(*vm, "parameters");

    // D6: the flexible budget under the lock: two threads cannot map more than it together.
    std::atomic<int> mapped{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 2; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 64; ++i) {
                u64 a = 0;
                if (Map(*vm, &a, 0, 0x4000, Prot::kCpuReadWrite, 0, VmaType::Flexible) == kOk) {
                    ++mapped;
                }
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    g.Check("D6: two threads mapping flexible memory at once stay within the budget (1 MiB = 64 pages)",
            mapped == 64 && vm->FlexibleUsed() == kMiB, fmt::format("{} {:#x}", mapped.load(), vm->FlexibleUsed()));
    vm->Unmap(w, kWin);
    inv.After(*vm, "D6");
    g.Check("invariants", inv.first.empty(), inv.first);

    // D16: two ranges with a gap between them.
    vm.reset();
    const u64 b = FindFreeWindow(48 * kMiB);
    std::string error = "no free window";
    Layout two = WindowLayout(b, 16 * kMiB, 8 * kMiB, kMiB, CommitMode::Upfront, sdk);
    two.ranges.push_back({b + 32 * kMiB, 16 * kMiB});
    two.search_limit = b + 48 * kMiB;
    two.gpu = {b, 48 * kMiB};
    auto gapped = b != 0 ? VirtualMemory::Create(two, &error) : nullptr;
    if (gapped == nullptr) {
        g.Check("two ranges with a gap", false, error);
        return;
    }
    Memory::QueryInfo qi;
    u64 r = 0;
    const bool gap_invalid = !gapped->IsValid(b + 20 * kMiB, 0) && !gapped->IsValid(b, 20 * kMiB) &&
                             gapped->IsValid(b + 32 * kMiB, 16 * kMiB);
    const bool gap_query = gapped->Query(b + 20 * kMiB, false, &qi) == kEacces;
    const s32 big = Map(*gapped, &r, b + 8 * kMiB, 12 * kMiB, 3, 0, VmaType::Reserved);
    const bool search_skips = big == kOk && r == b + 32 * kMiB;
    const bool next_query = gapped->Query(b + 20 * kMiB, true, &qi) == kOk && qi.start == b + 32 * kMiB;
    const bool fixed_across = Map(*gapped, &r, b + 12 * kMiB, 24 * kMiB, 3, MapFlag::kFixed, VmaType::Code) == kEnomem;
    const std::string gap_inv = gapped->CheckInvariants();
    g.Check("D16: two ranges with a gap: an address in the gap is not valid and not answered (the bridge answers for "
            "the area before it), a search skips the gap, find_next finds the next range, a fixed map across the gap "
            "is refused",
            gap_invalid && gap_query && search_skips && next_query && fixed_across && gap_inv.empty(),
            fmt::format("{} {} {} {} {} {}", gap_invalid, gap_query, search_skips, next_query, fixed_across, gap_inv));
}

// ---------------------------------------------------------------------------- NativeVmm(model): random operations

/// One guest page as the model sees it.
struct ModelPage {
    VmaType type = VmaType::Free;
    u32 prot = 0;
    std::string name;
    u64 phys = kNoPhys;
    int alloc = -1; ///< private host allocation
    int view = -1;  ///< host view of the section
    bool exec = false;
    HostAccess host = HostAccess::None;
    u64 tag = 0; ///< private memory: the first 8 bytes
};

/// The manager's semantics page by page (docs/PHASE2_P233_NATIVE_VMM.md), written without areas.
class PageModel {
public:
    PageModel(u64 w, u32 pages, u64 direct, u64 flex_base, u64 flex_size)
        : w_{w}, p_(pages), direct_pages_{direct / kPage}, flex_budget_{flex_size} {
        for (u64 f = flex_base; f < flex_base + flex_size; f += kPage) {
            flex_free_.insert(f);
        }
    }

    s32 Map(u32 page, u32 n, u32 prot, u32 flags, VmaType type, const std::string& name, u64 phys, bool exclusive,
            u32 align_pages, u32* out_page) {
        const bool fixed = (flags & MapFlag::kFixed) != 0;
        if (type == VmaType::Flexible && n * kPage > flex_budget_ - flex_used_) {
            return kEinval;
        }
        if (type == VmaType::Direct) {
            if (phys / kPage + n > direct_pages_) {
                return kEnomem;
            }
            for (u32 i = 0; i < n; ++i) {
                const u64 pp = phys + i * kPage;
                if (!dalloc_.contains(pp)) {
                    return kEnomem;
                }
                if (exclusive && dviews_[pp] > 0) {
                    return kEbusy;
                }
            }
        }
        u32 at = page;
        if (fixed) {
            if (page + n > p_.size()) {
                return kEnomem;
            }
            if ((flags & MapFlag::kNoOverwrite) != 0) {
                for (u32 i = page; i < page + n; ++i) {
                    if (p_[i].type != VmaType::Free) {
                        return kEnomem;
                    }
                }
            } else if (SplitsPrivate(page, n)) {
                return kEnomem;
            }
        } else {
            const auto found = Search(page, n, align_pages == 0 ? 1 : align_pages);
            if (!found) {
                return kEnomem;
            }
            at = *found;
        }
        UnmapPages(at, n);
        u32 stored = prot;
        if ((stored & Prot::kCpuWrite) != 0) {
            stored |= Prot::kCpuRead;
        }
        const bool exec = (prot & Prot::kCpuExec) != 0;
        const HostAccess mapped = exec ? HostAccess::ExecuteReadWrite : HostAccess::ReadWrite;
        const int alloc = next_alloc_++;
        int view = next_view_++;
        u64 last_phys = kNoPhys;
        for (u32 i = 0; i < n; ++i) {
            ModelPage& m = p_[at + i];
            m = ModelPage{};
            m.type = type;
            m.prot = stored;
            m.name = name;
            m.exec = exec;
            switch (type) {
            case VmaType::Reserved:
                break;
            case VmaType::Direct:
                m.phys = phys + i * kPage;
                m.view = view;
                m.host = mapped;
                ++dviews_[m.phys];
                break;
            case VmaType::Flexible:
                m.phys = *flex_free_.begin();
                flex_free_.erase(flex_free_.begin());
                if (last_phys != kNoPhys && m.phys != last_phys + kPage) {
                    view = next_view_++; // each physical piece is its own view
                }
                last_phys = m.phys;
                m.view = view;
                m.host = mapped;
                flex_used_ += kPage;
                break;
            default:
                m.alloc = alloc;
                m.host = mapped;
                break;
            }
        }
        *out_page = at;
        return kOk;
    }

    s32 Unmap(u64 addr, u64 size) {
        const u64 a = addr - addr % kPage;
        const u64 s = (size + kPage - 1) / kPage * kPage;
        const u64 window = p_.size() * kPage;
        if (a < w_ || a - w_ >= window || s > window - (a - w_)) {
            return kEinval;
        }
        const u32 page = static_cast<u32>((a - w_) / kPage);
        const u32 n = static_cast<u32>(s / kPage);
        if (SplitsPrivate(page, n)) {
            return kEinval;
        }
        UnmapPages(page, n);
        return kOk;
    }

    void Protect(u32 page, u32 n, u32 prot) {
        for (u32 i = page; i < page + n; ++i) {
            ModelPage& m = p_[i];
            if (m.type == VmaType::Free) {
                continue;
            }
            u32 stored = prot;
            if ((stored & Prot::kCpuWrite) != 0) {
                stored |= Prot::kCpuRead;
            }
            const HostAccess access = GuestToHost(stored);
            if (m.type == VmaType::Direct || m.type == VmaType::File) {
                stored &= ~Prot::kCpuExec;
            }
            if (m.type != VmaType::Reserved && prot != m.prot) {
                m.host = access;
            }
            m.prot = stored;
        }
    }

    void Name(u32 page, u32 n, const std::string& name) {
        for (u32 i = page; i < page + n; ++i) {
            if (p_[i].type != VmaType::Free) {
                p_[i].name = name;
            }
        }
    }

    /// First fit of n pages aligned to `align` pages in the direct memory (DirectMemory's placement).
    std::optional<u64> AllocateFit(u64 n, u64 align) const {
        for (u64 p = 0; p + n <= direct_pages_; p += align) {
            bool free = true;
            for (u64 i = 0; i < n && free; ++i) {
                free = !dalloc_.contains((p + i) * kPage);
            }
            if (free) {
                return p * kPage;
            }
        }
        return std::nullopt;
    }
    void Allocated(u64 phys, u64 n) {
        for (u64 i = 0; i < n; ++i) {
            dalloc_.insert(phys + i * kPage);
        }
    }

    /// Release of [phys, phys + n pages): the views of its allocated pages go first.
    void Release(u64 phys, u64 n) {
        std::set<u64> freed;
        for (u64 i = 0; i < n; ++i) {
            if (dalloc_.contains(phys + i * kPage)) {
                freed.insert(phys + i * kPage);
            }
        }
        for (u32 i = 0; i < p_.size();) {
            if (p_[i].type == VmaType::Direct && freed.contains(p_[i].phys)) {
                u32 j = i + 1;
                while (j < p_.size() && p_[j].type == VmaType::Direct && freed.contains(p_[j].phys) &&
                       p_[j].view == p_[i].view && p_[j].phys == p_[j - 1].phys + kPage) {
                    ++j;
                }
                UnmapPages(i, j - i);
                i = j;
            } else {
                ++i;
            }
        }
        for (u64 f : freed) {
            dalloc_.erase(f);
        }
    }

    void Wrote(u32 page, u64 tag) {
        ModelPage& m = p_[page];
        if (m.phys != kNoPhys) {
            phys_tag_[m.phys] = tag;
        } else {
            m.tag = tag;
        }
    }

    const std::vector<ModelPage>& Pages() const {
        return p_;
    }
    u64 PhysTag(u64 phys) const {
        const auto it = phys_tag_.find(phys);
        return it == phys_tag_.end() ? 0 : it->second;
    }
    u64 FlexUsed() const {
        return flex_used_;
    }

private:
    bool SplitsPrivate(u32 page, u32 n) const {
        std::set<int> touched;
        for (u32 i = page; i < page + n; ++i) {
            if (p_[i].alloc >= 0) {
                touched.insert(p_[i].alloc);
            }
        }
        for (u32 i = 0; i < p_.size(); ++i) {
            if (p_[i].alloc >= 0 && touched.contains(p_[i].alloc) && (i < page || i >= page + n)) {
                return true;
            }
        }
        return false;
    }

    /// SearchFree over maximal free runs (free areas are always merged).
    std::optional<u32> Search(u32 hint, u32 n, u32 align) const {
        const u32 count = static_cast<u32>(p_.size());
        if (hint >= count) {
            return std::nullopt;
        }
        const u32 candidate = (hint + align - 1) / align * align;
        if (candidate >= count) {
            return std::nullopt;
        }
        const auto RunEnd = [&](u32 i) {
            while (i < count && p_[i].type == VmaType::Free) {
                ++i;
            }
            return i;
        };
        u32 next = candidate + 1;
        if (p_[candidate].type == VmaType::Free) {
            const u32 e = RunEnd(candidate);
            if (e - candidate >= n) {
                return candidate;
            }
            next = e;
        }
        // The free runs that start after the candidate's area.
        for (u32 i = next; i < count;) {
            if (p_[i].type != VmaType::Free) {
                ++i;
                continue;
            }
            const u32 e = RunEnd(i);
            const u32 c = (i + align - 1) / align * align;
            if (c <= e && e - c >= n) {
                return c;
            }
            i = e;
        }
        return std::nullopt;
    }

    void UnmapPages(u32 page, u32 n) {
        std::set<int> cut_views;
        for (u32 i = page; i < page + n; ++i) {
            ModelPage& m = p_[i];
            if (m.view >= 0) {
                cut_views.insert(m.view);
            }
            if (m.type == VmaType::Direct) {
                --dviews_[m.phys];
            } else if (m.type == VmaType::Flexible) {
                flex_free_.insert(m.phys);
                phys_tag_.erase(m.phys); // cleared when freed
                flex_used_ -= kPage;
            }
            m = ModelPage{};
        }
        // Windows cannot shorten a view: what is left of a cut view is mapped again, a new view on each side with
        // read/write(+execute) protection.
        for (int v : cut_views) {
            int left = -1, right = -1;
            for (u32 i = 0; i < p_.size(); ++i) {
                if (p_[i].view != v) {
                    continue;
                }
                int& id = i < page ? left : right;
                if (id < 0) {
                    id = next_view_++;
                }
                p_[i].view = id;
                p_[i].host = p_[i].exec ? HostAccess::ExecuteReadWrite : HostAccess::ReadWrite;
            }
        }
    }

    u64 w_;
    std::vector<ModelPage> p_;
    u64 direct_pages_;
    std::set<u64> dalloc_;
    std::map<u64, u32> dviews_;
    std::set<u64> flex_free_;
    u64 flex_budget_;
    u64 flex_used_ = 0;
    std::map<u64, u64> phys_tag_;
    int next_alloc_ = 0;
    int next_view_ = 0;
};

/// "" when the manager, the host and the memory contents match the model page by page.
std::string CompareWithModel(const VirtualMemory& vm, const PageModel& model, u64 w) {
    const std::vector<AreaInfo> areas = vm.Areas(true);
    const auto& pages = model.Pages();
    for (u32 i = 0; i < pages.size(); ++i) {
        const u64 addr = w + i * kPage;
        const ModelPage& m = pages[i];
        const auto a = std::find_if(areas.begin(), areas.end(),
                                    [addr](const AreaInfo& x) { return addr >= x.base && addr - x.base < x.size; });
        if (a == areas.end()) {
            return fmt::format("page {} has no area", i);
        }
        const std::string name = m.type == VmaType::Free ? "" : m.name;
        if (a->type != m.type || a->prot != m.prot || a->name != name || vm.PhysicalAddress(addr) != m.phys) {
            return fmt::format("page {}: area type {} prot {:#x} \"{}\" phys {:#x}, model type {} prot {:#x} \"{}\" "
                               "phys {:#x}",
                               i, static_cast<int>(a->type), a->prot, a->name, vm.PhysicalAddress(addr),
                               static_cast<int>(m.type), m.prot, name, m.phys);
        }
        const HostPage h = Host(addr);
        if (m.type == VmaType::Free || m.type == VmaType::Reserved) {
            if (h.state != MEM_RESERVE) {
                return fmt::format("page {}: host state {:#x}, expected a placeholder", i, h.state);
            }
            continue;
        }
        const DWORD expected_type = m.phys != kNoPhys ? MEM_MAPPED : MEM_PRIVATE;
        if (h.state != MEM_COMMIT || h.type != expected_type || h.protect != Win(m.host)) {
            return fmt::format("page {}: host {:#x}/{:#x}/{:#x}, expected type {:#x} protect {:#x}", i, h.state, h.type,
                               h.protect, expected_type, Win(m.host));
        }
        u64 first = 0;
        if (m.phys != kNoPhys) {
            vm.CopySparse(addr, reinterpret_cast<u8*>(&first), 8);
            if (first != model.PhysTag(m.phys)) {
                return fmt::format("page {} (phys {:#x}): {:#x}, model {:#x}", i, m.phys, first, model.PhysTag(m.phys));
            }
        } else if (m.host == HostAccess::ReadWrite || m.host == HostAccess::ExecuteReadWrite) {
            first = *reinterpret_cast<const u64*>(addr);
            if (first != m.tag) {
                return fmt::format("page {} (private): {:#x}, model {:#x}", i, first, m.tag);
            }
        }
    }
    if (vm.FlexibleUsed() != model.FlexUsed()) {
        return fmt::format("flexible used {:#x}, model {:#x}", vm.FlexibleUsed(), model.FlexUsed());
    }
    return "";
}

void TestModel(TestGroup& g, u32 sdk) {
    constexpr u32 kPages = 256; // 4 MiB of addresses
    constexpr u64 kDirect = 2 * kMiB;
    constexpr u64 kFlexible = 512 * 1024;
    u64 w = 0;
    auto vm = MakeVm(g, kPages * kPage, kDirect, kFlexible, CommitMode::Upfront, sdk, &w);
    if (vm == nullptr) {
        return;
    }
    PageModel model(w, kPages, kDirect, kDirect, kFlexible);
    std::mt19937_64 rng(0x5EC01DC0DE000233ull);
    const auto R = [&](u64 n) { return n == 0 ? 0 : rng() % n; };
    struct Block {
        u64 phys;
        u64 pages;
    };
    std::vector<Block> blocks;
    constexpr u32 kProts[] = {0, 1, 2, 3, 4, 5, 7, 0x10, 0x13, 0x33, 0x37};
    const char* const kNames[] = {"anon", "a", "b"};
    constexpr VmaType kTypes[] = {VmaType::Reserved, VmaType::Code,   VmaType::Stack, VmaType::File,
                                  VmaType::Flexible, VmaType::Direct, VmaType::Direct};
    std::string failure;
    std::vector<std::string> recent;
    int ops = 0;
    for (; ops < 3000 && failure.empty(); ++ops) {
        std::string what;
        s32 got = 0, want = 0;
        const u32 kind = static_cast<u32>(R(100));
        if (kind < 40) { // map
            const VmaType type = kTypes[R(std::size(kTypes))];
            const bool fixed = R(3) != 0;
            u32 flags = fixed ? MapFlag::kFixed : 0;
            if (fixed && R(4) == 0) {
                flags |= MapFlag::kNoOverwrite;
            }
            if (R(8) == 0) {
                flags |= MapFlag::kNoCoalesce;
            }
            const u32 n = static_cast<u32>(1 + R(12));
            const u32 page = static_cast<u32>(R(kPages + 2)); // sometimes past the end
            const u32 align = fixed ? 0 : static_cast<u32>(R(3) == 0 ? 4 : 0);
            u32 prot = kProts[R(std::size(kProts))];
            if (type == VmaType::Direct) {
                prot &= ~Prot::kCpuExec;
            }
            const std::string name = kNames[R(3)];
            u64 phys = kNoPhys;
            bool exclusive = false;
            if (type == VmaType::Direct) {
                if (!blocks.empty() && R(5) != 0) {
                    const Block& blk = blocks[R(blocks.size())];
                    phys = blk.phys + R(blk.pages) * kPage;
                } else {
                    phys = R(kDirect / kPage + 2) * kPage;
                }
                exclusive = R(3) == 0;
            }
            u32 model_page = 0;
            want = model.Map(page, n, prot, flags, type, name, phys, exclusive, align, &model_page);
            u64 out = 0;
            got = Map(*vm, &out, w + u64{page} * kPage, n * kPage, prot, flags, type, name, phys, exclusive,
                      u64{align} * kPage);
            what = fmt::format("map {} pages at {} type {} prot {:#x} flags {:#x} phys {:#x} excl {} align {}", n,
                               page, static_cast<int>(type), prot, flags, phys, exclusive, align);
            if (got == kOk && want == kOk && out != w + u64{model_page} * kPage) {
                failure = fmt::format("op {}: {}: at page {}, model {}", ops, what, (out - w) / kPage, model_page);
            }
        } else if (kind < 60) { // unmap, also unaligned and outside
            const u64 addr = w + R(kPages + 4) * kPage + (R(4) == 0 ? R(kPage) : 0);
            const u64 size = (1 + R(16)) * kPage - (R(4) == 0 ? R(kPage) : 0);
            want = model.Unmap(addr, size);
            got = vm->Unmap(addr, size);
            what = fmt::format("unmap {:#x}+{:#x}", addr - w, size);
        } else if (kind < 75) { // protect
            const u32 page = static_cast<u32>(R(kPages));
            const u32 n = static_cast<u32>(1 + R(std::min<u64>(12, kPages - page)));
            const u32 prot = kProts[R(std::size(kProts))];
            model.Protect(page, n, prot);
            got = vm->Protect(w + u64{page} * kPage, n * kPage, prot);
            what = fmt::format("protect {} pages at {} to {:#x}", n, page, prot);
        } else if (kind < 80) { // name
            const u32 page = static_cast<u32>(R(kPages));
            const u32 n = static_cast<u32>(1 + R(std::min<u64>(8, kPages - page)));
            const std::string name = kNames[R(3)];
            model.Name(page, n, name);
            got = vm->NameRange(w + u64{page} * kPage, n * kPage, name);
            what = fmt::format("name {} pages at {} \"{}\"", n, page, name);
        } else if (kind < 87) { // allocate direct memory
            const u64 n = 1 + R(8);
            const u64 align = R(2) == 0 ? 0 : 2;
            const auto fit = model.AllocateFit(n, align == 0 ? 1 : align);
            u64 phys = 0;
            got = vm->AllocateDirect(0, kDirect, n * kPage, align * kPage, static_cast<s32>(R(2) * 3), &phys);
            want = fit ? kOk : Memory::kErrEagain;
            what = fmt::format("allocate {} pages align {}", n, align);
            if (got == kOk && want == kOk && phys != *fit) {
                failure = fmt::format("op {}: {}: at {:#x}, model {:#x}", ops, what, phys, *fit);
            }
            if (got == kOk) {
                model.Allocated(phys, n);
                blocks.push_back({phys, n});
            }
        } else if (kind < 92) { // release, with or without views
            u64 phys = 0, n = 0;
            if (!blocks.empty() && R(4) != 0) {
                const size_t i = R(blocks.size());
                const Block blk = blocks[i];
                const u64 from = R(blk.pages);
                phys = blk.phys + from * kPage;
                n = 1 + R(blk.pages - from);
                if (from == 0 && n == blk.pages) {
                    blocks.erase(blocks.begin() + static_cast<std::ptrdiff_t>(i));
                }
            } else {
                phys = R(kDirect / kPage) * kPage;
                n = 1 + R(8);
            }
            model.Release(phys, n);
            got = vm->ReleaseDirect(phys, n * kPage, false);
            what = fmt::format("release {:#x}+{} pages", phys, n);
        } else { // write a tag
            const u32 page = static_cast<u32>(R(kPages));
            const ModelPage& m = model.Pages()[page];
            const u64 tag = rng() | 1;
            const u64 addr = w + u64{page} * kPage;
            what = fmt::format("write page {}", page);
            if (m.phys != kNoPhys) {
                got = vm->TryWriteBacking(addr, &tag, 8) ? kOk : -1;
                model.Wrote(page, tag);
            } else if (m.type != VmaType::Free && m.type != VmaType::Reserved &&
                       (m.host == HostAccess::ReadWrite || m.host == HostAccess::ExecuteReadWrite)) {
                *reinterpret_cast<u64*>(addr) = tag;
                model.Wrote(page, tag);
            }
        }
        recent.push_back(fmt::format("{} -> {:#x} (model {:#x})", what, static_cast<u32>(got), static_cast<u32>(want)));
        if (recent.size() > 6) {
            recent.erase(recent.begin());
        }
        if (failure.empty() && got != want) {
            failure = fmt::format("op {}: {} returned {:#x}, model {:#x}", ops, what, static_cast<u32>(got),
                                  static_cast<u32>(want));
        }
        if (failure.empty()) {
            const std::string e = vm->CheckInvariants();
            if (!e.empty()) {
                failure = fmt::format("op {} ({}): invariant: {}", ops, what, e);
            }
        }
        if (failure.empty()) {
            const std::string cmp = CompareWithModel(*vm, model, w);
            if (!cmp.empty()) {
                failure = fmt::format("op {} ({}): {}", ops, what, cmp);
            }
        }
    }
    std::string log;
    for (const auto& r : recent) {
        log += "\n        " + r;
    }
    const auto stats = vm->GetStats();
    g.Check("3000 random operations (map fixed/searched/no-overwrite/no-coalesce of every type, unmap also unaligned "
            "and outside, protect, name, allocate, release with views, writes) against a page model: results, "
            "addresses, areas, physical pages, host state and protection, contents, invariants after each",
            failure.empty(), failure + log);
    g.Info(fmt::format("model run: {} operations ({} refused as the model expected); areas split {} merged {}; host "
                       "views {} private {} remaps {} coalesces {}",
                       ops, stats.failures, stats.vma.splits, stats.vma.merges, stats.host.views, stats.host.privates,
                       stats.host.remaps, stats.host.coalesces));
}

// ---------------------------------------------------------------------------- NativeVmm(mt): threads

void TestThreads(TestGroup& g, u32 sdk) {
    constexpr int kWorkers = 6;
    constexpr u64 kSlice = 4 * kMiB;
    u64 w = 0;
    auto vm = MakeVm(g, (kWorkers + 1) * kSlice, 16 * kMiB, 4 * kMiB, CommitMode::Upfront, sdk, &w);
    if (vm == nullptr) {
        return;
    }
    // A stable view the readers check all the time.
    u64 shared_phys = 0, shared = 0;
    vm->AllocateDirect(0, vm->DirectSize(), 0x40000, 0, 0, &shared_phys);
    Map(*vm, &shared, w + kWorkers * kSlice, 0x40000, 0x33, MapFlag::kFixed, VmaType::Direct, "anon", shared_phys);
    const auto Pattern = [](u64 o) { return static_cast<u8>(o * 29 + 3); };
    for (u64 o = 0; o < 0x40000; ++o) {
        reinterpret_cast<u8*>(shared)[o] = Pattern(o);
    }
    std::atomic<int> done{0}, errors{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int k = 0; k < kWorkers; ++k) {
        threads.emplace_back([&, k] {
            std::mt19937_64 rng(0x1234 + k);
            const u64 base = w + k * kSlice;
            for (int i = 0; i < 300; ++i) {
                const u64 n = (1 + rng() % 8) * kPage;
                const u64 at = base + (rng() % (kSlice / kPage - 8)) * kPage;
                u64 out = 0;
                s32 r = kOk;
                switch (rng() % 3) {
                case 0: {
                    u64 phys = 0;
                    if (vm->AllocateDirect(0, vm->DirectSize(), n, 0, 0, &phys) != kOk) {
                        ++errors;
                        break;
                    }
                    r = Map(*vm, &out, at, n, 0x33, MapFlag::kFixed, VmaType::Direct, "anon", phys);
                    if (r == kOk) {
                        *reinterpret_cast<u64*>(at) = at;
                        errors += *reinterpret_cast<const u64*>(at) == at ? 0 : 1;
                        r = vm->Unmap(at, n);
                    }
                    errors += vm->ReleaseDirect(phys, n, false) == kOk ? 0 : 1;
                    break;
                }
                case 1:
                    r = Map(*vm, &out, at, n, Prot::kCpuReadWrite, MapFlag::kFixed, VmaType::Flexible);
                    if (r == kOk) {
                        errors += *reinterpret_cast<const u64*>(at) == 0 ? 0 : 1; // flexible memory comes cleared
                        *reinterpret_cast<u64*>(at) = ~at;
                        vm->Protect(at, n, Prot::kCpuRead);
                        r = vm->Unmap(at, n);
                    }
                    break;
                default:
                    r = Map(*vm, &out, at, n, Prot::kCpuReadWrite, MapFlag::kFixed, VmaType::Code, "worker");
                    if (r == kOk) {
                        vm->NameRange(at, kPage, "first");
                        r = vm->Unmap(at, n);
                    }
                    break;
                }
                if (r != kOk) {
                    ++errors;
                }
            }
            ++done;
        });
    }
    for (int k = 0; k < 2; ++k) {
        threads.emplace_back([&, k] {
            std::mt19937_64 rng(0x9999 + k);
            std::vector<u8> buf(0x1000);
            while (!stop) {
                const u64 off = rng() % 0x3F000;
                const u64 len = 1 + rng() % 0x1000;
                vm->CopySparse(shared + off, buf.data(), len);
                for (u64 j = 0; j < len; ++j) {
                    if (buf[j] != Pattern(off + j)) {
                        ++errors;
                        break;
                    }
                }
                std::array<u8, 16> same{};
                for (u64 j = 0; j < same.size(); ++j) {
                    same[j] = Pattern(off + j);
                }
                errors += vm->TryWriteBacking(shared + off, same.data(), same.size()) ? 0 : 1;
                errors += vm->ClampRange(shared + off, 1ull << 30) >= 0x40000 - off ? 0 : 1;
                vm->ProtectTracking(shared, 0x40000, HostAccess::ReadWrite);
                Memory::QueryInfo qi;
                errors += vm->Query(shared, false, &qi) == kOk ? 0 : 1;
                errors += vm->IsValid(w + (rng() % ((kWorkers + 1) * kSlice)), 0) ? 0 : 1;
            }
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (done < kWorkers && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (done < kWorkers) {
        SSPC_LOG(App, Critical, "selftest: native virtual memory threads did not finish in 60 s (deadlock)");
        Log::Flush();
        std::quick_exit(5);
    }
    stop = true;
    for (auto& t : threads) {
        t.join();
    }
    bool intact = true;
    for (u64 o = 0; o < 0x40000; ++o) {
        intact = intact && reinterpret_cast<const u8*>(shared)[o] == Pattern(o);
    }
    const std::string inv = vm->CheckInvariants();
    const auto areas = vm->Areas(false);
    g.Check("6 workers (direct, flexible, code: map, write, protect, name, unmap, release) and 2 readers (copy, "
            "write, clamp, tracking protection, query, valid) at once: no error, no deadlock (60 s watchdog), the "
            "shared view intact, only it left, invariants hold",
            errors == 0 && intact && inv.empty() && areas.size() == 1 && vm->FlexibleUsed() == 0,
            fmt::format("errors {} intact {} areas {} flexible {:#x} {}", errors.load(), intact, areas.size(),
                        vm->FlexibleUsed(), inv));
    const auto stats = vm->GetStats();
    g.Info(fmt::format("threads: maps {} unmaps {} protects {}; areas split {} merged {}; host views {} private {} "
                       "coalesces {}",
                       stats.maps, stats.unmaps, stats.protects, stats.vma.splits, stats.vma.merges, stats.host.views,
                       stats.host.privates, stats.host.coalesces));
}

// ---------------------------------------------------------------------------- NativeVmm(game): the game's layout

u32 WindowsBuild() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    const auto rtl = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    return rtl != nullptr && rtl(&info) == 0 ? info.dwBuildNumber : 0;
}

void TestGameLayout(TestGroup& g, u32 sdk) {
    // The bridge took every free region at its start; seen as "free" in a host map with everything else in use,
    // GameLayout has to find exactly those regions again.
    auto* mm = Core::Memory::Instance();
    auto& space = mm->GetAddressSpace();
    std::vector<Memory::AddressRange> bridge;
    for (const auto& r : space.GetUsableRegions()) {
        bridge.push_back({r.lower(), r.upper() - r.lower()});
    }
    std::vector<Memory::HostRegionInfo> host_map;
    u64 cursor = 0;
    for (const auto& r : bridge) {
        if (r.base > cursor) {
            host_map.push_back({cursor, r.base - cursor, false});
        }
        host_map.push_back({r.base, r.size, true});
        cursor = r.End();
    }
    host_map.push_back({cursor, ~0ull - cursor, false});
    const Layout layout = Memory::GameLayout(host_map, sdk, WindowsBuild(), 0x10000, 0, 0);
    bool same = layout.ranges.size() == bridge.size();
    for (size_t i = 0; same && i < bridge.size(); ++i) {
        same = layout.ranges[i].base == bridge[i].base && layout.ranges[i].size == bridge[i].size;
    }
    g.Check("GameLayout over the host's map finds the bridge's regions (the same free regions above 16 MiB below the "
            "user maximum, aligned)",
            same, fmt::format("{} regions, the bridge {}", layout.ranges.size(), bridge.size()));
    g.Check("GameLayout's constants are the bridge's: lowest = its first region, search limit = the end of its user "
            "area, system reserved [0x7FFFFC000, 0x1000000000), GPU 40 bits, direct 7936 MiB, flexible 512 MiB after "
            "it",
            !bridge.empty() && layout.lowest == bridge.front().base &&
                layout.search_limit == space.UserVirtualBase() + space.UserVirtualSize() &&
                layout.system_reserved_base == space.SystemReservedVirtualBase() &&
                layout.system_reserved_end == space.SystemReservedVirtualBase() + space.SystemReservedVirtualSize() &&
                layout.search_base == Core::DEFAULT_MAPPING_BASE && layout.gpu.base == 0 &&
                layout.gpu.size == (1ull << 40) && layout.direct_size == 7936 * kMiB &&
                layout.flexible_base == 7936 * kMiB && layout.flexible_size == 512 * kMiB,
            fmt::format("lowest {:#x} limit {:#x} reserved {:#x}-{:#x}", layout.lowest, layout.search_limit,
                        layout.system_reserved_base, layout.system_reserved_end));
}

// ---------------------------------------------------------------------------- NativeVmm(commit): experiment E1

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

void TestCommit(TestGroup& g, u32 sdk) {
    // The game's physical layout (direct 7936 MiB + flexible 512 MiB = the bridge's 8448 MiB section) with commit
    // on demand; then a 256 MiB SEC_COMMIT section for comparison. Other programs move the commit too: the deltas
    // are checked with a tolerance and reported.
    constexpr s64 kTolerance = 48 * kMiB;
    if (SystemCommit().second < 2048 * kMiB) {
        g.Check("free system commit >= 2 GiB for the experiment", false,
                fmt::format("{} MiB", SystemCommit().second >> 20));
        return;
    }
    const auto Delta = [](u64 before) { return static_cast<s64>(SystemCommit().first) - static_cast<s64>(before); };
    const auto Near = [](s64 value, s64 expected) { return std::llabs(value - expected) < kTolerance; };
    const u64 c0 = SystemCommit().first;
    u64 w = 0;
    auto vm = MakeVm(g, 64 * kMiB, 7936 * kMiB, 512 * kMiB, CommitMode::OnDemand, sdk, &w);
    if (vm == nullptr) {
        return;
    }
    const s64 created = Delta(c0);
    // The game's budgets (SetupRegions with its SceKernelMemParam: flexible 0x20000000, no extended memory).
    const Memory::Budget budget{.flexible_size = 0x20000000, .extended1 = false, .extended2 = false};
    const bool setup = vm->SetupRegions(budget);
    const u64 direct = vm->DirectSize();
    const u64 flexible = vm->FlexibleBudget();
    const u64 c1 = SystemCommit().first;
    u64 phys = 0;
    const s32 alloc = vm->AllocateDirect(0, vm->DirectSize(), 64 * kMiB, 0, 0, &phys);
    const s64 allocated = Delta(c1);
    u64 view = 0;
    const s32 mapped = Map(*vm, &view, 0, 64 * kMiB, 0x33, 0, VmaType::Direct, "anon", phys);
    if (mapped == kOk) {
        std::memset(reinterpret_cast<void*>(view), 0x5A, 64 * kMiB); // touched: still the same commit
    }
    const s64 touched = Delta(c1);
    vm->Unmap(view, 64 * kMiB);
    vm->ReleaseDirect(phys, 64 * kMiB, false);
    const s64 released = Delta(c1);
    vm.reset();
    const s64 destroyed = Delta(c0);
    g.Check("the game's budgets on the native manager: SetupRegions gives direct 0x120000000 (4608 MiB) and flexible "
            "0x1C000000 (448 MiB), the bridge's numbers for this game",
            setup && direct == 0x120000000ull && flexible == 0x1C000000ull,
            fmt::format("{} {:#x} {:#x}", setup, direct, flexible));
    g.Check("E1, commit on demand (SEC_RESERVE section of 8448 MiB): creation charges ~0 instead of 8448 MiB; 64 MiB "
            "of direct memory charges 64 MiB when allocated (touching it adds nothing); a release does not give it "
            "back (a section page cannot be decommitted); the manager's end gives everything back",
            alloc == kOk && mapped == kOk && created < kTolerance && Near(allocated, 64 * kMiB) &&
                Near(touched, 64 * kMiB) && Near(released, 64 * kMiB) && Near(destroyed, 0),
            fmt::format("MiB: created {} allocated {} touched {} released {} destroyed {}", created >> 20,
                        allocated >> 20, touched >> 20, released >> 20, destroyed >> 20));
    const u64 c2 = SystemCommit().first;
    auto upfront = MakeVm(g, 64 * kMiB, 192 * kMiB, 64 * kMiB, CommitMode::Upfront, sdk, &w);
    const s64 upfront_delta = Delta(c2);
    upfront.reset();
    g.Check("for comparison, SEC_COMMIT (the bridge's way): a 256 MiB section charges 256 MiB at creation",
            Near(upfront_delta, 256 * kMiB), fmt::format("{} MiB", upfront_delta >> 20));
    g.Info(fmt::format("E1 (MiB, system commit deltas): on-demand 8448 MiB section created {}, +64 MiB allocated {}, "
                       "touched {}, released {}, destroyed {}; SEC_COMMIT 256 MiB section {}",
                       created >> 20, allocated >> 20, touched >> 20, released >> 20, destroyed >> 20,
                       upfront_delta >> 20));
}

// ---------------------------------------------------------------------------- performance (informational)

void MeasureLatencies(TestGroup& g, u32 sdk) {
    u64 w = 0;
    auto vm = MakeVm(g, 64 * kMiB, 16 * kMiB, 4 * kMiB, CommitMode::Upfront, sdk, &w);
    if (vm == nullptr) {
        return;
    }
    u64 phys = 0;
    vm->AllocateDirect(0, vm->DirectSize(), 0x10000, 0, 0, &phys);
    const auto Percentiles = [](std::vector<double>& v) {
        std::sort(v.begin(), v.end());
        const auto at = [&](double q) { return v[static_cast<size_t>(q * static_cast<double>(v.size() - 1))]; };
        return fmt::format("{:.1f}/{:.1f}/{:.1f}", at(0.5), at(0.95), at(0.99));
    };
    std::vector<double> map_us, unmap_us, protect_us, query_us;
    for (int i = 0; i < 2000; ++i) {
        u64 out = 0;
        const auto t0 = std::chrono::steady_clock::now();
        Map(*vm, &out, w, 0x10000, 0x33, MapFlag::kFixed, VmaType::Direct, "anon", phys);
        const auto t1 = std::chrono::steady_clock::now();
        vm->Protect(w, 0x4000, i % 2 == 0 ? 1 : 0x33);
        const auto t2 = std::chrono::steady_clock::now();
        Memory::QueryInfo qi;
        vm->Query(w, false, &qi);
        const auto t3 = std::chrono::steady_clock::now();
        vm->Unmap(w, 0x10000);
        const auto t4 = std::chrono::steady_clock::now();
        map_us.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        protect_us.push_back(std::chrono::duration<double, std::micro>(t2 - t1).count());
        query_us.push_back(std::chrono::duration<double, std::micro>(t3 - t2).count());
        unmap_us.push_back(std::chrono::duration<double, std::micro>(t4 - t3).count());
    }
    g.Info(fmt::format("latency, P50/P95/P99 us (2000 x 64 KiB direct view): map {}, protect {}, query {}, unmap {}",
                       Percentiles(map_us), Percentiles(protect_us), Percentiles(query_us), Percentiles(unmap_us)));
}

} // namespace

std::vector<TestGroup> TestNativeVirtualMemory(u32 sdk_version) {
    std::vector<TestGroup> groups;
    const auto Run = [&](const char* name, auto&& test) {
        TestGroup g;
        g.name = name;
        try {
            test(g, sdk_version);
        } catch (const std::exception& e) {
            g.Check("the group ran to its end", false, fmt::format("exception: {}", e.what()));
        }
        groups.push_back(std::move(g));
    };
    Run("NativeVmm", TestBasics);
    Run("NativeVmm(D)", TestDefects);
    Run("NativeVmm(model)", TestModel);
    Run("NativeVmm(mt)", TestThreads);
    Run("NativeVmm(game)", TestGameLayout);
    Run("NativeVmm(commit)", TestCommit);
    Run("NativeVmm(perf)", MeasureLatencies);
    return groups;
}

} // namespace SecondSon::Bridge
