// SecondSonPC — self-test of the bridge's memory interfaces over SecondSonPC's own manager (Phase 2, P2.3.3c;
// docs/PHASE2_P233_NATIVE_VMM.md). Everything goes through Core::GuestVirtualMemory, Core::GuestPhysicalMemory,
// Core::GuestWriteNotifier and Core::GpuMemory: on the bridge (Core::Get*(), a free window of the game's address
// space; the game is not running) and on NativeGuestMemory/NativeGpuMemory (a manager on a window above 1 TiB).
// Addresses are compared relative to the windows, direct memory relative to a block each side found free.
//   NativeMem(diff)  a scripted scenario: each step's result, the GPU notifications (order, ranges, writer lock
//                    held or not) and the window's areas identical on both sides; the transcript goes to
//                    logs/selftest_native_diff.txt
//   NativeMem(rand)  400 random operations the same way, the window's contents through the backing too; direct
//                    memory mapped without aliases (the bridge asserts on some aliases: D20)
//   NativeMem(D20)   the aliases the bridge asserts on, on the manager (on the bridge too with the environment
//                    variable SSPC_SELFTEST_BRIDGE_D20=1: the self-test then ends in the bridge's assert)
//   NativeMem(mt)    the adapters under threads, the listener querying from its callbacks; 60 s watchdog
#include "bridge/shadps4/selftest_vmm.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

#include <fmt/format.h>

#include "base/log.h"
#include "bridge/shadps4/memory_host.h"
#include "bridge/shadps4/native_memory.h"

namespace SecondSon::Bridge {
namespace {

using Core::MemoryMapFlags;
using Core::MemoryPermission;
using Core::MemoryProt;
using Core::VMAType;

constexpr u64 kPage = Memory::kPageSize;
constexpr u64 kMiB = 1ull << 20;
constexpr u64 kGuard = 0x10000;        // free on both sides of a window: nothing of the bridge merges or clamps in
constexpr u64 kWindow = 2 * kMiB;      // the scenario's addresses: [w, w + kWindow)
constexpr u64 kPhysBlock = 2 * kMiB;   // the scenario's direct memory: [pb, pb + kPhysBlock)
constexpr PAddr kNoPhys = static_cast<PAddr>(-1);
constexpr MemoryProt kRw = MemoryProt::CpuReadWrite | MemoryProt::GpuReadWrite;
constexpr MemoryProt kRead = MemoryProt::CpuRead | MemoryProt::GpuRead;

std::string transcript; ///< NativeMem(diff)'s steps, for logs/selftest_native_diff.txt

/// One side's GPU listener: the notifications that touch the window, relative to it, with the writer lock's
/// state; from OnGpuUnmap/OnGpuMap it queries like the rasterizer does (reader lock). The bridge's are handed
/// on to its listener (the rasterizer); the manager's go nowhere else.
class Recorder final : public Core::GpuMemoryListener {
public:
    Recorder(u64 window, Core::GpuMemory& gpu, std::function<bool()> writer_locked)
        : window_{window}, gpu_{gpu}, writer_locked_{std::move(writer_locked)} {}

    void OnGpuUnmap(VAddr addr, u64 size) override {
        Record('U', addr, size);
        if (next != nullptr) {
            next->OnGpuUnmap(addr, size);
        }
    }
    void OnGpuRegister(VAddr addr, u64 size) override {
        Record('R', addr, size);
        if (next != nullptr) {
            next->OnGpuRegister(addr, size);
        }
    }
    void OnGpuMap(VAddr addr, u64 size) override {
        Record('M', addr, size);
        if (next != nullptr) {
            next->OnGpuMap(addr, size);
        }
    }
    void OnGuestWrite(VAddr addr, u64 size) override {
        Record('W', addr, size);
        if (next != nullptr) {
            next->OnGuestWrite(addr, size);
        }
    }

    std::string Take() {
        std::scoped_lock lock{mutex_};
        return std::exchange(text_, {});
    }

    Core::GpuMemoryListener* next = nullptr;

private:
    void Record(char kind, VAddr addr, u64 size) {
        if (addr >= window_ + kWindow || addr + size <= window_) {
            return;
        }
        const bool locked = writer_locked_();
        if ((kind == 'U' || kind == 'M') && !locked) {
            (void)gpu_.ClampRangeSize(addr, Core::GpuMemory::MinSizeToClamp);
        }
        std::scoped_lock lock{mutex_};
        text_ += fmt::format("{}{:#x}+{:#x}{} ", kind, static_cast<s64>(addr - window_), size, locked ? "w" : "");
    }

    u64 window_;
    Core::GpuMemory& gpu_;
    std::function<bool()> writer_locked_;
    std::mutex mutex_;
    std::string text_;
};

struct Side {
    const char* name;
    Core::GuestVirtualMemory& v;
    Core::GuestPhysicalMemory& p;
    Core::GuestWriteNotifier& n;
    Core::GpuMemory& g;
    u64 w = 0;  ///< the window
    u64 pb = 0; ///< the block of direct memory
    u64 pa = 0; ///< the scenario's allocations, relative to pb
    u64 pc = 0;
    std::unique_ptr<Recorder> rec;

