// SecondSonPC R6-P - the command processor's local optimizations of phase R6 (cp_r6_opt.h).
#include <windows.h>

#include "runtime/gpu/cp_r6_opt.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include "base/log.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::Native::R6 {
namespace {

std::string Env(const char* name) {
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string{};
}

Options Parse() {
    Options o;
    const std::string pf = Env("SSPC_R6_CP_PREFETCH");
    o.prefetch_skip = pf == "skip" || pf == "1";
    o.diag = Env("SSPC_R6_CP_DIAG") == "1";
    o.run_pool = Env("SSPC_R6_CP_RUN_POOL") == "1";
    const std::string pt = Env("SSPC_R6_CP_POLL_TRIVIAL");
    o.poll_trivial = pt == "1" ? 1 : pt == "verify" ? 2 : 0;
    if (o.poll_trivial != 0) {
        SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R6_CP_POLL_TRIVIAL={} - a memory wait whose compare holds for every "
                               "value does not read the memory{}",
                 pt, o.poll_trivial == 2 ? " (verify: read as before, the outcome checked)" : "");
    }
    if (o.run_pool) {
        SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R6_CP_RUN_POOL=1 - long runs' member vectors pooled between the threads");
    }
    if (o.prefetch_skip) {
        SSPC_LOG(GPU, Warning, "EXPERIMENT: SSPC_R6_CP_PREFETCH=skip - Gnm's L2 prefetches (CP_DMA_READ_TAGS) are not "
                               "queued to the recording thread");
    }
    if (o.diag) {
        SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_R6_CP_DIAG=1 - R6-P counters");
    }
    return o;
}

constexpr std::string_view kKindNames[kKinds] = {"draw",  "dispatch", "journal",  "stop",     "cache action",
                                                 "fence", "dma",      "wait met", "acquired", "copy",
                                                 "flip",  "round end", "write",   "gds",      "draw (tagged, refused)",
                                                 "?"};
constexpr std::string_view kDmaNames[kDmaKinds] = {"m2m", "data2mem", "m2gds", "data2gds", "gds2mem", "prefetch", "?",
                                                   "?"};

} // namespace

const Options& Opt() {
    static const Options o = Parse();
    return o;
}

Counters& Count() {
    static Counters c;
    return c;
}

namespace {
struct WalkMemo {
    u64 prev_program = ~0ull;
    std::vector<u32> prev; ///< the stage's walk before (whole flattened data)
    std::unordered_map<u64, std::vector<u32>> last; ///< per program: its last walk
};
std::array<WalkMemo, 2> g_walks; // (thread 1 only)
} // namespace

void NoteWalk(u32 stage, u64 program_address, const u32* user_data, const u32* flat, size_t flat_size) {
    if (stage > 1 || flat_size <= 16) {
        return;
    }
    Counters& c = Count();
    WalkMemo& m = g_walks[stage];
    Bump(c.walks[stage]);
    Bump(c.walk_dwords[stage], flat_size - 16);
    const auto past_regs_equal = [&](const std::vector<u32>& v) {
        return v.size() == flat_size && std::memcmp(v.data() + 16, flat + 16, (flat_size - 16) * sizeof(u32)) == 0;
    };
    if (m.prev_program == program_address) {
        Bump(c.walk_same_prog[stage]);
        if (past_regs_equal(m.prev)) {
            Bump(c.walk_eq_prev[stage]);
        } else if (m.prev.size() >= 16 && std::memcmp(m.prev.data(), user_data, 16 * sizeof(u32)) == 0) {
            Bump(c.walk_ud_same_flat_diff[stage]);
        }
    }
    if (m.last.size() > 65536) {
        m.last.clear();
    }
    std::vector<u32>& l = m.last[program_address];
    if (past_regs_equal(l)) {
        Bump(c.walk_eq_last[stage]);
    }
    l.assign(flat, flat + flat_size);
    m.prev.assign(flat, flat + flat_size);
    m.prev_program = program_address;
}

namespace {
std::mutex g_pool_mutex;
std::vector<std::vector<Cp::RunMember>> g_pool; // (empty vectors with their capacity)
} // namespace

bool TakeMembers(std::vector<Cp::RunMember>& v) {
    std::vector<Cp::RunMember> p;
    {
        std::scoped_lock lock{g_pool_mutex};
        if (g_pool.empty()) {
            Bump(Count().pool_empty);
            return false;
        }
        p = std::move(g_pool.back());
        g_pool.pop_back();
    }
    if (p.capacity() <= v.capacity()) {
        Bump(Count().pool_empty);
        std::scoped_lock lock{g_pool_mutex};
        g_pool.push_back(std::move(p)); // (back: not larger than this one; the counters stay single-writer)
        return false;
    }
    p.assign(v.begin(), v.end());
    v.swap(p);
    Bump(Count().pool_taken);
    return true;
}

void GiveMembers(std::vector<Cp::RunMember>& v) {
    v.clear();
    {
        std::scoped_lock lock{g_pool_mutex};
        if (g_pool.size() < kPooled) {
            g_pool.push_back(std::move(v));
            v = {};
            Bump(Count().pool_given);
            return;
        }
    }
    std::vector<Cp::RunMember>().swap(v);
    Bump(Count().pool_freed);
}

namespace {
struct ReadAt {
    u64 drained = 0, synced = 0, ticks = 0, bytes = 0;
    u8 site = 0;
    u32 function = 0, ref = 0, mask = 0; ///< the last poll's
    u32 value = 0, values_seen = 0;      ///< the last value read; how many reads gave a value other than the one before
};
std::mutex g_reads_mutex; // (the processor's thread writes, the report reads)
std::unordered_map<u64, ReadAt> g_reads;
constexpr std::string_view kSiteNames[kReadSites] = {"other",      "submission", "indirect buffer",
                                                     "DMA source", "wait poll",  "wait poll again"};
} // namespace

