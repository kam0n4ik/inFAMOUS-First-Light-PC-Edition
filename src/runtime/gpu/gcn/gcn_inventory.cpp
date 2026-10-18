// SecondSonPC — `--gcn-inventory`: every GCN program of the game (the offline index, --scan-game-shaders) decoded by
// SecondSonPC's decoder, read only from the packs: which instructions the game's shaders use, how often, in how many
// programs - the scope of SecondSonPC's own translator - and every program the decoder cannot split.
#include "runtime/gpu/gcn/gcn_inventory.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <vector>

#include <fmt/format.h>

#include "runtime/gpu/gcn/gcn_decoder.h"
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
        if (!Decode(code, insts, why)) {
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
    text += fmt::format("\nunknown opcodes: {}\ndecode failures: {}\n", unknown, failures.size());
    for (const auto& s : failures) {
        text += "  " + s + "\n";
    }
    std::ofstream out(report, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

} // namespace SecondSon::Gpu::Gcn
