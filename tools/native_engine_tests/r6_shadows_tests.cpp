// SecondSonPC — the tests of R6-S (shadow engine), owned by that branch alone (docs/NATIVE_RENDER_R6_CORE.md §3). main.cpp calls
// R6ShadowsTests with the eboot mapped (before the CUE setters' jumps go into the image); it returns the number of failures and
// adds its checks to the total through the counters it is given.
//
// The leaves of the shadow casters loop 0x43d590 that work memory to memory, recompiled (recomp.dll from
// tools/native-reconstruction/recomp) against clones of the eboot's own code, bit for bit:
//   0x612110 / 0x612150  the ISAAC-style generator the loop draws from (state 0x810 bytes, refill through a jump table)
//   0x451290             the per-pass parameter writer: three record kinds into a constants block through 0x11e5d0,
//                        one of them through 0x60d4c0 (vector normalisation + an imported libm call)
//   0x450460             the light/material parameter writer: 18 record kinds through a jump table (kind 4 holds the
//                        vpinsrd lanes SLEIGH mis-lifted before the pcode2c.py repair)
// Imported libm calls go through GOT slots the test fills with stand-ins (the same for both sides); the stack guard's
// slot and 0x451290/0x450460's lazily built singleton are set for the run and restored.
// docs/NATIVE_RENDER_R6_SHADOWS.md §4.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include <windows.h>
#include <xmmintrin.h>

#include "base/abi.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/reconstructed/shadow_trace.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

constexpr u64 kGuardSlot = 0xd9de00;   // GOT: __stack_chk_guard (the functions compare it on return)
constexpr u64 kLibmSlot = 0xd9e190;    // GOT of the import stub 0x984530 that 0x60d4c0 calls
constexpr u64 kSingleton = 0x7f8e800;  // 0x451290's lazily built object (+0x3d0: a float scale); 0x59d700 builds it
constexpr u32 kPrngBytes = 0x810;

using Fn1 = SSPC_SYSV_ABI u64 (*)(u64);
using Fn6 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64);

/// The import 0x60d4c0 calls (the test's stand-in: the same for both sides, deterministic, a float in and out).
SSPC_SYSV_ABI float LibmStandIn(float x) {
    return std::exp2(x);
}

void* Clone(u64 image_base, u64 rva, u32 size) {
    std::string error;
    const CloneResult c =
        CloneGuestFunction(image_base + rva, reinterpret_cast<const u8*>(image_base + rva), size, error);
    if (c.code == nullptr) {
        std::printf("  clone of %#llx failed: %s\n", static_cast<unsigned long long>(rva), error.c_str());
    }
    return c.code;
}

float RandomFloat(std::mt19937_64& rng) {
    switch (rng() % 16) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return std::ldexp(static_cast<float>(rng() % 1000), -140); // denormal
    case 3: return (rng() & 1) ? INFINITY : -INFINITY;
    case 4: return std::nanf("");
    case 5: return std::ldexp(static_cast<float>(rng() % 1000) / 1000.0f, -20); // tiny (0x60d4c0's epsilon test)
    default: {
        const float v = static_cast<float>(static_cast<s64>(rng() % 2000001) - 1000000) / 1000.0f;
        return (rng() % 4 == 0) ? v * 1000.0f : v;
    }
    }
}

// --- 0x612110 / 0x612150 -----------------------------------------------------------------------------------------

