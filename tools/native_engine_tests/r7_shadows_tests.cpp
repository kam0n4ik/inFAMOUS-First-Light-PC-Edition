// SecondSonPC — the tests of R7-S (native shadow engine), owned by that branch alone (docs/NATIVE_ENGINE_R7_CORE.md
// §3). main.cpp calls R7ShadowsTests with the eboot mapped (before the CUE setters' jumps go into the image); it returns
// the number of failures and adds its checks to the total through `checks`.
//
// The hand-written reconstruction of 0x43d590 / 0x43d480 (shadow_casters.h) against the eboot's own code, by the
// R6-S differentials (r6_shadows_tests.cpp): the original as a recording clone, the reconstruction with its callee
// table aimed at the same recorder thunks, both on the same generated worlds / draws; calls, argument registers, the
// bytes the setters get, the world, the allocations and the generator's state compared exactly. No recompiled module
// is needed. Then: the install's self-checks and the registry's modes with the hand-written implementation, the
// Native Render Layer command of 0x43d480 against the original's calls, the draw-state dedup's packet scan.
// docs/NATIVE_ENGINE_R7_SHADOWS.md §3.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <windows.h>
#include <xmmintrin.h>

#include "base/abi.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/r7/shadows/shadow_draw_command.h"
#include "native_engine/reconstructed/shadow_casters.h"
#include "native_engine/reconstructed/shadow_loop.h"

namespace SecondSon::NativeEngine::Tests {

using RedirectsFn = int (*)(const u64*, const u64*, u32);
u64 R6ShadowLoopDifferential(void* original, void* impl, RedirectsFn redirects, u64 image_base, u64 cases,
                             std::mt19937_64& rng, std::string& first, u64& calls, u64& instanced_cases, bool perturb);
u64 R6ShadowDrawDifferential(void* original, void* impl, RedirectsFn redirects, u64 image_base, u64 cases,
                             std::mt19937_64& rng, std::string& first, u64& calls);
void* R6ShadowRecordingClone(u64 image_base, bool loop);
u32 R6ShadowLoopCoverage(const u64*& calls, const u64*& rvas);
int R6ShadowRegistryModes(u64 image_base, std::mt19937_64& rng, int& checks, const char* label);

namespace {

namespace SC = Reconstructed::ShadowCasters;
namespace SL = Reconstructed::ShadowLoop;

constexpr u64 kGuardSlot = 0xd9de00;

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

// --- 0x450460's two branches R6-S left out (S2) --------------------------------------------------------------------
// kind 0x10: 0x4502e0(n, use_owner, owner byte, &4 floats) - the cell of a sprite sheet: 0x28fa30 copies the current
// 32-byte sheet record of the global object 0x1d1faa0 under its pthread mutex (initialised on first use), then integer
// division / modulo by its columns x rows and, with a texture, a half-texel inset from the T#'s width / height. Here the
// eboot's 0x4502e0 / 0x28fa30 / 0x290380 run for real on both sides; the four pthread imports are stand-ins in their
// GOT slots that count (lock / unlock balanced, the same calls on both sides) and the global object is generated.
// kind 0xa with an object at buffers + 0x88: a virtual getter (+0x28 of its vtable: here a host function returning the
// case's type; 5..0x4a take the branch), 0x4740c0 (the skeleton's bone lookup: with +0x190 == 0 it returns -1 - the
// hash path 0x785b0 -> 0x6b860 only in the game), 0x476ed0 (the bone's parent from a short table) and the inverse of
// the parent's 3x3 times a translation (vrcpps + one Newton step, horizontal adds) from the object's matrices.
constexpr u64 kSheet = 0x1d1faa0;
constexpr u32 kSheetBytes = 0x278; // mutex +0x20, cond +0x28, initialised +0x30, records +0x230 (2 x 0x20), index +0x270
constexpr u64 kMutexInitSlot = 0xd9e1d0, kCondInitSlot = 0xd9e1c0, kMutexLockSlot = 0xd9e1d8,
              kMutexUnlockSlot = 0xd9e1e0;

thread_local s64 t_locked = 0;
thread_local u64 t_lock_calls = 0, t_inits = 0, t_getter_calls = 0;
thread_local u32 t_type = 0;
void Put64(std::vector<u8>& v, u32 at, u64 x) {
    std::memcpy(v.data() + at, &x, 8);
}
SSPC_SYSV_ABI int InitStandIn(u64, u64, u64) {
    ++t_inits;
    return 0;
}
SSPC_SYSV_ABI int LockStandIn(u64) {
    ++t_locked;
    ++t_lock_calls;
    return 0;
}
SSPC_SYSV_ABI int UnlockStandIn(u64) {
    --t_locked;
    return 0;
}
SSPC_SYSV_ABI u32 TypeGetter(u64) {
    ++t_getter_calls;
    return t_type;
}

float RandomFloat(std::mt19937_64& rng) {
    switch (rng() % 16) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return std::ldexp(static_cast<float>(rng() % 1000), -140);
    case 3: return (rng() & 1) ? INFINITY : -INFINITY;
    case 4: return std::nanf("");
    case 5: return std::ldexp(static_cast<float>(rng() % 1000) / 1000.0f, -20);
    default: {
        const float v = static_cast<float>(static_cast<s64>(rng() % 2000001) - 1000000) / 1000.0f;
        return (rng() % 4 == 0) ? v * 1000.0f : v;
    }
    }
}

struct GapCase {
    std::vector<u8> records = std::vector<u8>(0x50), constants = std::vector<u8>(0x500), atlas = std::vector<u8>(0x80),
                    view = std::vector<u8>(0x200), table = std::vector<u8>(0x400), light = std::vector<u8>(0x100),
                    owner = std::vector<u8>(0x40), owner_target = std::vector<u8>(0x80),
                    object = std::vector<u8>(0x400), skeleton = std::vector<u8>(0x200), chain_a = std::vector<u8>(0x20),
                    chain_b = std::vector<u8>(0x60), bones = std::vector<u8>(0x20), matrices = std::vector<u8>(0x200),
                    vtable = std::vector<u8>(0x40), texture = std::vector<u8>(0x20), sheet = std::vector<u8>(kSheetBytes);
    u32 type = 0;
    bool sheet_kind = false, object_kind = false; // the case reaches 0x4502e0 / the object branch

