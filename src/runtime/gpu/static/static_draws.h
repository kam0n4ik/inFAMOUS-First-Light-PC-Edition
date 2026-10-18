// SecondSonPC — the game's material draws as its packs describe them, read without running the game
// (docs/REA_ENGINE_REVERSE.md §8). What the engine binds for a material draw is in the KCAP packs: the program object
// (passes, their VS/PS Gnmx structs, fetch shader, API-slot tables, named resources), the material instances (their
// textures, whose T# are in the packs too) and the geometry (its V# array, its material instance). Reversed in the
// eboot (0x4468e0 -> 0x447190 builds a pass's slot tables at load, 0x448640 binds them per draw) and checked against
// earlier runs' binding logs and shader caches. The static predictor (static_predictor.h) turns this into translation
// requests; nothing here translates or touches the GPU. Research tools with the same logic: E:\research\rea\q\
// (material_db.py, shader_structs.py).
//
// KCAP: header (header_size at 0x28, segment count at 0xA0, segment table at 0xC8, 0x28 bytes each: pool u16 at +2,
// size u32 at +4, offset u64 at +8); a pool-2 segment is a chunk stream {u32 tag, u32 size} with KNLI (internal links:
// kind 0/3 pointer, 1 T#, 2 V#, 4 GPU address >> 8; locations relative to the data start = header_size) and DIC (type
// fixups {u32 location, u32, u64 class hash}).
#pragma once

#include <array>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Gpu::Static {

/// A VS or PS as a pass binds it: its program bytes (code up to and with its footer) and its Gnmx struct.
struct StaticShader {
    u64 footer_hash = 0;
    bool ps = false;
    bool cs = false;             ///< a compute shader (its struct's NUM_THREAD_X/Y/Z in regs[0..2])
    std::vector<u8> image;       ///< the program from its first instruction to the end of its footer
    u32 rsrc1 = 0, rsrc2 = 0;
    /// PS: Z_FORMAT, COL_FORMAT, PS_INPUT_ENA, PS_INPUT_ADDR, PS_IN_CONTROL, BARYC_CNTL, DB_SHADER_CONTROL, CB_SHADER_MASK
    /// VS: VS_OUT_CONFIG, POS_FORMAT, PA_CL_VS_OUT_CNTL
    std::array<u32, 8> regs{};
    std::vector<u32> slots;      ///< input usage slots {u8 type, u8 API slot, u8 start register, u8 flags}
    std::vector<u16> ps_inputs;  ///< PS input semantics (semantic | flags << 8)
    std::vector<u16> vs_exports; ///< VS export semantics (semantic | out index << 8)
};

/// A pass's resource: a pair {stage, id} of the pass with its program record (name, API slot).
struct StaticSlot {
    u8 stage = 0;      ///< 0 VS, 3 PS
    u8 api = 0;
    s16 material = -1; ///< the program's material resource index (instance binding), -1: named (engine global)
    u64 name = 0;
};

struct StaticPass {
    u64 cls = 0;        ///< the runtime pass class (DIC hash at pass +0x20)
    u32 vs = 0, ps = 0; ///< StaticDraws::Shaders() index + 1 (0: none)
    u32 fetch = 0;      ///< StaticDraws::Fetches() index + 1 (0: none)
    std::vector<StaticSlot> textures, buffers;
};

struct StaticProgram {
    u32 pack = 0;
    u32 first_pass = 0, passes = 0;
};

struct StaticInstance {
    u32 program = 0;
    std::vector<u32> textures; ///< StaticDraws::Textures() index + 1 per material texture (0: not found)
};

struct StaticGeometry {
    u32 instance = 0;
    std::vector<std::array<u32, 4>> streams; ///< the V# array
};

struct StaticScanStats {
    u64 packs = 0, bytes = 0, unreadable = 0;
    u64 programs = 0, passes = 0, instances = 0, geometries = 0, textures = 0, shaders = 0, fetches = 0, computes = 0;
    u64 instance_textures_missing = 0;
    double ms = 0.0;
};

class StaticDraws {
public:
    /// Reads every .xpps under `game_dir` (nothing is written there). `progress(done, total)` now and then.
    bool Scan(const std::filesystem::path& game_dir, StaticScanStats& stats, std::string& error,
              void (*progress)(void* ctx, u64 done, u64 total) = nullptr, void* ctx = nullptr);

    const std::vector<StaticShader>& Shaders() const { return shaders_; }
    /// The compute shaders of every pack (Shaders() indices): bound by the engine's code, not by a program object.
    const std::vector<u32>& Computes() const { return computes_; }
    const std::vector<std::vector<u32>>& Fetches() const { return fetches_; }
    const std::vector<std::array<u32, 8>>& Textures() const { return textures_; }
    const std::vector<StaticProgram>& Programs() const { return programs_; }
    const std::vector<StaticPass>& Passes() const { return passes_; }
    const std::vector<StaticInstance>& Instances() const { return instances_; }
    const std::vector<StaticGeometry>& Geometries() const { return geometries_; }
    const std::vector<std::string>& Packs() const { return packs_; }

private:
    void ScanPack(const std::vector<u8>& data, u32 pack, StaticScanStats& stats);

    std::vector<std::string> packs_;
    std::vector<StaticShader> shaders_;
    std::unordered_map<u64, u32> shader_by_hash_;
    std::vector<u32> computes_;
    std::vector<std::vector<u32>> fetches_;
    std::unordered_map<u64, u32> fetch_by_hash_;
    std::vector<std::array<u32, 8>> textures_;
    std::unordered_map<u64, u32> texture_by_id_;
    std::vector<StaticProgram> programs_;
    std::vector<StaticPass> passes_;
    std::vector<StaticInstance> instances_;
    std::vector<StaticGeometry> geometries_;
    /// instance texture ids not resolved in their own pack (a texture may sit in a later one)
    struct PendingTexture {
        u32 instance, binding;
        u64 id;
    };
    std::vector<PendingTexture> pending_;
};

} // namespace SecondSon::Gpu::Static
