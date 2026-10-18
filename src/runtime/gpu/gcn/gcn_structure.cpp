// SecondSonPC — the structure of a GCN program's control flow (gcn_structure.h).
#include "runtime/gpu/gcn/gcn_structure.h"

#include <algorithm>
#include <set>

#include <fmt/format.h>

namespace SecondSon::Gpu::Gcn {

namespace {

std::vector<s32> Successors(const Block& b) {
    std::vector<s32> s;
    if (b.taken >= 0) {
        s.push_back(b.taken);
    }
    if (b.next >= 0 && b.next != b.taken) {
        s.push_back(b.next);
    }
    return s;
}

} // namespace

Structure AnalyzeStructure(const Cfg& cfg) {
    Structure st;
    const u32 n = static_cast<u32>(cfg.blocks.size());
    const u32 vexit = n; // the virtual exit
    std::vector<std::vector<u32>> succ(n + 1), pred(n + 1);
    for (u32 b = 0; b < n; ++b) {
        auto s = Successors(cfg.blocks[b]);
        if (s.empty()) {
            s.push_back(static_cast<s32>(vexit)); // s_endpgm (or running off the end)
        }
        for (const s32 x : s) {
            succ[b].push_back(static_cast<u32>(x));
            pred[x].push_back(b);
        }
    }

    // post-dominators: dominators of the reverse graph from the virtual exit
    std::vector<s32> order(n + 1, -1);
    std::vector<u32> rpo;
    {
        std::vector<u8> seen(n + 1, 0);
        std::vector<u32> post;
        std::vector<std::pair<u32, u32>> stack{{vexit, 0}};
        seen[vexit] = 1;
        while (!stack.empty()) {
            auto& [b, k] = stack.back();
            if (k < pred[b].size()) {
                const u32 p = pred[b][k++];
                if (!seen[p]) {
                    seen[p] = 1;
                    stack.push_back({p, 0});
                }
                continue;
            }
            post.push_back(b);
            stack.pop_back();
        }
        rpo.assign(post.rbegin(), post.rend());
        for (u32 i = 0; i < rpo.size(); ++i) {
            order[rpo[i]] = static_cast<s32>(i);
        }
    }
    std::vector<s32> ipd(n + 1, -1);
    ipd[vexit] = static_cast<s32>(vexit);
    const auto intersect = [&](s32 a, s32 b) {
        while (a != b) {
            while (order[a] > order[b]) {
                a = ipd[a];
            }
            while (order[b] > order[a]) {
                b = ipd[b];
            }
        }
        return a;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 1; i < rpo.size(); ++i) {
            const u32 b = rpo[i];
            s32 d = -1;
            for (const u32 s : succ[b]) {
                if (ipd[s] >= 0) {
                    d = d < 0 ? static_cast<s32>(s) : intersect(static_cast<s32>(s), d);
                }
            }
            if (d >= 0 && ipd[b] != d) {
                ipd[b] = d;
                changed = true;
            }
        }
    }
    st.ipdom.assign(n, -1);
    for (u32 b = 0; b < n; ++b) {
        st.ipdom[b] = ipd[b] == static_cast<s32>(vexit) ? -2 : ipd[b];
    }

    const auto dominates = [&](u32 a, u32 b) {
        for (u32 x = b;; x = static_cast<u32>(cfg.idom[x])) {
            if (x == a) {
                return true;
            }
            if (cfg.idom[x] < 0) {
                return false;
            }
        }
    };

