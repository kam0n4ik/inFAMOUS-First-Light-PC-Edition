// SecondSonPC — reconstruction of eboot 0x16aa0 / 0x16b70, the CUE's buffer setters (see cue_set_buffers.h).
#include "native_engine/reconstructed/cue_set_buffers.h"

#include <atomic>
#include <cstring>
#include <random>
#include <vector>

#include <fmt/format.h>

#include "engine/guest_hooks.h"
#include "native_engine/guest_abi_bridge.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/native_engine_verify.h"

namespace SecondSon::NativeEngine::Reconstructed {

namespace CSB = CueSetBuffers;

namespace {

constexpr u64 kTablePointers = 0xc180; ///< + stage * 8: the bound shader's user-data layout
constexpr u64 kContext = 0xc090;       ///< the command buffer context setUserDataRegion writes to
constexpr u64 kChanged = 0xc1d0;       ///< + stage: "resources changed"
constexpr u64 kShadow = 0x90;          ///< + stage * 0x1800: the stage's resource shadow
constexpr u64 kWindow = 0x1800;
constexpr u64 kTailBegin = 0xc080, kTailEnd = 0xc1e0; ///< context, table pointers and changed bytes
constexpr u32 kMaxStage = 7;
/// A shadow copy: CUE offsets up to the farthest entry of stage 7 (0x90 + 7 * 0x1800 + 0x3fff * 4 + 16 = 0x1a8a0).
constexpr u64 kShadowBytes = 0x1b000;

constexpr Engine::GuestFunction kFunction[CSB::kKinds] = {
    {0x16aa0, 0xc6, 0xf6c333198de2deedull},
    {0x16b70, 0xc6, 0x713c9f7b4b870c4dull},
};

std::atomic<u64> g_callee{0};
CSB::Fn g_recording[CSB::kKinds] = {};
CSB::Fn g_impl[CSB::kKinds] = {&CSB::NativeVertexBuffers, &CSB::NativeBuffers};
const char* g_impl_source[CSB::kKinds] = {"hand-written reconstruction", "hand-written reconstruction"};
CSB::RedirectFn g_redirect[CSB::kKinds] = {};

thread_local CSB::Recording* t_recording = nullptr;
thread_local CSB::CalleeFn t_callee = nullptr; ///< the hand-written reconstruction's callee in a recorded run

inline u64 ShadowOffset(u32 stage) {
    return static_cast<u64>(static_cast<s64>(static_cast<s32>(stage * 0x600u)) * 4) + kShadow;
}

template <u32 kTable>
inline u64 Run(u64 cue, u32 stage, u32 slot, u32 count, u64 src) {
    const u64 st = static_cast<u64>(static_cast<s64>(static_cast<s32>(stage)));
    if (count != 0) {
        const u64 table = GuestLoad<u64>(cue + kTablePointers + st * 8);
        const u64 shadow = cue + ShadowOffset(stage);
        do {
            const u32 entry = GuestLoad<u16>(table + kTable + static_cast<u64>(slot) * 2);
            if (entry != 0xffff) {
                const u32 index = entry & 0x3fff;
                if ((entry & 0x8000) != 0) {
                    const CSB::CalleeFn callee = t_callee != nullptr
                                                     ? t_callee
                                                     : reinterpret_cast<CSB::CalleeFn>(
                                                           g_callee.load(std::memory_order_relaxed));
                    callee(GuestLoad<u64>(cue + kContext), stage, index, src, 4);
                } else {
                    u8 v[16]; // read whole, then written (the original's vmovups pair)
                    std::memcpy(v, reinterpret_cast<const void*>(src), 16);
                    std::memcpy(reinterpret_cast<void*>(shadow + static_cast<u64>(index) * 4), v, 16);
                }
            }
            src += 16;
            ++slot;
        } while (--count != 0);
    }
    *GuestPtr<u8>(cue + kChanged + st) = 1;
    return cue;
}

bool HandWritten(CSB::Fn fn) {
    return fn == &CSB::NativeVertexBuffers || fn == &CSB::NativeBuffers;
}

// --- shadow comparison ---------------------------------------------------------------------------------------------

/// The CUE byte ranges one call reads or writes: the stage's shadow window, the tail, entries beyond the window.
struct Plan {
    u32 n = 0;
    u64 lo[2 + CSB::kMaxSlots];
    u64 hi[2 + CSB::kMaxSlots];
    void Add(u64 a, u64 b) {
        lo[n] = a;
        hi[n] = b;
        ++n;
    }
};

/// False when the call cannot be shadowed (stage outside 0..7, slots beyond the 64 entries): the original runs.
template <u32 kTable>
bool MakePlan(u64 cue, u32 stage, u32 slot, u32 count, Plan& p) {
    const s32 st = static_cast<s32>(stage);
    if (st < 0 || st > static_cast<s32>(kMaxStage) || count > CSB::kMaxSlots || slot > CSB::kMaxSlots ||
        slot + count > CSB::kMaxSlots) {
        return false;
    }
    const u64 so = ShadowOffset(stage);
    p.n = 0;
    p.Add(so, so + kWindow);
    p.Add(kTailBegin, kTailEnd);
    if (count != 0) {
        const u64 table = GuestLoad<u64>(cue + kTablePointers + static_cast<u64>(st) * 8);
        for (u32 i = 0; i < count; ++i) {
            const u32 entry = GuestLoad<u16>(table + kTable + static_cast<u64>(slot + i) * 2);
            if (entry != 0xffff && (entry & 0x8000) == 0) {
                const u64 d = so + static_cast<u64>(entry & 0x3fff) * 4;
                if (d + 16 > so + kWindow) {
                    p.Add(d, d + 16);
                }
            }
        }
    }
    return true;
}

struct ShadowSet {
    u8* a;
    u8* b;
    CSB::Recording ra;
    CSB::Recording rb;
};
thread_local ShadowSet* t_set = nullptr;

ShadowSet& Shadows() {
    if (t_set == nullptr) { // per verifying thread, never freed (a handful of render threads)
        t_set = new ShadowSet{new u8[kShadowBytes](), new u8[kShadowBytes](), {}, {}};
    }
    return *t_set;
}

bool SameRecordings(const CSB::Recording& a, const CSB::Recording& b) {
    return !a.overflow && !b.overflow && a.calls == b.calls &&
           std::memcmp(a.call, b.call, sizeof(CSB::Call) * a.calls) == 0;
}

/// Original (recording clone) on copy A, the implementation on copy B, both with their calls recorded.
bool Compare(CSB::Kind kind, const Plan& p, u64 cue, u32 stage, u32 slot, u32 count, u64 src, ShadowSet& s) {
    for (u32 i = 0; i < p.n; ++i) {
        std::memcpy(s.a + p.lo[i], reinterpret_cast<const void*>(cue + p.lo[i]), p.hi[i] - p.lo[i]);
        std::memcpy(s.b + p.lo[i], s.a + p.lo[i], p.hi[i] - p.lo[i]);
    }
    const u64 a = reinterpret_cast<u64>(s.a), b = reinterpret_cast<u64>(s.b);
    const u64 ra = CSB::RunRecorded(kind, g_recording[kind], true, a, stage, slot, count, src, s.ra);
    const u64 rb = CSB::RunRecorded(kind, g_impl[kind], false, b, stage, slot, count, src, s.rb);
    if (ra - a != rb - b || !SameRecordings(s.ra, s.rb)) {
        return false;
    }
    for (u32 i = 0; i < p.n; ++i) {
        if (std::memcmp(s.a + p.lo[i], s.b + p.lo[i], p.hi[i] - p.lo[i]) != 0) {
            return false;
        }
    }
    return true;
}

/// The authoritative call's result against the shadow original's: another thread changed the CUE in between.
bool LiveDiff(const Plan& p, u64 cue, const ShadowSet& s) {
    for (u32 i = 0; i < p.n; ++i) {
        if (std::memcmp(s.a + p.lo[i], reinterpret_cast<const void*>(cue + p.lo[i]), p.hi[i] - p.lo[i]) != 0) {
            return true;
        }
    }
    return false;
}

/// A mismatch case: everything the call's result depends on (the descriptors overwrite whole 16-byte slots, the
/// changed byte is set unconditionally), replayable against the eboot (Replay).
struct Case {
    u32 kind;
    u32 stage;
    u32 slot;
    u32 count;
    u64 cue;
    u64 context;
    u16 entries[CSB::kMaxSlots]; ///< table entries of API slots 0..63 (those of slot..slot+count-1 are read)
    u8 descriptors[CSB::kMaxSlots][16];
};
static_assert(sizeof(Case) <= kCaseBytes);

void RecordMismatch(u32 index, CSB::Kind kind, u64 cue, u32 stage, u32 slot, u32 count, u64 src) {
    Case c{};
    c.kind = kind;
    c.stage = stage;
    c.slot = slot;
    c.count = count;
    c.cue = cue;
    c.context = GuestLoad<u64>(cue + kContext);
    const u64 table = count != 0 ? GuestLoad<u64>(cue + kTablePointers + static_cast<u64>(stage) * 8) : 0;
    for (u32 i = 0; i < count; ++i) {
        c.entries[slot + i] = GuestLoad<u16>(table + CSB::kTableOffset[kind] + static_cast<u64>(slot + i) * 2);
        std::memcpy(c.descriptors[i], reinterpret_cast<const void*>(src + 16 * i), 16);
    }
    RecordCase(index, {reinterpret_cast<const u8*>(&c), sizeof(c)});
}

/// Generated or replayed inputs on whole shadow buffers (stray writes anywhere in them are seen).
bool CompareWhole(CSB::Kind kind, const Case& c, std::vector<u8>& a, std::vector<u8>& b, std::string& report) {
    std::vector<u8> table(CSB::kTableOffset[kind] + 2 * CSB::kMaxSlots + 16, 0);
    std::memcpy(table.data() + CSB::kTableOffset[kind], c.entries, sizeof(c.entries));
    const u64 table_address = reinterpret_cast<u64>(table.data());
    std::memcpy(a.data() + kTablePointers + static_cast<u64>(c.stage) * 8, &table_address, 8);
    std::memcpy(a.data() + kContext, &c.context, 8);
    b = a;
    const u64 src = reinterpret_cast<u64>(c.descriptors[0]);
    CSB::Recording ra, rb;
    const u64 pa = reinterpret_cast<u64>(a.data()), pb = reinterpret_cast<u64>(b.data());
    const u64 ret_a = CSB::RunRecorded(kind, g_recording[kind], true, pa, c.stage, c.slot, c.count, src, ra);
    const u64 ret_b = CSB::RunRecorded(kind, g_impl[kind], false, pb, c.stage, c.slot, c.count, src, rb);
    const bool same_mem = a == b, same_calls = SameRecordings(ra, rb), same_ret = ret_a - pa == ret_b - pb;
    if (!same_mem || !same_calls || !same_ret) {
        u64 at = 0;
        while (at < a.size() && a[at] == b[at]) {
            ++at;
        }
        report = fmt::format("stage {} slot {} count {}: memory {} (first difference +{:#x}), calls {} ({} / {}), "
                             "return {}",
                             c.stage, c.slot, c.count, same_mem ? "same" : "DIFFERENT", at,
                             same_calls ? "same" : "DIFFERENT", ra.calls, rb.calls, same_ret ? "same" : "DIFFERENT");
        return false;
    }
    return true;
}

template <CSB::Kind K>
struct Impl {
    static Slot slot;

