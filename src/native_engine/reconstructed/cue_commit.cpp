// SecondSonPC — the CUE commit 0x14ad0: verification and replacement of its static recompilation (see cue_commit.h).
#include "native_engine/reconstructed/cue_commit.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <random>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <windows.h>

#include "engine/guest_hooks.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

namespace SecondSon::NativeEngine::Reconstructed {

namespace CC = CueCommit;

namespace {

constexpr Engine::GuestFunction kFunction{0x14ad0, 0x1184, 0x8bb6e7985e565584ull};
/// The pure builders it calls run for real in a shadow run: pinned, a changed one refuses the install.
constexpr Engine::GuestFunction kAssumed[] = {
    {0x61e0c0, 0x36, 0xb1818409f8d8290dull}, {0x61e330, 0x26, 0xc7c96ff3e831e24aull},
    {0x639250, 0x17, 0x8346b35d1d31c839ull}, {0x6393a0, 0x14, 0x499d0bd23ce9a4ffull},
    {0x639450, 0x84, 0x31bee38c7a86b666ull}, {0x6394e0, 0x24, 0x97a9f7dc32e188cdull},
};

constexpr u64 kCopyTag = 0xc0e0000000000000ull;  ///< a pointer into the CUE copy: the tag | its offset
constexpr u64 kStackTag = 0x57ac000000000000ull; ///< a stack pointer: the tag | the argument (content recorded)

using RecorderFn = SSPC_SYSV_ABI u64 (*)(u64, u64, u64, u64, u64, u64);

std::atomic<u64> g_module{0};
CC::Fn g_recording = nullptr;
CC::Fn g_impl = nullptr; ///< no hand-written reconstruction: the recompiled one, or nothing installs
const char* g_impl_source = "none";
void (*g_redirect)(u64, u64) = nullptr;

/// One verifying thread's shadow state (off the guest stack, never freed; ~0x7a000 bytes).
struct Shadow {
    alignas(64) u8 copy[2][CC::kCopyBytes];
    alignas(64) u8 arena[CC::kArenaBytes];
    alignas(64) u8 arena_original[CC::kArenaBytes];
    u64 arena_used = 0;
    u64 arena_dirty = 0; ///< bytes of the arena the previous run may have written
    u64 arena_original_used = 0;
    CC::Recording rec[2];
};
thread_local Shadow* t_shadow = nullptr;
thread_local CC::Recording* t_recording = nullptr;
thread_local u64 t_copy = 0;
/// The stack a shadow run's frames lie in: below RunRecorded's frame (a guest thread's stack need not be the one the
/// TEB describes, so the window is taken around the current stack pointer).
thread_local u64 t_stack_lo = 0, t_stack_hi = 0;
constexpr u64 kStackWindow = 0x10000;

Shadow& Shadows() {
    if (t_shadow == nullptr) {
        t_shadow = new Shadow;
    }
    return *t_shadow;
}

template <u32 I>
SSPC_SYSV_ABI u64 Record(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    CC::Recording* r = t_recording;
    Shadow* s = t_shadow;
    if (r == nullptr || s == nullptr) {
        return 0;
    }
    constexpr CC::Callee kCallee = CC::kRecorded[I];
    u64 result = 0;
    if constexpr (kCallee.rva == 0x123d0) { // (cue, dwords): the next snapshot of the thread's arena
        const u64 bytes = (u64{static_cast<u32>(a1)} * 4 + 15) & ~15ull;
        if (s->arena_used + bytes <= CC::kArenaBytes) {
            result = reinterpret_cast<u64>(s->arena + s->arena_used);
            s->arena_used += bytes;
        } else {
            r->overflow = true;
        }
    }
    if constexpr (kCallee.rva == 0x9843a0) { // memcpy(dst, src, n): made, into the arena only
        const u64 lo = reinterpret_cast<u64>(s->arena);
        if (a0 >= lo && a2 <= CC::kArenaBytes && a0 + a2 <= lo + CC::kArenaBytes) {
            std::memcpy(reinterpret_cast<void*>(a0), reinterpret_cast<const void*>(a1), a2);
        } else {
            r->overflow = true;
        }
        result = a0;
    }
    if (r->calls >= CC::kMaxCalls) {
        r->overflow = true;
        return result;
    }
    CC::Call& c = r->call[r->calls++];
    c = CC::Call{I, {a0, a1, a2, a3, a4, a5}, {}};
    for (u32 k = 0; k < 6; ++k) {
        u64& v = c.a[k];
        if (k >= kCallee.arity) {
            v = 0;
        } else if (v >= t_copy && v < t_copy + CC::kCopyBytes) {
            v = kCopyTag | (v - t_copy);
        } else if (v >= t_stack_lo && v < t_stack_hi) {
            // setUserDataRegion's values: a4 dwords; any other stack pointer: the 32 bytes at it
            const u64 n = kCallee.rva == 0x626520 && k == 3 ? std::min<u64>(u64{static_cast<u32>(a4)} * 4, 32) : 32;
            std::memcpy(c.content[k], reinterpret_cast<const void*>(v), std::min<u64>(n, t_stack_hi - v));
            v = kStackTag | k;
        }
    }
    return result;
}

template <size_t... I>
constexpr std::array<RecorderFn, sizeof...(I)> MakeRecorders(std::index_sequence<I...>) {
    return {&Record<static_cast<u32>(I)>...};
}
constexpr std::array<RecorderFn, CC::kRecordedCount> kRecorders =
    MakeRecorders(std::make_index_sequence<CC::kRecordedCount>{});

/// Zeroes the stack the next run's frames use: their uninitialised locals are then equal in both runs. Under a page
/// (no stack probe): the clone's frame is 0xc8 bytes, recomp_14ad0's ~2.6 KiB (R, U, the private stack S, CallIO).
__declspec(noinline) void WipeStackBelow() {
    volatile u8 area[0xf00];
    for (u32 i = 0; i < sizeof(area); i += 8) {
        *reinterpret_cast<volatile u64*>(&area[i]) = 0;
    }
}

/// Both runs on copies of the live CUE `cue`: false when they differ (`first` = the first difference's offset in
/// the copies, ~0 when it is in the calls or the arena).
bool Compare(u64 cue, Shadow& s, u64& first) {
    first = ~0ull;
    for (u32 k = 0; k < 2; ++k) {
        std::memset(s.copy[k] + CC::kCueBytes, 0, CC::kCopyBytes - CC::kCueBytes);
        std::memcpy(s.copy[k], reinterpret_cast<const void*>(cue), CC::kCueBytes);
        CC::RunRecorded(k == 0 ? g_recording : g_impl, k == 0, reinterpret_cast<u64>(s.copy[k]), s.rec[k]);
        if (k == 0) {
            s.arena_original_used = s.arena_used;
            std::memcpy(s.arena_original, s.arena, s.arena_used);
        }
    }
    if (!CC::SameRecordings(s.rec[0], s.rec[1]) || s.arena_used != s.arena_original_used ||
        std::memcmp(s.arena, s.arena_original, s.arena_used) != 0) {
        return false;
    }
    if (std::memcmp(s.copy[0], s.copy[1], CC::kCopyBytes) != 0) {
        for (u64 i = 0; i < CC::kCopyBytes; ++i) {
            if (s.copy[0][i] != s.copy[1][i]) {
                first = i;
                break;
            }
        }
        return false;
    }
    return true;
}

/// A mismatch case: the CUE's flags and switches, both recordings' sizes and the first differing call or offset.
struct Case {
    u64 cue;
    u8 flags[0x30];  ///< cue + 0xc1c0
    u8 tail[0x30];   ///< cue + 0xc6e0
    u32 calls_original, calls_reconstruction;
    u64 first_offset;
    u32 first_call;
    CC::Call call[2];
};
static_assert(sizeof(Case) <= kCaseBytes);

void RecordMismatch(u32 index, u64 cue, const Shadow& s, u64 first) {
    Case c{};
    c.cue = cue;
    std::memcpy(c.flags, reinterpret_cast<const void*>(cue + 0xc1c0), sizeof(c.flags));
    std::memcpy(c.tail, reinterpret_cast<const void*>(cue + 0xc6e0), sizeof(c.tail));
    c.calls_original = s.rec[0].calls;
    c.calls_reconstruction = s.rec[1].calls;
    c.first_offset = first;
    c.first_call = ~0u;
    const u32 n = std::min(s.rec[0].calls, s.rec[1].calls);
    for (u32 i = 0; i < n; ++i) {
        if (std::memcmp(&s.rec[0].call[i], &s.rec[1].call[i], sizeof(CC::Call)) != 0) {
            c.first_call = i;
            c.call[0] = s.rec[0].call[i];
            c.call[1] = s.rec[1].call[i];
            break;
        }
    }
    RecordCase(index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
}

struct Impl {
    static Slot slot;

    static CC::Fn Original() {
        return reinterpret_cast<CC::Fn>(slot.original);
    }

    /// False when the call could not be compared (a recording or the arena overflowed): counted as skipped.
    static bool Check(u64 cue, bool& same) {
        Shadow& s = Shadows();
        u64 first;
        same = Compare(cue, s, first);
        if (s.rec[0].overflow || s.rec[1].overflow) {
            slot.skipped.fetch_add(1, std::memory_order_relaxed);
            same = true;
            return false;
        }
        Count(slot.index, kCallsChecked);
        slot.verified.fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            slot.mismatches.fetch_add(1, std::memory_order_relaxed);
            RecordMismatch(slot.index, cue, s, first);
        }
        return true;
    }

    static SSPC_SYSV_ABI void Native(u64 cue) { // counted by the dispatcher
        g_impl(cue);
    }

    /// The original stays authoritative; every Nth call is compared first (a shadow run costs two CUE copies).
    static SSPC_SYSV_ABI void Verify(u64 cue) {
        bool same;
        if (SampleThisCall(slot.index, slot.sample_period)) {
            Check(cue, same);
            // the shadow runs left their frames where the real call's will be: zeroed, so the real call's
            // uninitialised locals hold what they would without verification (no observable effect of it)
            WipeStackBelow();
        }
        Count(slot.index, kCallsOriginal);
        Original()(cue);
    }

    static SSPC_SYSV_ABI void Guarded(u64 cue) {
        bool same = true;
        if (SampleThisCall(slot.index, slot.sample_period)) {
            const bool checked = Check(cue, same);
            WipeStackBelow(); // as in Verify
            if (checked && !same) {
                Fallback(slot); // the original from now on, on every thread
                Count(slot.index, kCallsOriginal);
                Original()(cue);
                return;
            }
        }
        Count(slot.index, kCallsNative);
        g_impl(cue);
    }

    static SSPC_SYSV_ABI void Bench(u64 cue) {
        if (SampleThisCall(slot.index, slot.sample_period)) {
            Shadow& s = Shadows();
            const u64 e0 = TimerTicks();
            const u64 e1 = TimerTicks();
            u64 t[2] = {};
            const bool original_first = (slot.bench_samples.load(std::memory_order_relaxed) & 1) == 0;
            for (u32 n = 0; n < 2; ++n) {
                const u32 k = (n == 0) == original_first ? 0 : 1;
                std::memcpy(s.copy[k], reinterpret_cast<const void*>(cue), CC::kCueBytes);
                const u64 t0 = TimerTicks();
                CC::RunRecorded(k == 0 ? g_recording : g_impl, k == 0, reinterpret_cast<u64>(s.copy[k]), s.rec[k]);
                t[k] = TimerTicks() - t0;
            }
            slot.bench_samples.fetch_add(1, std::memory_order_relaxed);
            slot.bench_original_ticks.fetch_add(t[0], std::memory_order_relaxed);
            slot.bench_native_ticks.fetch_add(t[1], std::memory_order_relaxed);
            slot.bench_empty_ticks.fetch_add(e1 - e0, std::memory_order_relaxed);
            WipeStackBelow(); // as in Verify
        }
        Count(slot.index, kCallsOriginal);
        Original()(cue);
    }
};
Slot Impl::slot;

using D = Dispatch<void(u64)>;

bool PrepareInstall(u64 module_base, std::string& error) {
    if (g_impl == nullptr) {
        error = "no implementation: cue_commit has no hand-written reconstruction, it needs SSPC_NATIVE_ENGINE_RECOMP "
                "with recomp_14ad0 and recomp_set_redirect";
        return false;
    }
    return CC::Prepare(module_base, error);
}

/// Generated CUEs (random contents; dirty flags 0 / 1; layouts, shader structures and the prebuilt descriptor in
/// the same buffer; counts and snapshot offsets inside the stage shadows): the recording clone against the
/// implementation. `original` is not run - its calls would reach the real command buffer.
bool SelfCheck(void* /*original*/, std::string& error) {
    if (g_recording == nullptr || g_impl == nullptr) {
        error = "no recording clone or no implementation";
        return false;
    }
    constexpr u64 kShaders = 0x20000, kShaderBytes = 0x1000, kLayouts = 0x26400, kLayoutBytes = 0x200,
                  kBytes = 0x28000;
    std::vector<u8> mem(kBytes);
    const auto put64 = [&](u64 off, u64 v) { std::memcpy(mem.data() + off, &v, 8); };
    std::mt19937_64 rng{0x53504e4500000040ull};
    Shadow& s = Shadows();
    for (u32 n = 0; n < 300; ++n) {
        for (u64 i = 0; i < kBytes; i += 8) {
            put64(i, rng());
        }
        const u64 base = reinterpret_cast<u64>(mem.data());
        for (u32 i = 0; i < 6; ++i) {
            put64(0xc0a8 + 8 * i, base + kShaders + kShaderBytes * i);
        }
        if (rng() % 10 == 0) {
            put64(0xc0a8, 0);
        }
        put64(0xc1c0, rng() % 2 ? 0 : base + kShaders);
        for (u32 i = 0; i < 6; ++i) {
            u8* t = mem.data() + kLayouts + kLayoutBytes * i;
            const u16 count = static_cast<u16>(1 + rng() % 0x600);
            std::memcpy(t, &count, 2);
            for (u32 b = 4; b < 0x10; ++b) {
                t[b] = rng() % 3 == 0 ? 0xff : static_cast<u8>(rng() % 16);
            }
            for (u32 o = 0x16; o <= 0x20; o += 2) {
                const u16 v = static_cast<u16>(rng() % count);
                std::memcpy(t + o, &v, 2);
            }
            // a shadow slot inside the stage's window (0x600 dwords), as in the game
            const u16 w = rng() % 4 == 0 ? u16{0xffff}
                                         : static_cast<u16>((rng() % 2 ? 0x8000 : 0) | (rng() % 0x5fc));
            std::memcpy(t + 0x108, &w, 2);
            put64(0xc188 + 8 * i, base + kLayouts + kLayoutBytes * i);
        }
        for (u64 f = 0xc1c9; f <= 0xc1de; ++f) {
            mem[f] = static_cast<u8>(rng() % 2);
        }
        mem[0x88] = static_cast<u8>(rng() % 2);
        mem[0xc70e] = static_cast<u8>(rng() % 2);
        const u32 stages = (rng() % 2 ? 0x20u : 0u) | (rng() % 2 ? 1u : 0u);
        std::memcpy(mem.data() + 0xc1e0, &stages, 4);
        const u32 alt = rng() % 2 ? 0 : static_cast<u32>(rng());
        std::memcpy(mem.data() + 0xc6e4, &alt, 4);
        u64 first;
        if (!Compare(base, s, first) || s.rec[0].overflow || s.rec[1].overflow) {
            error = fmt::format("case {}: calls {} / {}, first copy difference {:#x}{}", n, s.rec[0].calls,
                                s.rec[1].calls, first, s.rec[0].overflow || s.rec[1].overflow ? " (overflow)" : "");
            return false;
        }
    }
    return true;
}

} // namespace

void CC::RunRecorded(Fn fn, bool recording_original, u64 cue, Recording& out) {
    Shadow& s = Shadows();
    out.calls = 0;
    out.overflow = false;
    std::memset(s.arena, 0, s.arena_dirty);
    s.arena_used = 0;
    Recording* const saved = t_recording;
    const u64 saved_copy = t_copy;
    t_recording = &out;
    t_copy = cue;
    volatile u8 here = 0;
    t_stack_hi = reinterpret_cast<u64>(&here);
    t_stack_lo = t_stack_hi - kStackWindow;
    const bool redirected = !recording_original && g_redirect != nullptr;
    if (redirected) {
        const u64 module = g_module.load(std::memory_order_relaxed);
        for (u32 i = 0; i < kRecordedCount; ++i) {
            g_redirect(module + kRecorded[i].rva, reinterpret_cast<u64>(kRecorders[i]));
        }
    }
    if (recording_original || redirected) {
        WipeStackBelow();
        fn(cue);
    } else {
        out.overflow = true; // an implementation without a redirection: its calls cannot be recorded
    }
    if (redirected) {
        g_redirect(0, 0);
    }
    s.arena_dirty = std::max(s.arena_dirty, s.arena_used);
    t_recording = saved;
    t_copy = saved_copy;
}

bool CC::SameRecordings(const Recording& a, const Recording& b) {
    return !a.overflow && !b.overflow && a.calls == b.calls && std::memcmp(a.call, b.call, sizeof(Call) * a.calls) == 0;
}

bool CC::Prepare(u64 module_base, std::string& error) {
    g_module.store(module_base, std::memory_order_relaxed);
    std::vector<CodeRedirect> redirects;
    for (u32 i = 0; i < kRecordedCount; ++i) {
        redirects.push_back({module_base + kRecorded[i].rva, reinterpret_cast<u64>(kRecorders[i])});
    }
    std::vector<u8> bytes;
    const CloneResult clone = CloneGuestFunction(module_base + kFunction.rva, OriginalBytes(module_base, kFunction, bytes),
                                                 kFunction.size, error, redirects);
    if (clone.code == nullptr) {
        error = "recording clone: " + error;
        return false;
    }
    g_recording = reinterpret_cast<Fn>(clone.code);
    return true;
}

CC::Fn CC::RecordingOriginal() {
    return g_recording;
}

void CC::UseImplementation(Fn fn, const char* source, void (*redirect)(u64 from, u64 to)) {
    g_impl = fn;
    g_impl_source = source;
    g_redirect = redirect;
}

CC::Fn CC::Implementation() {
    return g_impl;
}

const FunctionSpec kCueCommitSpec{
    "cue_commit",
    "CUE::Commit",
    kFunction,
    kAssumed,
    static_cast<u32>(std::size(kAssumed)),
    nullptr,
    0,
    reinterpret_cast<const void*>(&D::Entry<Impl>),
    &Impl::slot,
    &SelfCheck,
    &PrepareInstall,
    true, // explicit only: not part of "all functions" (there is no implementation without SSPC_NATIVE_ENGINE_RECOMP)
};

} // namespace SecondSon::NativeEngine::Reconstructed
