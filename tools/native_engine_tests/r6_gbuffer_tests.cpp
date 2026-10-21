// SecondSonPC — the tests of R6-G (geometry engine), owned by that branch alone (docs/NATIVE_RENDER_R6_CORE.md §3). main.cpp calls
// R6GBufferTests with the eboot mapped (before the CUE setters' jumps go into the image); it returns the number of failures and
// adds its checks to the total through the counters it is given.
//
// 0x1a1770 / 0x1a1960 (gbuffer_draw.h): the recording clone of the eboot code against the hand-written and the recompiled
// reconstructions on generated cases (the install's self-check, more cases), a wrong implementation caught, then both
// installed on the mapped image (0x1a1960 below an entry probe, as CITY-OPT puts one there in the game) and called through
// their entries in every mode with the image's callees aimed at the trace thunks. The installs stay (mode Original: the
// original's behaviour); the callees' patches are undone.
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/abi.h"
#include "engine/guest_probe.h"
#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/gbuffer_draw.h"
#include "native_engine/reconstructed/gbuffer_trace.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

namespace GD = Reconstructed::GBufferDraw;
namespace GT = Reconstructed::GTrace;

int g_fail = 0;
int* g_checks_out = nullptr;

#define R6G_CHECK(cond)                                                                                               \
    do {                                                                                                              \
        ++*g_checks_out;                                                                                              \
        if (!(cond)) {                                                                                                \
            ++g_fail;                                                                                                 \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
        }                                                                                                             \
    } while (0)

/// Writable code bytes, restored on destruction.
class Patch {
public:
    Patch(u64 at, const void* bytes, u32 size) : at_(at), size_(size) {
        std::memcpy(saved_, reinterpret_cast<const void*>(at), size);
        Write(bytes);
    }
    ~Patch() {
        Write(saved_);
    }
    Patch(const Patch&) = delete;
    Patch& operator=(const Patch&) = delete;

private:
    void Write(const void* bytes) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(at_), size_, PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(reinterpret_cast<void*>(at_), bytes, size_);
        VirtualProtect(reinterpret_cast<void*>(at_), size_, old, &old);
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at_), size_);
    }
    u64 at_;
    u32 size_;
    u8 saved_[16];
};

std::vector<u8> Jump(u64 to) {
    std::vector<u8> j = {0xff, 0x25, 0, 0, 0, 0};
    for (u32 i = 0; i < 8; ++i) {
        j.push_back(static_cast<u8>(to >> (8 * i)));
    }
    return j;
}

/// Generated memory for one call (as the install's self-check, gbuffer_draw.cpp).
struct Memory {
    std::vector<u8> mem = std::vector<u8>(0x10000);
    std::vector<u8> r = std::vector<u8>(GD::kShadowBytes);
    std::mt19937_64 rng;
    explicit Memory(u64 seed) : rng(seed) {}
    u64 b() {
        return reinterpret_cast<u64>(mem.data());
    }
    void Fill() {
        for (u64 i = 0; i < mem.size(); i += 8) {
            const u64 v = rng();
            std::memcpy(mem.data() + i, &v, 8);
        }
        std::fill(r.begin(), r.end(), u8{0});
        const u32 st = static_cast<u32>(rng());
        std::memcpy(r.data() + GD::kStencilState, &st, 4);
    }
    void P64(u64 o, u64 v) {
        std::memcpy(mem.data() + o, &v, 8);
    }
    // 0x1a1770: material b, mesh b+0x400, records b+0x4000
    void Instanced() {
        Fill();
        if (rng() % 4 == 0) {
            mem[0x1c] = 0;
        }
        if (rng() % 3 == 0) {
            mem[0x135] = 0xff;
        }
        if (rng() % 3 == 0) {
            mem[0x13c] = 0xff;
        }
        mem[0x46c] = static_cast<u8>(rng() % 33);
        P64(0x460, b() + 0x1000);
        if (rng() % 2 == 0) { // the state this call computes: no stencil write
            const u8 lo = mem[0x1c], hi = static_cast<u8>(mem[0x1d] | mem[0x41d]);
            const u32 v = (u32{lo} << 16) + (u32{hi} << 24) + 0xff00u;
            std::memcpy(r.data() + GD::kStencilState, &v, 4);
        }
    }
    // 0x1a1960: material b (+0xf0 -> 0x600, +0x10 -> 0x700 -> +0x58 -> 0x800), instance b+0x200 (+0x18 -> 0x300 -> mesh
    // 0x400), skin 0x900 (+8 -> 0xb00 -> +0x58 -> 0xc00 the indices), streams 0x1000
    void Mesh(bool followed_only) {
        Fill();
        P64(0xf0, b() + 0x600);
        P64(0x10, b() + 0x700);
        P64(0x758, b() + 0x800);
        P64(0x218, b() + 0x300);
        P64(0x300, b() + 0x400);
        const u32 n = static_cast<u32>(rng() % 17);
        mem[0x46c] = static_cast<u8>(n);
        P64(0x460, b() + 0x1000);
        const bool skin = rng() % 3 != 0;
        P64(0x220, skin ? b() + 0x900 : 0);
        if (rng() % 4 == 0) {
            mem[0x900] &= 0xf8;
        }
        P64(0x908, b() + 0xb00);
        P64(0xb58, b() + 0xc00);
        const u32 lim = std::max<u32>(n, 1);
        mem[0xc00] = static_cast<u8>(followed_only ? rng() % lim : (rng() % 16 == 0 ? rng() % GD::kMaxStreams : rng() % lim));
        mem[0xc01] = static_cast<u8>(rng() % 3 == 0 ? 0x80 | rng() : rng() % lim);
        mem[0xc02] = static_cast<u8>(rng() % 3 == 0 ? 0x80 | rng() : rng() % lim);
        if (rng() % 3 == 0) {
            P64(0x920, 0);
        }
        if (rng() % 3 == 0) {
            mem[0xd8] = 0xff;
        }
    }
};

