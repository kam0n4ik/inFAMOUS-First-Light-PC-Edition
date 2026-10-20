// SecondSonPC — the registry of reconstructed engine functions (Native Engine Reconstruction;
// docs/NATIVE_ENGINE_RECONSTRUCTION.md, the list of functions and their status: docs/NATIVE_ENGINE_FUNCTIONS.md).
//
// A reconstructed function is SecondSonPC's own C++ implementation of one function of the game's code, executed
// INSTEAD of the original: the first 14 bytes of the guest function become a jump (engine/guest_hooks.h) to the
// function's dispatcher (native_dispatch.h), so every caller reaches native code with nothing else changed. The ELF
// on disk is not touched; the replacement exists only in the process's memory.
//
// Installation (the loader's window, before any guest thread runs) for each selected function:
//   1. the loaded body hashes to the value measured on the eboot the reconstruction was made from (another game
//      version, a patch or another hook already on it: not installed), and so does every function and data range
//      the reconstruction assumes (constants it embeds, callees whose behaviour it relies on);
//   2. the original is cloned (guest_code_clone.h): it stays callable for verification and fallback;
//   3. a self-check runs the clone and the reconstruction on generated inputs and compares them bit for bit (a
//      mismatch: not installed);
//   4. the jump goes in.
// Any failure leaves that function as the game has it and is logged; nothing else changes.
//
// Modes (SSPC_NATIVE_ENGINE; per function with SSPC_NATIVE_ENGINE_FUNCS=key[:mode],...):
//   off (default)  nothing is installed: the game runs exactly as before;
//   1 / native     the reconstruction runs instead of the original;
//   verify         the original stays authoritative; both run on isolated shadow copies of everything the function
//                  writes and their results are compared bit for bit (no double side effects in game memory);
//   guarded        the reconstruction is authoritative; every Nth call is also verified on shadow copies, and a
//                  mismatch switches the function back to the original for the rest of the run (fallback);
//   bench          the original stays authoritative; every Nth call times both on shadow copies.
// The mode is a live value per function: Fallback() switches a function back to its original at any time.
#pragma once

#include <atomic>
#include <filesystem>
#include <string>

#include "base/abi.h"
#include "engine/guest_hooks.h"

namespace SecondSon::NativeEngine {

enum class Mode : u8 { Original = 0, Native = 1, Verify = 2, Guarded = 3, Bench = 4 };
const char* ModeName(Mode mode);
bool ParseMode(const std::string& text, Mode& mode);

/// A data range of the eboot a reconstruction embeds (constants the original reads from memory).
struct GuestData {
    u64 rva;
    u32 size;
    u64 fnv1a64;
};

/// The live state of one registered function (one per reconstruction, static storage).
struct Slot {
    std::atomic<u8> mode{static_cast<u8>(Mode::Original)};
    void* original = nullptr; ///< the clone of the guest function (System V)
    u32 index = 0;            ///< in the registry
    u32 sample_period = 256;  ///< guarded/bench: every Nth call per thread
    std::atomic<u64> verified{0};
    std::atomic<u64> mismatches{0};
    std::atomic<u64> skipped{0};    ///< verify could not shadow this call (aliasing, unknown state): ran the original
    std::atomic<u64> live_diff{0};  ///< verify: the authoritative call's result differed from the shadow run's
                                    ///< (another thread changed the state in between - expected for shared state)
    std::atomic<u64> fallbacks{0};
    std::atomic<u64> bench_samples{0};
    std::atomic<u64> bench_original_ticks{0};
    std::atomic<u64> bench_native_ticks{0};
    std::atomic<u64> bench_empty_ticks{0}; ///< the timer's own cost per sample (an empty timed region)
};

/// A self-check run at install with the clone of the loaded original: false (with `error`) refuses the install.
using SelfCheckFn = bool (*)(void* original, std::string& error);
/// Run at install before the self-check, with the module the function is in (its code still pristine): what the
/// reconstruction takes from the loaded module (callee addresses, a clone whose calls are recorded); false refuses.
using PrepareFn = bool (*)(u64 module_base, std::string& error);

struct FunctionSpec {
    const char* key;  ///< switch name (SSPC_NATIVE_ENGINE_FUNCS)
    const char* name; ///< our name for it (no symbols in the eboot)
    Engine::GuestFunction function;
    const Engine::GuestFunction* assumed;
    u32 assumed_count;
    const GuestData* data;
    u32 data_count;
    const void* entry; ///< the dispatcher (System V) the jump goes to
    Slot* slot;
    SelfCheckFn self_check;
    PrepareFn prepare = nullptr;
};

/// Every reconstruction SecondSonPC has (registry.cpp lists them).
u32 FunctionCount();
const FunctionSpec& Function(u32 index);

/// Hot-path call counters, per thread (no shared cache line on the hot path); summed by the reporter.
enum Counter : u32 { kCallsNative = 0, kCallsOriginal = 1, kCallsChecked = 2, kCounters = 3 };
inline constexpr u32 kMaxFunctions = 16;

struct ThreadCounters {
    std::atomic<u64> counters[kMaxFunctions][kCounters]{};
    u32 sample[kMaxFunctions]{};
    bool shared = false; ///< the overflow block shared by threads beyond the registry's limit (atomic increments)
};
/// The calling thread's block, registered on its first counted call (a small heap block, never freed).
ThreadCounters* RegisterThreadCounters();
inline thread_local ThreadCounters* t_counters = nullptr;

inline void Count(u32 function, Counter counter) {
    ThreadCounters* b = t_counters != nullptr ? t_counters : RegisterThreadCounters();
    std::atomic<u64>& c = b->counters[function][counter];
    if (!b->shared) {
        c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // owner thread only: no lock
    } else {
        c.fetch_add(1, std::memory_order_relaxed);
    }
}
u64 Total(u32 function, Counter counter);

/// Switches a function back to its original for the rest of the run (any thread; takes effect at its next call).
void Fallback(Slot& slot);

/// The loader's window before the guest runs: installs what SSPC_NATIVE_ENGINE selects (off: nothing). Logs every
/// decision; writes reports to <logs_dir>/native_engine/. Returns false when a selected function was refused.
bool InstallFromEnvironment(u64 eboot_base, const std::filesystem::path& logs_dir);

/// For tests: installs one function over code at `module_base` with `mode` (the same steps as above).
bool InstallFunction(u64 module_base, const FunctionSpec& spec, Mode mode, std::string& error);

} // namespace SecondSon::NativeEngine
