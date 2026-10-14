// SecondSonPC — replacing functions of the game's own code with native ones (Native Engine Reconstruction,
// docs/native-reconstruction/NATIVE_MIGRATION_ROADMAP.md).
//
// The eboot's x86-64 code runs on the host CPU as it is; a reconstructed engine function replaces the guest one by
// a jump at the guest function's entry to a host function with the System V ABI (SSPC_SYSV_ABI), so every caller
// of the guest function reaches the native code unchanged. A replacement is installed only when the loaded bytes of
// the function, and of every function its reconstruction assumes the behaviour of, hash to the values measured on
// the eboot the reconstruction was made from (another game version or patched code: nothing is installed).
#pragma once

#include <string>

#include "base/abi.h"

namespace SecondSon::Engine {

/// A function of the eboot: module-relative address, byte size (entry to the last byte of its body) and FNV-1a 64
/// of those bytes in eboot.bin (tools/native-reconstruction/function_hashes.py).
struct GuestFunction {
    u64 rva;
    u32 size;
    u64 fnv1a64;
};

u64 HashBytes(const void* data, size_t size);

/// The eboot's mapped range (set by the loader once the eboot is loaded; 0/0 before): diagnostics that attribute
/// addresses to the game's code (the CPU sampler's raw mode) read it.
void SetGuestImage(u64 base, u64 size);
u64 GuestImageBase();
u64 GuestImageEnd();

/// True when the loaded function at base + f.rva hashes to f.fnv1a64; otherwise `error` says which.
bool VerifyGuestFunction(u64 module_base, const GuestFunction& f, std::string& error);

/// Overwrites the first 14 bytes at `at` with `jmp qword [rip+0]; dq target`. Call before any thread can run the
/// code (the loader's window between loading the eboot and starting it). The function must be at least 14 bytes and
/// have no branch target inside them (checked while reconstructing; see JOB_SYSTEM_RECONSTRUCTION.md).
bool InstallJump(u64 at, const void* target, std::string& error);

} // namespace SecondSon::Engine
