// SecondSonPC — the control flow graph of a GCN program (gcn_cfg.h).
#include "runtime/gpu/gcn/gcn_cfg.h"

#include <map>
#include <set>

#include <fmt/format.h>

namespace SecondSon::Gpu::Gcn {

namespace {

constexpr u16 kEndpgm = 1, kBranch = 2;

/// The condition of a branching SOPP instruction; `is_branch` false for any other.
Cond BranchCond(const Inst& in, bool& is_branch) {
    is_branch = true;
    switch (in.op) {
    case kBranch:
        return Cond::Always;
    case 4:
        return Cond::Scc0;
    case 5:
        return Cond::Scc1;
    case 6:
        return Cond::Vccz;
    case 7:
        return Cond::Vccnz;
    case 8:
        return Cond::Execz;
    default:
        is_branch = false;
        return Cond::Never;
    }
}

/// SOPP's simm16: dwords from the next instruction.
s32 Simm16(const Inst& in) {
    return static_cast<s16>(in.w0 & 0xFFFF);
}

} // namespace

std::string_view CondName(Cond c) {
    switch (c) {
    case Cond::Always:
        return "always";
    case Cond::Scc0:
        return "scc0";
    case Cond::Scc1:
        return "scc1";
    case Cond::Vccz:
        return "vccz";
    case Cond::Vccnz:
        return "vccnz";
    case Cond::Execz:
        return "execz";
    default:
        return "never";
    }
}

bool BuildCfg(std::span<const Inst> insts, Cfg& cfg, std::string& error) {
    cfg = {};
    if (insts.empty()) {
        error = "no instructions";
        return false;
    }
    std::map<u32, u32> index_of; // dword offset -> instruction index
    for (u32 i = 0; i < insts.size(); ++i) {
        index_of[insts[i].offset] = i;
    }
    const auto target_of = [&](const Inst& in, u32& index) {
        const s64 target = static_cast<s64>(in.offset) + in.dwords + Simm16(in);
        const auto it = target >= 0 ? index_of.find(static_cast<u32>(target)) : index_of.end();
        if (it == index_of.end()) {
            error = fmt::format("dword {}: branch target {} is not an instruction", in.offset, target);
            return false;
        }
        index = it->second;
        return true;
    };
    // the instructions reachable from the entry (code after an s_endpgm only when a branch goes there; data never).
    // s_swappc_b64 is the call of the fetch shader at a vertex program's start: an ordinary instruction here.
    std::vector<u8> reach(insts.size(), 0);
    {
        std::vector<u32> work{0};
        while (!work.empty()) {
            u32 i = work.back();
            work.pop_back();
            for (; i < insts.size() && !reach[i]; ++i) {
                reach[i] = 1;
                const Inst& in = insts[i];
                if (in.format != Format::Sopp) {
                    continue;
                }
                bool is_branch = false;
                const Cond c = BranchCond(in, is_branch);
                if (is_branch) {
                    u32 t = 0;
                    if (!target_of(in, t)) {
                        return false;
                    }
                    work.push_back(t);
                    if (c == Cond::Always) {
                        break;
                    }
                }
                if (in.op == kEndpgm) {
                    break;
                }
            }
        }
    }
    std::set<u32> leaders{0};
    for (u32 i = 0; i < insts.size(); ++i) {
        if (!reach[i]) {
            continue;
        }
        if (i > 0 && !reach[i - 1]) {
            leaders.insert(i);
        }
        const Inst& in = insts[i];
        const bool unsupported = (in.format == Format::Sop1 && (in.op == 32 || in.op == 50)) ||
                                 (in.format == Format::Sop2 && in.op == 43) ||
                                 (in.format == Format::Sopk && in.op == 17) ||
                                 (in.format == Format::Sopp && in.op >= 23 && in.op <= 26);
        if (unsupported) {
            error = fmt::format("dword {}: {} is not supported", in.offset, Mnemonic(in.format, in.op));
            return false;
        }
        if (in.format != Format::Sopp) {
            continue;
        }
        bool is_branch = false;
        BranchCond(in, is_branch);
        if ((in.op == kEndpgm || is_branch) && i + 1 < insts.size()) {
            leaders.insert(i + 1);
        }
        if (is_branch) {
            u32 t = 0;
            if (!target_of(in, t)) {
                return false;
            }
            leaders.insert(t);
        }
    }
    std::vector<u32> starts;
    for (const u32 l : leaders) {
        if (l < insts.size() && reach[l]) {
            starts.push_back(l);
        }
    }
    std::map<u32, s32> block_of; // first instruction -> block
    for (u32 b = 0; b < starts.size(); ++b) {
        Block blk;
        blk.first = starts[b];
        u32 last = blk.first;
        while (last + 1 < insts.size() && reach[last + 1] && !leaders.contains(last + 1)) {
            ++last;
        }
        blk.last = last;
        cfg.blocks.push_back(blk);
        block_of[blk.first] = static_cast<s32>(b);
    }
    for (u32 b = 0; b < cfg.blocks.size(); ++b) {
        Block& blk = cfg.blocks[b];
        const Inst& end = insts[blk.last];
        const auto fall_it = block_of.find(blk.last + 1);
        const s32 fall = fall_it != block_of.end() ? fall_it->second : -1;
        if (end.format == Format::Sopp && end.op == kEndpgm) {
            blk.exit = true;
            continue;
        }
        bool is_branch = false;
        const Cond c = end.format == Format::Sopp ? BranchCond(end, is_branch) : Cond::Never;
        if (!is_branch) {
            blk.taken = fall; // (-1: runs off the end - none of the game's programs does)
            continue;
        }
        u32 t = 0;
        target_of(end, t);
        const s32 target = block_of[t];
        if (c == Cond::Always || target == fall) {
            blk.taken = target; // (a conditional branch to the next instruction is a fall-through)
        } else {
            blk.cond = c;
            blk.taken = target;
            blk.next = fall;
        }
    }

    // reverse post-order from the entry
    const u32 n = static_cast<u32>(cfg.blocks.size());
    std::vector<u8> seen(n, 0);
    std::vector<u32> post;
    std::vector<std::pair<u32, u32>> stack{{0, 0}};
    seen[0] = 1;
    while (!stack.empty()) {
        auto& [b, k] = stack.back();
        const s32 succ[2] = {cfg.blocks[b].taken, cfg.blocks[b].next};
        if (k < 2) {
            const s32 s = succ[k++];
            if (s >= 0 && !seen[s]) {
                seen[s] = 1;
                stack.push_back({static_cast<u32>(s), 0});
            }
            continue;
        }
        post.push_back(b);
        stack.pop_back();
    }
    cfg.rpo.assign(post.rbegin(), post.rend());
    std::vector<s32> order(n, -1);
    for (u32 i = 0; i < cfg.rpo.size(); ++i) {
        order[cfg.rpo[i]] = static_cast<s32>(i);
    }
    cfg.unreachable = n - static_cast<u32>(cfg.rpo.size());

    // dominators (Cooper, Harvey, Kennedy)
    std::vector<std::vector<u32>> preds(n);
    for (u32 b = 0; b < n; ++b) {
        if (order[b] < 0) {
            continue;
        }
        for (const s32 s : {cfg.blocks[b].taken, cfg.blocks[b].next}) {
            if (s >= 0) {
                preds[s].push_back(b);
            }
        }
    }
    std::vector<s32> idom(n, -1);
    idom[0] = 0;
    const auto intersect = [&](s32 a, s32 b) {
        while (a != b) {
            while (order[a] > order[b]) {
                a = idom[a];
            }
            while (order[b] > order[a]) {
                b = idom[b];
            }
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 1; i < cfg.rpo.size(); ++i) {
            const u32 b = cfg.rpo[i];
            s32 d = -1;
            for (const u32 p : preds[b]) {
                if (idom[p] >= 0) {
                    d = d < 0 ? static_cast<s32>(p) : intersect(static_cast<s32>(p), d);
                }
            }
            if (d >= 0 && idom[b] != d) {
                idom[b] = d;
                changed = true;
            }
        }
    }
    const auto dominates = [&](u32 a, u32 b) {
        for (u32 x = b;; x = static_cast<u32>(idom[x])) {
            if (x == a) {
                return true;
            }
            if (x == 0) {
                return false;
            }
        }
    };
    std::set<u32> headers;
    for (const u32 b : cfg.rpo) {
        for (const s32 s : {cfg.blocks[b].taken, cfg.blocks[b].next}) {
            if (s < 0 || order[s] > order[b]) {
                continue; // (a forward edge)
            }
            if (dominates(static_cast<u32>(s), b)) {
                ++cfg.back_edges;
                headers.insert(static_cast<u32>(s));
            } else {
                ++cfg.irreducible_edges;
            }
        }
    }
    idom[0] = -1;
    cfg.idom = std::move(idom);
    cfg.loops = static_cast<u32>(headers.size());
    return true;
}

} // namespace SecondSon::Gpu::Gcn