    static CSB::Fn Original() {
        return reinterpret_cast<CSB::Fn>(slot.original);
    }

    static bool Check(const Plan& p, u64 cue, u32 stage, u32 s, u32 count, u64 src) {
        ShadowSet& set = Shadows();
        const bool same = Compare(K, p, cue, stage, s, count, src, set);
        Count(slot.index, kCallsChecked);
        slot.verified.fetch_add(1, std::memory_order_relaxed);
        if (!same) {
            slot.mismatches.fetch_add(1, std::memory_order_relaxed);
            RecordMismatch(slot.index, K, cue, stage, s, count, src);
        }
        return same;
    }

    static SSPC_SYSV_ABI u64 Native(u64 cue, u32 stage, u32 s, u32 count, u64 src) { // counted by the dispatcher
        return g_impl[K](cue, stage, s, count, src);
    }

    static SSPC_SYSV_ABI u64 Verify(u64 cue, u32 stage, u32 s, u32 count, u64 src) {
        const CSB::Fn original = Original();
        Plan p;
        if (!MakePlan<CSB::kTableOffset[K]>(cue, stage, s, count, p)) {
            slot.skipped.fetch_add(1, std::memory_order_relaxed);
            Count(slot.index, kCallsOriginal);
            return original(cue, stage, s, count, src);
        }
        Check(p, cue, stage, s, count, src);
        const u64 r = original(cue, stage, s, count, src); // authoritative
        if (LiveDiff(p, cue, Shadows())) {
            slot.live_diff.fetch_add(1, std::memory_order_relaxed);
        }
        return r;
    }

