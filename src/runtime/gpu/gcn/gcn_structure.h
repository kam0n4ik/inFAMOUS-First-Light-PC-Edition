// SecondSonPC — the structure of a GCN program's control flow as SPIR-V needs it (own shader translator, stage 3b):
// every loop's header, continue target (its one latch) and merge (where its exits go), every conditional branch's
// selection merge (its immediate post-dominator), and whether the graph keeps SPIR-V's nesting rules as it is - or
// which shapes need the translator's rewrites (a second latch, exits to several places, a selection region left other
// than through its merge / a break / a continue / the program's end).
#pragma once

#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/gcn/gcn_cfg.h"

namespace SecondSon::Gpu::Gcn {

struct LoopInfo {
    u32 header = 0;
    std::vector<u32> latches; ///< blocks with a back edge to the header
    s32 merge = -1;           ///< the one block outside the loop its exits go to (-1: only s_endpgm exits / several)
    u32 exit_targets = 0;     ///< distinct blocks outside the loop that its exits go to
    std::vector<u8> body;     ///< per block: in the loop
};

struct Structure {
    std::vector<LoopInfo> loops;
    std::vector<s32> ipdom;           ///< immediate post-dominator (-2: the virtual exit, -1: none)
    std::vector<s32> loop_of;         ///< innermost loop of each block (-1: none)
    std::vector<s32> selection_merge; ///< per conditional branch that is not a latch's: its merge (-2: exit, -1: none)
    // shapes the emitter cannot take as they are
    u32 multi_latch_loops = 0;
    u32 multi_exit_loops = 0;
    u32 escaping_selections = 0;
    /// selections whose merge another construct merges at too (nested ifs ending together): the emitter gives the
    /// inner one a merge block of its own (an empty block that goes on to the shared one) - always possible
    u32 shared_merges = 0;
    std::vector<u8> needs_own_merge; ///< per block: its selection needs that block
    std::vector<std::string> notes;

    bool Simple() const {
        return multi_latch_loops == 0 && multi_exit_loops == 0 && escaping_selections == 0;
    }
};

/// Analyses a built graph (gcn_cfg.h; reducible).
Structure AnalyzeStructure(const Cfg& cfg);

} // namespace SecondSon::Gpu::Gcn
