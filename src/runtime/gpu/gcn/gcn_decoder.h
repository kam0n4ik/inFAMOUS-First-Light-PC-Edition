// SecondSonPC — the GCN instruction decoder of SecondSonPC's own shader translator (Phase 4, docs/AUDIT.md: the
// GCN -> SPIR-V translator without the bridge's shader_recompiler). The PS4's GPU is Sea Islands (GCN 1.1, "CI"):
// 32-bit scalar/vector words, 64-bit memory/VOP3/export words, and a 32-bit literal after an instruction whose source
// field is 255 (also v_madmk/v_madak, s_setreg_imm32_b32 and an SMRD whose offset field is 255 without IMM).
// This layer only splits the code into instructions and names them; operands are read by the translator.
#pragma once

#include <array>
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

/// An operand as the ISA encodes a source: 0-103 SGPRs, 106/107 VCC, 124 M0, 126/127 EXEC, 128 zero, 129-192 the
/// integers 1..64, 193-208 -1..-16, 240-247 the float constants, 251 VCCZ, 252 EXECZ, 253 SCC, 255 the literal,
/// 256-511 the VGPRs (256 + n). Fixed register fields are put in the same space (a VGPR field n is 256 + n, an SGPR
/// field is its register: SMRD's sbase and the buffer/image resources are register numbers, not the field's /2, /4).
constexpr u16 kVgpr = 256;
constexpr u16 kLiteralOperand = 255;
constexpr u16 kVccLo = 106;

struct Operands {
    u8 num_src = 0;
    u8 num_dst = 0;
    std::array<u16, 4> src{};
    std::array<u16, 2> dst{};
};

/// The register / constant operands of an instruction (the ISA's fields; immediates such as SOPP's simm16, offsets
/// and the memory controls are read from the words by the translator). Formats not described yet: none of either.
Operands GetOperands(const Inst& in);

/// The mnemonic of (format, opcode), or "" when the table has none (an opcode SecondSonPC does not know).
std::string_view Mnemonic(Format f, u32 op);

/// The program's instructions up to and including its first s_endpgm (or the end of `code`). With `to_end`, on to
/// the end of `code`: code after the first s_endpgm (an early exit's target) is decoded too, and the first word that
/// is no GCN instruction after an s_endpgm ends it quietly (data: inline constants, padding). false: an encoding that
/// is not GCN's before any s_endpgm, or an instruction running past `code`; `error` says which.
bool Decode(std::span<const u32> code, std::vector<Inst>& out, std::string& error, bool to_end = false);

} // namespace SecondSon::Gpu::Gcn
