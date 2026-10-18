// SecondSonPC — the GCN instruction decoder of SecondSonPC's own shader translator (Phase 4, docs/AUDIT.md: the
// GCN -> SPIR-V translator without the bridge's shader_recompiler). The PS4's GPU is Sea Islands (GCN 1.1, "CI"):
// 32-bit scalar/vector words, 64-bit memory/VOP3/export words, and a 32-bit literal after an instruction whose source
// field is 255 (also v_madmk/v_madak, s_setreg_imm32_b32 and an SMRD whose offset field is 255 without IMM).
// This layer only splits the code into instructions and names them; operands are read by the translator.
#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Gpu::Gcn {

enum class Format : u8 {
    Sop2,
    Sopk,
    Sop1,
    Sopc,
    Sopp,
    Smrd,
    Vop2,
    Vop3,
    Vop1,
    Vopc,
    Vintrp,
    Ds,
    Mubuf,
    Mtbuf,
    Mimg,
    Exp,
    Unknown,
    Count,
};

std::string_view FormatName(Format f);

struct Inst {
    Format format = Format::Unknown;
    u16 op = 0;
    u8 dwords = 1;      ///< including the literal
    bool literal = false;
    u32 w0 = 0, w1 = 0; ///< w1: the second word of a 64-bit encoding
    u32 lit = 0;        ///< the literal, when `literal`
    u32 offset = 0;     ///< in dwords from the program's start
};

/// The mnemonic of (format, opcode), or "" when the table has none (an opcode SecondSonPC does not know).
std::string_view Mnemonic(Format f, u32 op);

/// The program's instructions up to and including its s_endpgm (or the end of `code`). false: an encoding that is
/// not GCN's, or an instruction running past `code`; `error` says which (instructions decoded so far are kept).
bool Decode(std::span<const u32> code, std::vector<Inst>& out, std::string& error);

} // namespace SecondSon::Gpu::Gcn