    u8* At(u64 offset) const {
        return reinterpret_cast<u8*>(w + offset);
    }
};

std::string Code(s32 r) {
    return r == 0 ? std::string("ok") : fmt::format("{:#x}", static_cast<u32>(r));
}

std::string Hex(const u8* data, u64 size) {
    std::string out;
    for (u64 i = 0; i < size; ++i) {
        out += fmt::format("{:02x}", data[i]);
    }
    return out;
}

std::string AreasText(Side& s) {
    std::string out;
    for (const auto& a : s.v.Areas()) {
        if (a.base >= s.w + kWindow || a.base + a.size <= s.w) {
            continue;
        }
        // Flexible memory's physical addresses are each manager's own; direct ones relative to the block.
        const std::string phys = a.type == VMAType::Direct
                                     ? fmt::format("{:#x}/{}", static_cast<s64>(a.phys - s.pb), a.phys_slices)
                                     : std::string("-");
        out += fmt::format("[{:#x}+{:#x} t{} p{:#x} {} '{}'] ", static_cast<s64>(a.base - s.w), a.size,
                           static_cast<u32>(a.type), static_cast<u32>(a.prot), phys, a.name);
    }
    return out;
}

std::string MapOp(Side& s, u64 offset, u64 size, MemoryProt prot, MemoryMapFlags flags, VMAType type,
                  std::string_view name, u64 phys = ~0ull, bool exclusive = false) {
    void* out = nullptr;
    const Core::GuestMapping m{
        .addr = s.w + offset,
        .size = size,
        .prot = prot,
        .flags = flags,
        .type = type,
        .name = name,
        .phys = phys == ~0ull ? kNoPhys : static_cast<PAddr>(s.pb + phys),
        .exclusive = exclusive,
    };
    const s32 r = s.v.Map(&out, m);
    return r == 0 ? fmt::format("ok @{:#x}", static_cast<s64>(reinterpret_cast<u64>(out) - s.w)) : Code(r);
}

std::string Alloc(Side& s, u64 size, s32 type, u64* rel) {
    const PAddr a = s.p.AllocateDirect(s.pb, s.pb + kPhysBlock, size, 0x10000, type);
    if (a == kNoPhys) {
        return "none";
    }
    *rel = a - s.pb;
    return fmt::format("{:#x}", *rel);
}

u64 Fnv(const u8* data, u64 size) {
    u64 h = 0xcbf29ce484222325ull;
    for (u64 i = 0; i < size; ++i) {
        h = (h ^ data[i]) * 0x100000001b3ull;
    }
    return h;
}

/// Runs an operation on the bridge, then on the manager; the result, the notifications and the window's
/// areas must be the same.
class Diff {
public:
    Diff(Side& bridge, Side& native) : sides_{&bridge, &native} {}

    /// "" when the same, else both sides' text.
    std::string Step(const std::function<std::string(Side&)>& op, std::string* text) {
        std::array<std::string, 2> out;
        for (size_t i = 0; i < 2; ++i) {
            Side& s = *sides_[i];
            const std::string result = op(s);
            out[i] = fmt::format("{} | {}| {}", result, s.rec->Take(), AreasText(s));
        }
        *text = out[0];
        return out[0] == out[1] ? std::string() : fmt::format("bridge: {} // native: {}", out[0], out[1]);
    }

private:
    std::array<Side*, 2> sides_;
};

// ---------------------------------------------------------------------------- the two sides

/// The bridge's free window: reserved through the interface, then given back, guards on both sides.
u64 BridgeWindow(Core::GuestVirtualMemory& v) {
    void* out = nullptr;
    const Core::GuestMapping m{.size = kWindow + 2 * kGuard, .type = VMAType::Reserved, .name = "selftest window"};
    if (v.Map(&out, m) != 0) {
        return 0;
    }
    v.Unmap(reinterpret_cast<u64>(out), m.size);
    return reinterpret_cast<u64>(out) + kGuard;
}

/// The lowest free block of a side's direct memory, found and given back.
bool FindBlock(Side& s) {
    const PAddr a = s.p.AllocateDirect(0, s.p.DirectSize(), kPhysBlock, 0x10000, 0);
    if (a == kNoPhys) {
        return false;
    }
    s.p.ReleaseDirect(a, kPhysBlock, true);
    s.pb = a;
    return true;
}

struct NativeSide {
    std::unique_ptr<Memory::VirtualMemory> vm;
    std::unique_ptr<NativeGuestMemory> guest;
    std::unique_ptr<NativeGpuMemory> gpu;
    u64 window = 0; ///< the manager's range starts here, the scenario's window kGuard further
};

NativeSide MakeNative(TestGroup& g, u64 size, u64 direct, u64 flexible, u32 sdk) {
    NativeSide n;
    n.window = FindFreeNativeWindow(size);
    std::string error = "no free window above 1 TiB";
    if (n.window != 0) {
        Memory::Layout l;
        l.ranges = {{n.window, size}};
        l.lowest = n.window;
        l.search_base = n.window;
        l.search_limit = n.window + size;
        l.system_reserved_base = n.window;
        l.system_reserved_end = n.window + size;
        l.gpu = {n.window, size};
        l.direct_size = direct;
        l.flexible_base = direct;
        l.flexible_size = flexible;
        l.sdk_version = sdk;
        n.vm = Memory::VirtualMemory::Create(l, &error);
    }
    if (n.vm == nullptr) {
        g.Check("a free window above 1 TiB and a manager on it", false, fmt::format("{:#x}: {}", n.window, error));
        return n;
    }
    n.guest = std::make_unique<NativeGuestMemory>(*n.vm, Memory::Budget{});
    n.gpu = std::make_unique<NativeGpuMemory>(*n.vm);
    return n;
}

/// Both sides with their listeners in place; the bridge's listener is restored by Close.
struct Pair {
    NativeSide native;
    std::unique_ptr<Side> b;
    std::unique_ptr<Side> n;

