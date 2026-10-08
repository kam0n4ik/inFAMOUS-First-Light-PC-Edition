// SecondSonPC — import call counter.
// Every function the game can bind to is reached through a thunk:
//     lock inc qword [rip+counter] ; jmp qword [rip+target]                      (16 bytes)
// With call-order recording on, the thunk first appends its index to a fixed buffer (the first N
// import calls of the session, in order) using only R10/R11:
//     cmp [cursor], N ; jae skip ; mov r11d, 1 ; lock xadd [cursor], r11 ; cmp r11, N ; jae skip
//     lea r10, [order] ; mov dword [r10 + r11*4], index ; skip: <counter thunk>     (96 bytes)
// RAX/AL (varargs), argument registers and the stack are not touched, so the SysV ABI is
// preserved (R10/R11 are scratch registers that carry no arguments).
#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Trace {

enum class Owner : u8 {
    Native,     // SecondSonPC implementation
    Observed,   // SecondSonPC observer in front of a BRIDGE implementation
    Bridge,     // temporary shadPS4 implementation (Phase 0-4)
    CommonStub, // explicit SecondSonPC stub (returns 0)
};

/// Must run after the guest address space is reserved (thunks are ordinary host allocations).
/// Records the first N import calls in order if SetCallOrderCapacity(N) was called before.
bool Init(size_t capacity);
/// Call-order capacity for the next Init() (0 = off, the default). Set from the command line.
void SetCallOrderCapacity(size_t call_order);

/// Returns the thunk to bind instead of `target`, or nullptr if not initialized/full/sealed.
void* Wrap(std::string_view nid, std::string_view library, std::string_view module,
           std::string_view name, Owner owner, void* target);

/// Makes the thunk pages executable (and read-only). Call once, before guest code runs.
void Seal();

/// The function a thunk jumps to, or nullptr if `address` is not a thunk.
const void* ThunkTarget(u64 address);

size_t WrappedCount();
size_t WrappedCount(Owner owner);

/// Writes {"entries":[...called functions...]} as JSON (atomic replace).
void DumpJson(const std::filesystem::path& file);
/// Writes the recorded call order (one "index<TAB>name<TAB>library<TAB>owner" line per call).
void DumpCallOrder(const std::filesystem::path& file);
void StartPeriodicDump(const std::filesystem::path& file, u32 seconds);
void FinalDump();

} // namespace SecondSon::Trace
