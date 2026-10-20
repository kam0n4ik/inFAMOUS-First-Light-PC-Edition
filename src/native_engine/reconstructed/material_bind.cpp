// SecondSonPC — reconstruction of eboot 0x448640, the mesh pass's material setup (see material_bind.h).
#include "native_engine/reconstructed/material_bind.h"

#include <atomic>
#include <cstring>
#include <iterator>
#include <random>
#include <vector>

#include <fmt/format.h>

#include "engine/guest_hooks.h"
#include "native_engine/guest_abi_bridge.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

namespace SecondSon::NativeEngine::Reconstructed {

namespace MB = MaterialBind;

namespace {

constexpr Engine::GuestFunction kFunction{0x448640, 0x15f, 0x22c6fc74651c7374ull};
/// 0x27530 is called, not inlined: the stage table it reads stays the game's (pinned by the function's hash only).
constexpr Engine::GuestFunction kAssumed[] = {{MB::kStageRva, 0xe, 0xf08ab1c7f617f7e7ull}};

constexpr u64 kCue = 0x128;             ///< context R + 0x128: its CUE
constexpr u64 kDescriptors = 0x28;      ///< material + 0x28: its descriptor block D
constexpr u64 kSamplerCount = 0x80;     ///< pass: u32 count, s32 base (in 32-byte units) at + 0x84,
constexpr u64 kSamplerBase = 0x84;      ///<       entries {u8 slot, u8 stage, u16 index} at + 0x70
constexpr u64 kSamplerEntries = 0x70;
constexpr u64 kTextureCount = 0x6c;     ///< pass: u32 count, entries {u8 slot, u8 stage, u16 index} at + 0x2c
constexpr u64 kTextureEntries = 0x2c;
constexpr u64 kConstantsStage[2] = {1, 2};
constexpr u64 kConstantsSlot = 0xd;

std::atomic<u64> g_module{0};
MB::SetterFn g_setters[MB::kSetters] = {};
MB::StageFn g_stage = nullptr;
MB::Fn g_recording = nullptr;
MB::Fn g_impl = &MB::Native;
const char* g_impl_source = "hand-written reconstruction";
void (*g_redirect)(u64, u64) = nullptr;

thread_local MB::Recording* t_recording = nullptr;
thread_local const MB::SetterFn* t_setters = nullptr; ///< the hand-written reconstruction's setters in a recorded run

template <MB::Setter S>
SSPC_SYSV_ABI u64 Record(u64 cue, u64 stage, u64 slot, u64 count, u64 src) {
    MB::Recording* r = t_recording;
    if (r == nullptr) {
        return 0;
    }
    if (r->calls >= MB::kMaxCalls) {
        r->overflow = true;
        return 0;
    }
    MB::Call& c = r->call[r->calls++];
    c = MB::Call{S, cue, stage, slot, count, src, {}};
    std::memcpy(c.data, reinterpret_cast<const void*>(src), MB::kSetterBytes[S]);
    return 0;
}
constexpr MB::SetterFn kRecorders[MB::kSetters] = {&Record<MB::kConstantBuffers>, &Record<MB::kSamplers>,
                                                   &Record<MB::kTextures>};

struct Shadow {
    MB::Recording a;
    MB::Recording b;
};
thread_local Shadow* t_shadow = nullptr;

Shadow& Shadows() {
    if (t_shadow == nullptr) { // per verifying thread, never freed (off the guest stack: 2 x 20 KiB)
        t_shadow = new Shadow;
    }
    return *t_shadow;
}

/// The original (recording clone) and the implementation, both recorded: the same calls in the same order.
bool Compare(u64 pass, u64 context, u64 material, Shadow& s) {
    MB::RunRecorded(g_recording, true, pass, context, material, s.a);
    MB::RunRecorded(g_impl, false, pass, context, material, s.b);
    return MB::SameRecordings(s.a, s.b);
}

/// A mismatch case: the pass's binding tables and the head of the descriptor block, and both recordings' calls
/// (setter and arguments; diagnostics - the descriptors themselves are the material's constant memory).
struct Case {
    u64 pass, context, material, descriptors;
    u8 pass_bytes[0x100];
    u8 descriptor_head[0x20];
    u32 calls_original, calls_reconstruction;
    u64 call[2][12][6];
};
static_assert(sizeof(Case) <= kCaseBytes);

void RecordMismatch(u32 index, u64 pass, u64 context, u64 material, const Shadow& s) {
    Case c{};
    c.pass = pass;
    c.context = context;
    c.material = material;
    std::memcpy(c.pass_bytes, reinterpret_cast<const void*>(pass), sizeof(c.pass_bytes));
    if (material != 0) {
        c.descriptors = GuestLoad<u64>(material + kDescriptors);
        std::memcpy(c.descriptor_head, reinterpret_cast<const void*>(c.descriptors), sizeof(c.descriptor_head));
    }
    c.calls_original = s.a.calls;
    c.calls_reconstruction = s.b.calls;
    const MB::Recording* rec[2] = {&s.a, &s.b};
    for (u32 k = 0; k < 2; ++k) {
        for (u32 i = 0; i < rec[k]->calls && i < 12; ++i) {
            const MB::Call& x = rec[k]->call[i];
            const u64 v[6] = {x.setter, x.cue, x.stage, x.slot, x.count, x.src};
            std::memcpy(c.call[k][i], v, sizeof(v));
        }
    }
    RecordCase(index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
}

struct Impl {
    static Slot slot;

    static MB::Fn Original() {
        return reinterpret_cast<MB::Fn>(slot.original);
    }

    /// False when the call could not be compared (a recording overflowed): counted as skipped.
    static bool Check(u64 pass, u64 context, u64 material, bool& same) {
        Shadow& s = Shadows();
        same = Compare(pass, context, material, s);
        if (s.a.overflow || s.b.overflow) {
            slot.skipped.fetch_add(1, std::memory_order_relaxed);
            same = true;
            return false;
        }
        Count(slot.index, kCallsChecked);
        slot.verified.fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            slot.mismatches.fetch_add(1, std::memory_order_relaxed);
            RecordMismatch(slot.index, pass, context, material, s);
        }
        return true;
    }

    static SSPC_SYSV_ABI void Native(u64 pass, u64 context, u64 material) { // counted by the dispatcher
        g_impl(pass, context, material);
    }

    static SSPC_SYSV_ABI void Verify(u64 pass, u64 context, u64 material) {
        bool same;
        if (!Check(pass, context, material, same)) {
            Count(slot.index, kCallsOriginal);
        }
        Original()(pass, context, material); // authoritative
    }

    static SSPC_SYSV_ABI void Guarded(u64 pass, u64 context, u64 material) {
        bool same = true;
        if (SampleThisCall(slot.index, slot.sample_period) && Check(pass, context, material, same) && !same) {
            Fallback(slot); // the original from now on, on every thread
            Count(slot.index, kCallsOriginal);
            Original()(pass, context, material);
            return;
        }
        Count(slot.index, kCallsNative);
        g_impl(pass, context, material);
    }

    static SSPC_SYSV_ABI void Bench(u64 pass, u64 context, u64 material) {
        if (SampleThisCall(slot.index, slot.sample_period)) {
            Shadow& s = Shadows();
            const u64 e0 = TimerTicks();
            const u64 e1 = TimerTicks();
            u64 to = 0, tn = 0;
            const bool original_first = (slot.bench_samples.load(std::memory_order_relaxed) & 1) == 0;
            for (int k = 0; k < 2; ++k) {
                const u64 t0 = TimerTicks();
                if ((k == 0) == original_first) {
                    MB::RunRecorded(g_recording, true, pass, context, material, s.a);
                    to = TimerTicks() - t0;
                } else {
                    MB::RunRecorded(g_impl, false, pass, context, material, s.b);
                    tn = TimerTicks() - t0;
                }
            }
            slot.bench_samples.fetch_add(1, std::memory_order_relaxed);
            slot.bench_original_ticks.fetch_add(to, std::memory_order_relaxed);
            slot.bench_native_ticks.fetch_add(tn, std::memory_order_relaxed);
            slot.bench_empty_ticks.fetch_add(e1 - e0, std::memory_order_relaxed);
        }
        Count(slot.index, kCallsOriginal);
        Original()(pass, context, material);
    }
};
Slot Impl::slot;

using D = Dispatch<void(u64, u64, u64)>;

bool PrepareInstall(u64 module_base, std::string& error) {
    return MB::Prepare(module_base, error);
}

/// Generated passes and materials (the setters recorded, so the context is never dereferenced): the recording clone
/// against the implementation. `original` is not run - its setter calls would reach the real CUE.
bool SelfCheck(void* /*original*/, std::string& error) {
    if (g_recording == nullptr) {
        error = "no recording clone (prepare did not run)";
        return false;
    }
    std::mt19937_64 rng{0x53504e4500000020ull};
    std::vector<u8> pass(0x100), material(0x40), block(0x10000);
    Shadow& s = Shadows();
    for (u32 n = 0; n < 2000; ++n) {
        for (u8& x : pass) {
            x = static_cast<u8>(rng());
        }
        for (u8& x : block) {
            x = static_cast<u8>(rng());
        }
        // the pass has room for 4 sampler entries (0x70..0x7f, the count follows) and 16 texture entries
        // (0x2c..0x6b); stage numbers the table maps; the samplers' base small (signed) so D + ... stays inside
        for (u32 i = 0; i < 16; ++i) {
            const u16 ti = static_cast<u16>(rng() % 0x100);
            pass[kTextureEntries + 4 * i + 1] = static_cast<u8>(rng() % 8);
            std::memcpy(pass.data() + kTextureEntries + 4 * i + 2, &ti, 2);
            if (i < 4) {
                const u16 si = static_cast<u16>(rng() % 0x100);
                pass[kSamplerEntries + 4 * i + 1] = static_cast<u8>(rng() % 8);
                std::memcpy(pass.data() + kSamplerEntries + 4 * i + 2, &si, 2);
            }
        }
        const u32 samplers = static_cast<u32>(rng() % 5), textures = static_cast<u32>(rng() % 17);
        const s32 base = static_cast<s32>(rng() % 9) - 4;
        std::memcpy(pass.data() + kSamplerCount, &samplers, 4);
        std::memcpy(pass.data() + kSamplerBase, &base, 4);
        std::memcpy(pass.data() + kTextureCount, &textures, 4);
        const u64 d = reinterpret_cast<u64>(block.data()) + 0x1000;
        if (n % 3 == 0) { // no constant buffer
            std::memset(block.data() + 0x1000, 0, 4);
            block[0x1004] = 0;
            block[0x1005] &= 0xf0;
        }
        std::memcpy(material.data() + kDescriptors, &d, 8);
        const u64 m = n % 50 == 0 ? 0 : reinterpret_cast<u64>(material.data());
        const u64 context = rng();
        if (!Compare(reinterpret_cast<u64>(pass.data()), context, m, s)) {
            error = fmt::format("case {}: calls {} / {}{}", n, s.a.calls, s.b.calls,
                                s.a.overflow || s.b.overflow ? " (overflow)" : "");
            return false;
        }
    }
    return true;
}

} // namespace

SSPC_SYSV_ABI void MB::Native(u64 pass, u64 context, u64 material) {
    if (material == 0) {
        return;
    }
    const SetterFn* set = t_setters != nullptr ? t_setters : g_setters;
    const u64 cue = context + kCue;
    const u64 d = GuestLoad<u64>(material + kDescriptors);
    if ((GuestLoad<u32>(d) | (u64{GuestLoad<u32>(d + 4) & 0xfffu} << 32)) != 0) {
        set[kConstantBuffers](cue, kConstantsStage[0], kConstantsSlot, 1, d);
        set[kConstantBuffers](cue, kConstantsStage[1], kConstantsSlot, 1, d);
    }
    if (GuestLoad<u32>(pass + kSamplerCount) != 0) {
        const u64 base = d + static_cast<u64>(static_cast<s64>(GuestLoad<s32>(pass + kSamplerBase)) * 32) + 0x20;
        u32 i = 0;
        do {
            const u64 e = pass + kSamplerEntries + static_cast<u64>(i) * 4;
            const u64 src = base + u64{GuestLoad<u16>(e + 2)} * 16;
            const u64 stage = static_cast<u32>(g_stage(GuestLoad<u8>(e + 1)));
            set[kSamplers](cue, stage, GuestLoad<u8>(e), 1, src);
        } while (++i < GuestLoad<u32>(pass + kSamplerCount));
    }
    if (GuestLoad<u32>(pass + kTextureCount) != 0) {
        u32 i = 0;
        do {
            const u64 e = pass + kTextureEntries + static_cast<u64>(i) * 4;
            const u64 src = d + u64{GuestLoad<u16>(e + 2)} * 32 + 0x20;
            const u64 slot = GuestLoad<u8>(e);
            const u64 stage = static_cast<u32>(g_stage(GuestLoad<u8>(e + 1)));
            set[kTextures](cue, stage, slot, 1, src);
        } while (++i < GuestLoad<u32>(pass + kTextureCount));
    }
}

void MB::RunRecorded(Fn fn, bool recording_original, u64 pass, u64 context, u64 material, Recording& out) {
    out.calls = 0;
    out.overflow = false;
    Recording* const saved = t_recording;
    t_recording = &out;
    if (recording_original) {
        fn(pass, context, material);
    } else if (fn == &Native) {
        const SetterFn* saved_setters = t_setters;
        t_setters = kRecorders;
        fn(pass, context, material);
        t_setters = saved_setters;
    } else if (g_redirect != nullptr) {
        const u64 module = g_module.load(std::memory_order_relaxed);
        for (u32 k = 0; k < kSetters; ++k) {
            g_redirect(module + kSetterRva[k], reinterpret_cast<u64>(kRecorders[k]));
        }
        fn(pass, context, material);
        g_redirect(0, 0);
    } else {
        out.overflow = true; // a foreign implementation without a redirection: its calls cannot be recorded
    }
    t_recording = saved;
}

bool MB::SameRecordings(const Recording& a, const Recording& b) {
    return !a.overflow && !b.overflow && a.calls == b.calls && std::memcmp(a.call, b.call, sizeof(Call) * a.calls) == 0;
}

bool MB::Prepare(u64 module_base, std::string& error) {
    g_module.store(module_base, std::memory_order_relaxed);
    std::vector<CodeRedirect> redirects;
    for (u32 k = 0; k < kSetters; ++k) {
        g_setters[k] = reinterpret_cast<SetterFn>(module_base + kSetterRva[k]);
        redirects.push_back({module_base + kSetterRva[k], reinterpret_cast<u64>(kRecorders[k])});
    }
    g_stage = reinterpret_cast<StageFn>(module_base + kStageRva);
    const u64 entry = module_base + kFunction.rva;
    std::vector<u8> bytes;
    const CloneResult clone =
        CloneGuestFunction(entry, OriginalBytes(module_base, kFunction, bytes), kFunction.size, error, redirects);
    if (clone.code == nullptr) {
        error = "recording clone: " + error;
        return false;
    }
    g_recording = reinterpret_cast<Fn>(clone.code);
    return true;
}

MB::Fn MB::RecordingOriginal() {
    return g_recording;
}

void MB::UseImplementation(Fn fn, const char* source, void (*redirect)(u64 from, u64 to)) {
    g_impl = fn;
    g_impl_source = source;
    g_redirect = redirect;
}

MB::Fn MB::Implementation() {
    return g_impl;
}

const char* MB::ImplementationSource() {
    return g_impl_source;
}

const FunctionSpec kMaterialBindSpec{
    "material_bind",
    "Material::Bind",
    kFunction,
    kAssumed,
    static_cast<u32>(std::size(kAssumed)),
    nullptr,
    0,
    reinterpret_cast<const void*>(&D::Entry<Impl>),
    &Impl::slot,
    &SelfCheck,
    &PrepareInstall,
};

} // namespace SecondSon::NativeEngine::Reconstructed
