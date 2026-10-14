// SecondSonPC — reading a context's draw command buffer for the render-pass trace (P5.3A, diagnostics;
// docs/native-reconstruction/RENDER_ENGINE_PASSES.md §2). Read-only: the packets the game's Gnm code wrote are walked
// with the register state they set (context registers and the shader program registers), and every draw records the
// colour and depth targets it can write and the shaders it runs. Nothing here executes or changes a packet.
#pragma once

#include <array>
#include <span>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Engine::Render {

/// A colour or depth target as the registers at a draw describe it (GPU address = guest virtual address).
struct TargetDesc {
    u64 base = 0;
    u32 width = 0;
    u32 height = 0;
    u32 format = 0; ///< colour: CB_COLORn_INFO.FORMAT | NUMBER_TYPE << 8; depth: DB_Z_INFO.FORMAT
    bool depth = false;
    bool operator==(const TargetDesc&) const = default;
};

/// The register state carried from packet to packet (and from one command buffer to the next in submission order,
/// as on the GPU).
struct ScanState {
    u32 context[0x400] = {}; ///< context registers 0xA000..0xA3FF
    u64 vs = 0, ps = 0, es = 0, gs = 0, ls = 0, hs = 0, cs = 0; ///< shader program addresses
    u32 num_instances = 1;
};

/// What one stretch of command buffer did.
struct ScanResult {
    u32 packets = 0;
    u32 dwords = 0;
    u32 draws = 0;          ///< DRAW_INDEX_2 / DRAW_INDEX_OFFSET_2 / DRAW_INDEX_AUTO (+ indirect below)
    u32 draws_indirect = 0; ///< DRAW_INDIRECT / DRAW_INDEX_INDIRECT (+ multi)
    u32 draws_instanced = 0;
    u64 indices = 0;        ///< direct draws: index/vertex count x instances
    u32 dispatches = 0;     ///< DISPATCH_DIRECT / DISPATCH_INDIRECT
    u32 draws_depth_test = 0;
    u32 draws_depth_write = 0;
    u32 draws_no_color = 0; ///< draws that write no colour target (depth-only or stencil)
    u32 events = 0;         ///< EVENT_WRITE / EVENT_WRITE_EOP / EOS / RELEASE_MEM
    u32 cache_actions = 0;  ///< ACQUIRE_MEM / SURFACE_SYNC
    u32 waits = 0;          ///< WAIT_REG_MEM
    u32 dma = 0;            ///< DMA_DATA / CP_DMA / WRITE_DATA (CP_DMA_READ_TAGS prefetches included)
    u32 indirect_buffers = 0;
    u32 malformed = 0;      ///< the walk stopped at a packet it could not decode
    std::vector<TargetDesc> targets; ///< distinct targets written by the draws (colour, then depth when written)
    std::vector<TargetDesc> depth_read; ///< depth targets tested but not written
    std::vector<u64> shaders;           ///< distinct shader programs (PS, VS, ..., CS) the draws/dispatches ran
    // P5.3D: what PM4 a stretch still holds (type-3 packets by opcode, register writes by register) and the native
    // operations SecondSonPC wrote in it (NOPs with kMarkerMagic, by OpKind 1..3).
    std::array<u32, 256> opcode_packets{};
    std::array<u32, 0x400> context_writes{}; ///< SET_CONTEXT_REG, by offset from 0xA000
    std::array<u32, 0x400> sh_writes{};      ///< SET_SH_REG, by offset from 0x2C00
    std::array<u32, 0x400> uconfig_writes{}; ///< SET_UCONFIG_REG, by offset from 0xC000
    std::array<u32, 4> native_ops{};
};

/// Walks `stream` (whole packets, as the command buffer holds them) from `state`, updating both.
void ScanCommands(std::span<const u32> stream, ScanState& state, ScanResult& out);

/// Context register offsets (from 0xA000) the walk reads.
namespace Reg {
inline constexpr u32 DbZInfo = 0x010;
inline constexpr u32 DbZReadBase = 0x012;
inline constexpr u32 DbZWriteBase = 0x014;
inline constexpr u32 DbDepthSize = 0x016;
inline constexpr u32 CbTargetMask = 0x08E;
inline constexpr u32 DbDepthControl = 0x200;
inline constexpr u32 CbColorControl = 0x202;
inline constexpr u32 CbColor0Base = 0x318; ///< + 0xF per target: BASE, PITCH, SLICE, VIEW, INFO, ATTRIB
inline constexpr u32 CbColorStride = 0xF;
} // namespace Reg

} // namespace SecondSon::Engine::Render
