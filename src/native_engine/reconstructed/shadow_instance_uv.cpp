// SecondSonPC — reconstruction of eboot 0x451700 (see shadow_instance_uv.h).
//
// Floating point: every operation is the original's own instruction with the original's operand order
// (vcvtsi2ss, vdivss, vmulss, vinsertps); the values are integer-derived (never NaN), so on the same CPU and MXCSR
// the 8 stored bytes are bit-identical. Built with /arch:AVX (CMakeLists.txt): the original is AVX (VEX) code.
#include "native_engine/reconstructed/shadow_instance_uv.h"

#include <immintrin.h>

#include <cstring>
#include <random>

#include <fmt/format.h>

#include "native_engine/guest_abi_bridge.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

#pragma clang fp contract(off)

namespace SecondSon::NativeEngine::Reconstructed {

namespace {

namespace SIU = ShadowInstanceUv;

constexpr Engine::GuestFunction kFunction{0x451700, 0xa0, 0x6ef118bfc2893f29ull};
constexpr u64 kOutField = 0x40; ///< the float2 the loop writes

SIU::Fn g_impl = &SIU::Native;
const char* g_impl_source = "hand-written reconstruction";

/// One matching record's atlas-tile offset, the original's instructions and operand order.
inline void StoreTile(u64 out, s32 cell0, s32 cell1, u8 instance_byte) {
    // n = byte % (cell1 * cell0); col = n % cell0; row = n / cell0 (signed IDIV, as the eboot; byte is 0..255)
    const s32 prod = static_cast<s32>(static_cast<u32>(cell1) * static_cast<u32>(cell0));
    const s32 n = static_cast<s32>(instance_byte) % prod;
    const s32 col = n % cell0;
    const s32 row = n / cell0;
    const __m128 one = _mm_set_ss(1.0f);
    const __m128 recip6 = _mm_div_ss(one, _mm_cvtsi32_ss(one, cell0)); // 1.0 / (float)cell0
    const __m128 recip5 = _mm_div_ss(one, _mm_cvtsi32_ss(one, cell1)); // 1.0 / (float)cell1
    const __m128 u = _mm_mul_ss(_mm_cvtsi32_ss(one, col), recip6);     // (col) * (1/cell0)
    const __m128 v = _mm_mul_ss(_mm_cvtsi32_ss(one, row), recip5);     // (row) * (1/cell1)
    const __m128 uv = _mm_insert_ps(u, v, 0x10);                       // {u[0], v[0], ...}
    _mm_storel_pi(reinterpret_cast<__m64*>(out + kOutField), uv);      // vmovlps: low 8 bytes
}

// --- shadow comparison: only out + 0x40 (8 bytes) is written; the rest is read-only ---------------------------------

struct ShadowSet {
    u8 a[SIU::kOutBytes];
    u8 b[SIU::kOutBytes];
};
thread_local ShadowSet* t_set = nullptr;

ShadowSet& Shadows() {
    if (t_set == nullptr) { // per verifying thread, never freed (a handful of render threads)
        t_set = new ShadowSet{};
    }
    return *t_set;
}

/// A mismatch case: the reduced footprint the stored result depends on, replayable against the eboot.
struct Case {
    s32 cell0;
    s32 cell1;
    u32 instance_byte;
    u8 out_original[8];
    u8 out_impl[8];
};
static_assert(sizeof(Case) <= kCaseBytes);

struct Impl {
    static Slot slot;

    static SSPC_SYSV_ABI void Native(u64 records, u64 out, u64 atlas, u64 u, u64 table, u64 instance) {
        g_impl(records, out, atlas, u, table, instance);
    }

