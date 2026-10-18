// SecondSonPC — the control flow graph of a GCN program (own shader translator, stage 3). Blocks are split at the
// targets of the real branches and after every branch and s_endpgm; EXEC does not split them (the translator predicates
// the vector instructions on the lane's EXEC bit instead: a lane is one SPIR-V invocation). The game's programs branch
// with s_branch and s_cbranch_{scc0,scc1,vccz,vccnz,execz} only (docs/generated/GCN_INVENTORY.txt); s_setpc,
// fork/join and the debug branches are refused. The analysis says whether the graph is reducible (every retreating
// edge goes to a block that dominates its source: SPIR-V's loops and selections can express it) and its loops.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/gcn/gcn_decoder.h"

namespace SecondSon::Gpu::Gcn {

enum class Cond : u8 { Always, Scc0, Scc1, Vccz, Vccnz, Execz, Never };

std::string_view CondName(Cond c);

struct Block {
    u32 first = 0;            ///< index of its first instruction
    u32 last = 0;             ///< index of its last instruction (inclusive)
    bool exit = false;        ///< ends with s_endpgm
    Cond cond = Cond::Always; ///< the taken branch's condition (Always: an unconditional jump or fall-through)
    s32 taken = -1;           ///< the block a taken branch goes to (or the only successor)
    s32 next = -1;            ///< the fall-through of a conditional branch
};

struct Cfg {
    std::vector<Block> blocks; ///< in program order; blocks[0] is the entry
    // analysis
    std::vector<s32> idom;     ///< the immediate dominator of each block (-1: the entry / unreachable)
    std::vector<u32> rpo;      ///< reverse post-order of the reachable blocks
    u32 back_edges = 0;        ///< edges to a dominator (natural loops)
    u32 irreducible_edges = 0; ///< retreating edges to a block that does not dominate the source
    u32 loops = 0;             ///< distinct loop headers
    u32 unreachable = 0;

    bool Reducible() const {
        return irreducible_edges == 0;
    }
};

/// The graph of `insts` (one decoded program). false: a branch the translator does not support, or a target that
/// is not an instruction's start (`error`).
bool BuildCfg(std::span<const Inst> insts, Cfg& cfg, std::string& error);

} // namespace SecondSon::Gpu::Gcn
