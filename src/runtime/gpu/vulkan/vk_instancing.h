// SecondSonPC - CITY-OPT Phase 4 (docs/CITY_DRAW_PRECOMPUTE.md): draws that differ in their constants too, recorded
// as one multi-draw (SSPC_CITY_INSTANCING).
//
// The translator (gcn_emit.cpp, Emitter::BufOffset) addresses a guest buffer's dwords at
//   (byte offset + bitfieldExtract(push.buf_offsets[k / 4], (k % 4) * 8, 8)) >> 2
// - buf_offsets[k] being the bytes the executor moved the descriptor's start down for buffer k. A series of draws that
// differ in one constant buffer (the engine's per-object block: a shadow caster's transform) becomes one
// vkCmdDrawMultiIndexedEXT when every draw's block is copied at `stride` after the one before it in one buffer and the
// stage reads its block at DrawIndex * stride further: the patch adds exactly that to each such extract, nothing else.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "base/abi.h"

namespace SecondSon::Gpu::Vk {

struct InstancedBuffer {
    u32 index = 0;  ///< the buffer's buf_offsets index (the stage's first buffer + its number)
    u32 stride = 0; ///< bytes between two draws' copies (a multiple of 4)
};

/// `spirv` (one translated vertex stage, one function) with every buf_offsets extract of `buffers` moved by
/// DrawIndex * stride. false with `why`: not the translator's form (another function, no push constant block, an
/// extract of another shape for one of them, no such extract at all).
bool PatchDrawIndexOffsets(std::span<const u32> spirv, std::span<const InstancedBuffer> buffers, std::vector<u32>& out,
                           std::string& why);

} // namespace SecondSon::Gpu::Vk