u64 PrngDifferential(Fn1 original, Fn1 recompiled, u64 cases, std::mt19937_64& rng, bool refill_only, std::string& first) {
    std::vector<u8> a(kPrngBytes), b(kPrngBytes);
    u64 bad = 0;
    for (u64 n = 0; n < cases; ++n) {
        for (u8& x : a) {
            x = static_cast<u8>(rng());
        }
        u32 counter;
        switch (rng() % 4) {
        case 0: counter = 0x100; break;                                   // exactly the refill threshold
        case 1: counter = 0x100 + static_cast<u32>(rng() % 0x10000); break; // above it (refill too)
        default: counter = static_cast<u32>(rng() % 0x100); break;
        }
        std::memcpy(a.data() + 0x40c, &counter, 4);
        b = a;
        const u64 ra = original(reinterpret_cast<u64>(a.data()));
        const u64 rb = recompiled(reinterpret_cast<u64>(b.data()));
        // 0x612150 returns nothing (its RAX is the loop's leftover): only the state counts there
        const bool same_ret = refill_only || static_cast<u32>(ra) == static_cast<u32>(rb);
        if ((!same_ret || a != b) && bad++ == 0) {
            u32 at = 0;
            while (at < kPrngBytes && a[at] == b[at]) {
                ++at;
            }
            first = "case " + std::to_string(n) + (same_ret ? "" : ": return differs") +
                    (at < kPrngBytes ? ": state +" + std::to_string(at) : "");
        }
    }
    return bad;
}

// --- 0x451290 ----------------------------------------------------------------------------------------------------

/// One generated call of 0x451290(records, constants, atlas, view, table, light): up to 4 records of kinds 0xe, 0xf,
/// 0x11 or another (skipped), slot numbers kept inside the constants block.
struct ParamCase {
    std::vector<u8> records = std::vector<u8>(0x50);
    std::vector<u8> constants = std::vector<u8>(0x500);
    std::vector<u8> atlas = std::vector<u8>(0x80);
    std::vector<u8> view = std::vector<u8>(0x200);
    std::vector<u8> table = std::vector<u8>(0x400);
    std::vector<u8> light = std::vector<u8>(0x60);
    std::vector<u8> singleton = std::vector<u8>(0x400);

    template <typename T>
    static void Put(std::vector<u8>& v, u32 at, T x) {
        std::memcpy(v.data() + at, &x, sizeof(T));
    }

    void Generate(std::mt19937_64& rng) {
        for (auto* v : {&records, &constants, &view, &table, &light, &singleton}) {
            for (u32 i = 0; i + 4 <= v->size(); i += 4) {
                Put(*v, i, RandomFloat(rng));
            }
        }
        std::fill(atlas.begin(), atlas.end(), 0);
        const s32 count = static_cast<s32>(rng() % 6) - 1; // -1 and 0: the loop is skipped
        Put(records, 0, count);
        static constexpr s32 kKinds[] = {0xe, 0xf, 0x11, 0x10, 0};
        for (u32 i = 0; i < 4; ++i) {
            Put(records, 8 + 4 * i, kKinds[rng() % 5]);
            Put(records, 0x18 + 4 * i, static_cast<s32>(rng() % 8)); // the parameter's index into the table
        }
        for (const u32 at : {0x3cu, 0x3eu, 0x42u}) {
            Put(records, at, static_cast<u16>(rng() % 0x100)); // the constant's slot (16 bytes max from there)
        }
        Put(atlas, 0x5c, static_cast<s32>(rng() % 4));
        Put(atlas, 0x60, static_cast<s32>(rng() % 4));
        // the dirty byte of the block is sometimes already set
        constants[0x404] = static_cast<u8>(rng() % 2);
    }
};