    static SSPC_SYSV_ABI u64 Guarded(u64 cue, u32 stage, u32 s, u32 count, u64 src) {
        Plan p;
        if (SampleThisCall(slot.index, slot.sample_period) && MakePlan<CSB::kTableOffset[K]>(cue, stage, s, count, p) &&
            !Check(p, cue, stage, s, count, src)) {
            Fallback(slot); // the original from now on, on every thread
            Count(slot.index, kCallsOriginal);
            return Original()(cue, stage, s, count, src);
        }
        Count(slot.index, kCallsNative);
        return g_impl[K](cue, stage, s, count, src);
    }

    static SSPC_SYSV_ABI u64 Bench(u64 cue, u32 stage, u32 s, u32 count, u64 src) {
        const CSB::Fn original = Original();
        Plan p;
        if (SampleThisCall(slot.index, slot.sample_period) && MakePlan<CSB::kTableOffset[K]>(cue, stage, s, count, p)) {
            ShadowSet& set = Shadows();
            for (u32 i = 0; i < p.n; ++i) {
                std::memcpy(set.a + p.lo[i], reinterpret_cast<const void*>(cue + p.lo[i]), p.hi[i] - p.lo[i]);
                std::memcpy(set.b + p.lo[i], set.a + p.lo[i], p.hi[i] - p.lo[i]);
            }
            const u64 e0 = TimerTicks();
            const u64 e1 = TimerTicks();
            u64 to = 0, tn = 0;
            const bool original_first = (slot.bench_samples.load(std::memory_order_relaxed) & 1) == 0;
            for (int k = 0; k < 2; ++k) {
                if ((k == 0) == original_first) {
                    const u64 t0 = TimerTicks();
                    CSB::RunRecorded(K, g_recording[K], true, reinterpret_cast<u64>(set.a), stage, s, count, src,
                                     set.ra);
                    to = TimerTicks() - t0;
                } else {
                    const u64 t0 = TimerTicks();
                    CSB::RunRecorded(K, g_impl[K], false, reinterpret_cast<u64>(set.b), stage, s, count, src, set.rb);
                    tn = TimerTicks() - t0;
                }
            }
            slot.bench_samples.fetch_add(1, std::memory_order_relaxed);
            slot.bench_original_ticks.fetch_add(to, std::memory_order_relaxed);
            slot.bench_native_ticks.fetch_add(tn, std::memory_order_relaxed);
            slot.bench_empty_ticks.fetch_add(e1 - e0, std::memory_order_relaxed);
        }
        Count(slot.index, kCallsOriginal);
        return original(cue, stage, s, count, src);
    }
};
template <CSB::Kind K>
Slot Impl<K>::slot;

using D = Dispatch<u64(u64, u32, u32, u32, u64)>;

// --- install: the recording clone, then generated inputs bit-exact against it ---------------------------------------

template <CSB::Kind K>
bool PrepareKind(u64 module_base, std::string& error) {
    return CSB::Prepare(K, module_base, error);
}

/// `original` is not run: its call would reach the real command buffer with generated arguments. The recording
/// clone (the same bytes, its one call redirected) is.
template <CSB::Kind K>
bool SelfCheck(void* /*original*/, std::string& error) {
    if (g_recording[K] == nullptr) {
        error = "no recording clone (prepare did not run)";
        return false;
    }
    std::mt19937_64 rng{0x53504e4500000010ull + K};
    std::vector<u8> a(kShadowBytes), b(kShadowBytes);
    for (u32 n = 0; n < 1000; ++n) {
        for (u8& x : a) {
            x = static_cast<u8>(rng());
        }
        Case c{};
        c.kind = K;
        c.stage = static_cast<u32>(rng() % (kMaxStage + 1));
        c.count = n % 10 == 0 ? 0 : static_cast<u32>(1 + rng() % 16);
        c.slot = static_cast<u32>(rng() % (CSB::kMaxSlots - c.count + 1));
        c.context = rng();
        for (u16& e : c.entries) {
            const u32 pick = static_cast<u32>(rng() % 8);
            e = pick == 0   ? u16{0xffff}
                : pick == 1 ? static_cast<u16>(0x8000 | (rng() & 0x7fff))   // direct: setUserDataRegion
                : pick == 2 ? static_cast<u16>(rng() & 0x7fff)              // anywhere the 14 bits reach
                            : static_cast<u16>((rng() % 0x5fc) & ~3ull);    // inside the stage's window
        }
        for (auto& d : c.descriptors) {
            for (u8& x : d) {
                x = static_cast<u8>(rng());
            }
        }
        if (!CompareWhole(K, c, a, b, error)) {
            error = fmt::format("case {}: {}", n, error);
            return false;
        }
    }
    return true;
}

} // namespace

SSPC_SYSV_ABI u64 CSB::NativeVertexBuffers(u64 cue, u32 stage, u32 slot, u32 count, u64 src) {
    return Run<kTableOffset[kVertexBuffers]>(cue, stage, slot, count, src);
}

SSPC_SYSV_ABI u64 CSB::NativeBuffers(u64 cue, u32 stage, u32 slot, u32 count, u64 src) {
    return Run<kTableOffset[kBuffers]>(cue, stage, slot, count, src);
}

SSPC_SYSV_ABI u64 CSB::Record(u64 context, u64 stage, u64 sgpr, u64 values, u64 count) {
    Recording* r = t_recording;
    if (r == nullptr) {
        return 0;
    }
    if (r->calls >= kMaxCalls) {
        r->overflow = true;
        return 0;
    }
    Call& c = r->call[r->calls++];
    c = Call{context, stage, sgpr, values, count, {}};
    std::memcpy(c.data, reinterpret_cast<const void*>(values), 16);
    return 0;
}

u64 CSB::RunRecorded(Kind kind, Fn fn, bool recording_original, u64 cue, u32 stage, u32 slot, u32 count, u64 src,
                     Recording& out) {
    out.calls = 0;
    out.overflow = false;
    Recording* const saved = t_recording;
    t_recording = &out;
    u64 r;
    if (recording_original) {
        r = fn(cue, stage, slot, count, src);
    } else if (HandWritten(fn)) {
        t_callee = &Record;
        r = fn(cue, stage, slot, count, src);
        t_callee = nullptr;
    } else if (g_redirect[kind] != nullptr) {
        g_redirect[kind](g_callee.load(std::memory_order_relaxed), reinterpret_cast<u64>(&Record));
        r = fn(cue, stage, slot, count, src);
        g_redirect[kind](0, 0);
    } else {
        out.overflow = true; // a foreign implementation without a redirection: its calls cannot be recorded
        r = 0;
    }
    t_recording = saved;
    return r;
}

CSB::Fn CSB::RecordingOriginal(Kind kind) {
    return g_recording[kind];
}

u64 CSB::CalleeAddress() {
    return g_callee.load(std::memory_order_relaxed);
}

bool CSB::Prepare(Kind kind, u64 module_base, std::string& error) {
    const u64 callee = module_base + kCalleeRva;
    g_callee.store(callee, std::memory_order_relaxed);
    const u64 entry = module_base + kFunction[kind].rva;
    const CloneResult clone = CloneGuestFunction(entry, reinterpret_cast<const u8*>(entry), kFunction[kind].size, error,
                                                 {{callee, reinterpret_cast<u64>(&Record)}});
    if (clone.code == nullptr) {
        error = "recording clone: " + error;
        return false;
    }
    g_recording[kind] = reinterpret_cast<Fn>(clone.code);
    return true;
}

bool CSB::Replay(Kind kind, const std::string& bytes, std::string& report) {
    if (bytes.size() != sizeof(Case)) {
        report = fmt::format("case of {} bytes, expected {}", bytes.size(), sizeof(Case));
        return false;
    }
    if (g_recording[kind] == nullptr) {
        report = "no recording clone (Prepare)";
        return false;
    }
    Case c;
    std::memcpy(&c, bytes.data(), sizeof(c));
    std::vector<u8> a(kShadowBytes, 0), b;
    if (!CompareWhole(kind, c, a, b, report)) {
        return false;
    }
    report = fmt::format("stage {} slot {} count {}: same", c.stage, c.slot, c.count);
    return true;
}

void CSB::UseImplementation(Kind kind, Fn fn, const char* source, RedirectFn redirect) {
    g_impl[kind] = fn;
    g_impl_source[kind] = source;
    g_redirect[kind] = redirect;
}

CSB::Fn CSB::Implementation(Kind kind) {
    return g_impl[kind];
}

const char* CSB::ImplementationSource(Kind kind) {
    return g_impl_source[kind];
}

const FunctionSpec kCueSetVertexBuffersSpec{
    "cue_set_vertex_buffers",
    "CUE::SetVertexBuffers",
    kFunction[CSB::kVertexBuffers],
    nullptr,
    0,
    nullptr,
    0,
    reinterpret_cast<const void*>(&D::Entry<Impl<CSB::kVertexBuffers>>),
    &Impl<CSB::kVertexBuffers>::slot,
    &SelfCheck<CSB::kVertexBuffers>,
    &PrepareKind<CSB::kVertexBuffers>,
};

const FunctionSpec kCueSetBuffersSpec{
    "cue_set_buffers",
    "CUE::SetBuffers",
    kFunction[CSB::kBuffers],
    nullptr,
    0,
    nullptr,
    0,
    reinterpret_cast<const void*>(&D::Entry<Impl<CSB::kBuffers>>),
    &Impl<CSB::kBuffers>::slot,
    &SelfCheck<CSB::kBuffers>,
    &PrepareKind<CSB::kBuffers>,
};

} // namespace SecondSon::NativeEngine::Reconstructed
