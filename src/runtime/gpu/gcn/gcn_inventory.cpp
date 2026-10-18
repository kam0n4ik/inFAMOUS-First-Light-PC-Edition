// SecondSonPC — `--gcn-inventory`: every GCN program of the game (the offline index, --scan-game-shaders) decoded by
// SecondSonPC's decoder, read only from the packs: which instructions the game's shaders use, how often, in how many
// programs - the scope of SecondSonPC's own translator - and every program the decoder cannot split.
#include "runtime/gpu/gcn/gcn_inventory.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <vector>

#include <fmt/format.h>

#include "runtime/gpu/gcn/gcn_cfg.h"
#include "runtime/gpu/gcn/gcn_decoder.h"
#include "runtime/gpu/gcn/gcn_structure.h"
#include "runtime/gpu/shader/game_shader_index.h"

namespace SecondSon::Gpu::Gcn {

bool RunInventory(const std::filesystem::path& game_dir, const std::filesystem::path& cache_dir,
                  const std::filesystem::path& report, std::string& error) {
    Gpu::Shader::GameShaderIndex index;
    const std::string game = game_dir.filename().string();
    const auto idx = Gpu::Shader::NativeCacheRoot(cache_dir, game) / "game_programs.idx";
    if (!index.Load(idx, error)) {
        error = fmt::format("{} ({}; make it with --scan-game-shaders)", error, idx.string());
        return false;
    }
    struct Count {
        u64 uses = 0;
        u64 programs = 0;
    };
    std::map<std::pair<u32, u32>, Count> ops; // (format, opcode)
    std::vector<std::string> failures;
    u64 programs = 0, decoded = 0, instructions = 0, without_end = 0, trailing = 0;
    u64 cfg_failed = 0, blocks = 0, with_loops = 0, loops = 0, irreducible = 0, unreachable = 0, max_blocks = 0;
    std::vector<std::string> cfg_failures, irreducible_programs, shape_examples;
    std::string dump_text;
    u64 simple = 0, multi_latch = 0, multi_exit = 0, escaping = 0, selections = 0, shared = 0;
    Cfg cfg;
    std::vector<u32> code;
    std::vector<Inst> insts;
    std::map<u32, std::ifstream> files;
    for (const auto& p : index.Programs()) {
        ++programs;
        const auto& loc = index.Members()[p.first_member];
        auto& f = files[loc.pack];
        if (!f.is_open()) {
            f.open(game_dir / index.Packs()[loc.pack], std::ios::binary);
        }
        code.assign(p.length / 4, 0);
        f.clear();
        f.seekg(static_cast<std::streamoff>(loc.offset));
        f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(code.size() * 4));
        if (!f) {
            failures.push_back(fmt::format("{} @ {:#x}: unreadable", index.Packs()[loc.pack], loc.offset));
            continue;
        }
        insts.clear();
        std::string why;
        if (!Decode(code, insts, why, true)) {
            failures.push_back(fmt::format("{} @ {:#x}: {}", index.Packs()[loc.pack], loc.offset, why));
            continue;
        }
        ++decoded;
        instructions += insts.size();
        const Inst& last = insts.back();
        if (!(last.format == Format::Sopp && last.op == 1)) {
            ++without_end;
        } else if (last.offset + last.dwords < code.size()) {
            ++trailing; // (data after s_endpgm: inline constants, padding)
        }
        std::string cfg_error;
        if (!BuildCfg(insts, cfg, cfg_error)) {
            ++cfg_failed;
            cfg_failures.push_back(fmt::format("{} @ {:#x}: {}", index.Packs()[loc.pack], loc.offset, cfg_error));
        } else {
            blocks += cfg.blocks.size();
            max_blocks = std::max<u64>(max_blocks, cfg.blocks.size());
            with_loops += cfg.loops > 0 ? 1 : 0;
            loops += cfg.loops;
            unreachable += cfg.unreachable;
            if (!cfg.Reducible()) {
                ++irreducible;
                irreducible_programs.push_back(fmt::format("{} @ {:#x}", index.Packs()[loc.pack], loc.offset));
            } else {
                const Structure st = AnalyzeStructure(cfg);
                // SSPC_GCN_DUMP=<pack>@<hex offset>: that program's blocks, edges and instructions in the report
                if (const char* dump = std::getenv("SSPC_GCN_DUMP");
                    dump != nullptr && fmt::format("{}@{:x}", index.Packs()[loc.pack], loc.offset) == dump) {
                    for (u32 k = 0; k < cfg.blocks.size(); ++k) {
                        const Block& bk = cfg.blocks[k];
                        dump_text += fmt::format("block {} [{}..{}] idom {} ipdom {} loop {} -> {} {} {}{}\n", k, bk.first,
                                                 bk.last, cfg.idom[k], st.ipdom[k], st.loop_of[k], bk.taken,
                                                 CondName(bk.cond), bk.next, bk.exit ? " EXIT" : "");
                        for (u32 i = bk.first; i <= bk.last; ++i) {
                            dump_text += fmt::format("    {:5} {}\n", insts[i].offset,
                                                     Mnemonic(insts[i].format, insts[i].op));
                        }
                    }
                    for (const auto& note : st.notes) {
                        dump_text += "  note: " + note + "\n";
                    }
                }
                simple += st.Simple() ? 1 : 0;
                multi_latch += st.multi_latch_loops;
                multi_exit += st.multi_exit_loops;
                escaping += st.escaping_selections;
                shared += st.shared_merges;
                for (const s32 m : st.selection_merge) {
                    selections += m != -1 ? 1 : 0;
                }
                if (!st.Simple() && shape_examples.size() < 40) {
                    shape_examples.push_back(fmt::format("{} @ {:#x}: {}", index.Packs()[loc.pack], loc.offset,
                                                         st.notes.empty() ? "" : st.notes.front()));
                }
            }
        }
        std::map<std::pair<u32, u32>, bool> seen;
        for (const Inst& in : insts) {
            const auto key = std::make_pair(static_cast<u32>(in.format), static_cast<u32>(in.op));
            ++ops[key].uses;
            if (!seen[key]) {
                seen[key] = true;
                ++ops[key].programs;
            }
        }
    }
    std::vector<std::pair<std::pair<u32, u32>, Count>> rows(ops.begin(), ops.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.programs > b.second.programs; });
    u64 unknown = 0;
    std::string text = fmt::format(
        "GCN inventory of the game's programs (SecondSonPC decoder): {} programs, {} decoded, {} instructions; {} "
        "without s_endpgm, {} with data after it; {} distinct opcodes\n\n{:<8} {:<34} {:>10} {:>10}\n",
        programs, decoded, instructions, without_end, trailing, rows.size(), "format", "opcode", "programs", "uses");
    for (const auto& [key, c] : rows) {
        const auto f = static_cast<Format>(key.first);
        const std::string_view name = Mnemonic(f, key.second);
        if (name.empty()) {
            ++unknown;
        }
        text += fmt::format("{:<8} {:<34} {:>10} {:>10}\n", FormatName(f),
                            name.empty() ? fmt::format("UNKNOWN #{}", key.second) : std::string(name), c.programs,
                            c.uses);
    }
    text += fmt::format("\ncontrol flow: {} blocks (at most {} in one program); {} programs with loops ({} loops); {} "
                        "irreducible programs; {} unreachable blocks; {} graphs not built\n",
                        blocks, max_blocks, with_loops, loops, irreducible, unreachable, cfg_failed);
    text += fmt::format("structure: {} programs as SPIR-V needs them; {} selections; to rewrite: {} loops with "
                        "several latches, {} loops exiting to several blocks, {} selections left before their merge; {} "
                        "selections sharing their merge (given one of their own)\n",
                        simple, selections, multi_latch, multi_exit, escaping, shared);
    for (const auto& s : shape_examples) {
        text += "  shape: " + s + "\n";
    }
    for (const auto& s : cfg_failures) {
        text += "  not built: " + s + "\n";
    }
    for (const auto& s : irreducible_programs) {
        text += "  irreducible: " + s + "\n";
    }
    text += fmt::format("\nunknown opcodes: {}\ndecode failures: {}\n", unknown, failures.size());
    text += dump_text;
    for (const auto& s : failures) {
        text += "  " + s + "\n";
    }
    std::ofstream out(report, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

} // namespace SecondSon::Gpu::Gcn
