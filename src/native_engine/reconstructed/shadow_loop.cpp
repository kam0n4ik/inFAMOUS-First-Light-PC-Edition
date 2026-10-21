// SecondSonPC — the shadow pass's orchestrators 0x43d590 / 0x43d480 in the registry (see shadow_loop.h).
#include "native_engine/reconstructed/shadow_loop.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <random>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_hooks.h"
#include "engine/guest_probe.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

namespace SecondSon::NativeEngine::Reconstructed {

namespace SL = ShadowLoop;
namespace ST = STrace;

namespace {

constexpr Engine::GuestFunction kLoopFunction{0x43d590, 0xdb9, 0xc098a21924b37b67ull};
constexpr Engine::GuestFunction kDrawFunction{0x43d480, 0x106, 0xaffc4c64777b1e37ull};
/// Run for real in a recording run (into the shadow context or the run's frame): pinned. Not 0x11c890 (the first
/// block, R+0xd190): R5's object_constants may have its jump on it - it then runs as that reconstruction, verified
/// equal; its body is pinned by that spec (no calls out: it writes the context only).
constexpr Engine::GuestFunction kAssumedLoop[] = {
    {0x11cde0, 0x459, 0x7195e831ed1fc41cull}, // the second block -> R+0xd2f0 (no calls out)
    {0x612110, 0x3d, 0x648f6c4eec62c132ull},  // the generator (run on the run's copy of its state)
    {0x612150, 0xf0, 0x8ee9e1692a6269e2ull},  // its refill
    {0x61e260, 0xce, 0x82958d25b30182e7ull},  // Gnm initAsRegularBuffer (-> 0x61f430)
    {0x61f430, 0x33, 0x75d0a6829a95f352ull},  // Gnm DataFormat
};
constexpr Engine::GuestFunction kAssumedDraw[] = {
    {0x61e260, 0xce, 0x82958d25b30182e7ull},
    {0x61f430, 0x33, 0x75d0a6829a95f352ull},
};

constexpr u64 kStackGuardSlot = 0xd9de00; ///< GOT: __stack_chk_guard
constexpr u64 kPrngState = 0x7f92020;     ///< the generator's object (0x810 bytes)
constexpr u64 kPrngRva = 0x612110;
constexpr u32 kPrngBytes = 0x810;

std::atomic<u64> g_module{0};
SL::LoopFn g_loop_impl = nullptr;
SL::DrawFn g_draw_impl = nullptr;
SL::RedirectsFn g_redirects = nullptr;
SL::LoopFn g_rec_loop = nullptr; ///< recording clones
SL::DrawFn g_rec_draw = nullptr;
u32 g_fault = 0;

// --- the recorded callees ------------------------------------------------------------------------------------------

u64 StageEffect(ST::Trace& t, const ST::CalleeSpec&, ST::CallIn&, const u64*) {
    return ST::Next(t) & 0xffffffffull; // 0x27530: the CUE stage of the pass (the same sequence in both runs)
}

u64 DcbAllocEffect(ST::Trace& t, const ST::CalleeSpec&, ST::CallIn& in, const u64*) {
    // 0x11be90(dst, context, bytes): a DCB allocation at *dst, its V# at dst + 8 (dst: in the shadow context)
    const u64 p = t.Allocate(static_cast<u32>(in.gpr[2]), 16);
    auto* dst = reinterpret_cast<u64*>(in.gpr[0]);
    dst[0] = p;
    dst[1] = ST::Next(t);
    dst[2] = ST::Next(t);
    return 0;
}

u64 ScratchAllocEffect(ST::Trace& t, const ST::CalleeSpec&, ST::CallIn& in, const u64*) {
    // 0x1f0870(state, bytes, alignment): the frame scratch; null now and then (the loop skips the group)
    if (ST::Next(t) % 16 == 0) {
        return 0;
    }
    return t.Allocate(static_cast<u32>(in.gpr[1]), in.gpr[2]);
}

u64 PrngEffect(ST::Trace& t, const ST::CalleeSpec&, ST::CallIn&, const u64*) {
    // 0x612110(&state): the real generator on the run's copy of the state (t.user[0])
    using Fn = SSPC_SYSV_ABI u64 (*)(u64);
    return reinterpret_cast<Fn>(g_module.load(std::memory_order_relaxed) + kPrngRva)(t.user[0]) & 0xffffffffull;
}

// the tables of the offline differentials (r6_shadows_tests.cpp), the generator added: in the game it runs on the
// run's copy of its state (the offline test ran it for real on the image's, saved and restored around each run)
constexpr ST::CalleeSpec kDrawCallees[] = {
    {0x120280, "commit draw constants 0x120280", 1, 0, 0, 0, {}, nullptr},
    {0x27530, "current stage 0x27530", 1, 0b1, 0, 0, {}, &StageEffect},
    {0x16b70, "set buffers 0x16b70", 5, 0b1110, 0, 0, {ST::Snap{4, -1, 16, 16}, {}}, nullptr},
    {0x16aa0, "set vertex buffer 0x16aa0", 5, 0b1110, 0, 0, {}, nullptr},
    {0x6250b0, "num instances 0x6250b0", 2, 0b10, 0, 0, {}, nullptr},
    {0x623740, "primitive type 0x623740", 2, 0b10, 0, 0, {}, nullptr},
    {0x624f80, "index size 0x624f80", 3, 0b110, 0, 0, {}, nullptr},
    {0x14ad0, "CUE commit 0x14ad0", 1, 0, 0, 0, {}, nullptr},
    {0x623c60, "draw index 0x623c60", 4, 0b1010, 0, 0, {}, nullptr},
};
constexpr u32 kDrawCallees_n = static_cast<u32>(std::size(kDrawCallees));

constexpr ST::CalleeSpec kLoopCallees[] = {
    {0x11be90, "DCB alloc 0x11be90", 3, 0, 0, 0, {}, &DcbAllocEffect},
    {0x11bed0, "bind constants 0x11bed0", 4, 0, 0, 0, {}, nullptr},
    {0x120280, "commit draw constants 0x120280", 1, 0, 0, 0, {}, nullptr},
    {0x16b70, "set buffers 0x16b70", 5, 0, 0, 0, {ST::Snap{4, -1, 16, 16}, {}}, nullptr},
    {0x1a1960, "mesh draw 0x1a1960", 3, 0, 0, 0, {}, nullptr},
    {0x1f0870, "frame scratch 0x1f0870", 3, 0, 0, 0, {}, &ScratchAllocEffect},
    {0x43d480, "instanced shadow draw 0x43d480", 5, 0, 0, 0, {}, nullptr},
    {0x448640, "material bind 0x448640", 3, 0, 0, 0, {}, nullptr},
    {0x450460, "parameter writer 0x450460", 6, 0, 0, 1, {}, nullptr},
    {0x451290, "parameter writer 0x451290", 6, 0, 0, 0, {}, nullptr},
    {0x451700, "instance UV 0x451700", 6, 0, 0, 0, {}, nullptr},
    {kPrngRva, "generator 0x612110", 1, 0, 0, 0, {}, &PrngEffect},
};
constexpr u32 kLoopCallees_n = static_cast<u32>(std::size(kLoopCallees));
static_assert(kLoopCallees_n <= 32 && kDrawCallees_n <= 32, "recomp_set_redirects takes 32");

// --- the shadow runs ---------------------------------------------------------------------------------------------
//
// As in the offline differential: both runs work on the same shadow buffers (the context, the allocations, the
// generator's state) at the same addresses - the first run's results are saved, the buffers restored, the second
// run made, the results compared. Pointers the effects store (a DCB allocation into the context) are then equal.

// a call with more recorded calls / snapshot bytes / allocations than this is not compared (Slot::skipped)
constexpr u32 kMaxCalls = 16384;
constexpr u32 kSnapBytes = 0x40000;
constexpr u64 kArenaBytes = 0x400000;

struct Shadow {
    ST::Trace t[2];
    ST::Call calls[2][kMaxCalls];
    alignas(64) u8 snap[2][kSnapBytes];
    alignas(64) u8 arena[kArenaBytes];      ///< the runs' allocations
    alignas(64) u8 arena_first[kArenaBytes]; ///< the first run's, saved
    alignas(64) u8 r[SL::kShadowBytes];      ///< the shadow context the runs write
    alignas(64) u8 r_live[SL::kShadowBytes]; ///< the live context as read
    alignas(64) u8 r_first[SL::kShadowBytes];
    alignas(64) u8 prng[kPrngBytes];
    alignas(64) u8 prng_live[kPrngBytes];
    alignas(64) u8 prng_first[kPrngBytes];
    u64 args[4];
    u64 arena_dirty = 0; ///< arena bytes the last run may have written
};
thread_local Shadow* t_shadow = nullptr;

Shadow& Shadows() {
    if (t_shadow == nullptr) { // per verifying thread (~17 MB, zeroed), never freed (off the guest stack)
        t_shadow = new Shadow();
    }
    return *t_shadow;
}

ST::Trace& BeginRun(Shadow& s, u32 k, const ST::CalleeSpec* callees, u32 n, u64 stack_top, bool context) {
    ST::Trace& t = s.t[k];
    t.callees = callees;
    t.ncallees = n;
    t.calls = s.calls[k];
    t.max_calls = kMaxCalls;
    t.snap = s.snap[k];
    t.snap_cap = kSnapBytes;
    t.arena = context ? s.arena : nullptr;
    t.arena_cap = context ? kArenaBytes : 0;
    t.nregions = 0;
    t.Begin(0x5348414430353930ull, stack_top);
    if (context) {
        // the allocations start zeroed (bytes a function leaves unwritten compare equal)
        std::memset(s.arena, 0, static_cast<size_t>(s.arena_dirty));
        s.arena_dirty = 0;
        t.AddRegion(reinterpret_cast<u64>(s.r), SL::kShadowBytes);
        t.user[0] = reinterpret_cast<u64>(s.prng);
    }
    return t;
}

/// Runs `body` with the calling thread's trace active; the implementation's calls aimed at the thunks.
template <typename F>
void Recorded(ST::Trace& t, bool original, const ST::CalleeSpec* callees, u32 n, F&& body) {
    ST::Trace* const saved = ST::Active();
    ST::SetActive(&t);
    if (original) {
        body();
    } else if (g_redirects != nullptr) {
        const u64 module = g_module.load(std::memory_order_relaxed);
        u64 from[32], to[32];
        for (u32 i = 0; i < n; ++i) {
            from[i] = module + callees[i].rva;
            to[i] = ST::ThunkAddress(i);
        }
        g_redirects(from, to, n);
        body();
        g_redirects(nullptr, nullptr, 0);
    } else {
        t.overflow = true; // an implementation without redirection: not comparable
    }
    ST::SetActive(saved);
}

/// The stack the runs' frames use, zeroed (their uninitialised locals equal in both runs; under a page: no probe).
__declspec(noinline) void WipeStackBelow() {
    volatile u8 area[0xf00];
    for (u32 i = 0; i < sizeof(area); i += 8) {
        *reinterpret_cast<volatile u64*>(&area[i]) = 0;
    }
}

template <size_t N>
std::string FirstDifference(const char* what, const u8 (&a)[N], const u8 (&b)[N], u64 bytes) {
    if (std::memcmp(a, b, static_cast<size_t>(bytes)) == 0) {
        return {};
    }
    u64 k = 0;
    while (a[k] == b[k]) {
        ++k;
    }
    return fmt::format("{} differs at +{:#x}", what, k);
}

} // namespace

std::string SL::CheckDraw(u64 pass, u64 r, u64 records, u64 n, u64 mesh, bool& skipped) {
    volatile u8 top = 0; // every frame of the runs is below
    Shadow& s = Shadows();
    for (u32 k = 0; k < 2; ++k) {
        ST::Trace& t = BeginRun(s, k, kDrawCallees, kDrawCallees_n, reinterpret_cast<u64>(&top), false);
        WipeStackBelow();
        if (k == 0) {
            Recorded(t, true, kDrawCallees, kDrawCallees_n, [&] { g_rec_draw(pass, r, records, n, mesh); });
        } else {
            const u64 nn = g_fault == 1 ? n + 1 : n;
            Recorded(t, false, kDrawCallees, kDrawCallees_n, [&] { g_draw_impl(pass, r, records, nn, mesh); });
        }
    }
    WipeStackBelow();
    skipped = s.t[0].overflow || s.t[1].overflow;
    return skipped ? std::string{} : ST::Compare(s.t[0], s.t[1], true);
}

std::string SL::CheckLoop(u64 pass, u64 args, bool& skipped) {
    volatile u8 top = 0;
    Shadow& s = Shadows();
    const auto* live_args = reinterpret_cast<const u64*>(args); // (the loop reads +0 R, +8 the scene, +0x18 the range)
    // the live state read once: both runs start from these copies (another thread may change the live one); the whole
    // context (what the loop and the callees that run for real read of it), written only in the shadow
    std::memcpy(s.r_live, reinterpret_cast<const void*>(live_args[0]), SL::kShadowBytes);
    std::memcpy(s.prng_live, reinterpret_cast<const void*>(g_module.load(std::memory_order_relaxed) + kPrngState),
                kPrngBytes);
    s.args[0] = reinterpret_cast<u64>(s.r);
    s.args[1] = live_args[1];
    s.args[2] = live_args[2];
    s.args[3] = live_args[3];
    const u64 a = reinterpret_cast<u64>(s.args);
    for (u32 k = 0; k < 2; ++k) {
        std::memcpy(s.r, s.r_live, SL::kShadowBytes);
        std::memcpy(s.prng, s.prng_live, kPrngBytes);
        ST::Trace& t = BeginRun(s, k, kLoopCallees, kLoopCallees_n, reinterpret_cast<u64>(&top), true);
        WipeStackBelow();
        if (k == 0) {
            Recorded(t, true, kLoopCallees, kLoopCallees_n, [&] { g_rec_loop(pass, a); });
            std::memcpy(s.r_first, s.r, SL::kShadowBytes);
            std::memcpy(s.prng_first, s.prng, kPrngBytes);
            std::memcpy(s.arena_first, s.arena, static_cast<size_t>(std::min(t.arena_used, kArenaBytes)));
        } else {
            Recorded(t, false, kLoopCallees, kLoopCallees_n, [&] { g_loop_impl(pass, a); });
        }
        s.arena_dirty = std::max(s.arena_dirty, std::min(t.arena_used, kArenaBytes));
    }
    WipeStackBelow();
    skipped = s.t[0].overflow || s.t[1].overflow;
    if (skipped) {
        return {};
    }
    // (the traces' own comparison of the allocations sees one buffer: the saved first run's is compared below)
    std::string d = ST::Compare(s.t[0], s.t[1], true);
    if (d.empty()) {
        d = FirstDifference("the shadow context", s.r_first, s.r, SL::kShadowBytes);
    }
    if (d.empty() && s.t[0].arena_used != s.t[1].arena_used) {
        d = fmt::format("allocations: {} vs {} bytes", s.t[0].arena_used, s.t[1].arena_used);
    }
    if (d.empty()) {
        d = FirstDifference("the allocations", s.arena_first, s.arena, s.t[0].arena_used);
    }
    if (d.empty()) {
        d = FirstDifference("the generator's state", s.prng_first, s.prng, kPrngBytes);
    }
    return d;
}

namespace {

// --- the registry's entries -------------------------------------------------------------------------------------

struct Case {
    u64 args[5];
    char what[512];
};
static_assert(sizeof(Case) <= kCaseBytes);

void RecordMismatch(u32 index, const u64 (&args)[5], const std::string& what) {
    Case c{};
    std::memcpy(c.args, args, sizeof(args));
    std::memcpy(c.what, what.data(), std::min(what.size(), sizeof(c.what) - 1));
    RecordCase(index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
}

bool CheckCounted(Slot& slot, const char* name, const std::string& what, bool skipped, const u64 (&args)[5]) {
    if (skipped) {
        slot.skipped.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    Count(slot.index, kCallsChecked);
    slot.verified.fetch_add(1, std::memory_order_relaxed);
    if (!what.empty()) {
        if (slot.mismatches.fetch_add(1, std::memory_order_relaxed) < 8) {
            SSPC_LOG(Engine, Error, "R6-S: {} mismatch: {}", name, what);
        }
        RecordMismatch(slot.index, args, what);
        return false;
    }
    return true;
}

struct LoopImpl {
    static Slot slot;
    static SL::LoopFn Original() {
        return reinterpret_cast<SL::LoopFn>(slot.original);
    }
    static SSPC_SYSV_ABI void Native(u64 pass, u64 args) {
        g_loop_impl(pass, args);
    }
    static bool Check(u64 pass, u64 args) {
        bool skipped = false;
        const std::string what = SL::CheckLoop(pass, args, skipped);
        return CheckCounted(slot, "shadow_casters_loop (0x43d590)", what, skipped, {pass, args, 0, 0, 0});
    }
    static SSPC_SYSV_ABI void Verify(u64 pass, u64 args) {
        Check(pass, args);
        Count(slot.index, kCallsOriginal);
        Original()(pass, args);
    }
    static SSPC_SYSV_ABI void Guarded(u64 pass, u64 args) {
        if (SampleThisCall(slot.index, slot.sample_period) && !Check(pass, args)) {
            Fallback(slot);
            Count(slot.index, kCallsOriginal);
            Original()(pass, args);
            return;
        }
        Count(slot.index, kCallsNative);
        Native(pass, args);
    }
    static SSPC_SYSV_ABI void Bench(u64 pass, u64 args) {
        Count(slot.index, kCallsOriginal);
        Original()(pass, args);
    }
};
Slot LoopImpl::slot;

struct DrawImpl {
    static Slot slot;
    static SL::DrawFn Original() {
        return reinterpret_cast<SL::DrawFn>(slot.original);
    }
    static SSPC_SYSV_ABI void Native(u64 pass, u64 r, u64 records, u64 n, u64 mesh) {
        g_draw_impl(pass, r, records, n, mesh);
    }
    static bool Check(u64 pass, u64 r, u64 records, u64 n, u64 mesh) {
        bool skipped = false;
        const std::string what = SL::CheckDraw(pass, r, records, n, mesh, skipped);
        return CheckCounted(slot, "shadow_instanced_draw (0x43d480)", what, skipped, {pass, r, records, n, mesh});
    }
    static SSPC_SYSV_ABI void Verify(u64 pass, u64 r, u64 records, u64 n, u64 mesh) {
        Check(pass, r, records, n, mesh);
        Count(slot.index, kCallsOriginal);
        Original()(pass, r, records, n, mesh);
    }
    static SSPC_SYSV_ABI void Guarded(u64 pass, u64 r, u64 records, u64 n, u64 mesh) {
        if (SampleThisCall(slot.index, slot.sample_period) && !Check(pass, r, records, n, mesh)) {
            Fallback(slot);
            Count(slot.index, kCallsOriginal);
            Original()(pass, r, records, n, mesh);
            return;
        }
        Count(slot.index, kCallsNative);
        Native(pass, r, records, n, mesh);
    }
    static SSPC_SYSV_ABI void Bench(u64 pass, u64 r, u64 records, u64 n, u64 mesh) {
        Count(slot.index, kCallsOriginal);
        Original()(pass, r, records, n, mesh);
    }
};
Slot DrawImpl::slot;

/// The stack guard's GOT slot while a check runs on an unrelocated image (0 before the linker's relocation:
/// cue_commit.cpp); restored on destruction, a slot already set is left alone.
class GuardForChecks {
public:
    explicit GuardForChecks(u64 module) : at_(reinterpret_cast<u64*>(module + kStackGuardSlot)) {
        static const u64 kGuard = 0x5ec0de5ec0de5ec0ull;
        MEMORY_BASIC_INFORMATION mbi{};
        constexpr DWORD kWritable = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
        if (VirtualQuery(at_, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
            (mbi.Protect & kWritable) == 0) {
            return;
        }
        saved_ = *at_;
        if (saved_ == 0) {
            *at_ = reinterpret_cast<u64>(&kGuard);
        }
        ok_ = true;
    }
    ~GuardForChecks() {
        if (ok_) {
            *at_ = saved_;
        }
    }
    GuardForChecks(const GuardForChecks&) = delete;
    GuardForChecks& operator=(const GuardForChecks&) = delete;
    bool ok() const {
        return ok_;
    }

private:
    u64* at_;
    u64 saved_ = 0;
    bool ok_ = false;
};

/// Both specs: the recompiled implementations from SSPC_NATIVE_ENGINE_RECOMP (there is no hand-written one).
bool PrepareCommon(u64 module_base, std::string& error) {
    static std::mutex mu;
    static bool done = false;
    static bool ok = false;
    static std::string saved;
    const std::scoped_lock lock(mu);
    if (done) {
        error = saved;
        return ok;
    }
    done = true;
    ok = SL::Prepare(module_base, saved);
    if (ok && (g_loop_impl == nullptr || g_draw_impl == nullptr)) {
        char path[260] = {};
        const DWORD n = GetEnvironmentVariableA("SSPC_NATIVE_ENGINE_RECOMP", path, sizeof(path));
        HMODULE dll = nullptr;
        if (n > 0 && n < sizeof(path)) {
            dll = GetModuleHandleA(path);
            if (dll == nullptr) {
                dll = LoadLibraryA(path);
            }
        }
        const auto base = dll != nullptr ? reinterpret_cast<u64*>(GetProcAddress(dll, "recomp_image_base")) : nullptr;
        const auto loop = dll != nullptr ? reinterpret_cast<SL::LoopFn>(GetProcAddress(dll, "recomp_43d590")) : nullptr;
        const auto draw = dll != nullptr ? reinterpret_cast<SL::DrawFn>(GetProcAddress(dll, "recomp_43d480")) : nullptr;
        const auto redirects =
            dll != nullptr ? reinterpret_cast<SL::RedirectsFn>(GetProcAddress(dll, "recomp_set_redirects")) : nullptr;
        if (base == nullptr || loop == nullptr || draw == nullptr || redirects == nullptr) {
            saved = "no implementation: SSPC_NATIVE_ENGINE_RECOMP must name a module with recomp_43d590, recomp_43d480, "
                    "recomp_set_redirects and recomp_image_base";
            ok = false;
        } else {
            *base = module_base;
            SL::UseImplementation(loop, draw, redirects);
            SSPC_LOG(Engine, Warning, "R6-S: the shadow loop's implementation: the recompiled 0x43d590 / 0x43d480 ({})",
                     path);
        }
    }
    error = saved;
    return ok;
}

bool PrepareLoopInstall(u64 module_base, std::string& error) {
    return PrepareCommon(module_base, error);
}

bool PrepareDrawInstall(u64 module_base, std::string& error) {
    // CITY-OPT's shadow draw runs put an entry probe on 0x43d480: only below it, never a second jump
    if (Engine::FindEntryProbe(module_base + kDrawFunction.rva) == nullptr) {
        error = "0x43d480 is installed only below CITY-OPT's entry probe (shadow draw runs), and there is none";
        return false;
    }
    return PrepareCommon(module_base, error);
}

bool SelfCheckLoopInstall(void*, std::string& error) {
    GuardForChecks guard(g_module.load(std::memory_order_relaxed));
    if (!guard.ok()) {
        error = "the stack guard's GOT slot is not writable memory: no self-check";
        return false;
    }
    error = SL::SelfCheckLoop(300, 0x5248414430353930ull);
    return error.empty();
}

bool SelfCheckDrawInstall(void*, std::string& error) {
    GuardForChecks guard(g_module.load(std::memory_order_relaxed));
    if (!guard.ok()) {
        error = "the stack guard's GOT slot is not writable memory: no self-check";
        return false;
    }
    error = SL::SelfCheckDraw(2000, 0x5248414430343830ull);
    return error.empty();
}

using DL = Dispatch<void(u64, u64)>;
using DD = Dispatch<void(u64, u64, u64, u64, u64)>;

} // namespace

bool SL::Prepare(u64 module_base, std::string& error) {
    g_module.store(module_base, std::memory_order_relaxed);
    for (u32 pass = 0; pass < 2; ++pass) {
        const Engine::GuestFunction& fn = pass == 0 ? kLoopFunction : kDrawFunction;
        const ST::CalleeSpec* callees = pass == 0 ? kLoopCallees : kDrawCallees;
        const u32 n = pass == 0 ? kLoopCallees_n : kDrawCallees_n;
        std::vector<CodeRedirect> redirects;
        for (u32 i = 0; i < n; ++i) {
            redirects.push_back({module_base + callees[i].rva, ST::ThunkAddress(i)});
        }
        std::vector<u8> bytes;
        const CloneResult clone = CloneGuestFunction(module_base + fn.rva, OriginalBytes(module_base, fn, bytes),
                                                     fn.size, error, redirects);
        if (clone.code == nullptr) {
            error = fmt::format("recording clone of {:#x}: {}", fn.rva, error);
            return false;
        }
        if (pass == 0) {
            g_rec_loop = reinterpret_cast<LoopFn>(clone.code);
        } else {
            g_rec_draw = reinterpret_cast<DrawFn>(clone.code);
        }
    }
    return true;
}

void SL::UseImplementation(LoopFn loop, DrawFn draw, RedirectsFn redirects) {
    g_loop_impl = loop;
    g_draw_impl = draw;
    g_redirects = redirects;
}

void SL::InjectFault(u32 fault) {
    g_fault = fault;
}

namespace {

float RandomFloat(std::mt19937_64& rng) {
    switch (rng() % 16) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return 1e-40f;
    case 3: return (rng() & 1) ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
    case 4: return std::numeric_limits<float>::quiet_NaN();
    default: return static_cast<float>(static_cast<s64>(rng() % 2000001) - 1000000) / 1000.0f;
    }
}

/// The world one generated call of 0x43d590 sees, in one buffer (every pointer the loop follows points inside it).
struct LoopWorld {
    static constexpr u32 kPass = 0x0, kArgs = 0x400, kContext = 0x1000, kScene = 0x10000, kView = 0x15000,
                         kItems = 0x16000, kKeys = 0x18000, kObjects = 0x19000, kMeshes = 0x1a000,
                         kMaterials = 0x1b000, kBuffers = 0x1c000, kMatrices = 0x1d000, kBytes = 0x20000;
    static constexpr u32 kMaxItems = 48;
    std::vector<u8> mem = std::vector<u8>(kBytes);
    u64 base() const {
        return reinterpret_cast<u64>(mem.data());
    }
    template <typename T>
    void Put(u32 at, T x) {
        std::memcpy(mem.data() + at, &x, sizeof(T));
    }
    void Generate(std::mt19937_64& rng) {
        for (u32 i = 0; i + 4 <= kBytes; i += 4) {
            Put(i, RandomFloat(rng));
        }
        const u64 b = base();
        Put(kPass + 0x1f, static_cast<u8>(rng() % 2));
        Put(kPass + 0x21, static_cast<u8>(rng()));
        Put(kPass + 0x90, static_cast<s32>(rng() % 3) - 1);
        Put(kPass + 0x94, static_cast<s32>(rng() % 3) - 1);
        // 0x43d590 passes 0x11c890 no `extra` (r8 = 0): never bit 3 (its block from extra)
        Put(kPass + 0x118, static_cast<s32>(rng() % 2 ? 0 : rng() & ~u64{8}));
        Put(kPass + 0x11c, static_cast<s32>(rng() % 2 ? 0 : rng()));
        Put(kPass + 0x120, static_cast<u8>(rng() % 3 == 0 ? 0xff : rng() % 16));
        static constexpr u16 kFlags[] = {0, 1, 0x100, 0x101, 0x1ff};
        Put(kContext + 0xd2e8, kFlags[rng() % 5]);
        Put(kContext + 0xd388, kFlags[rng() % 5]);
        Put(kScene + 0, b + kView);
        Put(kScene + 0x9b7 * 8, b + kItems);
        Put(kScene + 0x9b8 * 8, b + kKeys);
        const u32 n = 1 + static_cast<u32>(rng() % kMaxItems);
        const u32 lo = static_cast<u32>(rng() % 4);
        const u32 hi = (rng() % 8 == 0) ? lo : lo + static_cast<u32>(rng() % (n + 1));
        Put(kArgs + 0, b + kContext);
        Put(kArgs + 8, b + kScene);
        Put(kArgs + 0x10, rng());
        Put(kArgs + 0x18, static_cast<u64>(lo) | static_cast<u64>(hi) << 32);
        u64 group = rng() & 0xffff'ffffull;
        for (u32 i = 0; i < kMaxItems + 8; ++i) {
            if (rng() % 3 == 0) {
                group += 1 + rng() % 3;
            }
            Put(kKeys + i * 8, group << 16 | (rng() % kMaxItems));
        }
        for (u32 i = 0; i < kMaxItems; ++i) {
            const u32 item = kItems + i * 0x80;
            const u32 k = static_cast<u32>(rng() % 8);
            Put(item + 0x10, b + kBuffers + k * 0x80);
            Put(item + 0x18, b + kObjects + k * 0x40);
        }
        for (u32 k = 0; k < 8; ++k) {
            Put(kObjects + k * 0x40, b + kMeshes + k * 0x80);
            Put(kObjects + k * 0x40 + 8, b + kMaterials + (rng() % 8) * 0x80);
            Put(kMeshes + k * 0x80 + 0x18, static_cast<u16>(rng() % 4 == 0 ? 0 : 1 + rng() % 100));
            Put(kMeshes + k * 0x80 + 0x38, b + kMatrices + k * 0x80);
            Put(kMaterials + k * 0x80 + 0x28, b + kMatrices + 0x800 + k * 0x40);
            Put(kBuffers + k * 0x80 + 0x54, static_cast<u16>(rng() % 64));
        }
    }
};

} // namespace

std::string SL::SelfCheckLoop(u32 cases, u64 seed) {
    if (g_rec_loop == nullptr || g_loop_impl == nullptr) {
        return "no recording clone or implementation (prepare did not run)";
    }
    std::mt19937_64 rng(seed);
    static LoopWorld w; // (128 KB: off the stack)
    for (u32 n = 0; n < cases; ++n) {
        w.Generate(rng);
        bool skipped = false;
        const std::string d = CheckLoop(w.base() + LoopWorld::kPass, w.base() + LoopWorld::kArgs, skipped);
        if (skipped) {
            return fmt::format("case {}: not comparable (trace overflow)", n);
        }
        if (!d.empty()) {
            return fmt::format("case {}: {}", n, d);
        }
    }
    return {};
}

std::string SL::SelfCheckDraw(u32 cases, u64 seed) {
    if (g_rec_draw == nullptr || g_draw_impl == nullptr) {
        return "no recording clone or implementation (prepare did not run)";
    }
    std::mt19937_64 rng(seed);
    std::vector<u8> pass(0x200), context(0x200), mesh(0x80), records(16 * 0x48);
    for (u32 n = 0; n < cases; ++n) {
        for (auto* v : {&pass, &context, &mesh, &records}) {
            for (u8& x : *v) {
                x = static_cast<u8>(rng());
            }
        }
        const u64 recs = (rng() & 1) ? reinterpret_cast<u64>(records.data()) : rng();
        const u64 count = (rng() & 1) ? rng() % 64 : rng();
        bool skipped = false;
        const std::string d = CheckDraw(reinterpret_cast<u64>(pass.data()), reinterpret_cast<u64>(context.data()), recs,
                                        count, reinterpret_cast<u64>(mesh.data()), skipped);
        if (skipped) {
            return fmt::format("case {}: not comparable (trace overflow)", n);
        }
        if (!d.empty()) {
            return fmt::format("case {}: {}", n, d);
        }
    }
    return {};
}

const FunctionSpec kShadowCastersLoopSpec{
    "shadow_casters_loop",
    "Shadow::CastersLoop",
    kLoopFunction,
    kAssumedLoop,
    static_cast<u32>(std::size(kAssumedLoop)),
    nullptr,
    0,
    reinterpret_cast<const void*>(&DL::Entry<LoopImpl>),
    &LoopImpl::slot,
    &SelfCheckLoopInstall,
    &PrepareLoopInstall,
    true, // explicit_only: verify/guarded runs only (docs/NATIVE_RENDER_R6_SHADOWS.md §10)
};

const FunctionSpec kShadowInstancedDrawSpec{
    "shadow_instanced_draw",
    "Shadow::InstancedDraw",
    kDrawFunction,
    kAssumedDraw,
    static_cast<u32>(std::size(kAssumedDraw)),
    nullptr,
    0,
    reinterpret_cast<const void*>(&DD::Entry<DrawImpl>),
    &DrawImpl::slot,
    &SelfCheckDrawInstall,
    &PrepareDrawInstall,
    true,
};

} // namespace SecondSon::NativeEngine::Reconstructed