void NoteTimelineRead(u64 addr, u64 bytes, bool synced, u64 ticks) {
    Counters& c = Count();
    const u32 site = g_read_site < kReadSites ? g_read_site : 0;
    Bump(c.tread_drained[site]);
    Bump(c.tread_synced[site], synced ? 1 : 0);
    Bump(c.tread_ticks[site], ticks);
    std::scoped_lock lock{g_reads_mutex};
    if (g_reads.size() < 4096 || g_reads.contains(addr)) {
        ReadAt& r = g_reads[addr];
        ++r.drained;
        r.synced += synced ? 1 : 0;
        r.ticks += ticks;
        r.bytes = bytes;
        r.site = static_cast<u8>(site);
        r.function = g_poll_function;
        r.ref = g_poll_ref;
        r.mask = g_poll_mask;
    }
}

void NoteTimelineValue(u64 addr, u32 value) {
    std::scoped_lock lock{g_reads_mutex};
    if (const auto it = g_reads.find(addr); it != g_reads.end()) {
        it->second.values_seen += it->second.value != value ? 1 : 0;
        it->second.value = value;
    }
}

std::string Report() {
    const Options& o = Opt();
    if (!o.prefetch_skip && !o.diag && !o.run_pool && o.poll_trivial == 0) {
        return {};
    }
    Counters& c = Count();
    const auto get = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    std::string out = fmt::format("R6-P (docs/NATIVE_RENDER_R6_CP_OPTIMIZATION.md): prefetch reports not queued "
                                  "(SSPC_R6_CP_PREFETCH={}) {} (while a run was held {})\n",
                                  o.prefetch_skip ? "skip" : "off", get(c.prefetch_skipped),
                                  get(c.prefetch_skipped_run_open));
    if (o.run_pool) {
        out += fmt::format("  long runs' member vectors (SSPC_R6_CP_RUN_POOL=1): taken from the pool {} (none larger there "
                           "{}), given back {}, freed (the pool full) {}\n",
                           get(c.pool_taken), get(c.pool_empty), get(c.pool_given), get(c.pool_freed));
    }
    if (o.poll_trivial != 0) {
        out += fmt::format("  memory waits whose compare always holds (SSPC_R6_CP_POLL_TRIVIAL={}): {} {}; verify: the "
                           "read was over a write on the GPU's timeline {}, the compare did not hold {}\n",
                           o.poll_trivial == 2 ? "verify" : "1", o.poll_trivial == 2 ? "read" : "not read",
                           get(c.trivial_polls), get(c.trivial_over_timeline), get(c.trivial_not_met));
    }
    if (o.diag) {
        out += "  held runs published by a record of kind:";
        for (u32 k = 0; k < kKinds; ++k) {
            if (const u64 n = get(c.run_closed_by[k]); n != 0) {
                out += fmt::format(" {} {}", kKindNames[k], n);
            }
        }
        out += "\n  DMA reports by kind:";
        for (u32 k = 0; k < kDmaKinds; ++k) {
            if (const u64 n = get(c.dma_by[k]); n != 0) {
                out += fmt::format(" {} {}", kDmaNames[k], n);
            }
        }
        out += fmt::format("; met while a run was held {}\n", get(c.dma_run_open));
        for (u32 s = 0; s < 2; ++s) {
            out += fmt::format("  {} SRT walks (full preparations, flattened past the registers): {} ({:.1f} dwords "
                               "each); the same program as the walk before {}, of them equal to it {}; equal to the "
                               "program's last walk {}; same user data, other output {}\n",
                               s == 0 ? "VS" : "PS", get(c.walks[s]),
                               get(c.walks[s]) != 0 ? static_cast<double>(get(c.walk_dwords[s])) / get(c.walks[s]) : 0.0,
                               get(c.walk_same_prog[s]), get(c.walk_eq_prev[s]), get(c.walk_eq_last[s]),
                               get(c.walk_ud_same_flat_diff[s]));
        }
        const double ms_per_tick = 1000.0 / static_cast<double>(Kernel::Clock::TscFrequency());
        out += "  the processor's reads over a write on the GPU's timeline (drained / then waited for the GPU / ms):";
        for (u32 s = 0; s < kReadSites; ++s) {
            if (get(c.tread_drained[s]) != 0) {
                out += fmt::format(" {} {}/{}/{:.0f};", kSiteNames[s], get(c.tread_drained[s]), get(c.tread_synced[s]),
                                   static_cast<double>(get(c.tread_ticks[s])) * ms_per_tick);
            }
        }
        out += "\n";
        std::vector<std::pair<u64, ReadAt>> top;
        {
            std::scoped_lock lock{g_reads_mutex};
            top.assign(g_reads.begin(), g_reads.end());
        }
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second.ticks > b.second.ticks; });
        for (size_t i = 0; i < top.size() && i < 12; ++i) {
            const ReadAt& r = top[i].second;
            out += fmt::format("    {:#x} ({} bytes, {}): drained {}, waited for the GPU {}, {:.0f} ms; last poll function "
                               "{} ref {:#x} mask {:#x}, last value {:#x}, value changes {}\n",
                               top[i].first, r.bytes, kSiteNames[r.site], r.drained, r.synced,
                               static_cast<double>(r.ticks) * ms_per_tick, r.function, r.ref, r.mask, r.value,
                               r.values_seen);
        }
    }
    return out;
}

} // namespace SecondSon::Gpu::Native::R6
