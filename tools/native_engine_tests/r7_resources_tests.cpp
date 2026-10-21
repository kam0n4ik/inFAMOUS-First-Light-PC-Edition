// SecondSonPC — the tests of R7-R (native resource / material / CUE engine), owned by that branch alone
// (docs/NATIVE_ENGINE_R7_CORE.md §3). main.cpp calls R7ResourcesTests with the eboot mapped (before the CUE setters'
// jumps go into the image); it returns the number of failures and adds its checks to the total through `checks`.
//
// 1. The read-write resource setters 0x16c40 / 0x16de0 (cue_set_buffers.h kRwBuffers / kRwTextures): the install
//    self-check, the hand-written reconstruction against the eboot's recording clone on generated cases, and every
//    mode through the jump on the mapped image.
// 2. The Native Resource State (r7/resources/resource_state.h) on the eboot's own code: random sequences of setter
//    calls (the eboot's setters, recording clones; BeforeSetter first, as the dispatcher does), commits (recomp_14ad0
//    through Resources::Commit; its Gnm callees replaced in the mapped image by recorders that keep the user data a GPU
//    would hold), shader changes, single-value changes and resets (0x13d20 through its probe). After every commit each
//    present binding is checked against the bytes: the snapshot equals the shadow, equal versions mean equal snapshot
//    bytes / user data / layout, the shape's pointers and the direct values equal the recorded user data, the changed
//    dwords cover every dword that differs from the previous snapshot. Verify mode runs all along (0 violations), then
//    an unobserved shadow write must be caught by it.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "base/abi.h"
#include "native_engine/native_function_registry.h"
#include "native_engine/r7/resources/resource_state.h"
#include "native_engine/reconstructed/cue_commit.h"
#include "native_engine/reconstructed/cue_set_buffers.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

namespace CSB = Reconstructed::CueSetBuffers;
namespace Res = R7::Resources;

int g_fail = 0;
int* g_checks = nullptr;

