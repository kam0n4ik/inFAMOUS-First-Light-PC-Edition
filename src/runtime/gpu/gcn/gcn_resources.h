// SecondSonPC — where a GCN program's scalar data comes from (own shader translator, step 3: resources). A forward
// pass over the graph gives every SGPR one of: a dword of the flattened user data (flat[k]: k < 16 the user data
// registers, beyond them what the SRT walk copied), a constant, or unknown. Each s_load_dword[xN] through a pointer that
// is itself in flat becomes a load of SecondSonPC's SRT walk (copy N dwords from that pointer + offset to the end of
// flat), so its destinations are flat too; each buffer instruction's V# (4 SGPRs) is then a resource whose descriptor
// is found in flat (SharpLocation) - the runtime reads it there for the binding, the program reads its fields there.
// This is the oracle's ResourceDiscover + FlattenExtendedUserdata, without generating x86.
#pragma once

#include <array>
#include <span>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/gcn/gcn_cfg.h"
#include "runtime/gpu/gcn/gcn_decoder.h"

namespace SecondSon::Gpu::Gcn {

struct ScalarValue {
    enum class Kind : u8 { Unknown, Flat, Const };
    Kind kind = Kind::Unknown;
    u32 value = 0; ///< the flat dword index, or the constant
    bool operator==(const ScalarValue&) const = default;
};

/// One load of the walk: flat[dst .. dst + dwords) = memory[(flat[ptr] | flat[ptr + 1] << 32) + byte_offset ...].
struct WalkLoad {
    u32 ptr = 0;
    u32 byte_offset = 0;
    u32 dwords = 0;
    u32 dst = 0;
    bool operator==(const WalkLoad&) const = default;
};

/// An image the program reads or writes (MIMG): its T# dwords (8, or 4 with R128).
struct ImageUse {
    std::array<ScalarValue, 8> sharp{};
    bool r128 = false;
    bool written = false; ///< image_store / an atomic: a storage image
    bool array = false;   ///< the instructions' DA (an array view of an array type)
    bool sampled = false; ///< image_sample / image_gather on it
};

/// A sampler (S#: 4 dwords).
struct SamplerUse {
    std::array<ScalarValue, 4> sharp{};
};

/// A buffer the program reads or writes: its V# dwords.
struct BufferUse {
    std::array<ScalarValue, 4> sharp{};
    bool written = false;
};

struct ResourceInfo {
    u32 flat_dwords = 16;       ///< 16 + what the walk copies
    std::vector<WalkLoad> walk; ///< in order (a later load may use an earlier one's dwords as its pointer)
    std::vector<BufferUse> buffers;
    bool uses_gds = false;      ///< ds_append / ds_consume: the GDS buffer, at binding buffers.size() + 1
    bool uses_lds = false;      ///< the local data share (workgroup memory)
    std::vector<s32> buffer_of; ///< per instruction: its buffer (-1: none)
    std::vector<s32> load_of;   ///< per instruction (s_load_*): its walk load (-1: none)
    std::vector<ImageUse> images;     ///< bindings after the buffers (the flattened user data and GDS among them)
    std::vector<SamplerUse> samplers; ///< ... after the images
    std::vector<s32> image_of;        ///< per instruction (MIMG): its image (-1: none)
    std::vector<s32> sampler_of;      ///< per instruction (sampling MIMG): its sampler (-1: none)
    std::string error;          ///< a resource whose descriptor is not found (unknown SGPRs), and where
};

/// `user_sgprs`: how many SGPRs hold user data at the start (SPI_SHADER_PGM_RSRC2.USER_SGPR, at most 16).
bool AnalyzeResources(std::span<const Inst> insts, const Cfg& cfg, u32 user_sgprs, ResourceInfo& out);

/// The walk as x86 of the bridge's walker generator's forms (the runtime's SrtProgram interprets them): per load, the
/// chain of pointers from the user data (push rdi; mov rdi, [rdi + d]; mov r10, 2^48 - 1; and rdi, r10), its dwords
/// (mov r10d, [rdi + d]; mov [rsi + 4 * dst], r10d), pop rdi per pointer; ret. Empty for no loads. false with `error`
/// for a pointer that is not two dwords of the user data or of one earlier load.
bool BuildSrtWalker(const ResourceInfo& info, std::vector<u8>& out, std::string& error);

} // namespace SecondSon::Gpu::Gcn
