// SecondSonPC — observing a function of the game's own code without replacing it (P5.3A, diagnostics;
// docs/native-reconstruction/RENDER_ENGINE_PASSES.md §1).
//
// guest_hooks.h replaces a guest function. An entry probe keeps it: the whole instructions that cover the first 14
// bytes of the function (its prologue) move to an executable trampoline followed by a jump back to the first
// instruction after them, and the function's entry jumps to a host function with the System V ABI. The host function
// sees the arguments, may read the guest's data, and calls the trampoline as the original function with the same
// arguments; every caller of the guest function reaches the host function unchanged.
//
// The moved bytes must be position-independent (no RIP-relative operand, no branch) and no branch of the program may
// target them; both are checked in Ghidra per site (tools/native-reconstruction/ghidra/queries/prologue.py) and pinned
// by the function's hash (the probe is installed only on the eboot the check was made on).
#pragma once

#include <string>

#include "engine/guest_hooks.h"

namespace SecondSon::Engine {

/// A probed function: its hash-pinned body and the length of the whole instructions moved to the trampoline (>= 14).
struct ProbeSite {
    GuestFunction function;
    u32 prologue;
};

/// Writes the trampoline for `entry` into `out` (prologue + 14 bytes): the `prologue` bytes at `entry`, then
/// `jmp qword [rip+0]; dq resume`. Returns the bytes written.
u32 BuildTrampoline(const u8* entry, u32 prologue, u64 resume, u8* out);

/// Bytes BuildTrampoline writes for a prologue of `prologue` bytes.
constexpr u32 TrampolineSize(u32 prologue) {
    return prologue + 14;
}

/// Verifies the function (guest_hooks.h), writes its trampoline into executable memory SecondSonPC owns and installs
/// the entry jump to `target`. Returns the trampoline (the original function), or nullptr with `error`. Call before any
/// thread can run the code, like InstallJump.
///
/// `original_slot`: the variable the probe calls the original through (the caller stores the returned trampoline in
/// it). Recorded with the site: a replacement of the same function installed later (Native Engine R5-D) goes in
/// below the probe by storing its own entry there instead of a second entry jump (FindEntryProbe).
void* InstallEntryProbe(u64 module_base, const ProbeSite& site, const void* target, std::string& error,
                        void** original_slot = nullptr);

/// An installed probe: the verified function, the entry bytes its jump replaced and the probe's original variable.
struct ProbeRecord {
    u64 entry = 0;
    GuestFunction function{};
    u32 prologue = 0;
    u8 replaced[32]{}; ///< the first `prologue` bytes as they were before the probe's jump (pristine, verified)
    void** original_slot = nullptr;
};

/// The probe installed at `entry` (absolute), nullptr when there is none.
const ProbeRecord* FindEntryProbe(u64 entry);

} // namespace SecondSon::Engine
