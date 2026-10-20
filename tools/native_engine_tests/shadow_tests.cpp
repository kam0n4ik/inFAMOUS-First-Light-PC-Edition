// SecondSonPC — Native Engine R5-F (shadow subsystem) tests: the reconstruction of eboot 0x451700
// (ShadowInstanceUv, the shadow pass's per-instance atlas-tile UV) against the eboot's own code, and the recompiled
// version when a recomp.dll exports recomp_451700. Called from tools/native_engine_tests/main.cpp.
//   docs/NATIVE_RECOMP_R5_F_SHADOW.md
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include <windows.h>

#include "base/abi.h"
#include "engine/guest_hooks.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/reconstructed/shadow_instance_uv.h"

namespace SecondSon::NativeEngine::Tests {

namespace SIU = Reconstructed::ShadowInstanceUv;

namespace {

/// A clone of one function's bytes (the reference original), untouched by any install.
void* Clone(u64 image_base, const Engine::GuestFunction& f) {
    std::string error;
    const CloneResult c =
        CloneGuestFunction(image_base + f.rva, reinterpret_cast<const u8*>(image_base + f.rva), f.size, error);
    if (c.code == nullptr) {
        std::printf("  clone of %#llx failed: %s\n", static_cast<unsigned long long>(f.rva), error.c_str());
    }
    return c.code;
}

/// Differential: `original` (eboot clone) vs the current implementation on generated inputs, bit for bit on the
/// 8 bytes written at out + 0x40. Returns the number of differing cases.
u64 Differential(SIU::Fn original, u64 cases, std::mt19937_64& rng, std::string& first) {
    std::vector<s32> records(64);
    std::vector<s32> atlas(0x20, 0);
    std::vector<s32> table(0x400);
    std::vector<u8> instance(0x20, 0);
    u8 a[SIU::kOutBytes], b[SIU::kOutBytes];
    u64 bad = 0;
    for (u64 n = 0; n < cases; ++n) {
        const s32 start = static_cast<s32>(rng() % 8);
        const s32 count = static_cast<s32>(1 + rng() % 8);
        records[0] = start;
        records[1] = count;
        for (size_t i = 2; i < records.size(); ++i) {
            records[i] = (rng() & 1) ? 2 : static_cast<s32>(rng() % 4);
        }
        atlas[0x5c / 4] = static_cast<s32>(rng() % 4);
        atlas[0x60 / 4] = static_cast<s32>(rng() % 4);
        for (s32& c : table) {
            c = 1 + static_cast<s32>(rng() % 16); // nonzero cell dims (a zero divisor would #DE in the original too)
        }
        instance[0x11] = static_cast<u8>(rng());
        for (u8& x : a) {
            x = static_cast<u8>(rng());
        }
        std::memcpy(b, a, sizeof(b));
        const u64 r = reinterpret_cast<u64>(records.data());
        const u64 at = reinterpret_cast<u64>(atlas.data());
        const u64 tb = reinterpret_cast<u64>(table.data());
        const u64 in = reinterpret_cast<u64>(instance.data());
        original(r, reinterpret_cast<u64>(a), at, 0, tb, in);
        SIU::Implementation()(r, reinterpret_cast<u64>(b), at, 0, tb, in);
        if (std::memcmp(a + 0x40, b + 0x40, 8) != 0 && bad++ == 0) {
            first = "first mismatch at case " + std::to_string(n);
        }
    }
    return bad;
}

} // namespace

/// Returns the number of failures. `recomp_module` may be nullptr (then only the hand-written reconstruction).
int ShadowTests(u64 image_base, u64 cases, void* recomp_module) {
    std::printf("shadow subsystem R5-F: ShadowInstanceUv (0x451700) vs the eboot's code:\n");
    int failures = 0;
    const SIU::Fn orig = reinterpret_cast<SIU::Fn>(Clone(image_base, Reconstructed::kShadowInstanceUvSpec.function));
    if (orig == nullptr) {
        return 1;
    }
    std::mt19937_64 rng{0x5ad04};

    // install self-check (the clone against the implementation, generated inputs)
    {
        std::string error;
        const bool ok = Reconstructed::kShadowInstanceUvSpec.self_check(reinterpret_cast<void*>(orig), error);
        std::printf("  hand-written install self-check: %s%s%s\n", ok ? "passed" : "FAILED", ok ? "" : ": ",
                    error.c_str());
        failures += ok ? 0 : 1;
    }

    // hand-written differential
    {
        std::string first;
        const u64 bad = Differential(orig, cases, rng, first);
        std::printf("  hand-written: %llu cases, %llu different%s%s\n", static_cast<unsigned long long>(cases),
                    static_cast<unsigned long long>(bad), bad ? "; " : "", first.c_str());
        failures += bad ? 1 : 0;
    }

    // recompiled, if the module exports recomp_451700
    if (recomp_module != nullptr) {
        HMODULE m = reinterpret_cast<HMODULE>(recomp_module);
        if (auto* r = reinterpret_cast<SIU::Fn>(GetProcAddress(m, "recomp_451700"))) {
            if (auto* base = reinterpret_cast<u64*>(GetProcAddress(m, "recomp_image_base"))) {
                *base = image_base;
            }
            const SIU::Fn before = SIU::Implementation();
            SIU::UseImplementation(r, "static recompilation");
            std::string error;
            const bool ok = Reconstructed::kShadowInstanceUvSpec.self_check(reinterpret_cast<void*>(orig), error);
            std::printf("  recompiled install self-check: %s%s%s\n", ok ? "passed" : "FAILED", ok ? "" : ": ",
                        error.c_str());
            failures += ok ? 0 : 1;
            std::string first;
            const u64 bad = Differential(orig, cases, rng, first);
            std::printf("  recompiled (recomp_451700): %llu cases, %llu different%s%s\n",
                        static_cast<unsigned long long>(cases), static_cast<unsigned long long>(bad), bad ? "; " : "",
                        first.c_str());
            failures += bad ? 1 : 0;
            SIU::UseImplementation(before, "hand-written reconstruction");
        } else {
            std::printf("  (recomp.dll has no recomp_451700: the hand-written reconstruction only)\n");
        }
    }
    return failures;
}

} // namespace SecondSon::NativeEngine::Tests