struct Recording {
    GT::Trace t;
    GT::Call calls[64];
    u8 snap[0x4000];
    void Begin(u64 top) {
        t.callees = GD::kCalleeSpecs;
        t.ncallees = GD::kCallees;
        t.calls = calls;
        t.max_calls = 64;
        t.snap = snap;
        t.snap_cap = sizeof(snap);
        t.nregions = 0;
        t.Begin(0x5347425546464552ull, top);
    }
};

/// Calls `fn` with `rec` active and the shadow context `r` registered as region 0.
template <typename F>
void Run(Recording& rec, u8* r, u64 top, F&& fn) {
    rec.Begin(top);
    rec.t.AddRegion(reinterpret_cast<u64>(r), GD::kShadowBytes);
    GT::SetActive(&rec.t);
    fn();
    GT::SetActive(nullptr);
}

void SelfChecks(const char* what, u64 cases) {
    const u32 n = static_cast<u32>(std::min<u64>(cases, 200000));
    const std::string a = GD::SelfCheckInstanced(n, 0x1a1770 ^ cases);
    const std::string b = GD::SelfCheckMesh(n, 0x1a1960 ^ cases);
    std::printf("  %s vs eboot 0x1a1770 (recording clone): %u cases%s%s\n", what, n, a.empty() ? ", equal" : ": ",
                a.c_str());
    std::printf("  %s vs eboot 0x1a1960 (recording clone): %u cases%s%s\n", what, n, b.empty() ? ", equal" : ": ",
                b.c_str());
    R6G_CHECK(a.empty());
    R6G_CHECK(b.empty());
}