    template <typename T>
    static void Put(std::vector<u8>& v, u32 at, T x) {
        std::memcpy(v.data() + at, &x, sizeof(T));
    }

    bool Same(const GapCase& o) const {
        return records == o.records && atlas == o.atlas && view == o.view && table == o.table && light == o.light &&
               owner == o.owner && owner_target == o.owner_target && object == o.object && skeleton == o.skeleton &&
               chain_a == o.chain_a && chain_b == o.chain_b && bones == o.bones && matrices == o.matrices &&
               vtable == o.vtable && texture == o.texture;
    }

    void Generate(std::mt19937_64& rng) {
        for (auto* v : {&records, &constants, &view, &table, &light, &owner, &owner_target, &object, &skeleton,
                        &matrices, &texture, &sheet}) {
            for (u32 i = 0; i + 4 <= v->size(); i += 4) {
                Put(*v, i, RandomFloat(rng));
            }
        }
        std::fill(atlas.begin(), atlas.end(), 0);
        const s32 a5c = static_cast<s32>(rng() % 4), a60 = static_cast<s32>(rng() % 4);
        Put(atlas, 0x5c, a5c);
        Put(atlas, 0x60, a60);
        const u32 region = static_cast<u32>(a5c) * 32 + static_cast<u32>(a60) * 16 + 0x20;
        const s32 count = static_cast<s32>(rng() % 6) - 1;
        Put(records, 0, count);
        const u64 with_object = rng() % 4;
        static constexpr s32 kKinds[] = {0x10, 0x10, 0xa, 0xa, 0xb, -1};
        sheet_kind = object_kind = false;
        for (u32 i = 0; i < 4; ++i) {
            const s32 kind = kKinds[rng() % std::size(kKinds)];
            const s32 index = static_cast<s32>(8 * i);
            Put(records, 8 + 4 * i, kind);
            Put(records, 0x18 + 4 * i, index);
            const u32 at = region + static_cast<u32>(index) * 4;
            if (kind == 0x10) {
                // the cell number (truncated) and the sign that picks the owner's byte instead
                if (rng() % 2) {
                    Put(table, at, static_cast<float>(static_cast<s64>(rng() % 200) - 50));
                }
                sheet_kind = sheet_kind || static_cast<s32>(i) < count;
            }
            if (kind == 0xa && with_object != 0) {
                object_kind = object_kind || static_cast<s32>(i) < count;
            }
        }
        for (u32 at = 0x28; at < 0x44; at += 2) {
            Put(records, at, static_cast<u16>(rng() % 0x100));
        }
        Put(light, 0x88, with_object != 0 ? reinterpret_cast<u64>(object.data()) : u64{0});
        // the object: vtable (+0x28 the getter), skeleton (+0x28; +0x190 == 0), bone chain (+0x3f0), matrices (+0x288)
        static constexpr u32 kTypes[] = {0, 4, 5, 6, 0x20, 0x49, 0x4a, 0x4b, 0x4c, 0xffffffffu};
        type = (rng() % 4 == 0) ? static_cast<u32>(rng() % 0x100) : kTypes[rng() % std::size(kTypes)];
        std::fill(vtable.begin(), vtable.end(), 0);
        Put(object, 0, reinterpret_cast<u64>(vtable.data()));
        Put(object, 0x28, reinterpret_cast<u64>(skeleton.data()));
        Put(skeleton, 0x190, u64{0});
        Put(object, 0x3f0, reinterpret_cast<u64>(chain_a.data()));
        Put(chain_a, 0x10, reinterpret_cast<u64>(chain_b.data()));
        Put(chain_b, 0x48, reinterpret_cast<u64>(bones.data() + 8)); // bone -1: the short at +6
        std::fill(bones.begin(), bones.end(), 0);
        Put(bones, 6, static_cast<s16>(rng() % 4));
        Put(object, 0x288, reinterpret_cast<u64>(matrices.data() + 0x40)); // bone -1's row +0x70: matrices + 0x30
        Put(owner, 0, reinterpret_cast<u64>(owner_target.data()));
        // the global sheet: initialised (or not: the stand-ins), the current record, columns x rows, a T# or none
        Put(sheet, 0x30, static_cast<u8>(rng() % 4 != 0));
        const s32 current = static_cast<s32>(rng() % 2);
        Put(sheet, 0x270, current);
        for (u32 k = 0; k < 2; ++k) {
            const u32 rec = 0x230 + 0x20 * k;
            Put(sheet, rec + 8, static_cast<s32>(1 + rng() % 8));
            Put(sheet, rec + 0xc, static_cast<s32>(1 + rng() % 8));
            Put(sheet, rec + 0x18, rng() % 3 != 0 ? reinterpret_cast<u64>(texture.data()) : u64{0});
        }
        Put(texture, 8, static_cast<u32>(rng()));
        constants[0x404] = static_cast<u8>(rng() % 2);
    }
};

using Fn7 = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64, u64);

