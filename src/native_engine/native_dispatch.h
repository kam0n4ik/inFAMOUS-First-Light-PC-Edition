// SecondSonPC — the entry a replaced guest function jumps to (Native Engine Reconstruction).
//
// One dispatcher per reconstruction, generated from its signature: a System V function that reads the function's
// live mode (one relaxed load) and runs the reconstruction, the original clone, or the reconstruction's
// verification/benchmark path. The whole path is System V (sysv -> sysv calls need no register spills); the
// reconstruction's Native() is normally inlined into it.
//
// A reconstruction is a struct Impl with
//   static Slot slot;
//   static SSPC_SYSV_ABI R Native(A...);   // the reconstruction
//   static SSPC_SYSV_ABI R Verify(A...);   // original authoritative + shadow comparison
//   static SSPC_SYSV_ABI R Guarded(A...);  // reconstruction authoritative + sampled shadow comparison + fallback
//   static SSPC_SYSV_ABI R Bench(A...);    // original authoritative + sampled shadow timing
#pragma once

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine {

template <typename Signature>
struct Dispatch;

template <typename R, typename... A>
struct Dispatch<R(A...)> {
    using Fn = SSPC_SYSV_ABI R (*)(A...);

    template <typename Impl>
    static SSPC_SYSV_ABI R Entry(A... a) {
        Slot& s = Impl::slot;
        switch (static_cast<Mode>(s.mode.load(std::memory_order_relaxed))) {
        case Mode::Native:
            Count(s.index, kCallsNative);
            return Impl::Native(a...);
        case Mode::Verify:
            return Impl::Verify(a...);
        case Mode::Guarded:
            return Impl::Guarded(a...);
        case Mode::Bench:
            return Impl::Bench(a...);
        case Mode::Original:
        default:
            Count(s.index, kCallsOriginal);
            return reinterpret_cast<Fn>(s.original)(a...);
        }
    }

    static R CallOriginal(const Slot& s, A... a) {
        return reinterpret_cast<Fn>(s.original)(a...);
    }
};

/// Guarded/bench sampling: true on every `period`-th call of this thread (per function).
bool SampleThisCall(u32 function, u32 period);

} // namespace SecondSon::NativeEngine
