// SecondSonPC — modules of the game's material draws translated before the game meets them (docs/REA_ENGINE_REVERSE.md
// §8; docs/OFFLINE_SHADER_PRECOMPILATION.md §6 item 3). For every pass of every program object in the packs
// (static_draws.h) it builds the translation request a draw of that pass would give the frontend - the program, its
// runtime parameters, its user data and the memory behind them - translates it and adds the module to the persistent
// shader cache exactly as the frontend adds one it translated (the first module of a runtime as the layout). A draw
// that meets the program later finds it there; a prediction that misses costs nothing but its record (the draw
// translates as before).
//
// Where each input comes from:
//   - program, RSRC1/2, the PS's input/export registers, PS_INPUT_CNTL (the pass's VS export / PS input semantics),
//     VS_OUT_CNTL: the packs (checked: every material pass pair of three scenes' caches exact);
//   - material textures: the instances' T# (packs); named (engine) textures / constant buffers, samplers and the pass
//     class's render state (targets, clip, instancing): a template file (E:\research\rea\q\templates.py,
//     state_table.py) - the engine's fixed bindings, taken from earlier runs' logs and caches;
//   - vertex buffers: the geometry's V# array and the pass's fetch shader (packs).
// The user data follow the Gnmx convention the usage slots describe: a start register below 16 is that register, a
// higher one is that dword - 16 of the extended user data, whose address a slot of type 27 holds.
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/static/static_draws.h"

namespace SecondSon::Gpu::Shader {
class ShaderTranslator;
class ShaderDiskCache;
struct DeviceProfile;
} // namespace SecondSon::Gpu::Shader

namespace SecondSon::Gpu::Static {

struct StaticTemplates {
    std::unordered_map<u64, std::array<u32, 8>> textures; ///< named texture -> T# (usage type 0/4, 8 dwords)
    std::unordered_map<u64, std::array<u32, 4>> buffers;  ///< named buffer resource -> V# (usage type 0/4, 4 dwords)
    std::array<u32, 4> constant_buffer{};                 ///< usage type 2 (one form in every log)
    std::vector<std::array<u32, 4>> samplers;             ///< most frequent first
    struct State {
        u64 cls = 0;
        bool ps = false;
        u32 col_format = 0;
        u32 count = 0;
        std::vector<u8> runtime; ///< RuntimeParams bytes, the shader's own fields zero
    };
    std::vector<State> states; ///< most frequent first within a (role, class, COL_FORMAT)
    bool Load(const std::filesystem::path& file, std::string& error);
};

struct PredictOptions {
    u32 threads = 0;                ///< 0: hardware threads - 1
    u32 max_state_variants = 3;     ///< per (role, class[, COL_FORMAT])
    u32 max_material_variants = 16; ///< distinct material texture formats per program
    u32 max_vertex_variants = 16;   ///< distinct vertex stream formats per program
    std::filesystem::path report;   ///< TSV of the predicted modules (empty: none)
};

struct PredictStats {
    u64 requests = 0, duplicate_requests = 0, translated = 0, failed = 0, added = 0, layouts = 0;
    u64 passes = 0, passes_no_state = 0, passes_no_instance = 0;
    u64 named_textures_unknown = 0, named_buffers_unknown = 0;
    u64 resumed = 0;  ///< already in the cache (an earlier, interrupted run): read, not translated
    u64 skipped = 0;  ///< programs on the skip list
    u64 walk_key_mismatches = 0; ///< the key from the SRT walk over the synthetic memory differs from the translation's
    double ms = 0.0;
    std::string first_error;
};

/// The whole run: the packs under `game_dir` scanned, `templates` loaded, the modules predicted into `disk`; what it did
/// in `summary` (two lines). false: nothing was predicted (`summary` says why).
bool RunPrediction(const std::filesystem::path& game_dir, const std::filesystem::path& templates,
                   Gpu::Shader::ShaderTranslator& translator, const Gpu::Shader::DeviceProfile& profile,
                   Gpu::Shader::ShaderDiskCache* disk, const PredictOptions& options, std::string& summary);

/// Translates the predicted modules and adds them to `disk` (nullptr: translated and reported only).
bool PredictModules(const StaticDraws& draws, const StaticTemplates& templates, Gpu::Shader::ShaderTranslator& translator,
                    const Gpu::Shader::DeviceProfile& profile, Gpu::Shader::ShaderDiskCache* disk, const PredictOptions& options,
                    PredictStats& stats);

} // namespace SecondSon::Gpu::Static
