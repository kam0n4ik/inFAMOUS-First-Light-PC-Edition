// SecondSonPC — import call counter.
// Every function the game can bind to is reached through a 16-byte thunk:
//     lock inc qword [rip+counter] ; jmp qword [rip+target]
// No register (incl. RAX/AL for varargs) or stack slot is touched, so the SysV ABI is preserved.
#pragma once

#include <cstddef>
#include <filesystem>
#include <string_view>

#include "base/abi.h"

namespace SecondSon::Trace {

enum class Owner : u8 {
    Native,   // SecondSonPC implementation
    Observed, // SecondSonPC observer in front of a BRIDGE implementation
    Bridge,   // temporary shadPS4 implementation (Phase 0-4)
};

/// Must run after the guest address space is reserved (thunks are ordinary host allocations).
bool Init(size_t capacity);

/// Returns the thunk to bind instead of `target`, or nullptr if not initialized/full/sealed.
void* Wrap(std::string_view nid, std::string_view library, std::string_view module,
           std::string_view name, Owner owner, void* target);

/// Makes the thunk pages executable (and read-only). Call once, before guest code runs.
void Seal();

size_t WrappedCount();
size_t WrappedCount(Owner owner);

/// Writes {"entries":[...called functions...]} as JSON (atomic replace).
void DumpJson(const std::filesystem::path& file);
void StartPeriodicDump(const std::filesystem::path& file, u32 seconds);
void FinalDump();

} // namespace SecondSon::Trace
