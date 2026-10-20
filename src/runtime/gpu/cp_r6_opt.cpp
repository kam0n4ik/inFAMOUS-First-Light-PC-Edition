// SecondSonPC R6-P - the command processor's local optimizations of phase R6 (cp_r6_opt.h).
#include <windows.h>

#include "runtime/gpu/cp_r6_opt.h"

#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include "base/log.h"

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

std::string Report() {
    const Options& o = Opt();
    if (!o.prefetch_skip && !o.diag) {
        return {};
    }
    Counters& c = Count();
    const auto get = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    std::string out = fmt::format("R6-P (docs/NATIVE_RENDER_R6_CP_OPTIMIZATION.md): prefetch reports not queued "
                                  "(SSPC_R6_CP_PREFETCH={}) {} (while a run was held {})\n",
                                  o.prefetch_skip ? "skip" : "off", get(c.prefetch_skipped),
                                  get(c.prefetch_skipped_run_open));
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
    }
    return out;
}

} // namespace SecondSon::Gpu::Native::R6