/// Both installed on the image, called through their entries in every mode against the recording clone.
void Installed(u64 base, u64 cases) {
    GD::GotForChecks gotchecks(base);
    R6G_CHECK(gotchecks.ok());

    // 0x1a1960 below an entry probe (CITY-OPT's site: 20 prologue bytes); 0x1a1770 a plain jump
    static GD::MeshFn probe_next = nullptr;
    static u64 probe_calls = 0;
    struct Probe {
        static SSPC_SYSV_ABI void Mesh(u64 m, u64 r, u64 i) {
            ++probe_calls;
            probe_next(m, r, i);
        }
    };
    std::string error;
    const Engine::ProbeSite site{Reconstructed::kGBufferMeshDrawSpec.function, 20};
    void* original = Engine::InstallEntryProbe(base, site, reinterpret_cast<const void*>(&Probe::Mesh), error,
                                               reinterpret_cast<void**>(&probe_next));
    R6G_CHECK(original != nullptr);
    if (original == nullptr) {
        std::printf("  entry probe on 0x1a1960: %s\n", error.c_str());
    }
    probe_next = reinterpret_cast<GD::MeshFn>(original);
    const FunctionSpec& mspec = Reconstructed::kGBufferMeshDrawSpec;
    const FunctionSpec& ispec = Reconstructed::kGBufferInstancedDrawSpec;
    error.clear();
    const bool mi = original != nullptr && InstallFunction(base, mspec, Mode::Native, error);
    if (!mi) {
        std::printf("  install 0x1a1960 below the probe: %s\n", error.c_str());
    }
    R6G_CHECK(mi && InstalledBelowProbe(mspec) && probe_next == mspec.entry);
    error.clear();
    const bool ii = InstallFunction(base, ispec, Mode::Native, error);
    if (!ii) {
        std::printf("  install 0x1a1770: %s\n", error.c_str());
    }
    R6G_CHECK(ii);
    // the image's callees -> the trace thunks (functions: a jump at the entry; imports: their GOT slot); after the
    // installs, which check the callees' hashes
    std::vector<Patch*> patches;
    std::vector<std::pair<u64*, u64>> got;
    for (u32 c = 0; c < GD::kCallees; ++c) {
        const u64 at = base + GD::kCalleeSpecs[c].rva;
        const u8* p = reinterpret_cast<const u8*>(at);
        if (c == GD::kStackFail || c == GD::kMemmove) {
            R6G_CHECK(p[0] == 0xff && p[1] == 0x25);
            if (p[0] != 0xff || p[1] != 0x25) {
                continue;
            }
            s32 rel = 0;
            std::memcpy(&rel, p + 2, 4);
            u64* slot = reinterpret_cast<u64*>(at + 6 + rel);
            got.push_back({slot, *slot});
            *slot = GT::ThunkAddress(c);
            continue;
        }
        const std::vector<u8> j = Jump(GT::ThunkAddress(c));
        patches.push_back(new Patch(at, j.data(), static_cast<u32>(j.size())));
    }
    if (mi && ii) {
        const auto ientry = reinterpret_cast<GD::InstancedFn>(base + ispec.function.rva);
        const auto mentry = reinterpret_cast<GD::MeshFn>(base + mspec.function.rva);
        Memory m(0x52364700 ^ cases);
        static Recording a, b;
        volatile u8 top = 0;
        const u64 t = reinterpret_cast<u64>(&top);
        for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
            ispec.slot->mode.store(static_cast<u8>(mode));
            mspec.slot->mode.store(static_cast<u8>(mode));
            ispec.slot->sample_period = 3;
            mspec.slot->sample_period = 3;
            u64 differ[2] = {}, probed = probe_calls;
            std::string first;
            for (u32 n = 0; n < 2000; ++n) {
                for (u32 f = 0; f < 2; ++f) {
                    std::vector<u8> r2;
                    if (f == 0) {
                        m.Instanced();
                    } else {
                        m.Mesh(false);
                    }
                    r2 = m.r;
                    const u64 mb = m.b();
                    const u64 cnt = m.rng() % 64;
                    if (f == 0) {
                        Run(a, m.r.data(), t,
                            [&] { ientry(mb, reinterpret_cast<u64>(m.r.data()), mb + 0x4000, cnt, mb + 0x400); });
                        Run(b, r2.data(), t, [&] {
                            GD::RecordingInstanced()(mb, reinterpret_cast<u64>(r2.data()), mb + 0x4000, cnt, mb + 0x400);
                        });
                    } else {
                        Run(a, m.r.data(), t, [&] { mentry(mb, reinterpret_cast<u64>(m.r.data()), mb + 0x200); });
                        Run(b, r2.data(), t,
                            [&] { GD::RecordingMesh()(mb, reinterpret_cast<u64>(r2.data()), mb + 0x200); });
                    }
                    std::string d = GT::Compare(a.t, b.t, false);
                    if (d.empty() && std::memcmp(m.r.data() + GD::kStencilState, r2.data() + GD::kStencilState, 4) != 0) {
                        d = "stencil state";
                    }
                    if (!d.empty() && differ[f]++ == 0 && first.empty()) {
                        first = fmt::format("{} call {}: {}", f == 0 ? "0x1a1770" : "0x1a1960", n, d);
                    }
                }
            }
            probed = probe_calls - probed;
            std::printf("  installed %-8s: 2000 + 2000 calls through the entries, different from the original %llu / "
                        "%llu, the probe saw %llu%s%s\n",
                        ModeName(mode), static_cast<unsigned long long>(differ[0]),
                        static_cast<unsigned long long>(differ[1]), static_cast<unsigned long long>(probed),
                        first.empty() ? "" : "; first: ", first.c_str());
            R6G_CHECK(differ[0] == 0 && differ[1] == 0 && probed == 2000);
        }
        for (const FunctionSpec* s : {&ispec, &mspec}) {
            const Slot& sl = *s->slot;
            std::printf("  %s: verified %llu, mismatches %llu, skipped %llu\n", s->key,
                        static_cast<unsigned long long>(sl.verified.load()),
                        static_cast<unsigned long long>(sl.mismatches.load()),
                        static_cast<unsigned long long>(sl.skipped.load()));
            R6G_CHECK(sl.verified.load() > 0 && sl.mismatches.load() == 0);
        }
        // a wrong implementation under guarded: caught on its first sample, the original's calls made, mode Original
        for (u32 f = 0; f < 2; ++f) {
            Slot& g = *(f == 0 ? ispec.slot : mspec.slot);
            GD::InjectFault(f + 1);
            g.mode.store(static_cast<u8>(Mode::Guarded));
            g.sample_period = 1;
            const u64 mism = g.mismatches.load(), fb = g.fallbacks.load();
            std::vector<u8> r2;
            if (f == 0) {
                m.Instanced();
            } else {
                m.Mesh(true);
            }
            r2 = m.r;
            const u64 mb = m.b();
            if (f == 0) {
                Run(a, m.r.data(), t, [&] { ientry(mb, reinterpret_cast<u64>(m.r.data()), mb + 0x4000, 5, mb + 0x400); });
                Run(b, r2.data(), t, [&] {
                    GD::RecordingInstanced()(mb, reinterpret_cast<u64>(r2.data()), mb + 0x4000, 5, mb + 0x400);
                });
            } else {
                Run(a, m.r.data(), t, [&] { mentry(mb, reinterpret_cast<u64>(m.r.data()), mb + 0x200); });
                Run(b, r2.data(), t, [&] { GD::RecordingMesh()(mb, reinterpret_cast<u64>(r2.data()), mb + 0x200); });
            }
            GD::InjectFault(0);
            const bool same = GT::Compare(a.t, b.t, false).empty();
            const bool caught = g.mismatches.load() == mism + 1 && g.fallbacks.load() == fb + 1 &&
                                static_cast<Mode>(g.mode.load()) == Mode::Original;
            std::printf("  %s, a wrong implementation under guarded: caught %s, the original's calls %s\n",
                        f == 0 ? "0x1a1770" : "0x1a1960", caught ? "yes" : "NO", same ? "made" : "NOT made");
            R6G_CHECK(caught && same);
        }
        ispec.slot->mode.store(static_cast<u8>(Mode::Original));
        mspec.slot->mode.store(static_cast<u8>(Mode::Original));
    }
    for (auto& [slot, v] : got) {
        *slot = v;
    }
    for (Patch* p : patches) {
        delete p;
    }
}

} // namespace