    bool Open(TestGroup& g, u32 sdk) {
        native = MakeNative(g, kWindow + 2 * kGuard, 64 * kMiB, 16 * kMiB, sdk);
        if (native.vm == nullptr) {
            return false;
        }
        auto& bv = Core::GetGuestVirtualMemory();
        auto& bg = Core::GetGpuMemory();
        b = std::make_unique<Side>(Side{"bridge", bv, Core::GetGuestPhysicalMemory(), Core::GetGuestWriteNotifier(), bg});
        n = std::make_unique<Side>(Side{"native", *native.guest, *native.guest, *native.guest, *native.gpu});
        b->w = BridgeWindow(bv);
        n->w = native.window + kGuard;
        if (b->w == 0 || !FindBlock(*b) || !FindBlock(*n)) {
            g.Check("a free window and a free block of direct memory on both sides", false,
                    fmt::format("bridge {:#x} {:#x} native {:#x} {:#x}", b->w, b->pb, n->w, n->pb));
            return false;
        }
        b->rec = std::make_unique<Recorder>(b->w, bg, [] { return BridgeMemoryWriterLocked(); });
        Memory::VirtualMemory* vm = native.vm.get();
        n->rec = std::make_unique<Recorder>(n->w, *native.gpu, [vm] { return vm->WriterLocked(); });
        b->rec->next = bg.SetListener(b->rec.get());
        n->rec->next = native.gpu->SetListener(n->rec.get());
        return true;
    }

