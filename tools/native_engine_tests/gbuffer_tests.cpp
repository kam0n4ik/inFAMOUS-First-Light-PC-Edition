// SecondSonPC — Native Engine R5-E tests: the G-buffer / geometry pass functions against the eboot's own code
// (docs/NATIVE_RECOMP_R5_E_GBUFFER.md).
//
//   0x11e5d0 R::SetDrawConstants: the hand-written reconstruction (and recomp_11e5d0 when a DLL is given) against a
//            clone of the eboot's code, the install self-check, every mode through the installed jump, a wrong
//            implementation under guarded.
//   0x1a1770 / 0x1a1960 / 0x1a2030: instruction level only (--lift-check), see docs/NATIVE_RECOMP_R5_E_GBUFFER.md.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <windows.h>
#include <xbyak/xbyak.h>

#include "engine/guest_hooks.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"
#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/gbuffer_draw_constants.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

namespace DCS = Reconstructed::DrawConstantsSet;

int g_failures = 0;
int g_checks = 0;

#define GB_CHECK(cond)                                                                                                 \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
        }                                                                                                              \
    } while (0)

void* Pristine(u64 image_base, const Engine::GuestFunction& f) {
    std::string error;
    const CloneResult c =
        CloneGuestFunction(image_base + f.rva, reinterpret_cast<const u8*>(image_base + f.rva), f.size, error);
    if (c.code == nullptr) {
        std::printf("  clone of %#llx failed: %s\n", static_cast<unsigned long long>(f.rva), error.c_str());
    }
    return c.code;
}

u32 SpecialBits(std::mt19937_64& rng) {
    static constexpr u32 kBits[] = {0x00000000u, 0x80000000u, 0x3f800000u, 0xbf800000u, 0x00000001u, 0x807fffffu,
                                    0x00800000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc12345u,
                                    0x7fa00001u, 0x3f800001u};
    return rng() % 3 == 0 ? static_cast<u32>(rng()) : kBits[rng() % std::size(kBits)];
}

/// One generated call: an 8 KiB block, a source with every float kind, offsets with garbage above the low 16 bits,
/// counts <= 0 included, some slots unchanged, sometimes a source inside the block (aliasing).
struct DcsCase {
    std::vector<u8> block = std::vector<u8>(DCS::kMaxSpan + 0x1000);
    std::vector<u32> src = std::vector<u32>(512);
    u32 offset = 0;
    s32 count = 0;
    u64 src_address = 0;

    void Generate(std::mt19937_64& rng, u64 n) {
        for (size_t i = 0; i < block.size(); i += 4) {
            const u32 v = SpecialBits(rng);
            std::memcpy(block.data() + i, &v, 4);
        }
        offset = static_cast<u32>(rng() % 0x400) | (n % 5 == 0 ? static_cast<u32>(rng()) & 0xffff0000u : 0u);
        count = n % 13 == 0 ? -static_cast<s32>(rng() % 3) : static_cast<s32>(rng() % 300);
        for (u32& v : src) {
            v = SpecialBits(rng);
        }
        const u64 dst = static_cast<u64>(static_cast<u16>(offset)) * 4;
        for (s32 i = 0; i < count; ++i) {
            if (rng() % 3 == 0 && dst + i * 4 + 4 <= block.size()) {
                std::memcpy(&src[i], block.data() + dst + i * 4, 4);
            }
        }
        src_address = reinterpret_cast<u64>(src.data());
        if (n % 17 == 3 && count > 0) { // the source inside the block, overlapping the slots
            src_address = reinterpret_cast<u64>(block.data()) + dst + (rng() % 2 ? 4 : 0) - (rng() % 2 ? 4 : 0);
            if (src_address < reinterpret_cast<u64>(block.data())) {
                src_address = reinterpret_cast<u64>(block.data());
            }
        }
    }
};

/// A wrong implementation: also flips the dirty byte.
SSPC_SYSV_ABI void BrokenDrawConstants(u64 block, u32 offset, u64 src, u32 count) {
    DCS::Native(block, offset, src, count);
    *reinterpret_cast<u8*>(block + DCS::kDirtyOffset) ^= 2;
}