u64 ParamDifferential(Fn6 original, Fn6 recompiled, u64 image_base, u64 cases, std::mt19937_64& rng,
                      std::string& first) {
    ParamCase c;
    u64 bad = 0;
    auto* singleton_slot = reinterpret_cast<u64*>(image_base + kSingleton);
    for (u64 n = 0; n < cases; ++n) {
        c.Generate(rng);
        *singleton_slot = reinterpret_cast<u64>(c.singleton.data());
        const std::vector<u8> records = c.records, constants = c.constants, view = c.view, table = c.table,
                              light = c.light, atlas = c.atlas;
        const auto run = [&](Fn6 fn) {
            fn(reinterpret_cast<u64>(c.records.data()), reinterpret_cast<u64>(c.constants.data()),
               reinterpret_cast<u64>(c.atlas.data()), reinterpret_cast<u64>(c.view.data()),
               reinterpret_cast<u64>(c.table.data()), reinterpret_cast<u64>(c.light.data()));
        };
        run(original);
        const std::vector<u8> out_a = c.constants;
        const bool inputs_kept_a = c.records == records && c.view == view && c.table == table && c.light == light &&
                                   c.atlas == atlas;
        c.constants = constants;
        run(recompiled);
        const bool inputs_kept_b = c.records == records && c.view == view && c.table == table && c.light == light &&
                                   c.atlas == atlas;
        if ((out_a != c.constants || !inputs_kept_a || !inputs_kept_b) && bad++ == 0) {
            u32 at = 0;
            while (at < out_a.size() && out_a[at] == c.constants[at]) {
                ++at;
            }
            first = "case " + std::to_string(n) + ": constants +" + std::to_string(at) +
                    (inputs_kept_a && inputs_kept_b ? "" : " (an input changed)");
        }
    }
    *singleton_slot = 0;
    return bad;
}

// --- 0x450460 ----------------------------------------------------------------------------------------------------

constexpr u64 kSinCosSlot = 0xd9e0e8; // GOT of the import stub 0x9843e0 (0x600560 calls it twice: (1, 0, x), (0, 0, y))
constexpr u64 kPowSlot = 0xd9e2e8;    // GOT of the import stub 0x9847e0 (kind 7: a float of two)

SSPC_SYSV_ABI float SinCosStandIn(s32 which, s32 zero, float x) {
    return which != 0 ? std::cos(x) + static_cast<float>(zero) : std::sin(x);
}
SSPC_SYSV_ABI float PowStandIn(float a, float b) {
    return std::pow(a, b);
}

using Fn7 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64, u64);

/// One generated call of 0x450460(records, constants, atlas, view, table, light, owner): the light/material parameter
/// writer of the shadow loop - up to 4 records of kinds 0..0x11 through a jump table, each into the constants block
/// through 0x11e5d0. Kinds covered: all but 0x10 (0x4502e0 takes a lock on a global object) and kind 0xa only with
/// no attached object (its other branch calls a virtual method and two lookups on that object).
struct WriterCase {
    std::vector<u8> records = std::vector<u8>(0x50);
    std::vector<u8> constants = std::vector<u8>(0x500);
    std::vector<u8> atlas = std::vector<u8>(0x80);
    std::vector<u8> view = std::vector<u8>(0x200);
    std::vector<u8> table = std::vector<u8>(0x400);
    std::vector<u8> light = std::vector<u8>(0x100);
    std::vector<u8> entries = std::vector<u8>(6 * 0x40);
    std::vector<u8> owner = std::vector<u8>(0x40);
    std::vector<u8> owner_target = std::vector<u8>(0x80);
    std::vector<u8> singleton = std::vector<u8>(0x400);

    template <typename T>
    static void Put(std::vector<u8>& v, u32 at, T x) {
        std::memcpy(v.data() + at, &x, sizeof(T));
    }

