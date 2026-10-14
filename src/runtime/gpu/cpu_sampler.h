// SecondSonPC — a sampling profiler of one thread (P4.4 diagnostics: where the native backend's GPU thread spends its
// CPU time; SSPC_DIAG_SAMPLE=1). A sampler thread suspends the target every period, reads its context, unwinds a few
// frames (RtlVirtualUnwind) and counts the addresses in a preallocated table - nothing is allocated, logged or locked
// while the target is suspended (it may hold the heap's or the logger's lock). Report() symbolizes with DbgHelp (loaded
// at report time) and lists the functions by exclusive (leaf) and inclusive samples.
#pragma once

#include <windows.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Gpu {

class CpuSampler {
public:
    ~CpuSampler();
    /// Samples `thread` (suspend and context rights) every `period_us` once `delay_s` seconds have passed.
    bool Start(HANDLE thread, u32 period_us, u32 delay_s);
    /// Samples every thread of the process whose description contains `name_part` (found once `delay_s` have passed),
    /// without the unwinder (a game thread may be inside exception dispatch, holding what RtlLookupFunctionEntry
    /// takes): the instruction pointer and the nearest return address into the executable found on the stack.
    bool StartByName(const std::wstring& name_part, u32 period_us, u32 delay_s);
    /// P5.2 (SSPC_DIAG_SAMPLE_RAW=1): every thread of the process (found once `delay_s` have passed) each period,
    /// kept raw for attribution to the game's functions offline (tools/native-reconstruction/cpu_profile.py): the
    /// thread, the instruction pointer, the nearest return address into the eboot on the stack (when the thread is in
    /// SecondSonPC's or a system's code) and the eboot's frame-pointer chain from RBP (the game keeps frame pointers).
    /// The eboot's range comes from Engine::GuestImageBase/End.
    bool StartRaw(u32 period_us, u32 delay_s);
    /// The raw samples as text (StartRaw): `thread <i> <name>` lines, then `s <thread> <ip> <eboot caller or 0> <chain>...`
    bool WriteRaw(const std::wstring& path) const;
    /// Stops sampling (before the target thread ends).
    void Stop();
    bool Running() const { return started_; }
    /// The symbolized profile (the `top` functions by exclusive and by inclusive samples).
    std::string Report(size_t top) const;

private:
    static constexpr u32 kDepth = 16;
    static constexpr u32 kTableBits = 17; ///< distinct addresses (131072 entries, 2 MiB)
    struct Entry {
        u64 pc = 0;
        u32 leaf = 0;
        u32 incl = 0;
        u32 own = 0; ///< samples whose nearest frame in the executable is this one (driver/system time to its caller)
    };
    void Run();
    void Count(u64 pc, bool leaf, bool own);

    HANDLE thread_ = nullptr;
    std::wstring name_part_;          ///< StartByName: the threads to find
    std::vector<HANDLE> threads_;     ///< StartByName: the threads found (owned)
    bool unwind_ = true;
    alignas(16) CONTEXT last_ctx_{}; ///< StartByName: the registers of a sample at the hottest address (at report)
    bool have_ctx_ = false;
    struct PcContext {
        u64 pc = 0;
        alignas(16) CONTEXT ctx{};
    };
    std::vector<PcContext> ctx_by_pc_; ///< StartByName: the latest registers at each of the first 256 addresses
    u32 period_us_ = 1000;
    u32 delay_s_ = 0;
    std::thread sampler_;
    std::atomic<bool> stop_{false};
    bool started_ = false;
    std::vector<Entry> table_;
    u64 samples_ = 0, failed_ = 0, dropped_ = 0;
    // StartRaw
    void RunRawTick();
    bool raw_ = false;
    u64 guest_lo_ = 0, guest_hi_ = 0;
    std::vector<std::wstring> thread_names_;
    std::vector<u64> raw_buf_; ///< records: (thread << 32 | count), then `count` addresses (reserved, never grows)
    u64 raw_dropped_ = 0;
};

} // namespace SecondSon::Gpu
