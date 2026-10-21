// SecondSonPC R7-G — 0x1a2030 in the registry and its function-level verification (see gbuffer_mesh_pass.h).
#include "native_engine/reconstructed/gbuffer_mesh_pass.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#include <fmt/format.h>
#include <intrin.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_hooks.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"
#include "native_engine/r7/gbuffer/geometry_pass.h"
#include "native_engine/reconstructed/gbuffer_draw.h"
#include "native_engine/reconstructed/gbuffer_trace.h"

namespace SecondSon::NativeEngine::Reconstructed {

namespace MP = GBufferMeshPass;
namespace GT = GTrace;

namespace {

constexpr Engine::GuestFunction kPassFunction{0x1a2030, 0x1651, 0xd401384d1f123cc8ull};
/// Run for real in a recording run (they only write the caller's stack): pinned.
constexpr Engine::GuestFunction kAssumed[] = {
    {0x26ac70, 0x2b, 0x6507ff5835f4f397ull}, // bounds init
    {0x61e260, 0xce, 0x82958d25b30182e7ull}, // Gnm initAsRegularBuffer (-> 0x61f430)
    {0x61f430, 0x33, 0x75d0a6829a95f352ull}, // Gnm DataFormat
    {0x635350, 0xd, 0x7061272241737964ull},  // render control bits 0..1
};

enum Callee : u32 {
    kRandom,
    kMaterial,
    kLight,
    kSetBuffers,
    kPass1d,
    kObjConst,
    kUpload,
    kBind,
    kConst2,
    k446490,
    k445e00,
    kDrawConst,
    k450460,
    k1742f0,
    kCommitConst,
    kStencil,
    kRenderControl,
    kMeshDraw,
    kFrameAlloc,
    k451440,
    k451290,
    kInstancedDraw,
    kStackFail,
    kCallees
};
constexpr u64 kRva[kCallees] = {0x612110, 0x448640, 0x1a0a80, 0x16b70,  0x1a1b90, 0x11c890, 0x11be90, 0x11bed0,
                                0x11cde0, 0x446490, 0x445e00, 0x11e5d0, 0x450460, 0x1742f0, 0x120280, 0x6229a0,
                                0x6270e0, 0x1a1960, 0x1f0870, 0x451440, 0x451290, 0x1a1770, 0x984360};
constexpr u64 kBoundsInitRva = 0x26ac70, kInitBufferRva = 0x61e260;

std::atomic<u64> g_module{0};
R7G::PassFns g_real{};
R7G::PassFns g_recorders{};
R7G::PassConstants g_consts{};
R7G::PassOptions g_options{};
MP::PassFn g_rec_pass = nullptr;
MP::PassFn g_pass_original = nullptr; ///< slot.original (set by the self-check)
u32 g_fault = 0;

using ObjConstFn = SSPC_SYSV_ABI void (*)(u64, u64, u64, u64, u64, u64, u64, u64);
using Const2Fn = SSPC_SYSV_ABI void (*)(__m128, __m128, u64, u64, u64, u64);

// --- the recorded callees' effects ---------------------------------------------------------------------------------

u64 Random(GT::Trace& t, const GT::CalleeSpec&, GT::CallIn&, const u64*) {
    return static_cast<u32>(GT::Next(t));
}
u64 Pass1d(GT::Trace& t, const GT::CalleeSpec&, GT::CallIn& in, const u64*) {
    u64* out = reinterpret_cast<u64*>(in.gpr[0]);
    for (u32 i = 0; i < 8; ++i) {
        // finite floats: the pass copies them into matrices the next callees get
        const u64 v = GT::Next(t) & 0x3fff3fff3fff3fffull;
        out[i] = v;
    }
    return 0;
}
u64 ObjConst(GT::Trace&, const GT::CalleeSpec&, GT::CallIn& in, const u64* stack) {
    const auto fn = reinterpret_cast<ObjConstFn>(g_module.load(std::memory_order_relaxed) + kRva[kObjConst]);
    fn(in.gpr[0], in.gpr[1], in.gpr[2], in.gpr[3], in.gpr[4], in.gpr[5], stack[0], stack[1]);
    return 0;
}
u64 Const2(GT::Trace&, const GT::CalleeSpec&, GT::CallIn& in, const u64*) {
    const auto fn = reinterpret_cast<Const2Fn>(g_module.load(std::memory_order_relaxed) + kRva[kConst2]);
    __m128 a, b;
    std::memcpy(&a, in.xmm[0], 16);
    std::memcpy(&b, in.xmm[1], 16);
    fn(a, b, in.gpr[0], in.gpr[1], in.gpr[2], in.gpr[3]);
    return 0;
}
u64 Upload(GT::Trace& t, const GT::CalleeSpec&, GT::CallIn& in, const u64*) {
    const u64 bytes = (static_cast<u32>(in.gpr[2]) + 0xfu) & ~0xfull;
    const u64 p = t.Allocate(bytes, 16);
    u64* slot = reinterpret_cast<u64*>(in.gpr[0]);
    slot[0] = p;
    slot[1] = GT::Next(t);
    slot[2] = GT::Next(t);
    return 0;
}
/// Generated cases only: the frame allocator fails now and then (the pass skips the group), the material bind marks
/// the constant blocks for a rebind now and then (the rebind-only path) - both runs see the same.
bool g_generated = false;
u64 Material(GT::Trace& t, const GT::CalleeSpec&, GT::CallIn& in, const u64*) {
    if (g_generated && GT::Next(t) % 2 == 0) {
        *reinterpret_cast<u8*>(in.gpr[1] + 0xd2e9) |= 1;
        *reinterpret_cast<u8*>(in.gpr[1] + 0xd389) |= 1;
    }
    return 0;
}
u64 FrameAlloc(GT::Trace& t, const GT::CalleeSpec&, GT::CallIn& in, const u64*) {
    if (g_generated && GT::Next(t) % 8 == 0) {
        return 0;
    }
    return t.Allocate(static_cast<u32>(in.gpr[1]), static_cast<u32>(in.gpr[2]));
}
u64 StackFail(GT::Trace& t, const GT::CalleeSpec&, GT::CallIn&, const u64*) {
    t.stack_check_failed = true;
    return 0;
}

constexpr GT::Snap kNo{};
constexpr GT::Snap kVsharps{4, 3, 16, 16 * 64};    ///< 0x16b70: r8 -> rcx V#s
constexpr GT::Snap kMatRdi{0, -1, 0x40, 0x40};
constexpr GT::Snap kMatRsi{1, -1, 0x40, 0x40};
constexpr GT::Snap kMatRdx{2, -1, 0x40, 0x40};
constexpr GT::Snap kMatRcx{3, -1, 0x40, 0x40};
constexpr GT::Snap kMatR8{4, -1, 0x40, 0x40};
constexpr GT::Snap kBindSlot{0, -1, 0x18, 0x18};   ///< 0x11bed0: the buffer pointer and its V#
constexpr GT::Snap kBounds{7, -1, 0x80, 0x80};      ///< 0x445e00: stack argument 1
constexpr GT::Snap kFloats{2, 3, 4, 0x1000};        ///< 0x11e5d0: rdx -> ecx floats
constexpr GT::Snap kRecords{2, 3, R7G::kRecordBytes, R7G::kRecordBytes * 8192}; ///< 0x1a1770: rdx -> ecx records

const GT::CalleeSpec kSpecs[kCallees] = {
    {0x612110, "0x612110 random", 1, 0, 0, 0, {kNo}, &Random},
    {0x448640, "0x448640 material", 3, 0, 0, 0, {kNo}, &Material},
    {0x1a0a80, "0x1a0a80 light", 4, 0, 0, 0, {kNo}, nullptr, true, 0x4},
    {0x16b70, "0x16b70 CUE set buffers", 5, 0xe, 0, 0, {kVsharps}},
    {0x1a1b90, "0x1a1b90 pass 0x1d transform", 4, 0, 1, 0, {kNo}, &Pass1d},
    {0x11c890, "0x11c890 object constants", 6, 0x20, 0, 2, {kMatRdi, kMatRsi, kMatRdx, kMatR8}, &ObjConst, true, 0,
     0x1},
    {0x11be90, "0x11be90 upload", 3, 0x4, 0, 0, {kNo}, &Upload},
    {0x11bed0, "0x11bed0 bind", 4, 0x6, 0, 0, {kBindSlot}},
    {0x11cde0, "0x11cde0 constants 2", 4, 0x4, 2, 0, {kMatRdi, kMatRsi}, &Const2},
    {0x446490, "0x446490 setup", 5, 0, 0, 0, {kNo}},
    {0x445e00, "0x445e00 setup", 6, 0, 1, 2, {kMatRcx, kBounds}},
    {0x11e5d0, "0x11e5d0 draw constants", 4, 0xa, 0, 0, {kFloats}},
    {0x450460, "0x450460 constants", 6, 0, 0, 1, {kNo}},
    {0x1742f0, "0x1742f0 constants", 4, 0, 0, 0, {kNo}},
    {0x120280, "0x120280 commit constants", 1, 0, 0, 0, {kNo}},
    {0x6229a0, "0x6229a0 stencil", 2, 0x2, 0, 0, {kNo}},
    {0x6270e0, "0x6270e0 render control", 2, 0x2, 0, 0, {kNo}},
    {0x1a1960, "0x1a1960 mesh draw", 3, 0, 0, 0, {kNo}},
    {0x1f0870, "0x1f0870 frame alloc", 3, 0x6, 0, 0, {kNo}, &FrameAlloc},
    {0x451440, "0x451440 record", 5, 0, 0, 0, {kNo}},
    {0x451290, "0x451290 group", 6, 0, 0, 0, {kNo}},
    {0x1a1770, "0x1a1770 instanced draw", 5, 0x8, 0, 0, {kRecords}},
    {0x984360, "0x984360 __stack_chk_fail", 0, 0, 0, 0, {kNo}, &StackFail},
};

// --- verification ---------------------------------------------------------------------------------------------------

constexpr u64 kShadowBegin = 0xd190, kShadowEnd = 0xde90; ///< the context's fields the pass and its run callees touch
constexpr u32 kMaxCalls = 8192;
constexpr u32 kSnapBytes = 2u << 20;
constexpr u64 kArenaBytes = 1u << 20;

struct Run {
    GT::Trace t;
    GT::Call* calls;
    u8* snap;
    u8* r;          ///< shadow context (0xde90 bytes; only [kShadowBegin, kShadowEnd) is meaningful)
    alignas(16) u64 chunk[4];
};
struct Shadow {
    Run run[2];
    u8* arena;      ///< shared by both runs (equal addresses: pointers into it compare equal byte for byte)
    u8* arena_saved;
    u64 arena_used0;
};
thread_local Shadow* t_shadow = nullptr;

void* Big(u64 bytes) {
    return VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

Shadow* Shadows() {
    if (t_shadow == nullptr) { // per verifying thread, never freed
        auto* s = new Shadow{};
        for (Run& x : s->run) {
            x.calls = static_cast<GT::Call*>(Big(sizeof(GT::Call) * kMaxCalls));
            x.snap = static_cast<u8*>(Big(kSnapBytes));
            x.r = static_cast<u8*>(Big(kShadowEnd));
        }
        s->arena = static_cast<u8*>(Big(kArenaBytes));
        s->arena_saved = static_cast<u8*>(Big(kArenaBytes));
        if (s->arena == nullptr || s->arena_saved == nullptr || s->run[0].calls == nullptr ||
            s->run[1].calls == nullptr || s->run[0].snap == nullptr || s->run[1].snap == nullptr ||
            s->run[0].r == nullptr || s->run[1].r == nullptr) {
            return nullptr;
        }
        t_shadow = s;
    }
    return t_shadow;
}

GT::Trace& BeginRun(Shadow& s, u32 k, u64 r, u64 chunk, u64 stack_top) {
    Run& x = s.run[k];
    x.t.callees = kSpecs;
    x.t.ncallees = kCallees;
    x.t.calls = x.calls;
    x.t.max_calls = kMaxCalls;
    x.t.snap = x.snap;
    x.t.snap_cap = kSnapBytes;
    x.t.arena = s.arena;
    x.t.arena_cap = kArenaBytes;
    x.t.nregions = 0;
    x.t.AddRegion(reinterpret_cast<u64>(x.r), kShadowEnd);
    std::memcpy(x.r + kShadowBegin, reinterpret_cast<const void*>(r + kShadowBegin), kShadowEnd - kShadowBegin);
    std::memcpy(x.chunk, reinterpret_cast<const void*>(chunk), sizeof(x.chunk));
    x.chunk[0] = reinterpret_cast<u64>(x.r);
    x.t.Begin(0x5237474d50415353ull, stack_top);
    return x.t;
}

__declspec(noinline) void WipeStackBelow() {
    volatile u8 area[0x2000];
    for (u32 i = 0; i < sizeof(area); i += 8) {
        *reinterpret_cast<volatile u64*>(&area[i]) = 0;
    }
}

// --- stats ------------------------------------------------------------------------------------------------------------

bool g_stats = false;
struct StatBlock {
    std::atomic<u64> v[R7G::kPsStats]{};
    std::atomic<u64> ticks{0};
};
std::mutex g_stat_mu;
std::vector<StatBlock*> g_stat_blocks;
thread_local StatBlock* t_stat = nullptr;

StatBlock& Stats() {
    if (t_stat == nullptr) {
        t_stat = new StatBlock;
        const std::scoped_lock lock(g_stat_mu);
        g_stat_blocks.push_back(t_stat);
    }
    return *t_stat;
}

inline u64 Tsc() {
    return __rdtsc();
}

void RunNative(const R7G::PassFns& f, u64 m, u64 chunk) {
    R7G::PassOptions o = g_options;
    o.fault = g_fault;
    if (!g_stats) {
        R7G::RunPass(f, g_module.load(std::memory_order_relaxed), g_consts, m, chunk, o, nullptr);
        return;
    }
    R7G::PassCounters c;
    const u64 t0 = Tsc();
    R7G::RunPass(f, g_module.load(std::memory_order_relaxed), g_consts, m, chunk, o, &c);
    const u64 t1 = Tsc();
    StatBlock& s = Stats();
    for (u32 k = 0; k < R7G::kPsStats; ++k) {
        if (c.v[k] != 0) {
            s.v[k].store(s.v[k].load(std::memory_order_relaxed) + c.v[k], std::memory_order_relaxed);
        }
    }
    s.ticks.store(s.ticks.load(std::memory_order_relaxed) + (t1 - t0), std::memory_order_relaxed);
}

} // namespace

std::string MP::Check(u64 m, u64 chunk, bool& skipped) {
    volatile u8 top = 0;
    skipped = false;
    Shadow* sp = Shadows();
    if (sp == nullptr || g_rec_pass == nullptr) {
        skipped = true;
        return {};
    }
    Shadow& s = *sp;
    const u64 r = *reinterpret_cast<const u64*>(chunk);
    for (u32 k = 0; k < 2; ++k) {
        GT::Trace& t = BeginRun(s, k, r, chunk, reinterpret_cast<u64>(&top));
        const u64 ch = reinterpret_cast<u64>(s.run[k].chunk);
        WipeStackBelow();
        GT::Trace* const saved = GT::Active();
        GT::SetActive(&t);
        if (k == 0) {
            g_rec_pass(m, ch);
        } else {
            RunNative(g_recorders, m, ch);
        }
        GT::SetActive(saved);
        if (k == 0) {
            s.arena_used0 = t.arena_used;
            std::memcpy(s.arena_saved, s.arena, t.arena_used);
        }
    }
    WipeStackBelow();
    GT::Trace& a = s.run[0].t;
    GT::Trace& b = s.run[1].t;
    skipped = a.overflow || b.overflow;
    if (skipped) {
        return {};
    }
    // the arena is shared: compare the saved first run with the second here, not in GT::Compare
    const u64 used0 = a.arena_used, used1 = b.arena_used;
    a.arena_used = b.arena_used = 0;
    std::string d = GT::Compare(a, b, false);
    a.arena_used = used0;
    b.arena_used = used1;
    if (d.empty() && (used0 != used1 || std::memcmp(s.arena_saved, s.arena, used0) != 0)) {
        u64 i = 0;
        while (i < used0 && i < used1 && s.arena_saved[i] == s.arena[i]) {
            ++i;
        }
        d = fmt::format("allocations: {} vs {} bytes, first difference at +{:#x}", used0, used1, i);
    }
    if (d.empty() && std::memcmp(s.run[0].r + kShadowBegin, s.run[1].r + kShadowBegin, kShadowEnd - kShadowBegin) != 0) {
        u64 i = kShadowBegin;
        while (i < kShadowEnd && s.run[0].r[i] == s.run[1].r[i]) {
            ++i;
        }
        d = fmt::format("the context differs at R+{:#x}", i);
    }
    return d;
}

namespace {

// --- the registry's entry ----------------------------------------------------------------------------------------------

struct Case {
    u64 args[2];
    char what[512];
};
static_assert(sizeof(Case) <= kCaseBytes);

struct PassImpl {
    static Slot slot;
    static MP::PassFn Original() {
        return reinterpret_cast<MP::PassFn>(slot.original);
    }
    static SSPC_SYSV_ABI void Native(u64 m, u64 chunk) {
        RunNative(g_real, m, chunk);
    }
    static bool CheckOne(u64 m, u64 chunk) {
        bool skipped = false;
        const std::string what = MP::Check(m, chunk, skipped);
        if (skipped) {
            slot.skipped.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        Count(slot.index, kCallsChecked);
        slot.verified.fetch_add(1, std::memory_order_relaxed);
        if (!what.empty()) {
            slot.mismatches.fetch_add(1, std::memory_order_relaxed);
            Case c{{m, chunk}, {}};
            std::memcpy(c.what, what.data(), std::min(what.size(), sizeof(c.what) - 1));
            RecordCase(slot.index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
            return false;
        }
        return true;
    }
    static SSPC_SYSV_ABI void Verify(u64 m, u64 chunk) {
        CheckOne(m, chunk);
        Count(slot.index, kCallsOriginal);
        Original()(m, chunk);
    }
    static SSPC_SYSV_ABI void Guarded(u64 m, u64 chunk) {
        if (SampleThisCall(slot.index, slot.sample_period) && !CheckOne(m, chunk)) {
            Fallback(slot);
            Count(slot.index, kCallsOriginal);
            Original()(m, chunk);
            return;
        }
        Count(slot.index, kCallsNative);
        Native(m, chunk);
    }
    static SSPC_SYSV_ABI void Bench(u64 m, u64 chunk) {
        Count(slot.index, kCallsOriginal);
        Original()(m, chunk);
    }
};
Slot PassImpl::slot;

bool EnvValue(const char* name, char* value, u32 size) {
    const DWORD n = GetEnvironmentVariableA(name, value, size);
    return n > 0 && n < size;
}

void StatsReporter() {
    u64 last = R6GFrameId();
    for (;;) {
        Sleep(10000);
        const u64 now = R6GFrameId();
        const double frames = static_cast<double>(now - last);
        last = now;
        SSPC_LOG(Engine, Info, "r7g pass/10s ({:.0f} frames), per frame:{}", frames, MP::StatsText(frames, true));
    }
}

bool PrepareInstall(u64 module_base, std::string& error) {
    static std::mutex mu;
    static bool done = false, ok = false;
    static std::string saved;
    const std::scoped_lock lock(mu);
    if (!done) {
        done = true;
        ok = MP::Prepare(module_base, saved);
        char v[32] = {};
        if (ok && EnvValue("SSPC_R7G_PREFETCH", v, sizeof(v))) {
            g_options.prefetch = static_cast<u32>(std::strtoul(v, nullptr, 10));
            SSPC_LOG(Engine, Warning, "EXPERIMENT: SSPC_R7G_PREFETCH={} - the native geometry pass prefetches "
                                      "instances {} keys ahead", g_options.prefetch, g_options.prefetch);
        }
        if (ok && EnvValue("SSPC_R7G_STATS", v, sizeof(v)) && v[0] != '0') {
            g_stats = true;
            std::thread(StatsReporter).detach();
            SSPC_LOG(Engine, Warning, "DIAGNOSTICS: SSPC_R7G_STATS=1 - the native geometry pass counts its branches");
        }
    }
    error = saved;
    return ok;
}

bool SelfCheckInstall(void* original, std::string& error) {
    g_pass_original = reinterpret_cast<MP::PassFn>(original);
    error = MP::SelfCheck(1000, 0x5237473141323033ull);
    return error.empty();
}

using DP = Dispatch<void(u64, u64)>;

} // namespace

bool MP::Prepare(u64 module_base, std::string& error) {
    g_module.store(module_base, std::memory_order_relaxed);
    R7G::LoadPassConstants(module_base, g_consts);
    const auto at = [&](u64 rva) { return module_base + rva; };
    const auto th = [](Callee c) { return GT::ThunkAddress(c); };
    const auto fill = [&](R7G::PassFns& f, auto&& addr) {
        f.random = reinterpret_cast<decltype(f.random)>(addr(kRandom));
        f.material = reinterpret_cast<decltype(f.material)>(addr(kMaterial));
        f.light = reinterpret_cast<decltype(f.light)>(addr(kLight));
        f.set_buffers = reinterpret_cast<decltype(f.set_buffers)>(addr(kSetBuffers));
        f.pass1d = reinterpret_cast<decltype(f.pass1d)>(addr(kPass1d));
        f.object_constants = reinterpret_cast<decltype(f.object_constants)>(addr(kObjConst));
        f.upload = reinterpret_cast<decltype(f.upload)>(addr(kUpload));
        f.bind = reinterpret_cast<decltype(f.bind)>(addr(kBind));
        f.constants2 = reinterpret_cast<decltype(f.constants2)>(addr(kConst2));
        f.setup_446490 = reinterpret_cast<decltype(f.setup_446490)>(addr(k446490));
        f.setup_445e00 = reinterpret_cast<decltype(f.setup_445e00)>(addr(k445e00));
        f.draw_constants = reinterpret_cast<decltype(f.draw_constants)>(addr(kDrawConst));
        f.constants_450460 = reinterpret_cast<decltype(f.constants_450460)>(addr(k450460));
        f.constants_1742f0 = reinterpret_cast<decltype(f.constants_1742f0)>(addr(k1742f0));
        f.commit_constants = reinterpret_cast<decltype(f.commit_constants)>(addr(kCommitConst));
        f.stencil = reinterpret_cast<decltype(f.stencil)>(addr(kStencil));
        f.render_control = reinterpret_cast<decltype(f.render_control)>(addr(kRenderControl));
        f.mesh_draw = reinterpret_cast<decltype(f.mesh_draw)>(addr(kMeshDraw));
        f.frame_alloc = reinterpret_cast<decltype(f.frame_alloc)>(addr(kFrameAlloc));
        f.record_451440 = reinterpret_cast<decltype(f.record_451440)>(addr(k451440));
        f.group_451290 = reinterpret_cast<decltype(f.group_451290)>(addr(k451290));
        f.instanced_draw = reinterpret_cast<decltype(f.instanced_draw)>(addr(kInstancedDraw));
        f.bounds_init = reinterpret_cast<decltype(f.bounds_init)>(at(kBoundsInitRva));
        f.init_buffer = reinterpret_cast<decltype(f.init_buffer)>(at(kInitBufferRva));
    };
    fill(g_real, [&](Callee c) { return at(kRva[c]); });
    fill(g_recorders, [&](Callee c) { return th(c); });
    std::vector<CodeRedirect> redirects;
    for (u32 c = 0; c < kCallees; ++c) {
        redirects.push_back({at(kRva[c]), th(static_cast<Callee>(c))});
    }
    std::vector<u8> bytes;
    const CloneResult clone = CloneGuestFunction(at(kPassFunction.rva), OriginalBytes(module_base, kPassFunction, bytes),
                                                 kPassFunction.size, error, redirects);
    if (clone.code == nullptr) {
        error = fmt::format("recording clone of {:#x}: {}", kPassFunction.rva, error);
        return false;
    }
    g_rec_pass = reinterpret_cast<PassFn>(clone.code);
    return true;
}

MP::PassFn MP::RecordingPass() {
    return g_rec_pass;
}

void MP::InjectFault(u32 fault) {
    g_fault = fault;
}

void MP::EnableStats(bool on) {
    g_stats = on;
}

std::string MP::StatsText(double frames, bool reset) {
    u64 sum[R7G::kPsStats] = {};
    u64 ticks = 0;
    {
        const std::scoped_lock lock(g_stat_mu);
        for (StatBlock* b : g_stat_blocks) {
            for (u32 k = 0; k < R7G::kPsStats; ++k) {
                sum[k] += reset ? b->v[k].exchange(0, std::memory_order_relaxed) : b->v[k].load(std::memory_order_relaxed);
            }
            ticks += reset ? b->ticks.exchange(0, std::memory_order_relaxed) : b->ticks.load(std::memory_order_relaxed);
        }
    }
    const double div = frames > 0 ? frames : 1.0;
    std::string out;
    for (u32 k = 0; k < R7G::kPsStats; ++k) {
        out += fmt::format(" {}={:.1f}", R7G::kPassStatNames[k], static_cast<double>(sum[k]) / div);
    }
    out += fmt::format(" ticks={:.0f}", static_cast<double>(ticks) / div);
    return out;
}

// --- generated cases ---------------------------------------------------------------------------------------------------

namespace {

/// Eboot data a case sets, restored afterwards.
class DataPatch {
public:
    void Set(u64 at, const void* v, u32 n) {
        Saved s{at, {}};
        s.bytes.assign(reinterpret_cast<const u8*>(at), reinterpret_cast<const u8*>(at) + n);
        saved_.push_back(std::move(s));
        std::memcpy(reinterpret_cast<void*>(at), v, n);
    }
    ~DataPatch() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            std::memcpy(reinterpret_cast<void*>(it->at), it->bytes.data(), it->bytes.size());
        }
    }

private:
    struct Saved {
        u64 at;
        std::vector<u8> bytes;
    };
    std::vector<Saved> saved_;
};

bool Writable(u64 at, u64 n) {
    MEMORY_BASIC_INFORMATION mbi{};
    constexpr DWORD kW = PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == sizeof(mbi) && mbi.State == MEM_COMMIT &&
           (mbi.Protect & kW) != 0 && reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize >= at + n;
}

struct Gen;
u16 Ld16(const Gen& g, u64 o);
struct Gen {
    std::vector<u8> mem = std::vector<u8>(0x40000);
    std::vector<u8> r = std::vector<u8>(kShadowEnd);
    std::mt19937_64 rng;
    explicit Gen(u64 seed) : rng(seed) {}
    u64 b() {
        return reinterpret_cast<u64>(mem.data());
    }
    void P64(u64 o, u64 v) {
        std::memcpy(mem.data() + o, &v, 8);
    }
    void P32(u64 o, u32 v) {
        std::memcpy(mem.data() + o, &v, 4);
    }
    void P16(u64 o, u16 v) {
        std::memcpy(mem.data() + o, &v, 2);
    }
    void Pf(u64 o, float v) {
        std::memcpy(mem.data() + o, &v, 4);
    }
    float F() {
        return static_cast<float>(static_cast<s32>(rng() % 2001) - 1000) / 250.0f;
    }
    void Floats(u64 o, u32 n) {
        for (u32 i = 0; i < n; ++i) {
            Pf(o + 4 * i, F());
        }
    }
    u8 Slot() {
        return rng() % 3 == 0 ? 0xff : static_cast<u8>(rng() % 16);
    }
};

u16 Ld16(const Gen& g, u64 o) {
    u16 v;
    std::memcpy(&v, g.mem.data() + o, 2);
    return v;
}

// layout of a case (offsets in Gen::mem)
constexpr u64 kM = 0x0, kChunk = 0x300, kM10 = 0x400, kM10b = 0x480, kMf0 = 0x500, kView = 0x1000, kS = 0x2000,
              kInst = 0x8000, kMi = 0x9000, kMesh = 0xa000, kMat = 0xb000, kMd = 0xc000, kMd88 = 0xd000,
              kLights = 0xe000, kKeys = 0x10000, kStreams = 0x11000;
constexpr u32 kInstances = 24, kMis = 6;

} // namespace

std::string MP::SelfCheck(u32 cases, u64 seed) {
    if (g_rec_pass == nullptr) {
        return "no recording clone (prepare did not run)";
    }
    const u64 module = g_module.load(std::memory_order_relaxed);
    const u64 dither_index = module + R7G::kFrameObject + R7G::kFrameDitherIndex;
    const u64 dither_entry = module + R7G::kDitherTable + 0x10;
    if (!Writable(dither_index, 4) || !Writable(dither_entry, 4) || !Writable(module + R7G::kLightTable, 8 * 4)) {
        return "the eboot's data is not writable here: no generated cases";
    }
    GBufferDraw::GotForChecks got(module);
    if (!got.ok()) {
        return "the stack guard's GOT slot is not writable memory: no self-check";
    }
    Gen g(seed);
    u32 compared = 0;
    struct AllocFailures {
        AllocFailures() { g_generated = true; }
        ~AllocFailures() { g_generated = false; }
    } alloc_failures;
    for (u32 n = 0; n < cases; ++n) {
        std::fill(g.mem.begin(), g.mem.end(), u8{0});
        for (u64 i = 0; i < 0x200; i += 8) {
            g.P64(kM + i, g.rng());
        }
        const u64 b = g.b();
        // M
        g.mem[kM + 0x1f] = g.rng() % 2 == 0 ? 0 : 1;
        g.mem[kM + 0x21] = static_cast<u8>(g.rng() % 8);
        g.P64(kM + 0x10, b + kM10);
        g.P64(kM10 + 0x58, b + kM10b);
        g.mem[kM10b] = static_cast<u8>(static_cast<s8>(g.rng() % 7) - 3);
        g.P64(kM + 0xf0, b + kMf0);
        g.mem[kMf0 + 0x12] = static_cast<u8>(g.rng());
        g.P32(kM + 0x110, g.rng() % 3 == 0 ? 0x1d : static_cast<u32>(g.rng() % 0x1d));
        g.P32(kM + 0x11c, g.rng() % 4 == 0 ? 0 : static_cast<u32>(g.rng() & 0xff));
        g.P32(kM + 0x120, g.rng() % 3 == 0 ? 0 : static_cast<u32>(g.rng() % 4 + 1));
        g.mem[kM + 0x124] = g.rng() % 4 == 0 ? 1 : 0;
        g.mem[kM + 0x125] = g.Slot();
        g.mem[kM + 0x126] = g.Slot();
        g.mem[kM + 0x135] = g.Slot();
        g.P16(kM + 0x128, g.rng() % 3 == 0 ? 0xffff : static_cast<u16>(g.rng() % 200));
        g.P32(kM + 0x90, static_cast<u32>(static_cast<s32>(g.rng() % 3) - 1));
        g.P32(kM + 0x94, static_cast<u32>(static_cast<s32>(g.rng() % 3) - 1));
        g.mem[kM + 0x144] = g.rng() % 2 == 0 ? 0 : 1;
        if (g.rng() % 4 == 0) {
            g.mem[kM + 0x1c] = 0;
        }
        // view, S
        g.Floats(kView, 0x700 / 4);
        g.P64(kS, b + kView);
        g.P64(kS + 0x4db8, b + kInst);
        g.P64(kS + 0x4dc0, b + kKeys);
        // mesh instances, meshes, materials, mesh data, lights
        const u32 materials = 1 + static_cast<u32>(g.rng() % 3);
        for (u32 k = 0; k < kMis; ++k) {
            const u64 mi = kMi + 0x40 * k, mesh = kMesh + 0x80 * k;
            g.P64(mi, b + mesh);
            g.P64(mi + 8, b + kMat + 0x40 * (k % materials));
            g.mem[mi + 0x10] = static_cast<u8>(g.rng());
            g.mem[mi + 0x12] = g.rng() % 4 == 0 ? 64 : static_cast<u8>(g.rng());
            g.P16(mesh + 0x18, g.rng() % 6 == 0 ? 0 : static_cast<u16>(g.rng() | 1));
            g.mem[mesh + 0x1d] = static_cast<u8>(g.rng());
            g.Floats(mesh + 0x38, 4);
            g.P64(mesh + 0x60, b + kStreams);
            g.mem[mesh + 0x6e] = static_cast<u8>(g.rng() % 8);
            g.P64(kMat + 0x40 * k + 0x28, g.rng());
            const u64 md = kMd + 0x100 * k;
            g.P64(md + 8, g.rng() & ~0xfull);
            g.P64(md + 0x10, g.rng() & ~0xfull);
            g.P16(md + 0x54, static_cast<u16>(g.rng() % 64 + 1));
            g.P64(md + 0x88, b + kMd88 + 0x200 * k);
            g.P32(kMd88 + 0x200 * k + 0x138, g.rng() % 4 == 0 ? 0xffffffffu : 1u);
        }
        g.Floats(kLights, 0x800 / 4);
        for (u32 k = 0; k < kInstances; ++k) {
            const u64 inst = kInst + 0x80 * k;
            const u32 pick = static_cast<u32>(g.rng() % kMis);
            g.P64(inst + 0x10, b + kMd + 0x100 * pick);
            g.P64(inst + 0x18, b + kMi + 0x40 * pick);
            g.P32(inst + 0x28, g.rng() % 3 == 0 ? 0xffffffffu : static_cast<u32>(g.rng() % 4));
            g.P32(inst + 0x2c, static_cast<u32>(g.rng()));
            g.P32(inst + 0x30, static_cast<u32>(g.rng()));
            g.Floats(inst + 0x40, 16);
            if (k > 0 && g.rng() % 5 == 0) { // the same world as the previous instance
                std::memcpy(g.mem.data() + inst + 0x40, g.mem.data() + inst - 0x80 + 0x40, 0x40);
            }
        }
        // keys: runs of one group (one mesh instance's mesh + material per run in the instanced variant)
        const u32 nkeys = static_cast<u32>(g.rng() % 20 + 1);
        u32 group = 0;
        u32 run_left = 0, run_mi = 0;
        for (u32 k = 0; k < nkeys; ++k) {
            if (run_left == 0) {
                ++group;
                run_left = static_cast<u32>(g.rng() % 5 + 1);
                run_mi = static_cast<u32>(g.rng() % kMis);
            }
            --run_left;
            // an instance of this run's mesh instance (instanced: the group's members share it)
            // now and then the previous key's instance again (equal object constants: the rebind-only path)
            u32 inst = k > 0 && g.rng() % 4 == 0 ? static_cast<u32>(Ld16(g, kKeys + 8 * (k - 1)))
                                                 : static_cast<u32>(g.rng() % kInstances);
            g.P64(kInst + 0x80 * inst + 0x18, b + kMi + 0x40 * run_mi);
            g.P64(kKeys + 8 * k, (u64{group} << 16) | inst);
        }
        const s32 lo = static_cast<s32>(g.rng() % 3) - (g.rng() % 8 == 0 ? 3 : 0);
        const s32 hi = static_cast<s32>(nkeys) - static_cast<s32>(g.rng() % 2);
        g.P64(kChunk, reinterpret_cast<u64>(g.r.data()));
        g.P64(kChunk + 8, b + kS);
        g.P64(kChunk + 0x18, (u64{static_cast<u32>(hi)} << 32) | static_cast<u32>(std::max(lo, 0)));
        // the context's fields: random changed bytes and stencil cache
        std::fill(g.r.begin(), g.r.end(), u8{0});
        for (u64 i = kShadowBegin; i < kShadowEnd; i += 8) {
            const u64 v = g.rng();
            std::memcpy(g.r.data() + i, &v, 8);
        }
        const u16 c1 = static_cast<u16>(g.rng() % 4 == 0 ? 0 : (g.rng() % 2 == 0 ? 0x100 : 0));
        std::memcpy(g.r.data() + 0xd2e8, &c1, 2);
        std::memcpy(g.r.data() + 0xd388, &c1, 2);
        // eboot data: the light table, the dither
        DataPatch patch;
        for (u32 k = 0; k < 4; ++k) {
            const u64 lp = b + kLights + 0x200 * k;
            patch.Set(module + R7G::kLightTable + 8 * k, &lp, 8);
        }
        const s32 zero = 0;
        const s32 dither = g.rng() % 2 == 0 ? 0 : static_cast<s32>(g.rng() % 1100);
        patch.Set(dither_index, &zero, 4);
        patch.Set(dither_entry, &dither, 4);
        bool skipped = false;
        const std::string d = Check(b + kM, b + kChunk, skipped);
        if (!d.empty()) {
            return fmt::format("case {} (variant {}, keys {}..{}): {}", n, g.mem[kM + 0x1f], lo, hi, d);
        }
        compared += skipped ? 0 : 1;
    }
    if (compared < cases) {
        return fmt::format("only {} of {} cases comparable", compared, cases);
    }
    return {};
}

const FunctionSpec kGBufferMeshPassSpec{
    "gbuffer_mesh_pass",
    "GBuffer::MeshPass",
    kPassFunction,
    kAssumed,
    static_cast<u32>(std::size(kAssumed)),
    nullptr,
    0,
    reinterpret_cast<const void*>(&DP::Entry<PassImpl>),
    &PassImpl::slot,
    &SelfCheckInstall,
    &PrepareInstall,
    true, // explicit_only until GAME_VERIFIED (docs/NATIVE_ENGINE_R7_GBUFFER.md)
};

} // namespace SecondSon::NativeEngine::Reconstructed
