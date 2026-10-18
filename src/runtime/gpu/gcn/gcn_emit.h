// SecondSonPC — GCN -> SPIR-V emission of SecondSonPC's own shader translator (docs/OWN_SHADER_TRANSLATOR.md, step 2):
// a decoded program (gcn_decoder), its graph (gcn_cfg) and structure (gcn_structure) into a SPIR-V module. The model is
// the oracle's: a lane is an invocation; SGPRs, VGPRs, M0 and VCC are 32-bit function variables; a 64-bit mask (VCC,
// an SGPR pair from a compare, EXEC as a source) is the subgroup ballot of the lanes' bits and a lane's bit is the
// inverse ballot; EXEC and SCC are the lane's booleans; vector instructions run under `if (exec)` when the program
// writes EXEC at all.
//
// Step 2 emits every program as a compute module (local size 64): the control flow, the registers, the constants and
// the ALU. Memory, interpolation, exports and resources come next; an instruction not handled yet ends the emission with
// its mnemonic (the --gcn-translate-test report counts them).
#pragma once

#include <span>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/gcn/gcn_cfg.h"
#include "runtime/gpu/gcn/gcn_decoder.h"
#include "runtime/gpu/gcn/gcn_resources.h"
#include "runtime/gpu/gcn/gcn_structure.h"

namespace SecondSon::Gpu::Gcn {

struct EmitResult {
    bool ok = false;
    std::string error;       ///< the first instruction not handled ("unsupported: <mnemonic>") or a structure problem
    std::string unsupported; ///< its mnemonic alone (empty: none)
    std::vector<u32> spirv;
    /// the resources (bindings: 0 the flattened user data, then buffers[i] at 1 + i; push constants: PushData)
    ResourceInfo resources;
};

struct EmitOptions {
    /// tests only: memory, interpolation and export instructions do nothing and their destinations get zeros (the
    /// control flow, the registers and the ALU of every program can be checked before resources exist)
    bool stub_memory = false;
    /// SGPRs holding user data at the start (SPI_SHADER_PGM_RSRC2.USER_SGPR)
    u32 user_sgprs = 16;
};

EmitResult EmitProgram(std::span<const Inst> insts, const Cfg& cfg, const Structure& st,
                       const EmitOptions& options = {});

} // namespace SecondSon::Gpu::Gcn