void TestDrawConstants(u64 image_base, HMODULE recomp, u64 cases) {
    std::printf("R::SetDrawConstants 0x11e5d0 (R5-E):\n");
    const FunctionSpec& spec = Reconstructed::kDrawConstantsSetSpec;
    void* reference = Pristine(image_base, spec.function);
    GB_CHECK(reference != nullptr);
    if (reference == nullptr) {
        return;
    }
    const auto ref = reinterpret_cast<DCS::Fn>(reference);
    struct Variant {
        const char* name;
        DCS::Fn fn;
    };
    std::vector<Variant> variants{{"hand-written", &DCS::Native}};
    if (recomp != nullptr) {
        if (auto* r = reinterpret_cast<DCS::Fn>(GetProcAddress(recomp, "recomp_11e5d0"))) {
            variants.push_back({"recompiled", r});
        }
    }
    std::mt19937_64 rng{0x11e5d0};
    DcsCase c;
    std::vector<u8> a, b;
    // MXCSR: the guest's default and with DAZ/FTZ (the comparison must follow the thread's mode)
    const u32 csr0 = _mm_getcsr();
    for (const Variant& v : variants) {
        for (const u32 csr : {0x1f80u, 0x9fc0u}) {
            _mm_setcsr(csr);
            u64 bad = 0;
            std::string first;
            for (u64 n = 0; n < cases; ++n) {
                c.Generate(rng, n);
                a = c.block;
                const u64 rel = c.src_address - reinterpret_cast<u64>(c.block.data());
                const bool inside = rel < c.block.size();
                ref(reinterpret_cast<u64>(a.data()), c.offset, inside ? reinterpret_cast<u64>(a.data()) + rel : c.src_address,
                    static_cast<u32>(c.count));
                b = c.block;
                v.fn(reinterpret_cast<u64>(b.data()), c.offset, inside ? reinterpret_cast<u64>(b.data()) + rel : c.src_address,
                     static_cast<u32>(c.count));
                if (a != b && bad++ == 0) {
                    first = fmt::format("case {}: offset {:#x} count {}{}", n, c.offset, c.count, inside ? " (aliased)" : "");
                }
            }
            std::printf("  %s vs eboot, MXCSR %#x: %llu cases, %llu different%s%s\n", v.name, csr,
                        static_cast<unsigned long long>(cases), static_cast<unsigned long long>(bad),
                        first.empty() ? "" : "; first: ", first.c_str());
            GB_CHECK(bad == 0);
        }
    }
    _mm_setcsr(csr0);

    // the install self-check with the clone of the loaded original
    std::string error;
    for (const Variant& v : variants) {
        DCS::UseImplementation(v.fn, v.name);
        error.clear();
        const bool ok = spec.self_check(reference, error);
        std::printf("  %s: install self-check %s%s%s\n", v.name, ok ? "passed" : "FAILED", ok ? "" : ": ", error.c_str());
        GB_CHECK(ok);
    }
    DCS::UseImplementation(&DCS::Native, "hand-written reconstruction");

    // installed on the mapped image (hash checked, clone, jump): every mode through the jump against the reference
    FunctionSpec wrong = spec;
    wrong.function.fnv1a64 ^= 1;
    GB_CHECK(!InstallFunction(image_base, wrong, Mode::Native, error));
    error.clear();
    const bool installed = InstallFunction(image_base, spec, Mode::Native, error);
    if (!installed) {
        std::printf("  install: %s\n", error.c_str());
    }
    GB_CHECK(installed);
    if (!installed) {
        return;
    }
    const auto entry = reinterpret_cast<DCS::Fn>(image_base + spec.function.rva);
    for (const Variant& v : variants) {
        DCS::UseImplementation(v.fn, v.name);
        for (const Mode mode : {Mode::Native, Mode::Verify, Mode::Guarded, Mode::Bench, Mode::Original}) {
            spec.slot->mode.store(static_cast<u8>(mode));
            spec.slot->sample_period = 3;
            u64 differ = 0;
            for (int n = 0; n < 5000; ++n) {
                c.Generate(rng, static_cast<u64>(n));
                a = c.block;
                b = c.block;
                const u64 rel = c.src_address - reinterpret_cast<u64>(c.block.data());
                const bool inside = rel < c.block.size();
                entry(reinterpret_cast<u64>(a.data()), c.offset, inside ? reinterpret_cast<u64>(a.data()) + rel : c.src_address,
                      static_cast<u32>(c.count));
                ref(reinterpret_cast<u64>(b.data()), c.offset, inside ? reinterpret_cast<u64>(b.data()) + rel : c.src_address,
                    static_cast<u32>(c.count));
                differ += a != b ? 1 : 0;
            }
            std::printf("  %s %-8s: 5000 calls through the jump, different from the original %llu\n", v.name,
                        ModeName(mode), static_cast<unsigned long long>(differ));
            GB_CHECK(differ == 0);
        }
    }
    const Slot& s = *spec.slot;
    std::printf("  verified %llu, mismatches %llu, skipped %llu (aliased source), live-diff %llu, bench samples %llu\n",
                static_cast<unsigned long long>(s.verified.load()), static_cast<unsigned long long>(s.mismatches.load()),
                static_cast<unsigned long long>(s.skipped.load()), static_cast<unsigned long long>(s.live_diff.load()),
                static_cast<unsigned long long>(s.bench_samples.load()));
    GB_CHECK(s.verified.load() > 0 && s.mismatches.load() == 0 && s.live_diff.load() == 0 && s.skipped.load() > 0 &&
             s.bench_samples.load() > 0);

    // a wrong implementation (also sets the dirty byte when nothing changed): guarded sees it on its first sample,
    // falls back, and the call's result is the original's
    {
        Slot& m = *spec.slot;
        DCS::UseImplementation(&BrokenDrawConstants, "broken (test)");
        m.mode.store(static_cast<u8>(Mode::Guarded));
        m.sample_period = 1;
        const u64 mismatches = m.mismatches.load(), fallbacks = m.fallbacks.load(), recorded = CasesRecorded();
        c.Generate(rng, 1);
        a = c.block;
        b = c.block;
        entry(reinterpret_cast<u64>(a.data()), c.offset, c.src_address, static_cast<u32>(c.count));
        ref(reinterpret_cast<u64>(b.data()), c.offset, c.src_address, static_cast<u32>(c.count));
        const bool caught = m.mismatches.load() == mismatches + 1 && m.fallbacks.load() == fallbacks + 1 &&
                            static_cast<Mode>(m.mode.load()) == Mode::Original && CasesRecorded() == recorded + 1;
        std::printf("  a wrong implementation under guarded: mismatch %s, fallback %s, result the original's %s\n",
                    m.mismatches.load() > mismatches ? "seen" : "MISSED", m.fallbacks.load() > fallbacks ? "done" : "MISSING",
                    a == b ? "yes" : "NO");
        GB_CHECK(caught && a == b);
    }
    DCS::UseImplementation(&DCS::Native, "hand-written reconstruction");
    spec.slot->mode.store(static_cast<u8>(Mode::Native));
}

} // namespace

/// All R5-E tests; returns the number of failures (the caller adds them to its total).
int GBufferTests(u64 image_base, const std::string& recomp_dll, u64 cases) {
    HMODULE recomp = nullptr;
    if (!recomp_dll.empty()) {
        recomp = LoadLibraryA(recomp_dll.c_str());
        if (auto* base = recomp ? reinterpret_cast<u64*>(GetProcAddress(recomp, "recomp_image_base")) : nullptr) {
            *base = image_base;
        }
    }
    TestDrawConstants(image_base, recomp, std::max<u64>(cases / 10, 20000));
    std::printf("R5-E: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures;
}

} // namespace SecondSon::NativeEngine::Tests
