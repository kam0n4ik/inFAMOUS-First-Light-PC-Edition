// SecondSonPC R6-P - the command processor's local optimizations of phase R6 (docs/NATIVE_RENDER_R6_CP_OPTIMIZATION.md).
//
// Every switch is off by default and read once (the first Opt() call); each is its own variable, like the CITY-OPT
// ones. The counters are written by one thread each (single writer: load + store, no locked add) and read by the
// report at any time.
#pragma once

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/command_processor.h"

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
    /// SSPC_R6_CP_RUN_POOL=1: a long run's member vector (over 32 members) is not freed by thread 2 after its run and
    /// grown again from nothing by thread 1 for the next long run: thread 2 gives it to a small pool (TakeMembers /
    /// GiveMembers), thread 1 takes one when a run outgrows its slot's vector. The members are the same; only where
    /// their memory comes from changes.
    bool run_pool = false;
    /// SSPC_R6_CP_POLL_TRIVIAL=1|verify: a memory WAIT_REG_MEM whose compare holds for every value (TrivialPoll:
    /// function 0 "always", ">= 0", "<= x" with x >= the mask) does not read the memory. Its outcome is the same
    /// whatever the read returns; the read was not free: over a write still on the GPU's timeline (the city: the
    /// game's occlusion query records, polled ">= 0" right after the query's begin) it drained the queue and waited
    /// for the GPU - a bubble on both threads, 0.3 ms each. verify: read as before, the outcome checked.
    u8 poll_trivial = 0; ///< 0 off, 1 on, 2 verify
};
/// The compare of WAIT_REG_MEM ((value & mask) <function> ref) holds for every value.
inline bool TrivialPoll(u32 function, u32 ref, u32 mask) {
    return function == 0 || (function == 5 && ref == 0) || (function == 2 && ref >= mask);
}
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
    /// NoteTimelineRead by site: reads that drained, of them waited for the GPU, their time (TSC)
    std::array<std::atomic<u64>, 6> tread_drained{}, tread_synced{}, tread_ticks{}; ///< (kReadSites)
    /// SSPC_R6_CP_RUN_POOL: vectors taken from the pool (thread 1), the pool empty then, given back / freed (thread 2)
    std::atomic<u64> pool_taken{0}, pool_empty{0}, pool_given{0}, pool_freed{0};
    /// SSPC_R6_CP_POLL_TRIVIAL (thread 1): memory polls whose compare always holds - not read (on) / read (verify), of
    /// them the read was over a pending timeline write; verify: the outcome not met (impossible: 0)
    std::atomic<u64> trivial_polls{0}, trivial_over_timeline{0}, trivial_not_met{0};
};
Counters& Count();

/// SSPC_R6_CP_DIAG=1 (thread 1, after a draw's full preparation): a stage with an SRT walk (flattened data past the 16
/// user data registers) compared with the last walk output of the same program - equal past the registers: the engine
/// gave the same resources through another table copy (the CUE copies its tables at each commit). `stage`: 0 VS, 1 PS.
void NoteWalk(u32 stage, u64 program_address, const u32* user_data, const u32* flat, size_t flat_size);

/// SSPC_R6_CP_RUN_POOL=1. Thread 1, before a member is appended to a full vector: its members moved into a pooled
/// vector of more capacity (false: none pooled - the vector grows as before). Thread 2, after the run's draws: the
/// vector's memory pooled (at most kPooled kept, else freed); `v` is left empty without capacity either way.
inline constexpr size_t kPooled = 16;
bool TakeMembers(std::vector<Cp::RunMember>& v);
void GiveMembers(std::vector<Cp::RunMember>& v);

/// The processor's own guest memory reads, by site (set by the processor before each read; its thread only).
enum ReadSite : u8 { kReadOther, kReadSubmission, kReadIndirect, kReadDma, kReadWait, kReadWaitAgain, kReadSites };
inline thread_local u8 g_read_site = kReadOther; // (thread_local: thread 2's fallbacks read memory too)
/// SSPC_R6_CP_DIAG=1: the WAIT_REG_MEM being polled (function, reference, mask), set by the processor before its read
inline thread_local u32 g_poll_function = 0, g_poll_ref = 0, g_poll_mask = 0;
/// SSPC_R6_CP_POLL_TRIVIAL=verify: the poll's read went over a pending timeline write (Memory::Read sets it)
inline thread_local bool g_read_over_timeline = false;
/// SSPC_R6_CP_DIAG=1 (native_backend.cpp Memory::Read): the value a timeline read returned (after NoteTimelineRead)
void NoteTimelineValue(u64 addr, u32 value);
/// SSPC_R6_CP_DIAG=1 (native_backend.cpp Memory::Read): a read over a write still on the GPU's timeline - the queue
/// drained, and `synced`: the executor then waited for the GPU too; `ticks`: the whole wait (TSC).
void NoteTimelineRead(u64 addr, u64 bytes, bool synced, u64 ticks);

inline void Bump(std::atomic<u64>& a, u64 v = 1) {
    a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

/// The lines for gpu_native.txt (empty when nothing of R6-P is on).
std::string Report();

} // namespace SecondSon::Gpu::Native::R6