    void Generate(std::mt19937_64& rng) {
        for (auto* v : {&records, &constants, &view, &table, &light, &entries, &owner, &owner_target, &singleton}) {
            for (u32 i = 0; i + 4 <= v->size(); i += 4) {
                Put(*v, i, RandomFloat(rng));
            }
        }
        std::fill(atlas.begin(), atlas.end(), 0);
        const s32 a5c = static_cast<s32>(rng() % 4), a60 = static_cast<s32>(rng() % 4);
        Put(atlas, 0x5c, a5c);
        Put(atlas, 0x60, a60);
        const u32 region = static_cast<u32>(a5c) * 32 + static_cast<u32>(a60) * 16 + 0x20; // the parameters' base
        const s32 count = static_cast<s32>(rng() % 6) - 1;
        Put(records, 0, count);
        static constexpr s32 kKinds[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0xa, 0xb, 0xc, 0xd, 0xe, 0xf, 0x11, 0x12, -1};
        for (u32 i = 0; i < 4; ++i) {
            const s32 kind = kKinds[rng() % std::size(kKinds)];
            const s32 index = static_cast<s32>(8 * i);
            Put(records, 8 + 4 * i, kind);
            Put(records, 0x18 + 4 * i, index);
            const u32 at = region + static_cast<u32>(index) * 4;
            switch (kind) {
            case 2: case 5: // two nonzero integers (divisors)
                Put(table, at, static_cast<s32>(1 + rng() % 16) * ((rng() & 1) ? 1 : -1));
                Put(table, at + 4, static_cast<s32>(1 + rng() % 16));
                break;
            case 3: // +8 and +0xc truncate to positive integers (divisors)
                Put(table, at + 8, static_cast<float>(1 + rng() % 16) + 0.25f * static_cast<float>(rng() % 4));
                Put(table, at + 0xc, static_cast<float>(1 + rng() % 16) + 0.25f * static_cast<float>(rng() % 4));
                break;
            case 4: // the colour source: 2 (packed in the view), 1 (scaled from the view), other (white)
                Put(table, at, static_cast<s32>(rng() % 4));
                break;
            default:
                break;
            }
        }
        for (u32 at = 0x28; at < 0x44; at += 2) {
            Put(records, at, static_cast<u16>(rng() % 0x100)); // slots (and the kind 0xc/0xd pair at +0x38/+0x3a)
        }
        // the light: kinds 6/9 search its 64-byte entries for the table's key (+0x10), remembering a free one (-1)
        const u32 n = static_cast<u32>(rng() % 7);
        const u64 key = rng() % 4;
        Put(table, 0x10, key);
        for (u32 i = 0; i < 6; ++i) {
            const u64 r = rng() % 6;
            Put(entries, i * 0x40, r < 4 ? r : ~0ull);
        }
        Put(light, 0x40, reinterpret_cast<u64>(entries.data()));
        Put(light, 0x58, n);
        Put(light, 0x88, u64{0});
        // the owner (7th, stack): bytes +0x11/+0x12, +0 a pointer to an object with a float at +0x44
        Put(owner, 0, reinterpret_cast<u64>(owner_target.data()));
        constants[0x404] = static_cast<u8>(rng() % 2);
    }
};

u64 WriterDifferential(Fn7 original, Fn7 recompiled, u64 image_base, u64 cases, std::mt19937_64& rng,
                       std::string& first) {
    WriterCase c;
    u64 bad = 0;
    auto* singleton_slot = reinterpret_cast<u64*>(image_base + kSingleton);
    for (u64 n = 0; n < cases; ++n) {
        c.Generate(rng);
        *singleton_slot = reinterpret_cast<u64>(c.singleton.data());
        const WriterCase in = c;
        const auto run = [&](Fn7 fn) {
            fn(reinterpret_cast<u64>(c.records.data()), reinterpret_cast<u64>(c.constants.data()),
               reinterpret_cast<u64>(c.atlas.data()), reinterpret_cast<u64>(c.view.data()),
               reinterpret_cast<u64>(c.table.data()), reinterpret_cast<u64>(c.light.data()),
               reinterpret_cast<u64>(c.owner.data()));
        };
        const auto inputs_kept = [&] {
            return c.records == in.records && c.view == in.view && c.table == in.table && c.light == in.light &&
                   c.atlas == in.atlas && c.entries == in.entries && c.owner == in.owner &&
                   c.owner_target == in.owner_target && c.singleton == in.singleton;
        };
        run(original);
        const std::vector<u8> out_a = c.constants;
        const bool kept_a = inputs_kept();
        c.constants = in.constants;
        run(recompiled);
        const bool kept_b = inputs_kept();
        if ((out_a != c.constants || !kept_a || !kept_b) && bad++ == 0) {
            u32 at = 0;
            while (at < out_a.size() && out_a[at] == c.constants[at]) {
                ++at;
            }
            s32 kinds[4];
            std::memcpy(kinds, c.records.data() + 8, sizeof(kinds));
            first = "case " + std::to_string(n) + ": constants +" + std::to_string(at) + " (kinds " +
                    std::to_string(kinds[0]) + "," + std::to_string(kinds[1]) + "," + std::to_string(kinds[2]) + "," +
                    std::to_string(kinds[3]) + ")" + (kept_a && kept_b ? "" : " (an input changed)");
        }
    }
    *singleton_slot = 0;
    return bad;
}