/// recomp_450460 vs the eboot's 0x450460 (a clone) on kinds 0x10 / 0xa-with-object cases.
u64 GapDifferential(Fn7 original, Fn7 recompiled, u64 image_base, u64 cases, std::mt19937_64& rng, std::string& first,
                    u64& sheet_cases, u64& object_cases, u64& object_branch) {
    auto* sheet = reinterpret_cast<u8*>(image_base + kSheet);
    std::vector<u8> saved(sheet, sheet + kSheetBytes);
    GapCase c;
    u64 bad = 0;
    for (u64 n = 0; n < cases; ++n) {
        c.Generate(rng);
        const GapCase in = c;
        sheet_cases += c.sheet_kind ? 1 : 0;
        object_cases += c.object_kind ? 1 : 0;
        object_branch += c.object_kind && c.type - 5u < 0x46u ? 1 : 0;
        struct Side {
            std::vector<u8> constants, sheet;
            s64 locked;
            u64 locks, inits, getter;
            bool kept;
        };
        const auto run = [&](Fn7 fn) {
            c.constants = in.constants;
            std::memcpy(sheet, in.sheet.data(), kSheetBytes);
            t_locked = 0;
            t_lock_calls = t_inits = t_getter_calls = 0;
            t_type = in.type;
            Put64(c.vtable, 0x28, reinterpret_cast<u64>(&TypeGetter));
            fn(reinterpret_cast<u64>(c.records.data()), reinterpret_cast<u64>(c.constants.data()),
               reinterpret_cast<u64>(c.atlas.data()), reinterpret_cast<u64>(c.view.data()),
               reinterpret_cast<u64>(c.table.data()), reinterpret_cast<u64>(c.light.data()),
               reinterpret_cast<u64>(c.owner.data()));
            Side s{c.constants, std::vector<u8>(sheet, sheet + kSheetBytes), t_locked, t_lock_calls, t_inits,
                   t_getter_calls, false};
            Put64(c.vtable, 0x28, 0);
            s.kept = c.Same(in);
            return s;
        };
        const Side a = run(original);
        const Side b = run(recompiled);
        const bool ok = a.constants == b.constants && a.sheet == b.sheet && a.kept && b.kept && a.locked == 0 &&
                        b.locked == 0 && a.locks == b.locks && a.inits == b.inits && a.getter == b.getter;
        if (!ok && bad++ == 0) {
            u32 at = 0;
            while (at < a.constants.size() && a.constants[at] == b.constants[at]) {
                ++at;
            }
            s32 kinds[4];
            std::memcpy(kinds, c.records.data() + 8, sizeof(kinds));
            first = fmt::format("case {}: constants +{:#x}, kinds {} {} {} {}, type {:#x}, sheet {}, locks {}/{} "
                                "(balance {}/{}), getter {}/{}, inputs kept {}/{}",
                                n, at, kinds[0], kinds[1], kinds[2], kinds[3], c.type, a.sheet == b.sheet ? "same" : "differs",
                                a.locks, b.locks, a.locked, b.locked, a.getter, b.getter, a.kept, b.kept);
        }
    }
    std::memcpy(sheet, saved.data(), kSheetBytes);
    return bad;
}

