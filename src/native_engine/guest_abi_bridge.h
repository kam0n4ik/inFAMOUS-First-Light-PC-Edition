// SecondSonPC — the boundary between the game's code and a native reconstruction of one of its functions
// (Native Engine Reconstruction; docs/NATIVE_ENGINE_RECONSTRUCTION.md §2).
//
// What the boundary is (checked in the code of the loader and the runtime, not assumed):
// - The eboot's x86-64 code runs on the host CPU as it is, at the address the loader gave it (0x400000 + offset with
//   the SecondSonPC loader). Guest address = host address (memory/host_address_space.h, H1): a guest pointer is
//   dereferenced directly, no translation.
// - The guest calls with the System V AMD64 convention; SecondSonPC is a Windows x64 program. clang-cl compiles a
//   host function with `SSPC_SYSV_ABI` (base/abi.h) to the System V convention: arguments in RDI, RSI, RDX, RCX, R8,
//   R9, XMM0-7, then the stack; RBX, RBP, R12-R15 preserved; RSI/RDI and XMM6-15 are NOT preserved for the caller
//   (they are under Windows x64: the compiler saves them itself around any Windows call made inside). So the jump
//   that replaces a guest function lands in an ordinary C++ function: no assembly adapter is needed, and the
//   ABI tests (tools/native_engine_tests) check it on every build instead of trusting it.
// - Stack: the reconstruction runs on the guest thread's own stack (some engine threads have 16 KiB: Trophy,
//   GameState). A reconstruction keeps its frame small (kMaxFrameBytes), never logs or formats on the calling
//   thread (the registry's reporter thread does), and does not throw: there are no unwind tables that cross guest
//   frames.
// - TLS: guest threads are host threads; FS-relative guest accesses are rewritten by the CPU patcher, host TLS (GS)
//   works. Reconstructions keep per-thread counters in host thread_local storage.
// - Floating point: MXCSR and the x87 control word are the guest thread's; host code neither reads nor changes
//   them, so a reconstruction sees the same rounding and flush-to-zero/denormals-are-zero mode as the original.
//   Reconstructions with floating point are compiled without contraction (no FMA the original does not have):
//   see the per-file `#pragma clang fp contract(off)`.
#pragma once

#include <cstring>

#include "base/abi.h"

namespace SecondSon::NativeEngine {

/// The most a reconstruction may put on the guest thread's stack (the smallest engine thread stack is 16 KiB).
inline constexpr u32 kMaxFrameBytes = 1024;

/// A guest pointer (guest address = host address).
template <typename T>
inline T* GuestPtr(u64 address) {
    return reinterpret_cast<T*>(address);
}

/// An unaligned read of guest memory (the original may read any alignment).
template <typename T>
inline T GuestLoad(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

} // namespace SecondSon::NativeEngine
