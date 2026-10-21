// SecondSonPC — verification support for reconstructed functions (Native Engine Reconstruction).
//
// A reconstruction's Verify/Guarded/Bench paths never run two implementations against the game's real memory: each
// one copies everything the function writes (and reads back) into shadow buffers, runs the original clone on one
// copy and the reconstruction on another, and compares the copies and return values bit for bit. A mismatch becomes
// a case: the inputs and both outputs, appended without blocking to a fixed ring and written by the reporter thread
// to <logs>/native_engine/cases_<key>.txt, one hex line per case, which tools/native_engine_tests replays offline
// against the eboot's own code.
#pragma once

#include <span>
#include <string>

#include "base/abi.h"

namespace SecondSon::NativeEngine {

/// Serialised case: up to kCaseBytes of the function's own format (see each reconstruction's Case struct).
inline constexpr u32 kCaseBytes = 4096;

/// Cases kept per function (the first ones: later mismatches only count). R8: each function has its own slots - one
/// with many mismatches no longer drops the cases of the others.
inline constexpr u32 kCasesPerFunction = 8;

/// Records a mismatch case (lock-free; dropped when the function's slots are full). Any thread, no allocation, no
/// logging.
void RecordCase(u32 function, std::span<const u8> bytes);

/// Cases recorded and dropped so far (all functions).
u64 CasesRecorded();
u64 CasesDropped();
/// The same for one function (its registry index).
u64 CasesRecorded(u32 function);
u64 CasesDropped(u32 function);

/// The reporter: writes cases not yet written (hex, one per line, "<key> <hex>") to `dir`. Not reentrant.
void FlushCases(const std::string& dir);

/// rdtsc fenced on both sides (timing of one shadow run).
inline u64 TimerTicks() {
    u32 lo, hi;
    __asm__ volatile("lfence\n\trdtsc\n\tlfence" : "=a"(lo), "=d"(hi)::"memory");
    return (static_cast<u64>(hi) << 32) | lo;
}

/// Hex helpers for the case files.
std::string ToHex(std::span<const u8> bytes);
bool FromHex(const std::string& hex, std::string& out);

} // namespace SecondSon::NativeEngine