/// The dedup scan on hand-made PM4.
int ScanTests(int& checks) {
    int failures = 0;
    const auto check = [&](bool ok, const char* what) {
        ++checks;
        failures += ok ? 0 : 1;
        std::printf("  dedup scan: %s: %s\n", what, ok ? "ok" : "FAILED");
    };
    const auto t3 = [](u32 op, u32 count) { return 0xC0000000u | (count << 16) | (op << 8); };
    std::vector<u32> w;
    const auto scan = [&] {
        return SC::UntouchedState(reinterpret_cast<u64>(w.data()), reinterpret_cast<u64>(w.data() + w.size()));
    };
    w = {};
    check(scan() == 3, "nothing in between: everything kept");
    w = {t3(0x76, 2), 0x10, 1, 2, t3(0x10, 2), 0x52505353, 2, 0}; // SET_SH_REG (2 values), NOP
    check(scan() == 3, "user data and a NOP: kept");
    w = {t3(0x2a, 0), 0};
    check(scan() == 2, "INDEX_TYPE: primitive / index state written");
    w = {t3(0x2f, 0), 5};
    check(scan() == 1, "NUM_INSTANCES: instances written");
    w = {t3(0x79, 1), 0x242, 4};
    check(scan() == 2, "SET_UCONFIG_REG VGT_PRIMITIVE_TYPE");
    w = {t3(0x79, 3), 0x240, 0, 0, 0};
    check(scan() == 2, "SET_UCONFIG_REG range over 0x242");
    w = {t3(0x79, 1), 0x24d, 4};
    check(scan() == 1, "SET_UCONFIG_REG VGT_NUM_INSTANCES");
    w = {t3(0x79, 1), 0x244, 4};
    check(scan() == 3, "SET_UCONFIG_REG elsewhere: kept");
    w = {t3(0x12, 0), 0}; // CLEAR_STATE: not understood
    check(scan() == 0, "an unknown packet: nothing kept");
    w = {0x80000000u};
    check(scan() == 0, "a type 2 packet: nothing kept");
    w = {t3(0x76, 5), 0};
    check(scan() == 0, "a packet past the end: nothing kept");
    return failures;
}

} // namespace