    // loops
    std::vector<s32> header_loop(n, -1);
    for (const u32 b : cfg.rpo) {
        for (const u32 s : succ[b]) {
            if (s == vexit || !dominates(s, b)) {
                continue;
            }
            if (header_loop[s] < 0) {
                header_loop[s] = static_cast<s32>(st.loops.size());
                LoopInfo l;
                l.header = s;
                l.body.assign(n, 0);
                l.body[s] = 1;
                st.loops.push_back(std::move(l));
            }
            st.loops[header_loop[s]].latches.push_back(b);
        }
    }
    for (LoopInfo& l : st.loops) {
        std::vector<u32> work(l.latches.begin(), l.latches.end());
        while (!work.empty()) {
            const u32 b = work.back();
            work.pop_back();
            if (l.body[b]) {
                continue;
            }
            l.body[b] = 1;
            for (const u32 p : pred[b]) {
                work.push_back(p);
            }
        }
        std::set<u32> exits;
        for (u32 b = 0; b < n; ++b) {
            if (!l.body[b]) {
                continue;
            }
            for (const u32 s : succ[b]) {
                if (s != vexit && !l.body[s]) {
                    exits.insert(s);
                }
            }
        }
        l.exit_targets = static_cast<u32>(exits.size());
        l.merge = exits.size() == 1 ? static_cast<s32>(*exits.begin()) : -1;
        if (l.latches.size() > 1) {
            ++st.multi_latch_loops;
            st.notes.push_back(fmt::format("loop at block {}: {} latches", l.header, l.latches.size()));
        }
        if (exits.size() > 1) {
            ++st.multi_exit_loops;
            st.notes.push_back(fmt::format("loop at block {}: exits to {} blocks", l.header, exits.size()));
        }
    }
    st.loop_of.assign(n, -1);
    std::vector<u32> loop_size(st.loops.size());
    for (u32 i = 0; i < st.loops.size(); ++i) {
        loop_size[i] = static_cast<u32>(std::count(st.loops[i].body.begin(), st.loops[i].body.end(), 1));
    }
    for (u32 b = 0; b < n; ++b) {
        u32 best = ~0u;
        for (u32 i = 0; i < st.loops.size(); ++i) {
            if (st.loops[i].body[b] && loop_size[i] < best) {
                best = loop_size[i];
                st.loop_of[b] = static_cast<s32>(i);
            }
        }
    }

    // selections
    st.selection_merge.assign(n, -1);
    st.needs_own_merge.assign(n, 0);
    for (const u32 b : cfg.rpo) {
        const Block& blk = cfg.blocks[b];
        if (blk.next < 0) {
            continue;
        }
        const s32 li = st.loop_of[b];
        const LoopInfo* loop = li >= 0 ? &st.loops[li] : nullptr;
        // a latch's conditional back edge (continue / exit) is the loop's own branch
        if (loop != nullptr &&
            (static_cast<u32>(blk.taken) == loop->header || static_cast<u32>(blk.next) == loop->header)) {
            continue;
        }
        const s32 merge = st.ipdom[b];
        st.selection_merge[b] = merge;
        // the region: blocks reached from b before the merge; an edge out of it may only go to the merge, to the
        // enclosing loop's merge (break) or header (continue), or end the program
        std::vector<u8> in(n, 0);
        std::vector<u32> work;
        for (const u32 s : succ[b]) {
            if (s != vexit && static_cast<s32>(s) != merge) {
                work.push_back(s);
            }
        }
        bool escapes = false;
        while (!work.empty()) {
            const u32 x = work.back();
            work.pop_back();
            if (in[x]) {
                continue;
            }
            if (loop != nullptr && (static_cast<s32>(x) == loop->merge || x == loop->header)) {
                continue; // (break / continue)
            }
            if (!dominates(b, x)) {
                escapes = true;
                break;
            }
            in[x] = 1;
            for (const u32 s : succ[x]) {
                if (s != vexit && static_cast<s32>(s) != merge) {
                    work.push_back(s);
                }
            }
        }
        if (!escapes && merge >= 0 && !dominates(b, static_cast<u32>(merge))) {
            // the merge is entered from outside the region too: shared with an enclosing construct (or a break /
            // continue target) - the region's own exits all go to it, so a block of its own fixes the nesting
            st.needs_own_merge[b] = 1;
            ++st.shared_merges;
        }
        if (escapes) {
            ++st.escaping_selections;
            st.notes.push_back(fmt::format("branch at block {}: its region is left before merge {}", b, merge));
        }
    }
    return st;
}

} // namespace SecondSon::Gpu::Gcn
