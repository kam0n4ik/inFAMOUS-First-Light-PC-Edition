// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — `--gcn-verify-decoder`: SecondSonPC's GCN decoder (runtime/gpu/gcn) against the oracle's
// (shader_recompiler/frontend/decode.cpp) on every program of the game (the offline index, read only from the packs):
// the same instruction boundaries, the same opcode (by mnemonic) and the same operand of each source and destination
// the oracle decodes (its field + register in the ISA's 9-bit operand space). The report lists every difference kind
// with its first example; a difference is SecondSonPC's to explain before its translator relies on the decoder.
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>

#include "app/config.h"
#include "bridge/shadps4/gcn_decoder_check.h"
#include "runtime/gpu/gcn/gcn_decoder.h"
#include "runtime/gpu/shader/game_shader_index.h"
#include "shader_recompiler/frontend/decode.h"

namespace SecondSon::Bridge {

namespace {

namespace G = Gpu::Gcn;
using Shader::Gcn::GcnCodeSlice;
using Shader::Gcn::GcnDecodeContext;
using Shader::Gcn::OperandField;

/// The oracle's operand in the ISA's 9-bit space (-1: none). Its representations, normalised: a literal holds the
/// value in `code` (-> 255); `scale` turns a register field into its register (SMRD sbase x2, resource x4).
int RawOf(const Shader::Gcn::InstOperand& o, u32 scale = 1) {
    if (o.field == OperandField::Undefined) {
        return -1;
    }
    if (o.field == OperandField::LiteralConst) {
        return G::kLiteralOperand;
    }
    if (o.field == OperandField::VectorGPR) {
        return static_cast<int>(G::kVgpr + (o.code & 0xFF));
    }
    return static_cast<int>(o.code * scale);
}

/// The scale of the oracle's source `i` of `in` (it keeps the encoding's field for the resource registers).
u32 SourceScale(const G::Inst& in, u32 i) {
    switch (in.format) {
    case G::Format::Smrd:
        return i == 0 ? 2 : 1;
    case G::Format::Mubuf:
    case G::Format::Mtbuf:
        return i == 2 ? 4 : 1;
    case G::Format::Mimg:
        return i == 2 || i == 3 ? 4 : 1;
    default:
        return 1;
    }
}

/// The ISA's names that the oracle's table spells otherwise (the same instruction).
std::string Canonical(std::string name) {
    const size_t at = name.find("_lg_");
    if (name.starts_with("v_cmp") && at != std::string::npos &&
        (name.ends_with("_i32") || name.ends_with("_u32") || name.ends_with("_i64") || name.ends_with("_u64"))) {
        name.replace(at, 4, "_ne_");
    }
    return name;
}

std::string Lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return r;
}

} // namespace