// --- 0x43d480 (instanced shadow draw): an orchestrator, judged by its calls (shadow_trace) ------------------------

namespace ST = Reconstructed::STrace;

/// The stage the effect of 0x27530 (the current shader stage of the context) returns: the same sequence in both runs.
u64 StageEffect(ST::Trace& t, const ST::CalleeSpec&, ST::CallIn&, const u64*) {
    return ST::Next(t) & 0xffffffffull;
}

// 0x61e260 (the V# of the instance records) is not recorded: it runs for real into 0x43d480's own frame, and 0x16b70
// is compared by the 16 bytes its fifth argument points at.
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
constexpr u32 kDrawCalleeCount = static_cast<u32>(std::size(kDrawCallees));

struct DrawRecording {
    ST::Trace t;
    ST::Call calls[32];
    u8 snap[0x400];
    void Begin(u64 top, u64 context, u64 bytes) {
        t.callees = kDrawCallees;
        t.ncallees = kDrawCalleeCount;
        t.calls = calls;
        t.max_calls = 32;
        t.snap = snap;
        t.snap_cap = sizeof(snap);
        t.nregions = 0;
        t.Begin(0x43d480, top);
        t.AddRegion(context, bytes);
    }
};

using RedirectsFn = int (*)(const u64*, const u64*, u32);

u64 DrawDifferential(Fn6 original, Fn6 recompiled, RedirectsFn redirects, u64 image_base, u64 cases,
                     std::mt19937_64& rng, std::string& first, u64& calls) {
    u64 from[kDrawCalleeCount], to[kDrawCalleeCount];
    for (u32 i = 0; i < kDrawCalleeCount; ++i) {
        from[i] = image_base + kDrawCallees[i].rva;
        to[i] = ST::ThunkAddress(i);
    }
    std::vector<u8> pass(0x200), context(0x200), mesh(0x80);
    std::vector<u8> records(16 * 0x48);
    static DrawRecording a, b;
    volatile u8 top = 0;
    const u64 stack_top = reinterpret_cast<u64>(&top);
    u64 bad = 0;
    for (u64 n = 0; n < cases; ++n) {
        for (auto* v : {&pass, &context, &mesh, &records}) {
            for (u8& x : *v) {
                x = static_cast<u8>(rng());
            }
        }
        // records: sometimes the context's own memory, sometimes elsewhere (a pointer value, compared as such)
        const u64 recs = (rng() & 1) ? reinterpret_cast<u64>(records.data()) : rng();
        const u64 count = (rng() & 1) ? rng() % 64 : rng(); // the instance count (32-bit in the callee)
        const auto run = [&](DrawRecording& rec, Fn6 fn) {
            rec.Begin(stack_top, reinterpret_cast<u64>(context.data()), context.size());
            ST::SetActive(&rec.t);
            fn(reinterpret_cast<u64>(pass.data()), reinterpret_cast<u64>(context.data()), recs, count,
               reinterpret_cast<u64>(mesh.data()), 0x9999);
            ST::SetActive(nullptr);
        };
        run(a, original);
        if (redirects(from, to, kDrawCalleeCount) == 0) {
            first = "recomp_set_redirects refused";
            return cases;
        }
        run(b, recompiled);
        redirects(nullptr, nullptr, 0);
        calls += a.t.ncalls;
        std::string d = ST::Compare(a.t, b.t, true);
        if (d.empty() && a.t.ncalls != kDrawCalleeCount) {
            d = "the original made " + std::to_string(a.t.ncalls) + " recorded calls";
        }
        if (!d.empty() && bad++ == 0) {
            first = "case " + std::to_string(n) + ": " + d;
        }
    }
    return bad;
}