int R7ShadowsTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                   int& checks) {
    (void)recomp_module, (void)recomp_dll;
    std::printf("R7-S hand-written shadow loop 0x43d590 / draw 0x43d480 (vs the eboot's code):\n");
    if (image_size < 0x7f92020 + 0x810) {
        std::printf("  mapped image too small: skipped\n");
        return 0;
    }
    if (const char* c = std::getenv("SSPC_R7S_CASES")) {
        cases = std::strtoull(c, nullptr, 10);
    }
    int failures = 0;
    const auto result = [&](bool ok, const std::string& what) {
        ++checks;
        failures += ok ? 0 : 1;
        std::printf("  %s: %s\n", what.c_str(), ok ? "ok" : "FAILED");
    };
    failures += ScanTests(checks);
    std::string error;
    if (!SC::Prepare(image_base, error)) {
        result(false, "prepare " + error);
        return failures;
    }
    static u64 guard = 0x5ec0'7a1d'0000'0007ull;
    const SlotOverride guard_slot(image_base, kGuardSlot, reinterpret_cast<u64>(&guard));
    std::mt19937_64 rng{0x7543d590};

    // 0x43d480: recording clone vs the hand-written draw, 9 calls exactly
    void* const draw_clone = R6ShadowRecordingClone(image_base, false);
    void* const loop_clone = R6ShadowRecordingClone(image_base, true);
    if (draw_clone == nullptr || loop_clone == nullptr) {
        result(false, "recording clones");
        return failures;
    }
    {
        std::string first;
        u64 calls = 0;
        const u64 bad = R6ShadowDrawDifferential(draw_clone, reinterpret_cast<void*>(&SC::Draw), &SC::Redirects,
                                                 image_base, cases, rng, first, calls);
        result(bad == 0 && calls == cases * 9,
               fmt::format("hand 0x43d480 vs eboot (recording clone): {} cases, {} recorded calls, {} different{}{}",
                           cases, calls, bad, bad ? "; first: " : "", first));
    }
    // 0x43d590: generated worlds under the host MXCSR and two guest modes
    const u32 host = _mm_getcsr();
    for (const u32 mode : {host, 0x9fc0u, 0x7f80u}) {
        _mm_setcsr(mode);
        std::string first;
        u64 calls = 0, instanced = 0;
        const u64 n = mode == host ? std::max<u64>(cases / 10, 1000) : std::max<u64>(cases / 40, 1000);
        const u64 bad = R6ShadowLoopDifferential(loop_clone, reinterpret_cast<void*>(&SC::Loop), &SC::Redirects,
                                                 image_base, n, rng, first, calls, instanced, false);
        _mm_setcsr(host);
        result(bad == 0 && calls >= n,
               fmt::format("hand 0x43d590 vs eboot (recording clone, MXCSR {:#x}): {} worlds ({} with instancing), {} "
                           "recorded calls, {} different{}{}",
                           mode, n, instanced, calls, bad, bad ? "; first: " : "", first));
    }
    {
        const u64* calls = nullptr;
        const u64* rvas = nullptr;
        const u32 n = R6ShadowLoopCoverage(calls, rvas);
        std::string line;
        bool all = true;
        for (u32 k = 0; k < n; ++k) {
            line += fmt::format("{}{:#x} {}", k ? ", " : "", rvas[k], calls[k]);
            all = all && calls[k] != 0;
        }
        result(all, "every recorded callee reached (cumulative with R6-S): " + line);
    }
    {
        std::string first;
        u64 calls = 0, instanced = 0;
        const u64 differ = R6ShadowLoopDifferential(loop_clone, reinterpret_cast<void*>(&SC::Loop), &SC::Redirects,
                                                    image_base, 1000, rng, first, calls, instanced, true);
        result(differ > 100, fmt::format("sensitivity: 1000 worlds, the pass's slot changed for the hand-written run: "
                                         "{} different",
                                         differ));
    }

    // S2: recomp_450460 on the two branches R6-S left out
    if (recomp_module != nullptr) {
        auto* recompiled =
            reinterpret_cast<Fn7>(GetProcAddress(static_cast<HMODULE>(recomp_module), "recomp_450460"));
        std::string clone_error;
        const CloneResult clone = CloneGuestFunction(image_base + 0x450460,
                                                     reinterpret_cast<const u8*>(image_base + 0x450460), 0xb17,
                                                     clone_error);
        if (recompiled == nullptr || clone.code == nullptr) {
            result(false, "0x450460 gaps: " + (recompiled == nullptr ? std::string("no recomp_450460") : clone_error));
        } else {
            const SlotOverride init(image_base, kMutexInitSlot, reinterpret_cast<u64>(&InitStandIn));
            const SlotOverride cond(image_base, kCondInitSlot, reinterpret_cast<u64>(&InitStandIn));
            const SlotOverride lock(image_base, kMutexLockSlot, reinterpret_cast<u64>(&LockStandIn));
            const SlotOverride unlock(image_base, kMutexUnlockSlot, reinterpret_cast<u64>(&UnlockStandIn));
            for (const u32 mode : {host, 0x9fc0u, 0x7f80u, 0x3f80u, 0x5f80u}) {
                _mm_setcsr(mode);
                std::string first;
                u64 sheet = 0, object = 0, branch = 0;
                const u64 n = mode == host ? cases * 5 : cases;
                const u64 bad = GapDifferential(reinterpret_cast<Fn7>(clone.code), recompiled, image_base, n, rng, first,
                                                sheet, object, branch);
                _mm_setcsr(host);
                result(bad == 0 && sheet > n / 4 && branch > n / 20,
                       fmt::format("recomp_450460 vs eboot, kinds 0x10 / 0xa with an object (MXCSR {:#x}): {} cases "
                                   "({} reach 0x4502e0, {} the object, {} its branch), {} different{}{}",
                                   mode, n, sheet, object, branch, bad, bad ? "; first: " : "", first));
            }
        }
    }

    // the registry's path: the specs with the hand-written implementation (install self-checks, the NativeDrawCommand
    // against the original's calls, verify / guarded / a fault through the dispatchers)
    if (!SL::Prepare(image_base, error) || !SL::UseHandImplementation(image_base, error)) {
        result(false, "registry prepare " + error);
        return failures;
    }
    std::string d = SL::SelfCheckLoop(300, 0x5248414430353930ull);
    result(d.empty(), "install self-check shadow_casters_loop (300 worlds, hand-written) " + d);
    d = SL::SelfCheckDraw(2000, 0x5248414430343830ull);
    result(d.empty(), "install self-check shadow_instanced_draw (2000 draws, hand-written) " + d);
    {
        std::vector<u8> pass(0x200), context(0x200), mesh(0x80), records(16 * 0x48);
        u64 bad = 0, skipped = 0;
        std::string first;
        const u64 n = std::max<u64>(cases / 10, 1000);
        for (u64 k = 0; k < n; ++k) {
            for (auto* v : {&pass, &context, &mesh, &records}) {
                for (u8& x : *v) {
                    x = static_cast<u8>(rng());
                }
            }
            const u64 recs = (rng() & 1) ? reinterpret_cast<u64>(records.data()) : rng();
            const u64 count = (rng() & 1) ? 1 + rng() % 64 : rng();
            Engine::Render::R7::NativeRenderContext ctx;
            Engine::Render::R7::NativeDrawCommand cmd;
            R7Shadows::ShadowDrawInputs inputs;
            R7Shadows::BuildInstancedDraw(image_base, reinterpret_cast<u64>(pass.data()),
                                          reinterpret_cast<u64>(context.data()), recs, count,
                                          reinterpret_cast<u64>(mesh.data()), ctx, cmd, inputs);
            bool skip = false;
            std::string e = SL::CheckDraw(reinterpret_cast<u64>(pass.data()), reinterpret_cast<u64>(context.data()), recs,
                                          count, reinterpret_cast<u64>(mesh.data()), skip);
            if (skip) {
                ++skipped;
                continue;
            }
            if (e.empty()) {
                e = R7Shadows::CheckAgainstTrace(ctx, cmd, inputs, SL::LastTrace(0));
            }
            if (!e.empty() && bad++ == 0) {
                first = fmt::format("case {}: {}", k, e);
            }
        }
        result(bad == 0 && skipped == 0,
               fmt::format("NativeDrawCommand at 0x43d480's entry vs the original's calls: {} draws, {} different, {} "
                           "skipped{}{}",
                           n, bad, skipped, bad ? "; first: " : "", first));
        // the check is not vacuous: a command with one instance more is caught
        const u64 recs = reinterpret_cast<u64>(records.data());
        Engine::Render::R7::NativeRenderContext ctx;
        Engine::Render::R7::NativeDrawCommand cmd;
        R7Shadows::ShadowDrawInputs inputs;
        R7Shadows::BuildInstancedDraw(image_base, reinterpret_cast<u64>(pass.data()),
                                      reinterpret_cast<u64>(context.data()), recs, 7, reinterpret_cast<u64>(mesh.data()),
                                      ctx, cmd, inputs);
        bool skip = false;
        SL::CheckDraw(reinterpret_cast<u64>(pass.data()), reinterpret_cast<u64>(context.data()), recs, 7,
                      reinterpret_cast<u64>(mesh.data()), skip);
        cmd.state.num_instances = 8;
        const std::string caught = R7Shadows::CheckAgainstTrace(ctx, cmd, inputs, SL::LastTrace(0));
        cmd.state.num_instances = 7;
        inputs.records += 0x10;
        const std::string caught2 = R7Shadows::CheckAgainstTrace(ctx, cmd, inputs, SL::LastTrace(0));
        result(!caught.empty() && !caught2.empty(),
               "a wrong command is caught (" + caught + " / " + caught2 + ")");
    }
    failures += R6ShadowRegistryModes(image_base, rng, checks, "hand-written ");
    return failures;
}

} // namespace SecondSon::NativeEngine::Tests