int RunGcnDecoderCheck(const App::Config& cfg) {
    Gpu::Shader::GameShaderIndex index;
    std::string error;
    const auto idx = Gpu::Shader::NativeCacheRoot(cfg.shader_cache_dir, cfg.game_dir.filename().string()) /
                     "game_programs.idx";
    if (!index.Load(idx, error)) {
        std::fprintf(stderr, "--gcn-verify-decoder: %s (%s)\n", error.c_str(), idx.string().c_str());
        return 1;
    }
    struct Diff {
        u64 count = 0;
        std::string example;
    };
    std::map<std::string, Diff> diffs;
    const auto note = [&](const std::string& kind, const std::string& example) {
        Diff& d = diffs[kind];
        if (d.count++ == 0) {
            d.example = example;
        }
    };
    u64 programs = 0, instructions = 0, operands = 0;
    std::vector<u32> code;
    std::vector<G::Inst> ours;
    std::map<u32, std::ifstream> files;
    for (const auto& p : index.Programs()) {
        const auto& loc = index.Members()[p.first_member];
        auto& f = files[loc.pack];
        if (!f.is_open()) {
            f.open(cfg.game_dir / index.Packs()[loc.pack], std::ios::binary);
        }
        code.assign(p.length / 4, 0);
        f.clear();
        f.seekg(static_cast<std::streamoff>(loc.offset));
        f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size() * 4));
        ours.clear();
        if (!f || !G::Decode(code, ours, error)) {
            note("ours: not decoded", fmt::format("{} @ {:#x}: {}", index.Packs()[loc.pack], loc.offset, error));
            continue;
        }
        ++programs;
        const std::string where = fmt::format("{} @ {:#x}", index.Packs()[loc.pack], loc.offset);
        GcnCodeSlice slice(code.data(), code.data() + code.size());
        GcnDecodeContext ctx;
        for (const G::Inst& in : ours) {
            if (slice.atEnd()) {
                note("oracle: ended earlier", where);
                break;
            }
            const auto g = ctx.decodeInstruction(slice);
            ++instructions;
            const std::string name(G::Mnemonic(in.format, in.op));
            const std::string at =
                fmt::format("{} dword {}: {} ({} {})", where, in.offset, name, G::FormatName(in.format), in.op);
            if (g.length != in.dwords * 4u) {
                note(fmt::format("length {} {}", G::FormatName(in.format), name),
                     fmt::format("{}: ours {} bytes, oracle {}", at, in.dwords * 4, g.length));
                break; // (the streams are out of step from here)
            }
            const std::string gname = Lower(magic_enum::enum_name(g.opcode));
            if (Canonical(gname) != Canonical(name)) {
                note(fmt::format("opcode {} {} -> {}", G::FormatName(in.format), name, gname), at);
            }
            const G::Operands o = G::GetOperands(in);
            for (u32 i = 0; i < o.num_src && i < g.src_count && i < 4; ++i) {
                const int raw = RawOf(g.src[i], SourceScale(in, i));
                if (raw < 0) {
                    continue;
                }
                ++operands;
                if (raw != o.src[i]) {
                    note(fmt::format("src{} {} {}", i, G::FormatName(in.format), name),
                         fmt::format("{}: ours {}, oracle {} (field {}, code {})", at, o.src[i], raw,
                                     magic_enum::enum_name(g.src[i].field), g.src[i].code));
                }
            }
            for (u32 i = 0; i < o.num_dst && i < g.dst_count && i < 2; ++i) {
                int raw = RawOf(g.dst[i]);
                if (raw < 0) {
                    continue;
                }
                if (in.format == G::Format::Vop3 && in.op < 256 && raw >= G::kVgpr) {
                    raw -= G::kVgpr; // (the oracle marks a compare's SGPR pair as a VGPR field)
                }
                ++operands;
                if (raw != o.dst[i]) {
                    note(fmt::format("dst{} {} {}", i, G::FormatName(in.format), name),
                         fmt::format("{}: ours {}, oracle {} (field {}, code {})", at, o.dst[i], raw,
                                     magic_enum::enum_name(g.dst[i].field), g.dst[i].code));
                }
            }
        }
    }
    u64 total = 0;
    for (const auto& [k, d] : diffs) {
        total += d.count;
    }
    std::string text = fmt::format(
        "GCN decoder check: SecondSonPC's decoder against the oracle's on the game's programs\n{} programs, {} "
        "instructions, {} operands compared; {} differences in {} kinds\n\n",
        programs, instructions, operands, total, diffs.size());
    for (const auto& [k, d] : diffs) {
        text += fmt::format("{:>8}  {}\n          e.g. {}\n", d.count, k, d.example);
    }
    const auto report = cfg.logs_dir / "gcn_decoder_check.txt";
    std::ofstream(report, std::ios::binary | std::ios::trunc) << text;
    std::printf("SecondSonPC: GCN decoder check -> %s (%llu differences)\n", report.string().c_str(),
                static_cast<unsigned long long>(total));
    return total == 0 ? 0 : 2;
}

} // namespace SecondSon::Bridge