int R6GBufferTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases, int& checks) {
    (void)image_size, (void)recomp_dll;
    g_fail = 0;
    g_checks_out = &checks;
    std::printf("R6-G geometry draws 0x1a1770 / 0x1a1960 (gbuffer_draw.h):\n");
    {
        GD::GotForChecks got(image_base);
        R6G_CHECK(got.ok());
        std::string error;
        const bool prepared = GD::Prepare(image_base, error);
        if (!prepared) {
            std::printf("  prepare: %s\n", error.c_str());
        }
        R6G_CHECK(prepared);
        if (!prepared) {
            return g_fail;
        }
        GD::UseRecompiled(nullptr, nullptr, nullptr);
        SelfChecks("hand-written", cases);
        // the comparison sees a wrong implementation
        GD::InjectFault(1);
        const std::string f1 = GD::SelfCheckInstanced(200, 7);
        GD::InjectFault(2);
        const std::string f2 = GD::SelfCheckMesh(200, 7);
        GD::InjectFault(0);
        std::printf("  wrong implementations seen: 0x1a1770 \"%s\", 0x1a1960 \"%s\"\n", f1.c_str(), f2.c_str());
        R6G_CHECK(!f1.empty() && !f2.empty());
        if (recomp_module != nullptr) {
            const HMODULE dll = static_cast<HMODULE>(recomp_module);
            const auto inst = reinterpret_cast<GD::InstancedFn>(GetProcAddress(dll, "recomp_1a1770"));
            const auto mesh = reinterpret_cast<GD::MeshFn>(GetProcAddress(dll, "recomp_1a1960"));
            const auto redirects = reinterpret_cast<GD::RedirectsFn>(GetProcAddress(dll, "recomp_set_redirects"));
            if (inst != nullptr && mesh != nullptr && redirects != nullptr) {
                GD::UseRecompiled(inst, mesh, redirects);
                SelfChecks("recompiled", cases);
                GD::UseRecompiled(nullptr, nullptr, nullptr);
            } else {
                std::printf("  (the recompiled module has no recomp_1a1770 / recomp_1a1960)\n");
            }
        }
    }
    Installed(image_base, cases);
    std::printf("  orphan thunk calls: %llu\n", static_cast<unsigned long long>(GT::OrphanCalls()));
    R6G_CHECK(GT::OrphanCalls() == 0);
    return g_fail;
}

} // namespace SecondSon::NativeEngine::Tests