/// Restores a GOT slot of the mapped image on scope exit.
struct SlotOverride {
    u64* slot;
    u64 saved;
    SlotOverride(u64 image_base, u64 rva, u64 value) : slot(reinterpret_cast<u64*>(image_base + rva)), saved(*slot) {
        *slot = value;
    }
    ~SlotOverride() {
        *slot = saved;
    }
};

} // namespace

int R6ShadowsTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases, int& checks) {
    (void)recomp_dll;
    std::printf("R6-S shadow loop leaves (recompiled vs the eboot's code):\n");
    if (recomp_module == nullptr) {
        std::printf("  (no --recomp: skipped)\n");
        return 0;
    }
    if (image_size < kSingleton + 8) {
        std::printf("  mapped image too small (%#llx): skipped\n", static_cast<unsigned long long>(image_size));
        return 0;
    }
    HMODULE m = reinterpret_cast<HMODULE>(recomp_module);
    if (auto* base = reinterpret_cast<u64*>(GetProcAddress(m, "recomp_image_base"))) {
        *base = image_base;
    }
    int failures = 0;
    std::mt19937_64 rng{0x43d590};
    static u64 guard = 0x5ec0'7a1d'0000'0001ull;
    const SlotOverride guard_slot(image_base, kGuardSlot, reinterpret_cast<u64>(&guard));
    const SlotOverride libm_slot(image_base, kLibmSlot, reinterpret_cast<u64>(&LibmStandIn));
    const u32 host_csr = _mm_getcsr();

    // 0x612150 (refill) and 0x612110 (draw one word)
    struct Prng {
        const char* name;
        u64 rva;
        u32 size;
        bool refill_only;
    };
    for (const Prng& p : {Prng{"612150", 0x612150, 0xf0, true}, Prng{"612110", 0x612110, 0x3d, false}}) {
        const std::string sym = std::string("recomp_") + p.name;
        auto* r = reinterpret_cast<Fn1>(GetProcAddress(m, sym.c_str()));
        auto* o = reinterpret_cast<Fn1>(Clone(image_base, p.rva, p.size));
        if (r == nullptr || o == nullptr) {
            std::printf("  %s: %s\n", sym.c_str(), r == nullptr ? "not in recomp.dll" : "clone failed");
            ++checks;
            ++failures;
            continue;
        }
        std::string first;
        const u64 bad = PrngDifferential(o, r, cases, rng, p.refill_only, first);
        std::printf("  %s vs eboot %#llx: %llu cases, %llu different%s%s\n", sym.c_str(),
                    static_cast<unsigned long long>(p.rva), static_cast<unsigned long long>(cases),
                    static_cast<unsigned long long>(bad), bad ? "; first: " : "", first.c_str());
        ++checks;
        failures += bad ? 1 : 0;
    }

    // 0x451290 under the hardware MXCSR and the modes a guest thread may run with
    {
        auto* r = reinterpret_cast<Fn6>(GetProcAddress(m, "recomp_451290"));
        auto* o = reinterpret_cast<Fn6>(Clone(image_base, 0x451290, 0x1a5));
        if (r == nullptr || o == nullptr) {
            std::printf("  recomp_451290: %s\n", r == nullptr ? "not in recomp.dll" : "clone failed");
            ++checks;
            ++failures;
        } else {
            for (const u32 mode : {host_csr, 0x9fc0u, 0x3f80u, 0x5f80u, 0x7f80u}) {
                _mm_setcsr(mode);
                std::string first;
                const u64 n = mode == host_csr ? cases : cases / 4;
                const u64 bad = ParamDifferential(o, r, image_base, n, rng, first);
                _mm_setcsr(host_csr);
                std::printf("  recomp_451290 vs eboot 0x451290 (MXCSR %#x): %llu cases, %llu different%s%s\n", mode,
                            static_cast<unsigned long long>(n), static_cast<unsigned long long>(bad),
                            bad ? "; first: " : "", first.c_str());
                ++checks;
                failures += bad ? 1 : 0;
            }
        }
    }
    // 0x43d480 (instanced shadow draw): recording clone vs recompiled, calls compared exactly
    {
        auto* r = reinterpret_cast<Fn6>(GetProcAddress(m, "recomp_43d480"));
        auto* redirects = reinterpret_cast<RedirectsFn>(GetProcAddress(m, "recomp_set_redirects"));
        std::vector<CodeRedirect> to_thunks;
        for (u32 i = 0; i < kDrawCalleeCount; ++i) {
            to_thunks.push_back({image_base + kDrawCallees[i].rva, ST::ThunkAddress(i)});
        }
        std::string error;
        const CloneResult c = CloneGuestFunction(image_base + 0x43d480, reinterpret_cast<const u8*>(image_base + 0x43d480),
                                                 0x106, error, to_thunks);
        if (r == nullptr || redirects == nullptr || c.code == nullptr) {
            std::printf("  recomp_43d480: %s%s\n", r == nullptr || redirects == nullptr ? "not in recomp.dll" : "clone: ",
                        error.c_str());
            ++checks;
            ++failures;
        } else {
            std::string first;
            u64 calls = 0;
            const u64 orphans = ST::OrphanCalls();
            const u64 bad = DrawDifferential(reinterpret_cast<Fn6>(c.code), r, redirects, image_base, cases, rng, first,
                                             calls);
            const u64 orphaned = ST::OrphanCalls() - orphans;
            std::printf("  recomp_43d480 vs eboot 0x43d480 (recording clone): %llu cases, %llu recorded calls, %llu "
                        "different, %llu orphan calls%s%s\n",
                        static_cast<unsigned long long>(cases), static_cast<unsigned long long>(calls),
                        static_cast<unsigned long long>(bad), static_cast<unsigned long long>(orphaned),
                        bad ? "; first: " : "", first.c_str());
            ++checks;
            failures += bad != 0 || orphaned != 0 || calls != cases * kDrawCalleeCount ? 1 : 0;
        }
    }

    // 0x450460 (the parameter writer with the vpinsrd lanes SLEIGH mis-lifted, kind 4)
    {
        const SlotOverride sincos_slot(image_base, kSinCosSlot, reinterpret_cast<u64>(&SinCosStandIn));
        const SlotOverride pow_slot(image_base, kPowSlot, reinterpret_cast<u64>(&PowStandIn));
        auto* r = reinterpret_cast<Fn7>(GetProcAddress(m, "recomp_450460"));
        auto* o = reinterpret_cast<Fn7>(Clone(image_base, 0x450460, 0xb17));
        if (r == nullptr || o == nullptr) {
            std::printf("  recomp_450460: %s\n", r == nullptr ? "not in recomp.dll" : "clone failed");
            ++checks;
            ++failures;
        } else {
            for (const u32 mode : {host_csr, 0x9fc0u, 0x3f80u, 0x5f80u, 0x7f80u}) {
                _mm_setcsr(mode);
                std::string first;
                const u64 n = mode == host_csr ? cases : cases / 4;
                const u64 bad = WriterDifferential(o, r, image_base, n, rng, first);
                _mm_setcsr(host_csr);
                std::printf("  recomp_450460 vs eboot 0x450460 (MXCSR %#x): %llu cases, %llu different%s%s\n", mode,
                            static_cast<unsigned long long>(n), static_cast<unsigned long long>(bad),
                            bad ? "; first: " : "", first.c_str());
                ++checks;
                failures += bad ? 1 : 0;
            }
        }
    }
    _mm_setcsr(host_csr);
    return failures;
}

} // namespace SecondSon::NativeEngine::Tests