#define R7R_CHECK(cond)                                                                                                \
    do {                                                                                                               \
        ++*g_checks;                                                                                                   \
        if (!(cond)) {                                                                                                 \
            ++g_fail;                                                                                                  \
            std::printf("  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                          \
        }                                                                                                              \
    } while (0)

/// A 14-byte absolute jump at image + rva to `fn`, the bytes put back on destruction.
struct Patch {
    u8* at;
    u8 saved[14];
    Patch(u64 base, u64 rva, const void* fn) : at(reinterpret_cast<u8*>(base + rva)) {
        DWORD old = 0;
        VirtualProtect(at, 14, PAGE_EXECUTE_READWRITE, &old);
        std::memcpy(saved, at, 14);
        const u8 jmp[6] = {0xFF, 0x25, 0, 0, 0, 0};
        std::memcpy(at, jmp, 6);
        const u64 target = reinterpret_cast<u64>(fn);
        std::memcpy(at + 6, &target, 8);
        FlushInstructionCache(GetCurrentProcess(), at, 14);
    }
    ~Patch() {
        std::memcpy(at, saved, 14);
        FlushInstructionCache(GetCurrentProcess(), at, 14);
    }
};

// --- the GPU's user data as the recorded calls leave them --------------------------------------------------------------

u32 g_ud[8][16] = {};
u32 g_ud_mask[8] = {};
u8* g_arena = nullptr;
u64 g_arena_used = 0, g_arena_size = 0;
u64 g_stack_fail = 0;

SSPC_SYSV_ABI u64 RecSetPointer(u64, u64 stage, u64 sgpr, u64 pointer, u64, u64) {
    const u32 st = static_cast<u32>(stage), sg = static_cast<u32>(sgpr);
    if (st < 8 && sg + 1 < 16) {
        g_ud[st][sg] = static_cast<u32>(pointer);
        g_ud[st][sg + 1] = static_cast<u32>(pointer >> 32);
        g_ud_mask[st] |= 3u << sg;
    }
    return 0;
}

SSPC_SYSV_ABI u64 RecSetRegion(u64, u64 stage, u64 sgpr, u64 values, u64 count, u64) {
    const u32 st = static_cast<u32>(stage), sg = static_cast<u32>(sgpr);
    for (u32 j = 0; st < 8 && j < static_cast<u32>(count) && sg + j < 16; ++j) {
        std::memcpy(&g_ud[st][sg + j], reinterpret_cast<const void*>(values + 4ull * j), 4);
        g_ud_mask[st] |= 1u << (sg + j);
    }
    return 0;
}

SSPC_SYSV_ABI u64 RecAlloc(u64, u64 dwords, u64, u64, u64, u64) {
    const u64 bytes = u64{static_cast<u32>(dwords)} * 4;
    if (g_arena_used + bytes > g_arena_size) {
        return 0;
    }
    const u64 p = reinterpret_cast<u64>(g_arena + g_arena_used);
    g_arena_used += bytes;
    return p;
}

SSPC_SYSV_ABI u64 RecMemcpy(u64 dst, u64 src, u64 n, u64, u64, u64) {
    std::memcpy(reinterpret_cast<void*>(dst), reinterpret_cast<const void*>(src), n);
    return dst;
}

SSPC_SYSV_ABI u64 RecStackFail(u64, u64, u64, u64, u64, u64) {
    ++g_stack_fail;
    return 0;
}

SSPC_SYSV_ABI u64 RecNothing(u64, u64, u64, u64, u64, u64) {
    return 0;
}

// --- 1. the read-write setters --------------------------------------------------------------------------------------

void TestRwSetters(u64 image_base, u64 cases) {
    std::printf("R7-R read-write resource setters 0x16c40 / 0x16de0:\n");
    std::string error;
    std::mt19937_64 rng{0x16c40};
    for (const CSB::Kind kind : {CSB::kRwBuffers, CSB::kRwTextures}) {
        const FunctionSpec& spec = CSB::Spec(kind);
        error.clear();
        const bool prepared = CSB::Prepare(kind, image_base, error);
        R7R_CHECK(prepared);
        if (!prepared) {
            std::printf("  %s prepare: %s\n", spec.key, error.c_str());
            continue;
        }
        error.clear();
        const bool self = spec.self_check(nullptr, error);
        std::printf("  %s: install self-check %s%s%s\n", spec.key, self ? "passed" : "FAILED", self ? "" : ": ",
                    error.c_str());
        R7R_CHECK(self);
        // generated cases on whole buffers: the eboot's recording clone against the hand-written reconstruction
        std::vector<u8> a(0x1b000), b;
        std::vector<u8> table(0x200), src(64 * 32);
        u64 differ = 0;
        for (u64 n = 0; n < cases; ++n) {
            for (size_t i = 0; i < a.size(); i += 8) {
                const u64 v = rng();
                std::memcpy(a.data() + i, &v, 8);
            }
            const u32 stage = static_cast<u32>(rng() % 8), count = static_cast<u32>(rng() % 17);
            const u32 slot = static_cast<u32>(rng() % (64 - count + 1));
            for (size_t i = 0; i < table.size(); i += 2) {
                const u32 pick = static_cast<u32>(rng() % 4);
                const u16 w = pick == 0   ? u16{0xffff}
                              : pick == 1 ? static_cast<u16>(0x8000 | (rng() & 0x7fff))
                                          : static_cast<u16>(rng() % 0x5f8);
                std::memcpy(table.data() + i, &w, 2);
            }
            for (u8& x : src) {
                x = static_cast<u8>(rng());
            }
            const u64 t = reinterpret_cast<u64>(table.data());
            std::memcpy(a.data() + 0xc180 + 8 * stage, &t, 8);
            b = a;
            CSB::Recording ra, rb;
            const u64 pa = reinterpret_cast<u64>(a.data()), pb = reinterpret_cast<u64>(b.data());
            const u64 xa = CSB::RunRecorded(kind, CSB::RecordingOriginal(kind), true, pa, stage, slot, count,
                                            reinterpret_cast<u64>(src.data()), ra);
            const u64 xb = CSB::RunRecorded(kind, CSB::HandWritten(kind), false, pb, stage, slot, count,
                                            reinterpret_cast<u64>(src.data()), rb);
            const bool same = xa - pa == xb - pb && a == b && ra.calls == rb.calls && !ra.overflow && !rb.overflow &&
                              std::memcmp(ra.call, rb.call, sizeof(CSB::Call) * ra.calls) == 0;
            differ += same ? 0 : 1;
        }
        std::printf("  %s hand-written vs the eboot's code: %llu cases, %llu different\n", spec.key,
                    static_cast<unsigned long long>(cases), static_cast<unsigned long long>(differ));
        R7R_CHECK(differ == 0);
        // through the jump on the mapped image, every mode, against the recording clone on a copy
        error.clear();
        const bool installed = InstallFunction(image_base, spec, Mode::Native, error);
        R7R_CHECK(installed);
        if (!installed) {
            std::printf("  install %s: %s\n", spec.key, error.c_str());
            continue;
        }
        const Patch region(image_base, CSB::kCalleeRva, reinterpret_cast<const void*>(&RecNothing));
        const auto entry = reinterpret_cast<CSB::Fn>(image_base + spec.function.rva);
        for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
            spec.slot->mode.store(static_cast<u8>(mode));
            spec.slot->sample_period = 3;
            u64 bad = 0;
            for (int n = 0; n < 2000; ++n) {
                for (size_t i = 0; i < a.size(); i += 8) {
                    const u64 v = rng();
                    std::memcpy(a.data() + i, &v, 8);
                }
                const u32 stage = static_cast<u32>(rng() % 8), count = static_cast<u32>(rng() % 9);
                const u32 slot = static_cast<u32>(rng() % (16 - count + 1));
                for (size_t i = 0; i < table.size(); i += 2) {
                    const u16 w = rng() % 4 == 0 ? u16{0xffff} : static_cast<u16>(rng() % 0x5f8); // no direct entry
                    std::memcpy(table.data() + i, &w, 2);
                }
                const u64 t = reinterpret_cast<u64>(table.data());
                std::memcpy(a.data() + 0xc180 + 8 * stage, &t, 8);
                b = a;
                entry(reinterpret_cast<u64>(a.data()), stage, slot, count, reinterpret_cast<u64>(src.data()));
                CSB::Recording r;
                CSB::RunRecorded(kind, CSB::RecordingOriginal(kind), true, reinterpret_cast<u64>(b.data()), stage,
                                 slot, count, reinterpret_cast<u64>(src.data()), r);
                bad += a == b ? 0 : 1;
            }
            std::printf("  %s %s: 2000 calls through the jump, different %llu\n", spec.key,
                        mode == Mode::Native    ? "native"
                        : mode == Mode::Verify  ? "verify"
                        : mode == Mode::Guarded ? "guarded"
                        : mode == Mode::Bench   ? "bench"
                                                : "original",
                        static_cast<unsigned long long>(bad));
            R7R_CHECK(bad == 0);
        }
        std::printf("  %s: verified %llu, mismatches %llu\n", spec.key,
                    static_cast<unsigned long long>(spec.slot->verified.load()),
                    static_cast<unsigned long long>(spec.slot->mismatches.load()));
        R7R_CHECK(spec.slot->verified.load() > 0 && spec.slot->mismatches.load() == 0);
        spec.slot->mode.store(static_cast<u8>(Mode::Native));
    }
}

// --- 2. the resource state ------------------------------------------------------------------------------------------

constexpr u64 kCueBytes = 0x20000, kLayouts = 0x20000, kShader = 0x21000, kWorldBytes = 0x22000;
constexpr u32 kTableOff[CSB::kAllKinds] = {0x10a, 0x22, 0xe2, 0x22, 0xc2, 0xa2, 0xa2};
constexpr u32 kSlots[CSB::kAllKinds] = {16, 64, 20, 64, 16, 16, 16};

/// Four layouts: VS / PS x {pointers and single values, direct descriptors}.
void MakeLayout(u8* t, std::mt19937_64& rng, bool vertex, bool direct) {
    std::memset(t, 0xff, 0x138);
    const u16 dwords = static_cast<u16>(64 + rng() % (0x600 - 64));
    std::memcpy(t, &dwords, 2);
    const auto off = [&](u32 field) {
        const u16 v = static_cast<u16>(rng() % dwords);
        std::memcpy(t + field, &v, 2);
    };
    t[7] = 0;
    if (!direct) {
        t[8] = 2, off(0x16);
        t[0xA] = 4, off(0x1C);
        t[0xC] = 6;
        t[0xD] = 8;
        t[0xE] = 9;
        if (vertex) {
            t[4] = 10;
            t[5] = 12, off(0x18);
            t[0xF] = 14;
        }
    }
    for (u32 k = 0; k < CSB::kAllKinds; ++k) {
        for (u32 s = 0; s < kSlots[k]; ++s) {
            const u32 pick = static_cast<u32>(rng() % 6);
            u16 w;
            if (pick == 0) {
                w = 0xffff;
            } else if (pick == 1 && direct) {
                w = static_cast<u16>(0x8000 | (CSB::kDescriptorBytes[k] == 32 ? 8 : 4));
            } else {
                w = static_cast<u16>(rng() % (dwords - 8));
            }
            std::memcpy(t + kTableOff[k] + 2 * s, &w, 2);
        }
    }
}

struct Expect {
    std::vector<u8> bytes;
    u32 direct_mask;
    u32 direct[16];
    u64 layout;
};

void TestResourceState(u64 image_base, void* module, u64 cases) {
    std::printf("R7-R Native Resource State (SSPC_R7_RES_STATE) on the eboot's setters and recomp_14ad0:\n");
    const auto commit = module ? reinterpret_cast<Reconstructed::CueCommit::Fn>(
                                     GetProcAddress(static_cast<HMODULE>(module), "recomp_14ad0"))
                               : nullptr;
    const auto redirect = module ? reinterpret_cast<void (*)(u64, u64)>(
                                       GetProcAddress(static_cast<HMODULE>(module), "recomp_set_redirect"))
                                 : nullptr;
    if (commit == nullptr || redirect == nullptr) {
        std::printf("  no recomp_14ad0 / recomp_set_redirect in the module: skipped\n");
        return;
    }
    std::string error;
    for (u32 k = 0; k < CSB::kAllKinds; ++k) { // (the read-write ones are prepared and jumped to by now)
        const auto kind = static_cast<CSB::Kind>(k);
        R7R_CHECK(CSB::RecordingOriginal(kind) != nullptr || CSB::Prepare(kind, image_base, error));
    }
    // the stack guard: eboot+0xd9de00 holds its address (unrelocated in the mapped image)
    static u64 guard = 0x5ec0de5ec0de5ec0ull;
    u64* got = reinterpret_cast<u64*>(image_base + 0xd9de00);
    const u64 got_saved = *got;
    *got = reinterpret_cast<u64>(&guard);
    std::vector<std::unique_ptr<Patch>> patches;
    const auto patch = [&](u64 rva, const void* fn) { patches.push_back(std::make_unique<Patch>(image_base, rva, fn)); };
    patch(0x123d0, reinterpret_cast<const void*>(&RecAlloc));
    patch(0x9843a0, reinterpret_cast<const void*>(&RecMemcpy));
    patch(0x984360, reinterpret_cast<const void*>(&RecStackFail));
    patch(0x626400, reinterpret_cast<const void*>(&RecSetPointer));
    patch(0x626520, reinterpret_cast<const void*>(&RecSetRegion));
    for (const u64 rva : {0x623630ull, 0x624cb0ull, 0x625100ull, 0x6251c0ull, 0x625220ull, 0x625300ull, 0x625370ull,
                          0x6253e0ull, 0x625510ull, 0x6256a0ull, 0x625a40ull, 0x625aa0ull, 0x638540ull}) {
        patch(rva, reinterpret_cast<const void*>(&RecNothing));
    }
    Res::SetMode(Res::StateMode::Verify);
    Res::AssumeTracking(true);
    error.clear();
    const bool prepared = Res::Prepare(image_base, redirect, error);
    std::printf("  prepare (reset probes 0x13d20 / 0x12390): %s%s\n", prepared ? "done" : "FAILED: ", error.c_str());
    R7R_CHECK(prepared);
    if (!prepared) {
        *got = got_saved;
        return;
    }
    std::vector<u8> arena(0x400000);
    g_arena = arena.data();
    g_arena_size = arena.size();
    std::vector<u8> world(kWorldBytes);
    u8* const cue_bytes = world.data();
    const u64 cue = reinterpret_cast<u64>(cue_bytes);
    u8* const layouts = world.data() + kLayouts;
    const auto put64 = [&](u64 off, u64 v) { std::memcpy(cue_bytes + off, &v, 8); };
    const auto get64 = [&](u64 off) {
        u64 v;
        std::memcpy(&v, cue_bytes + off, 8);
        return v;
    };
    const auto reset_fn = reinterpret_cast<SSPC_SYSV_ABI void (*)(u64)>(image_base + 0x13d20); // through its probe
    std::mt19937_64 rng{0x7e5};
    u64 commits = 0, present = 0, version_seen = 0, kept = 0, setters = 0, resets = 0, delta_checked = 0;
    u64 bad_bytes = 0, bad_version = 0, bad_user = 0, bad_present = 0, bad_delta = 0, bad_version0 = 0;
    std::string first;
    const auto fail = [&](u64& counter, const std::string& what) {
        if (counter++ == 0 && first.empty()) {
            first = what;
        }
    };
    const u32 kOps = 64;
    std::vector<u8> src(16 * 32);
    for (u64 n = 0; n < cases; ++n) {
        // a fresh CUE: shadows random, four layouts, the PS / VS layouts set and dirty, the rest as CUE::Init leaves it
        for (size_t i = 0; i < kCueBytes; i += 8) {
            put64(i, rng());
        }
        std::memset(cue_bytes + 0xc090, 0, 0xc838 - 0xc090);
        put64(0xc090, 0x6cb0000000ull);
        for (u32 i = 0; i < 6; ++i) {
            put64(0xc0a8 + 8 * i, reinterpret_cast<u64>(world.data() + kShader));
        }
        put64(0x78, rng());
        put64(0xc130, rng());
        for (u32 i = 0; i < 0x40; i += 4) {
            const u32 v = static_cast<u32>(rng());
            std::memcpy(cue_bytes + 0xc0e0 + i, &v, 4);
        }
        cue_bytes[0x88] = 0;
        u8* lay[2][2] = {{layouts, layouts + 0x140}, {layouts + 0x280, layouts + 0x3c0}}; // [vs?][direct?]
        u8* empty = layouts + 0x500;
        std::memset(empty, 0xff, 0x138);
        std::memset(empty, 0, 4);
        for (u32 v = 0; v < 2; ++v) {
            for (u32 d = 0; d < 2; ++d) {
                MakeLayout(lay[v][d], rng, v == 1, d == 1);
            }
        }
        for (u32 s = 0; s < 8; ++s) {
            put64(0xc180 + 8 * s, reinterpret_cast<u64>(empty));
        }
        const auto set_shader = [&](u32 st, bool direct) { // as 0x165b0 / 0x16690 leave it: the layout, dirty
            put64(0xc180 + 8 * st, reinterpret_cast<u64>(lay[st == 2][direct ? 1 : 0]));
            cue_bytes[0xc1d0 + st] = 1;
        };
        set_shader(1, rng() % 2);
        set_shader(2, rng() % 2);
        Res::Forget(cue);
        std::memset(g_ud, 0, sizeof(g_ud));
        std::memset(g_ud_mask, 0, sizeof(g_ud_mask));
        g_arena_used = 0;
        bool committed[8] = {};
        std::map<u64, Expect> seen[8];
        std::vector<u8> prev_snap[8];
        for (u32 op = 0; op < kOps; ++op) {
            const u32 pick = static_cast<u32>(rng() % 100);
            if (pick < 60) { // a setter: fresh descriptors, or the shadow's own (a redundant call)
                const u32 kind = static_cast<u32>(rng() % CSB::kAllKinds);
                // (stages 1..6: the ones with a shader object in this world)
                const u32 st = rng() % 10 == 0 ? 1 + static_cast<u32>(rng() % 6) : 1 + static_cast<u32>(rng() % 2);
                const u32 count = 1 + static_cast<u32>(rng() % 4);
                const u32 slot = static_cast<u32>(rng() % (kSlots[kind] - count + 1));
                const u32 bytes = CSB::kDescriptorBytes[kind];
                const u64 table = get64(0xc180 + 8 * st);
                const bool same = rng() % 3 == 0;
                for (u32 i = 0; i < count; ++i) {
                    u16 e;
                    std::memcpy(&e, reinterpret_cast<const void*>(table + kTableOff[kind] + 2 * (slot + i)), 2);
                    if (same && e != 0xffff && (e & 0x8000) == 0) {
                        std::memcpy(src.data() + i * bytes, cue_bytes + 0x90 + st * 0x1800 + (e & 0x3fff) * 4, bytes);
                    } else {
                        for (u32 j = 0; j < bytes; ++j) {
                            src[i * bytes + j] = static_cast<u8>(rng() % 4); // few values: equal ones happen
                        }
                    }
                }
                Res::BeforeSetter(kind, cue, st, slot, count, reinterpret_cast<u64>(src.data()));
                CSB::Recording r;
                CSB::RunRecorded(static_cast<CSB::Kind>(kind), CSB::RecordingOriginal(static_cast<CSB::Kind>(kind)),
                                 true, cue, st, slot, count, reinterpret_cast<u64>(src.data()), r);
                for (u32 c = 0; c < r.calls; ++c) { // the direct descriptors reach the GPU's user data
                    const CSB::Call& call = r.call[c];
                    for (u32 j = 0; j < static_cast<u32>(call.count) && call.sgpr + j < 16; ++j) {
                        std::memcpy(&g_ud[st][call.sgpr + j], call.data + 4 * j, 4);
                        g_ud_mask[st] |= 1u << (call.sgpr + j);
                    }
                }
                ++setters;
            } else if (pick < 88) { // a commit
                Res::Commit(commit, cue);
                ++commits;
                for (u32 st = 1; st <= 2; ++st) {
                    if (cue_bytes[0xc1d0 + st] != 0) {
                        fail(bad_present, fmt::format("case {} op {}: stage {} still dirty after the commit", n, op, st));
                    }
                }
                // what the commit snapshotted: the stages whose binding is current
                Engine::Render::R7::NativeResourceBindings b;
                Res::Fill(cue, b);
                for (u32 d = 0; d < 2; ++d) {
                    const u32 st = d == 0 ? 2 : 1;
                    const auto& r = b.stage[d];
                    const u64 layout = get64(0xc180 + 8 * st);
                    if (r.present) {
                        committed[st] = true;
                    }
                    if (r.present != committed[st]) {
                        fail(bad_present, fmt::format("case {} op {}: stage {} present {} expected {}", n, op, st,
                                                      r.present, committed[st]));
                    }
                    if (!r.present) {
                        continue;
                    }
                    ++present;
                    if (r.version == 0) {
                        fail(bad_version0, fmt::format("case {} op {}: stage {} version 0", n, op, st));
                        continue;
                    }
                    u16 dwords;
                    std::memcpy(&dwords, reinterpret_cast<const void*>(layout), 2);
                    std::vector<u8> snap(reinterpret_cast<const u8*>(r.snapshot),
                                         reinterpret_cast<const u8*>(r.snapshot) + u64{r.dwords} * 4);
                    if (r.dwords != dwords || r.shadow != cue + 0x90 + st * 0x1800 ||
                        std::memcmp(snap.data(), reinterpret_cast<const void*>(r.shadow), snap.size()) != 0) {
                        fail(bad_bytes, fmt::format("case {} op {}: stage {} snapshot != shadow", n, op, st));
                    }
                    // user data: the pointers of the shape and the direct values, as the recorded calls left them
                    for (u32 i = 0; i < r.shape.refs; ++i) {
                        const u32 sg = r.shape.sgpr[i];
                        const u64 want = r.snapshot + u64{r.shape.offset[i]} * 4;
                        const u64 have = g_ud[st][sg] | (u64{g_ud[st][sg + 1]} << 32);
                        if (have != want || (g_ud_mask[st] >> sg & 3) != 3) {
                            fail(bad_user, fmt::format("case {} op {}: stage {} sgpr {} pointer {:#x} / {:#x}", n, op,
                                                       st, sg, have, want));
                        }
                    }
                    for (u32 sg = 0; sg < 16; ++sg) {
                        if ((r.direct_mask >> sg & 1) != 0 && ((g_ud_mask[st] >> sg & 1) == 0 || g_ud[st][sg] != r.direct[sg])) {
                            fail(bad_user, fmt::format("case {} op {}: stage {} direct sgpr {}", n, op, st, sg));
                        }
                    }
                    // equal versions: equal bytes, user data, layout
                    auto it = seen[st].find(r.version);
                    if (it != seen[st].end()) {
                        ++version_seen;
                        const Expect& e = it->second;
                        if (e.bytes != snap || e.direct_mask != r.direct_mask || e.layout != layout ||
                            std::memcmp(e.direct, r.direct, sizeof(e.direct)) != 0) {
                            fail(bad_version, fmt::format("case {} op {}: stage {} version {:#x} with other content", n,
                                                          op, st, r.version));
                        }
                    } else {
                        Expect e{snap, r.direct_mask, {}, layout};
                        std::memcpy(e.direct, r.direct, sizeof(e.direct));
                        seen[st].emplace(r.version, std::move(e));
                    }
                    // the changed dwords cover every dword that differs from the stage's previous snapshot
                    u32 bits[0x600 / 32];
                    if (!prev_snap[st].empty() && prev_snap[st].size() == snap.size() && prev_snap[st] != snap &&
                        Res::ChangedDwords(cue, st, bits)) {
                        ++delta_checked;
                        for (u32 dw = 0; dw < r.dwords; ++dw) {
                            if (std::memcmp(prev_snap[st].data() + 4 * dw, snap.data() + 4 * dw, 4) != 0 &&
                                (bits[dw >> 5] >> (dw & 31) & 1) == 0) {
                                fail(bad_delta, fmt::format("case {} op {}: stage {} dword {} changed, not marked", n,
                                                            op, st, dw));
                                break;
                            }
                        }
                    }
                    prev_snap[st] = std::move(snap);
                }
            } else if (pick < 94) { // a shader change (another layout of the stage), dirty
                const u32 st = 1 + static_cast<u32>(rng() % 2);
                set_shader(st, rng() % 2);
                if (rng() % 2) {
                    put64(0xc130, rng());
                }
            } else if (pick < 97) { // single values (0x16970 / 0x169a0), sometimes with the dirty byte
                const u32 st = 1 + static_cast<u32>(rng() % 2);
                const u32 v = static_cast<u32>(rng());
                std::memcpy(cue_bytes + (rng() % 2 ? 0xc0e0 : 0xc100) + 4 * st, &v, 4);
                if (rng() % 2) {
                    cue_bytes[0xc1d0 + st] = 1;
                }
            } else { // R::Reset's CUE::ResetShaders through its probe, then the shader pair again (R::SetShaderPair)
                reset_fn(cue);
                ++resets;
                committed[1] = committed[2] = false;
                for (u32 s = 0; s < 8; ++s) {
                    if (get64(0xc180 + 8 * s) == 0) {
                        put64(0xc180 + 8 * s, reinterpret_cast<u64>(empty));
                    }
                }
                cue_bytes[0xc1c9] = 0; // (no PS shader object to bind)
                set_shader(1, rng() % 2);
                set_shader(2, rng() % 2);
            }
        }
    }
    const Res::Stats t = Res::Totals();
    std::printf("  %llu sequences: %llu setter calls, %llu commits, %llu resets; bindings present %llu, a version seen "
                "again %llu; delta checked %llu\n",
                static_cast<unsigned long long>(cases), static_cast<unsigned long long>(setters),
                static_cast<unsigned long long>(commits), static_cast<unsigned long long>(resets),
                static_cast<unsigned long long>(present), static_cast<unsigned long long>(version_seen),
                static_cast<unsigned long long>(delta_checked));
    std::printf("  different: snapshot/shadow %llu, version/content %llu, user data %llu, presence %llu, delta %llu, "
                "version 0 %llu%s%s\n",
                static_cast<unsigned long long>(bad_bytes), static_cast<unsigned long long>(bad_version),
                static_cast<unsigned long long>(bad_user), static_cast<unsigned long long>(bad_present),
                static_cast<unsigned long long>(bad_delta), static_cast<unsigned long long>(bad_version0),
                first.empty() ? "" : "; first: ", first.c_str());
    std::printf("  state: %s\n", Res::Report(t, 0).c_str());
    (void)kept;
    R7R_CHECK(bad_bytes == 0 && bad_version == 0 && bad_user == 0 && bad_present == 0 && bad_delta == 0 &&
              bad_version0 == 0);
    R7R_CHECK(present > 0 && version_seen > 0 && delta_checked > 0 && resets > 0);
    R7R_CHECK(t.binding_kept > 0 && t.setter_redundant > 0 && t.shape_checked > 0 && t.shape_mismatch == 0);
    R7R_CHECK(t.verify_same_version > 0 && t.verify_violations == 0 && t.verify_shadow_checked > 0 &&
              t.verify_shadow_violations == 0);
    R7R_CHECK(t.verify_fill_checked > 0 && t.verify_fill_mismatch == 0 && t.fills_stale == 0);
    R7R_CHECK(g_stack_fail == 0);

    // an unobserved shadow writer: game-time verification must see it (shadow vs snapshot, then the snapshot of an
    // unchanged version with other bytes)
    {
        Res::Commit(commit, cue);
        Engine::Render::R7::NativeResourceBindings b;
        Res::Fill(cue, b);
        const u32 d = b.stage[0].present ? 0 : 1, st = d == 0 ? 2 : 1;
        const u64 version = b.stage[d].version;
        cue_bytes[0x90 + st * 0x1800] ^= 0x5a; // dword 0: inside every snapshot
        cue_bytes[0xc1d0 + st] = 1;
        const Res::Stats before = Res::Totals();
        Res::Commit(commit, cue);
        Res::Fill(cue, b);
        const Res::Stats after = Res::Totals();
        const bool caught = after.verify_shadow_violations > before.verify_shadow_violations &&
                            after.verify_violations > before.verify_violations && b.stage[d].version == version;
        std::printf("  an unobserved shadow write: %s (shadow check %llu -> %llu, same-version snapshot %llu -> %llu)\n",
                    caught ? "caught" : "MISSED", static_cast<unsigned long long>(before.verify_shadow_violations),
                    static_cast<unsigned long long>(after.verify_shadow_violations),
                    static_cast<unsigned long long>(before.verify_violations),
                    static_cast<unsigned long long>(after.verify_violations));
        R7R_CHECK(caught);
    }
    Res::Deactivate();
    patches.clear();
    *got = got_saved;
}

} // namespace

int R7ResourcesTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                     int& checks) {
    (void)image_size, (void)recomp_dll;
    g_fail = 0;
    g_checks = &checks;
    TestRwSetters(image_base, std::max<u64>(cases / 10, 1000));
    TestResourceState(image_base, recomp_module, std::max<u64>(cases / 500, 200));
    return g_fail;
}

} // namespace SecondSon::NativeEngine::Tests