    static SSPC_SYSV_ABI void Verify(u64 records, u64 out, u64 atlas, u64 u, u64 table, u64 instance) {
        ShadowSet& s = Shadows();
        std::memcpy(s.a, reinterpret_cast<const void*>(out), SIU::kOutBytes);
        std::memcpy(s.b, s.a, SIU::kOutBytes);
        const SIU::Fn original = reinterpret_cast<SIU::Fn>(slot.original);
        original(records, reinterpret_cast<u64>(s.a), atlas, u, table, instance);
        g_impl(records, reinterpret_cast<u64>(s.b), atlas, u, table, instance);
        Count(slot.index, kCallsChecked);
        slot.verified.fetch_add(1, std::memory_order_relaxed);
        if (std::memcmp(s.a + kOutField, s.b + kOutField, 8) != 0) {
            slot.mismatches.fetch_add(1, std::memory_order_relaxed);
            // reconstruct the reduced inputs from the authoritative run for the record
            Case c{};
            std::memcpy(c.out_original, s.a + kOutField, 8);
            std::memcpy(c.out_impl, s.b + kOutField, 8);
            RecordCase(slot.index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
        }
        // authoritative: the original writes the real memory
        original(records, out, atlas, u, table, instance);
    }

    static SSPC_SYSV_ABI void Guarded(u64 records, u64 out, u64 atlas, u64 u, u64 table, u64 instance) {
        if (SampleThisCall(slot.index, slot.sample_period)) {
            ShadowSet& s = Shadows();
            std::memcpy(s.a, reinterpret_cast<const void*>(out), SIU::kOutBytes);
            std::memcpy(s.b, s.a, SIU::kOutBytes);
            reinterpret_cast<SIU::Fn>(slot.original)(records, reinterpret_cast<u64>(s.a), atlas, u, table, instance);
            g_impl(records, reinterpret_cast<u64>(s.b), atlas, u, table, instance);
            slot.verified.fetch_add(1, std::memory_order_relaxed);
            if (std::memcmp(s.a + kOutField, s.b + kOutField, 8) != 0) {
                slot.mismatches.fetch_add(1, std::memory_order_relaxed);
                Fallback(slot);
                Count(slot.index, kCallsOriginal);
                return reinterpret_cast<SIU::Fn>(slot.original)(records, out, atlas, u, table, instance);
            }
        }
        Count(slot.index, kCallsNative);
        g_impl(records, out, atlas, u, table, instance);
    }

    static SSPC_SYSV_ABI void Bench(u64 records, u64 out, u64 atlas, u64 u, u64 table, u64 instance) {
        const SIU::Fn original = reinterpret_cast<SIU::Fn>(slot.original);
        if (SampleThisCall(slot.index, slot.sample_period)) {
            ShadowSet& s = Shadows();
            std::memcpy(s.a, reinterpret_cast<const void*>(out), SIU::kOutBytes);
            std::memcpy(s.b, s.a, SIU::kOutBytes);
            const u64 e0 = TimerTicks();
            const u64 e1 = TimerTicks();
            u64 to = 0, tn = 0;
            const bool original_first = (slot.bench_samples.load(std::memory_order_relaxed) & 1) == 0;
            for (int k = 0; k < 2; ++k) {
                if ((k == 0) == original_first) {
                    const u64 t0 = TimerTicks();
                    original(records, reinterpret_cast<u64>(s.a), atlas, u, table, instance);
                    to = TimerTicks() - t0;
                } else {
                    const u64 t0 = TimerTicks();
                    g_impl(records, reinterpret_cast<u64>(s.b), atlas, u, table, instance);
                    tn = TimerTicks() - t0;
                }
            }
            slot.bench_samples.fetch_add(1, std::memory_order_relaxed);
            slot.bench_original_ticks.fetch_add(to, std::memory_order_relaxed);
            slot.bench_native_ticks.fetch_add(tn, std::memory_order_relaxed);
            slot.bench_empty_ticks.fetch_add(e1 - e0, std::memory_order_relaxed);
        }
        Count(slot.index, kCallsOriginal);
        original(records, out, atlas, u, table, instance);
    }
};
Slot Impl::slot;

using D = Dispatch<void(u64, u64, u64, u64, u64, u64)>;

/// Self-check at install: generated records/atlas/table/instance, the clone against the implementation, bit for bit.
bool SelfCheck(void* original, std::string& error) {
    const SIU::Fn orig = reinterpret_cast<SIU::Fn>(original);
    std::mt19937_64 rng{0x53504e4500451700ull};
    // a records array, an atlas header, a table of cells, an instance: the implementation reads only these
    std::vector<s32> records(64);
    std::vector<s32> atlas(0x20, 0);
    std::vector<s32> table(0x400);
    std::vector<u8> instance(0x20, 0);
    u8 a[SIU::kOutBytes], b[SIU::kOutBytes];
    for (u32 n = 0; n < 4096; ++n) {
        const s32 start = static_cast<s32>(rng() % 8);
        const s32 count = static_cast<s32>(1 + rng() % 8);
        records[0] = start;
        records[1] = count;
        for (u32 i = 2; i < records.size(); ++i) {
            records[i] = (rng() & 1) ? 2 : static_cast<s32>(rng() % 4); // kind; value 2 matches
        }
        atlas[0x5c / 4] = static_cast<s32>(rng() % 4);
        atlas[0x60 / 4] = static_cast<s32>(rng() % 4);
        for (s32& c : table) {
            c = 1 + static_cast<s32>(rng() % 16); // nonzero cell dims (a zero would #DE in the original too)
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
        orig(r, reinterpret_cast<u64>(a), at, 0, tb, in);
        g_impl(r, reinterpret_cast<u64>(b), at, 0, tb, in);
        if (std::memcmp(a, b, sizeof(a)) != 0) {
            error = fmt::format("case {}: start {} count {} differs", n, start, count);
            return false;
        }
    }
    return true;
}

} // namespace

SSPC_SYSV_ABI void SIU::Native(u64 records, u64 out, u64 atlas, u64 /*unused*/, u64 table, u64 instance) {
    const s32* rec = GuestPtr<s32>(records);
    const s32 start = rec[0];
    const s32 count = rec[1];
    if (count <= 0) {
        return;
    }
    const s64 base = static_cast<s64>(table) +
                     static_cast<s64>(GuestLoad<s32>(atlas + 0x5c)) * 0x20 +
                     static_cast<s64>(GuestLoad<s32>(atlas + 0x60)) * 0x10 + 0x20;
    const u8 instance_byte = GuestLoad<u8>(instance + 0x11);
    const s32 end = start + count;
    for (s32 i = start; i < end; ++i) {
        if (GuestLoad<s32>(records + static_cast<u64>(i + 2) * 4) == 2) {
            const s32 tile = GuestLoad<s32>(records + static_cast<u64>(i + 6) * 4);
            const s32 cell0 = GuestLoad<s32>(static_cast<u64>(base) + static_cast<u64>(tile) * 4);
            const s32 cell1 = GuestLoad<s32>(static_cast<u64>(base) + static_cast<u64>(tile) * 4 + 4);
            StoreTile(out, cell0, cell1, instance_byte);
        }
    }
}

bool SIU::Compare(Fn original, u64 records, u64 out, u64 atlas, u64 table, u64 instance, u8* a, u8* b) {
    std::memcpy(a, reinterpret_cast<const void*>(out), kOutBytes);
    std::memcpy(b, a, kOutBytes);
    original(records, reinterpret_cast<u64>(a), atlas, 0, table, instance);
    g_impl(records, reinterpret_cast<u64>(b), atlas, 0, table, instance);
    return std::memcmp(a + kOutField, b + kOutField, 8) == 0;
}

bool SIU::Replay(Fn /*original*/, const std::string& bytes, std::string& report) {
    if (bytes.size() != sizeof(Case)) {
        report = fmt::format("case of {} bytes, expected {}", bytes.size(), sizeof(Case));
        return false;
    }
    Case c;
    std::memcpy(&c, bytes.data(), sizeof(c));
    alignas(16) u8 out[kOutBytes] = {};
    StoreTile(reinterpret_cast<u64>(out), c.cell0, c.cell1, static_cast<u8>(c.instance_byte));
    const bool ok = std::memcmp(out + kOutField, c.out_original, 8) == 0;
    report = fmt::format("cell0 {} cell1 {} byte {}: {}", c.cell0, c.cell1, c.instance_byte, ok ? "same" : "DIFFERENT");
    return ok;
}

void SIU::UseImplementation(Fn fn, const char* source) {
    g_impl = fn;
    g_impl_source = source;
}
SIU::Fn SIU::Implementation() {
    return g_impl;
}
const char* SIU::ImplementationSource() {
    return g_impl_source;
}

const FunctionSpec kShadowInstanceUvSpec{
    "shadow_instance_uv",
    "ShadowInstanceUv",
    kFunction,
    nullptr,
    0,
    nullptr,
    0,
    reinterpret_cast<const void*>(&D::Entry<Impl>),
    &Impl::slot,
    &SelfCheck,
    nullptr,
};

} // namespace SecondSon::NativeEngine::Reconstructed
