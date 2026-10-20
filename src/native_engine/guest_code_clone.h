// SecondSonPC — a callable copy of a guest function as the eboot has it (Native Engine Reconstruction).
//
// Replacing a function overwrites its first 14 bytes with a jump (engine/guest_hooks.h). The verification and the
// fallback still need the original: an entry probe's trampoline (engine/guest_probe.h) only works when the moved
// prologue has no branch and no RIP-relative operand. A clone works for any self-contained body: the whole function
// is copied into executable memory SecondSonPC owns and every relative operand (branch, call, RIP-relative memory)
// that points outside the body is re-aimed at the same absolute target; targets inside the body move with it.
// Indirect jumps (jump tables) land in the original body, which is intact after its first 14 bytes, so they still
// run the original code. A rel8 branch out of the body, or a target beyond ±2 GiB of the copy, refuses the clone.
#pragma once

#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::NativeEngine {

struct CloneResult {
    void* code = nullptr; ///< the copy (execute-read), nullptr on failure
    u32 relocated = 0;    ///< relative operands re-aimed outside the body
    u32 instructions = 0;
};

/// A relative operand of the body that targets `from` (a callee) is aimed at `to` instead, through a 14-byte
/// `jmp [rip+0]` thunk placed after the copy (no register changes: `to` sees the call's own registers and stack).
/// Verification uses it to run an original whose calls are recorded instead of made.
struct CodeRedirect {
    u64 from;
    u64 to;
};

/// Copies [address, address + size) - the code bytes `source` (normally the same address, or the pristine bytes
/// before the CPU patcher) - into executable memory within ±2 GiB of the body when it has outside references.
/// Decodes every instruction (Zydis); stops with `error` on an undecodable byte, a body that does not end on an
/// instruction boundary, or a relative operand that cannot be re-aimed. Every `redirects` entry must be used.
CloneResult CloneGuestFunction(u64 address, const u8* source, u32 size, std::string& error,
                               const std::vector<CodeRedirect>& redirects = {});

/// Where a clone with outside references goes first: memory within rel32 reach of `address` that stays read/write/
/// execute and is never freed (the game: Cpu::AllocateModuleCode, the module's trampoline area), 0 when there is
/// none. Without it, or when it returns 0, a free region within ±2 GiB is searched (the tests).
using NearCodeAllocator = u64 (*)(u64 address, u64 bytes);
void SetNearCodeAllocator(NearCodeAllocator allocator);

/// The relocation itself, for tests: re-aims the relative operands of `code` (decoded as if at `from`) for `to`;
/// an outside target equal to a `retarget` entry's `from` goes to its `to` (counted in `retargeted`).
bool RelocateCode(std::vector<u8>& code, u64 from, u64 to, u32& relocated, u32& instructions, std::string& error,
                  const std::vector<CodeRedirect>& retarget = {}, u32* retargeted = nullptr);

} // namespace SecondSon::NativeEngine
