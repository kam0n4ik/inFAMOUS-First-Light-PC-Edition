// SecondSonPC R6-P - the command processor's local optimizations of phase R6 (docs/NATIVE_RENDER_R6_CP_OPTIMIZATION.md).
//
// Every switch is off by default and read once (the first Opt() call); each is its own variable, like the CITY-OPT
// ones. The counters are written by one thread each (single writer: load + store, no locked add) and read by the
// report at any time.
#pragma once

#include <array>
#include <atomic>
#include <string>

#include "base/abi.h"

namespace SecondSon::Gpu::Native::R6 {

struct Options {
    /// SSPC_R6_CP_PREFETCH=skip: Gnm's L2 prefetches (DMA_DATA into CP_DMA_READ_TAGS - Cp::DmaKind::ReadTagsPrefetch,
    /// no memory is written) are not handed to the recording thread. Before, each was a record of the parallel queue: it
    /// closed a held engine run, moved the frontend's memory generation (the stage memo, the geometry template, the
    /// recording thread's streamed-copy memo) and cost both threads a record (thread 2: Executor::Dma - a count and a
    /// memo bump that is off while the frontend runs on thread 1).
    bool prefetch_skip = false;
    /// SSPC_R6_CP_DIAG=1: counters only (the records that closed a held run, by kind; DMA reports by kind).
    bool diag = false;
};
const Options& Opt();

/// Record kinds as ParallelCp's (parallel_cp.h Kind), for the counters; 14: a tagged draw that could not join.
inline constexpr u32 kKinds = 16;
inline constexpr u32 kKindTaggedRefused = 14;
/// DMA kinds as Cp::DmaKind.
inline constexpr u32 kDmaKinds = 8;

struct Counters {
    std::array<std::atomic<u64>, kKinds> run_closed_by{};  ///< thread 1: a held run published by a record of this kind
    std::array<std::atomic<u64>, kDmaKinds> dma_by{};      ///< thread 1: DMA reports by kind
    std::atomic<u64> dma_run_open{0};                      ///< thread 1: DMA reports met while a run was held
    std::atomic<u64> prefetch_skipped{0};                  ///< thread 1: prefetch reports not queued (prefetch_skip)
    std::atomic<u64> prefetch_skipped_run_open{0};         ///< ... of them while a run was held (it stays open)
    /// NoteWalk, per stage (VS, PS): walks; of them the same program as the stage's walk before; past the registers
    /// equal to that one / equal to the last walk of the program (any distance); user data equal but the walk's
    /// output not (a sanity count: should be 0); flattened dwords compared
    std::array<std::atomic<u64>, 2> walks{}, walk_same_prog{}, walk_eq_prev{}, walk_eq_last{}, walk_ud_same_flat_diff{},
        walk_dwords{};
};
Counters& Count();

/// SSPC_R6_CP_DIAG=1 (thread 1, after a draw's full preparation): a stage with an SRT walk (flattened data past the 16
/// user data registers) compared with the last walk output of the same program - equal past the registers: the engine
/// gave the same resources through another table copy (the CUE copies its tables at each commit). `stage`: 0 VS, 1 PS.
void NoteWalk(u32 stage, u64 program_address, const u32* user_data, const u32* flat, size_t flat_size);

inline void Bump(std::atomic<u64>& a, u64 v = 1) {
    a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

/// The lines for gpu_native.txt (empty when nothing of R6-P is on).
std::string Report();

} // namespace SecondSon::Gpu::Native::R6