    void Close() {
        if (b != nullptr && b->rec != nullptr) {
            Core::GetGpuMemory().SetListener(b->rec->next);
        }
        if (native.gpu != nullptr) {
            native.gpu->SetListener(nullptr);
        }
    }
};

// ---------------------------------------------------------------------------- NativeMem(diff)

void TestScenario(TestGroup& g, u32 sdk) {
    Pair pair;
    if (!pair.Open(g, sdk)) {
        pair.Close();
        return;
    }
    Diff diff{*pair.b, *pair.n};
    transcript = fmt::format("bridge window {:#x} block {:#x}; native window {:#x} block {:#x}\n", pair.b->w,
                             pair.b->pb, pair.n->w, pair.n->pb);
    const auto Step = [&](const std::string& what, const std::function<std::string(Side&)>& op) {
        std::string text;
        const std::string difference = diff.Step(op, &text);
        transcript += fmt::format("{}: {}\n", what, difference.empty() ? text : difference);
        g.Check("same on the bridge and the manager: " + what, difference.empty(), difference);
    };

    // The window by offset: A [0, 0x40000) direct type 0; C [0x40000, 0x50000) direct type 3; an alias of A's
    // second half at 0x60000; F flexible at 0x100000; S a stack at 0x140000; R reserved at 0x180000.
    Step("allocate A (256 KiB, type 0) in the block", [](Side& s) { return Alloc(s, 0x40000, 0, &s.pa); });
    Step("allocate C (64 KiB, type 3)", [](Side& s) { return Alloc(s, 0x10000, 3, &s.pc); });
    Step("map A (direct, fixed)", [](Side& s) {
        return MapOp(s, 0, 0x40000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "diff A", s.pa);
    });
    Step("map C right after A, same name, another memory type", [](Side& s) {
        return MapOp(s, 0x40000, 0x10000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "diff A", s.pc);
    });
    Step("map A's second half again at 0x60000 (an alias)", [](Side& s) {
        return MapOp(s, 0x60000, 0x20000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "alias", s.pa + 0x20000);
    });
    Step("map flexible memory at 0x100000", [](Side& s) {
        return MapOp(s, 0x100000, 0x20000, MemoryProt::CpuReadWrite, MemoryMapFlags::Fixed, VMAType::Flexible,
                     "diff F");
    });
    Step("map a stack at 0x140000", [](Side& s) {
        return MapOp(s, 0x140000, 0x10000, MemoryProt::CpuReadWrite, MemoryMapFlags::Fixed, VMAType::Stack,
                     "diff S");
    });
    Step("reserve 0x180000", [](Side& s) {
        return MapOp(s, 0x180000, 0x40000, MemoryProt::NoAccess, MemoryMapFlags::Fixed, VMAType::Reserved, "diff R");
    });
    Step("reserve, searching from 0x1E0000", [](Side& s) {
        return MapOp(s, 0x1E0000, 0x10000, MemoryProt::NoAccess, MemoryMapFlags::NoFlags, VMAType::Reserved,
                     "diff search");
    });
    Step("map direct memory over part of the reservation", [](Side& s) {
        return MapOp(s, 0x190000, 0x10000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "over R", s.pa);
    });
    Step("no-overwrite over A", [](Side& s) {
        return MapOp(s, 0, 0x10000, MemoryProt::NoAccess, MemoryMapFlags::Fixed | MemoryMapFlags::NoOverwrite,
                     VMAType::Reserved, "x");
    });
    Step("exclusive map of A's mapped pages", [](Side& s) {
        return MapOp(s, 0x1C0000, 0x10000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "excl", s.pa, true);
    });
    Step("protect part of A read-only", [](Side& s) { return Code(s.v.Protect(s.w + 0x10000, 0x10000, kRead)); });
    Step("protect it back", [](Side& s) { return Code(s.v.Protect(s.w + 0x10000, 0x10000, kRw)); });
    Step("protect across A and C read-only", [](Side& s) {
        return Code(s.v.Protect(s.w + 0x30000, 0x20000, kRead));
    });
    Step("name C", [](Side& s) {
        s.v.NameRange(s.w + 0x40000, 0x10000, "diff C");
        return std::string("done");
    });
    Step("valid ranges", [](Side& s) {
        return fmt::format("{} {} {}", s.v.IsValid(s.w, kWindow), s.v.IsValid(s.w + kWindow - kPage, kPage),
                           s.v.IsValid(s.w, 0));
    });
    Step("write across A and C through the backing, read back", [](Side& s) {
        std::array<u8, 32> data{};
        for (size_t i = 0; i < data.size(); ++i) {
            data[i] = static_cast<u8>(0x40 + i);
        }
        // Neither manager clears direct memory when it hands it out (the bridge's still holds earlier tests'
        // bytes): the 64 bytes around are zeroed first, the same way.
        const std::array<u8, 64> zeros{};
        const bool cleared = s.g.TryWriteBacking(s.At(0x3FFE0), zeros.data(), zeros.size());
        const bool ok = cleared && s.g.TryWriteBacking(s.At(0x3FFF0), data.data(), data.size());
        std::array<u8, 64> back{};
        s.g.CopySparseMemory(s.w + 0x3FFE0, back.data(), back.size());
        return fmt::format("{} {}", ok, Hex(back.data(), back.size()));
    });
    Step("host writes into F and S, read through the backing (S and R have none)", [](Side& s) {
        for (u64 at : {0x100010ull, 0x140010ull}) {
            for (u64 i = 0; i < 16; ++i) {
                *s.At(at + i) = static_cast<u8>(0x80 + i);
            }
        }
        std::array<u8, 32> f{}, st{}, r{};
        s.g.CopySparseMemory(s.w + 0x100000, f.data(), f.size());
        s.g.CopySparseMemory(s.w + 0x140000, st.data(), st.size());
        s.g.CopySparseMemory(s.w + 0x180000, r.data(), r.size());
        return fmt::format("{} {} {}", Hex(f.data(), 32), Hex(st.data(), 32), Hex(r.data(), 32));
    });
    Step("tracking protection of A's first page: none, then read/write", [](Side& s) {
        MEMORY_BASIC_INFORMATION none{}, rw{};
        s.g.ProtectTracking(s.w, 0x10000, MemoryPermission::None);
        VirtualQuery(s.At(0), &none, sizeof(none));
        s.g.ProtectTracking(s.w, 0x10000, MemoryPermission::ReadWrite);
        VirtualQuery(s.At(0), &rw, sizeof(rw));
        return fmt::format("{:#x} {:#x}", none.Protect, rw.Protect);
    });
    Step("clamp from A, from the alias, from a free page, a small range", [](Side& s) {
        return fmt::format("{:#x} {:#x} {:#x} {:#x}", s.g.ClampRangeSize(s.w, 2ull << 30),
                           s.g.ClampRangeSize(s.w + 0x60000, 1ull << 30),
                           s.g.ClampRangeSize(s.w + 0x50000, 1ull << 30), s.g.ClampRangeSize(s.w, 0x1000));
    });
    Step("a host write is announced", [](Side& s) {
        s.n.NotifyHostWrite(s.w + 0x100, 0x200);
        return std::string("done");
    });
    Step("unmap across A's end and C's start", [](Side& s) { return Code(s.v.Unmap(s.w + 0x30000, 0x18000)); });
    Step("release C while the rest of it is mapped", [](Side& s) {
        return Code(s.p.ReleaseDirect(s.pb + s.pc, 0x10000, true));
    });
    Step("release A (its views are unmapped first)", [](Side& s) {
        return Code(s.p.ReleaseDirect(s.pb + s.pa, 0x40000, true));
    });
    Step("release A again", [](Side& s) { return Code(s.p.ReleaseDirect(s.pb + s.pa, 0x40000, true)); });
    Step("unmap the window", [](Side& s) { return Code(s.v.Unmap(s.w, kWindow)); });

    const std::string inv = pair.native.vm->CheckInvariants();
    g.Check("the manager's invariants hold after the scenario", inv.empty(), inv);
    pair.Close();
}

// ---------------------------------------------------------------------------- NativeMem(rand)

void TestRandom(TestGroup& g, u32 sdk) {
    Pair pair;
    if (!pair.Open(g, sdk)) {
        pair.Close();
        return;
    }
    Diff diff{*pair.b, *pair.n};
    // Direct memory without aliases: the window's [0, 1 MiB) maps P (type 0) page for page, [1 MiB, kSpan) Q
    // (type 3, not adjacent to P), so no physical page is mapped twice (D20). Both zeroed through a view first.
    constexpr u64 kSpan = kWindow - 0x10000; // the window's last 64 KiB stay free: clamps stop there
    constexpr u64 kP = 0x100000;
    constexpr u64 kQ = kSpan - kP;
    constexpr u64 kQAt = kP + 0x10000;
    std::string text;
    std::string difference = diff.Step(
        [](Side& s) {
            std::string r = Alloc(s, kP, 0, &s.pa);
            const PAddr q = s.p.AllocateDirect(s.pb + kQAt, s.pb + kPhysBlock, kQ, 0x10000, 3);
            s.pc = q - s.pb;
            r += q == kNoPhys ? std::string(" none") : fmt::format(" {:#x}", s.pc);
            MapOp(s, 0, kP, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "zero", s.pa);
            MapOp(s, kP, kQ, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "zero", s.pc);
            std::memset(s.At(0), 0, kSpan);
            s.v.Unmap(s.w, kSpan);
            s.rec->Take();
            return r;
        },
        &text);
    constexpr int kOps = 400;
    std::mt19937_64 rng(0x5EC0'2333);
    int done = 0;
    static constexpr MemoryProt kProts[] = {kRw, MemoryProt::CpuReadWrite, kRead, MemoryProt::CpuRead};
    static constexpr const char* kNames[] = {"r0", "r1"};
    for (; done < kOps && difference.empty(); ++done) {
        const u64 kind = rng() % 13;
        const u64 pages = 1 + rng() % 8;
        const u64 off = (rng() % (kSpan / kPage - 16)) * kPage;
        const MemoryProt prot = kProts[rng() % 4];
        const char* name = kNames[rng() % 2];
        const u64 seed = rng();
        // An area of the window to protect or name (the bridge asserts on free memory: D5).
        std::vector<Core::GuestArea> areas;
        for (auto& a : pair.b->v.Areas()) {
            if (a.base >= pair.b->w && a.base < pair.b->w + kWindow) {
                areas.push_back(a);
            }
        }
        std::function<std::string(Side&)> op;
        switch (kind) {
        case 0:
        case 1:
        case 2:
            op = [=](Side& s) {
                // The page's own physical page: P below 1 MiB, Q above, never across.
                const u64 end = off < kP ? kP : kSpan;
                const u64 n = std::min<u64>(pages, (end - off) / kPage);
                const u64 phys = off < kP ? s.pa + off : s.pc + (off - kP);
                return MapOp(s, off, n * kPage, prot, MemoryMapFlags::Fixed, VMAType::Direct, name, phys);
            };
            break;
        case 3:
            op = [=](Side& s) {
                return MapOp(s, off, std::min<u64>(pages, 4) * kPage, MemoryProt::CpuReadWrite,
                             MemoryMapFlags::Fixed, VMAType::Flexible, name);
            };
            break;
        case 4:
            op = [=](Side& s) {
                return MapOp(s, off, pages * kPage, MemoryProt::NoAccess, MemoryMapFlags::Fixed, VMAType::Reserved,
                             name);
            };
            break;
        case 5:
        case 6:
            op = [=](Side& s) { return Code(s.v.Unmap(s.w + off, (pages * 2) * kPage)); };
            break;
        case 7:
        case 8: {
            if (areas.empty()) {
                op = [](Side&) { return std::string("no area"); };
                break;
            }
            const auto& a = areas[seed % areas.size()];
            const u64 a_pages = a.size / kPage;
            const u64 first = (seed >> 16) % a_pages;
            const u64 count = 1 + (seed >> 32) % (a_pages - first);
            const u64 rel = a.base - pair.b->w + first * kPage;
            if (kind == 7) {
                op = [=](Side& s) { return Code(s.v.Protect(s.w + rel, count * kPage, prot)); };
            } else {
                op = [=](Side& s) {
                    s.v.NameRange(s.w + rel, count * kPage, name);
                    return std::string("named");
                };
            }
            break;
        }
        case 9:
            op = [=](Side& s) {
                std::vector<u8> data(1 + seed % 0x8000);
                std::mt19937_64 fill(seed);
                for (auto& b : data) {
                    b = static_cast<u8>(fill());
                }
                const u64 at = std::min<u64>(off + (seed >> 40) % kPage, kSpan - data.size());
                return fmt::format("{}", s.g.TryWriteBacking(s.At(at), data.data(), data.size()));
            };
            break;
        case 10:
            op = [=](Side& s) { return fmt::format("{:#x}", s.g.ClampRangeSize(s.w + off, 1ull << 30)); };
            break;
        case 11:
            op = [](Side& s) {
                std::vector<u8> all(kWindow);
                s.g.CopySparseMemory(s.w, all.data(), all.size());
                return fmt::format("{:016x}", Fnv(all.data(), all.size()));
            };
            break;
        default:
            op = [](Side& s) {
                // P given back (its views unmapped first) and taken again: the same block, its data kept.
                const std::string r = Code(s.p.ReleaseDirect(s.pb + s.pa, kP, true));
                return r + " " + Alloc(s, kP, 0, &s.pa);
            };
            break;
        }
        difference = diff.Step(op, &text);
        if (!difference.empty()) {
            difference = fmt::format("operation {} (kind {}): {}", done, kind, difference);
        }
    }
    const int ran = done;
    // The contents once more, then everything given back.
    if (difference.empty()) {
        difference = diff.Step(
            [](Side& s) {
                std::vector<u8> all(kWindow);
                s.g.CopySparseMemory(s.w, all.data(), all.size());
                const std::string sum = fmt::format("{:016x}", Fnv(all.data(), all.size()));
                return sum + " " + Code(s.v.Unmap(s.w, kWindow)) + " " +
                       Code(s.p.ReleaseDirect(s.pb + s.pa, kP, true)) + " " +
                       Code(s.p.ReleaseDirect(s.pb + s.pc, kQ, true));
            },
            &text);
    } else {
        for (Side* s : {pair.b.get(), pair.n.get()}) {
            s->v.Unmap(s->w, kWindow);
            s->p.ReleaseDirect(s->pb + s->pa, kP, false);
            s->p.ReleaseDirect(s->pb + s->pc, kQ, false);
        }
    }
    const std::string inv = pair.native.vm->CheckInvariants();
    g.Check(fmt::format("{} random operations (map direct/flexible/reserved, unmap, protect, name, backing writes, "
                        "clamps, contents, release and allocate again): results, notifications, areas and contents "
                        "the same on the bridge and the manager; invariants hold",
                        kOps),
            difference.empty() && ran == kOps && inv.empty(), difference + inv);
    pair.Close();
}

// ---------------------------------------------------------------------------- NativeMem(D20)

/// D20: the bridge keeps one state per physical area (allocated or mapped), not a count of the views. Two views
/// of the same physical pages that overlap only partly: unmapping the smaller one marks its pages unmapped, the
/// physical area splits, and unmapping the larger one asks CarvePhysArea for a range that no longer fits in one
/// area: "Mapping cannot fit inside free region" (MemoryManager::UnmapMemoryImpl; the same from Free when it
/// unmaps the views of released memory). The manager counts the views per page.
std::string AliasSequence(Side& s) {
    std::string out = Alloc(s, 0x10000, 0, &s.pa);
    out += " " + MapOp(s, 0x20000, 0xC000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "alias big", s.pa);
    out += " " + MapOp(s, 0, 0x8000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "alias small", s.pa);
    out += " " + Code(s.v.Unmap(s.w, 0x8000));
    out += " " + Code(s.v.Unmap(s.w + 0x20000, 0xC000));
    // And through a release: both views again, the memory given back with them mapped.
    out += " " + MapOp(s, 0x20000, 0xC000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "alias big", s.pa);
    out += " " + MapOp(s, 0, 0x8000, kRw, MemoryMapFlags::Fixed, VMAType::Direct, "alias small", s.pa);
    out += " " + Code(s.p.ReleaseDirect(s.pb + s.pa, 0x10000, true));
    return out + " | " + AreasText(s);
}

void TestAliases(TestGroup& g, u32 sdk) {
    Pair pair;
    if (!pair.Open(g, sdk)) {
        pair.Close();
        return;
    }
    const std::string native = AliasSequence(*pair.n);
    pair.n->rec->Take();
    const std::string inv = pair.native.vm->CheckInvariants();
    g.Check("D20: two partly overlapping views of the same direct memory, unmapped one after the other, then "
            "released while mapped: all succeed on the manager, nothing left, invariants hold (the bridge asserts)",
            native == "0x0 ok @0x20000 ok @0x0 ok ok ok @0x20000 ok @0x0 ok | " && inv.empty(), native + " " + inv);
    char flag[8]{};
    if (GetEnvironmentVariableA("SSPC_SELFTEST_BRIDGE_D20", flag, sizeof(flag)) != 0 && flag[0] == '1') {
        SSPC_LOG(App, Warning, "selftest: SSPC_SELFTEST_BRIDGE_D20=1, the bridge's D20 next (expected to assert)");
        Log::Flush();
        const std::string bridge = AliasSequence(*pair.b);
        g.Check("D20 on the bridge (SSPC_SELFTEST_BRIDGE_D20=1): no assert, the same as the manager",
                bridge == native, bridge);
    }
    pair.Close();
}

// ---------------------------------------------------------------------------- NativeMem(mt)

/// Counts the notifications; OnGpuRegister must come with the writer lock held; from OnGpuUnmap/OnGpuMap it
/// reads like the rasterizer does (reader lock: a deadlock here is the watchdog's).
class CountingListener final : public Core::GpuMemoryListener {
public:
    CountingListener(Memory::VirtualMemory& vm, Core::GpuMemory& gpu) : vm_{vm}, gpu_{gpu} {}
    void OnGpuUnmap(VAddr addr, u64 size) override {
        ++unmaps;
        Query(addr);
    }
    void OnGpuRegister(VAddr, u64) override {
        ++registers;
        unlocked_registers += vm_.WriterLocked() ? 0 : 1;
    }
    void OnGpuMap(VAddr addr, u64 size) override {
        ++maps;
        Query(addr);
    }
    void OnGuestWrite(VAddr, u64) override {
        ++writes;
    }
    std::atomic<u64> unmaps{0}, registers{0}, maps{0}, writes{0}, unlocked_registers{0};

private:
    void Query(VAddr addr) {
        std::array<u8, 16> buffer{};
        gpu_.CopySparseMemory(addr, buffer.data(), buffer.size());
        (void)gpu_.ClampRangeSize(addr, Core::GpuMemory::MinSizeToClamp);
    }
    Memory::VirtualMemory& vm_;
    Core::GpuMemory& gpu_;
};

void TestThreads(TestGroup& g, u32 sdk) {
    constexpr int kWriters = 4;
    constexpr u64 kSlice = 2 * kMiB;
    constexpr u64 kShared = 0x40000;
    NativeSide native = MakeNative(g, (kWriters + 1) * kSlice, 32 * kMiB, 8 * kMiB, sdk);
    if (native.vm == nullptr) {
        return;
    }
    const u64 w = native.window;
    NativeGuestMemory& guest = *native.guest;
    NativeGpuMemory& gpu = *native.gpu;
    CountingListener listener{*native.vm, gpu};
    const bool no_previous = gpu.SetListener(&listener) == nullptr;

    // A stable view the readers check all the time.
    const PAddr shared_phys = guest.AllocateDirect(0, guest.DirectSize(), kShared, 0x10000, 0);
    void* shared_out = nullptr;
    const s32 shared_map = guest.Map(&shared_out, Core::GuestMapping{.addr = w + kWriters * kSlice,
                                                                     .size = kShared,
                                                                     .prot = kRw,
                                                                     .flags = MemoryMapFlags::Fixed,
                                                                     .type = VMAType::Direct,
                                                                     .name = "shared",
                                                                     .phys = shared_phys});
    if (shared_phys == kNoPhys || shared_map != 0) {
        g.Check("the shared view", false, fmt::format("{:#x} {:#x}", shared_phys, static_cast<u32>(shared_map)));
        gpu.SetListener(nullptr);
        return;
    }
    u8* const shared = static_cast<u8*>(shared_out);
    const auto Pattern = [](u64 o) { return static_cast<u8>(o * 31 + 7); };
    for (u64 o = 0; o < kShared; ++o) {
        shared[o] = Pattern(o);
    }

    std::atomic<int> finished{0}, errors{0};
    std::atomic<bool> stop{false};
    std::vector<std::thread> threads;
    for (int k = 0; k < kWriters; ++k) {
        threads.emplace_back([&, k] {
            std::mt19937_64 rng(0xADA0 + k);
            const u64 base = w + k * kSlice;
            const PAddr block = guest.AllocateDirect(0, guest.DirectSize(), 0x40000, 0x10000, k % 2 == 0 ? 0 : 3);
            errors += block == kNoPhys ? 1 : 0;
            for (int i = 0; i < 300 && block != kNoPhys; ++i) {
                const u64 pages = 1 + rng() % 8;
                const u64 at = base + (rng() % (kSlice / kPage - 8)) * kPage;
                void* out = nullptr;
                switch (rng() % 5) {
                case 0:
                    errors += guest.Map(&out, {.addr = at,
                                               .size = pages * kPage,
                                               .prot = kRw,
                                               .flags = MemoryMapFlags::Fixed,
                                               .type = VMAType::Direct,
                                               .name = "mt",
                                               .phys = block + (rng() % (16 - pages + 1)) * kPage}) == 0
                                  ? 0
                                  : 1;
                    break;
                case 1:
                    // Flexible memory may run out with the other writers: ENOMEM is an answer, not an error.
                    (void)guest.Map(&out, {.addr = at,
                                           .size = std::min<u64>(pages, 4) * kPage,
                                           .prot = MemoryProt::CpuReadWrite,
                                           .flags = MemoryMapFlags::Fixed,
                                           .type = VMAType::Flexible,
                                           .name = "mt"});
                    break;
                case 2:
                    errors += guest.Unmap(at, pages * kPage) == 0 ? 0 : 1;
                    break;
                case 3: {
                    const auto areas = guest.Areas();
                    for (const auto& a : areas) {
                        if (a.base >= base && a.base < base + kSlice) {
                            errors += guest.Protect(a.base, a.size, (rng() & 1) != 0 ? kRead : kRw) == 0 ? 0 : 1;
                            guest.NameRange(a.base, a.size, "mt named");
                            break;
                        }
                    }
                    break;
                }
                default:
                    errors += guest.Map(&out, {.addr = at,
                                               .size = pages * kPage,
                                               .flags = MemoryMapFlags::Fixed,
                                               .type = VMAType::Reserved,
                                               .name = "mt"}) == 0
                                  ? 0
                                  : 1;
                    break;
                }
            }
            errors += guest.Unmap(base, kSlice) == 0 ? 0 : 1;
            if (block != kNoPhys) {
                errors += guest.ReleaseDirect(block, 0x40000, true) == 0 ? 0 : 1;
            }
            ++finished;
        });
    }
    for (int k = 0; k < 2; ++k) {
        threads.emplace_back([&, k] {
            std::mt19937_64 rng(0xBEE0 + k);
            while (!stop) {
                const u64 off = rng() % (kShared - 64);
                std::array<u8, 64> buffer{};
                gpu.CopySparseMemory(reinterpret_cast<u64>(shared) + off, buffer.data(), buffer.size());
                for (u64 j = 0; j < buffer.size(); ++j) {
                    errors += buffer[j] == Pattern(off + j) ? 0 : 1;
                }
                errors += gpu.TryWriteBacking(shared + off, buffer.data(), buffer.size()) ? 0 : 1;
                errors += gpu.ClampRangeSize(reinterpret_cast<u64>(shared), 1ull << 30) == kShared ? 0 : 1;
                gpu.ProtectTracking(reinterpret_cast<u64>(shared), kShared, MemoryPermission::ReadWrite);
                errors += guest.IsValid(w + rng() % ((kWriters + 1) * kSlice), 0) ? 0 : 1;
                guest.NotifyHostWrite(reinterpret_cast<u64>(shared) + off, 64);
            }
        });
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (finished < kWriters && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (finished < kWriters) {
        SSPC_LOG(App, Critical, "selftest: the native memory adapters' threads did not finish in 60 s (deadlock)");
        Log::Flush();
        std::quick_exit(5);
    }
    stop = true;
    for (auto& t : threads) {
        t.join();
    }
    bool intact = true;
    for (u64 o = 0; o < kShared; ++o) {
        intact = intact && shared[o] == Pattern(o);
    }
    const u64 events = listener.unmaps + listener.registers + listener.maps;
    // Without a listener nothing is heard any more.
    const bool was_mine = gpu.SetListener(nullptr) == &listener;
    const u64 maps_before = listener.maps;
    void* out = nullptr;
    const s32 again = guest.Map(&out, {.addr = w,
                                       .size = kPage,
                                       .prot = kRw,
                                       .flags = MemoryMapFlags::Fixed,
                                       .type = VMAType::Direct,
                                       .name = "after",
                                       .phys = shared_phys});
    const bool silent = again == 0 && listener.maps == maps_before;
    guest.Unmap(w, kPage);
    guest.Unmap(reinterpret_cast<u64>(shared), kShared);
    const s32 released = guest.ReleaseDirect(shared_phys, kShared, true);
    const std::string inv = native.vm->CheckInvariants();
    const bool empty = guest.Areas().empty() && native.vm->FlexibleUsed() == 0;
    g.Check("4 writers (direct, flexible, reserved: map, protect, name, unmap, release) and 2 readers (copy, write, "
            "clamp, tracking protection, valid, host writes) through the adapters: no error, no deadlock (60 s "
            "watchdog), OnGpuRegister always under the writer lock, the shared view intact, everything given back, "
            "invariants hold; SetListener hands back the previous one and nothing is heard without one",
            errors == 0 && listener.unlocked_registers == 0 && events > 0 && listener.writes > 0 && intact &&
                no_previous && was_mine && silent && released == 0 && empty && inv.empty(),
            fmt::format("errors {} unlocked {} events {} writes {} intact {} listener {} {} silent {} released {:#x} "
                        "empty {} {}",
                        errors.load(), listener.unlocked_registers.load(), events, listener.writes.load(), intact,
                        no_previous, was_mine, silent, static_cast<u32>(released), empty, inv));
    g.Info(fmt::format("threads through the adapters: notifications unmap {} register {} map {} write {}",
                       listener.unmaps.load(), listener.registers.load(), listener.maps.load(),
                       listener.writes.load()));
}

} // namespace

std::vector<TestGroup> TestNativeMemoryAdapters(u32 sdk_version, std::string* diff_transcript) {
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
    Run("NativeMem(diff)", TestScenario);
    Run("NativeMem(rand)", TestRandom);
    Run("NativeMem(D20)", TestAliases);
    Run("NativeMem(mt)", TestThreads);
    *diff_transcript = std::move(transcript);
    return groups;
}

} // namespace SecondSon::Bridge
