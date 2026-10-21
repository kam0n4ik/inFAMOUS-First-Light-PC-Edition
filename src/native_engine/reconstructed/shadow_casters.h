// SecondSonPC — R7-S: the hand-written reconstruction of the shadow pass's orchestrators (docs/NATIVE_ENGINE_R7_SHADOWS.md
// §2): eboot 0x43d590 "Shadow::CastersLoop" (the pass method that walks the sorted visible objects of one shadow
// context - a cascade - and draws them) and 0x43d480 "Shadow::InstancedDraw" (one instanced draw of a group).
//
// Our own C++, written from the disassembly (R5-F §2-3, R6-S §2), instruction order kept where it is observable:
//   - the calls (callee, arguments, order) - every side effect of these functions is a call (CUE/Gnm setters, the
//     material bind, the parameter writers, the frame and DCB allocators, the draws) or a copy into the context;
//   - the reads of game memory between the calls (a callee could change what is read after it);
//   - the floating point of the instance records (object rows x the cascade's view-projection): the original's
//     vmulps/vaddps with its operand order ({vex3}-encoded inline asm, as R5's ObjectConstants), so NaN payloads
//     come out bit for bit the same.
// The prefetch-only loads of the instanced branch (0x43e000..0x43e04c, results only fed to `nop [reg]`) are left out.
//
// The callees are called through a table: the eboot's entries (module + rva) live, the shadow_trace thunks while a
// verification records the calls (Redirects - the same contract as a recompiled module's recomp_set_redirects, so
// shadow_loop.cpp verifies this implementation with the machinery it has for the recompiled one). 0x43d480 is called
// at its entry: under CITY-OPT that is the shadow runs' probe, which then runs the original or the registry's
// dispatcher below it (shadow_instanced_draw) - the shadow runs keep every draw.
#pragma once

#include <string>

#include "base/abi.h"

namespace SecondSon::NativeEngine::Reconstructed::ShadowCasters {

// --- the callees (eboot offsets) ----------------------------------------------------------------------------------
inline constexpr u64 kDcbAlloc = 0x11be90;        ///< (dst, R, bytes): a DCB block at *dst, its V# at dst + 8
inline constexpr u64 kBindConstants = 0x11bed0;   ///< (block, stage, slot, R): a constant buffer into a CUE stage slot
inline constexpr u64 kObjectConstants = 0x11c890; ///< R5 ObjectConstants: the object block R + 0xd190
inline constexpr u64 kCameraConstants = 0x11cde0; ///< (xmm0, xmm1, rows, camera, flags, out): the block R + 0xd2f0
inline constexpr u64 kCommitDrawConstants = 0x120280; ///< (R)
inline constexpr u64 kSetBuffers = 0x16b70;       ///< (CUE, stage, slot, count, V#[count])
inline constexpr u64 kSetVertexBuffers = 0x16aa0; ///< (CUE, stage, slot, count, V#*)
inline constexpr u64 kMeshDraw = 0x1a1960;        ///< (pass, R, item): the mesh pass's single draw
inline constexpr u64 kFrameScratch = 0x1f0870;    ///< (frame, bytes, alignment): the frame's scratch allocator
inline constexpr u64 kInstancedDraw = 0x43d480;   ///< (pass, R, records, n, mesh) - under CITY-OPT's probe
inline constexpr u64 kMaterialBind = 0x448640;    ///< (pass, R, material)
inline constexpr u64 kParams = 0x450460;          ///< (records, R + 0xda18, atlas, view, table, buffers, [object])
inline constexpr u64 kParamsInstanced = 0x451290; ///< (records, R + 0xda18, atlas, view, table, mesh)
inline constexpr u64 kInstanceUv = 0x451700;      ///< (records, record, atlas, view, table, object)
inline constexpr u64 kPrng = 0x612110;            ///< (state): the quality drop's generator
inline constexpr u64 kInitBuffer = 0x61e260;      ///< Gnm initAsRegularBuffer (V#, base, stride, count)
inline constexpr u64 kStage = 0x27530;            ///< ShaderStage -> CUE stage
inline constexpr u64 kNumInstances = 0x6250b0;    ///< (DCB, n)
inline constexpr u64 kPrimType = 0x623740;        ///< (DCB, VGT_PRIMITIVE_TYPE)
inline constexpr u64 kIndexSize = 0x624f80;       ///< (DCB, INDEX_TYPE, cache policy)
inline constexpr u64 kCueCommit = 0x14ad0;        ///< (CUE)
inline constexpr u64 kDrawIndex = 0x623c60;       ///< (DCB, count, index base, flags)

// --- the data the loop reads (eboot offsets) ----------------------------------------------------------------------
inline constexpr u64 kPrngState = 0x7f92020;
inline constexpr u64 kFrame = 0xf1bcd0;         ///< the frame object (scratch allocator state)
inline constexpr u64 kQualityIndex = 0xf39978;  ///< s32: the quality row
inline constexpr u64 kQualityTable = 0xda08d0;  ///< s32 drop threshold, stride 0x14
inline constexpr u64 kOne = 0xaac76c;           ///< 1.0f
inline constexpr u64 kIdentityRows = 0xaacb30;  ///< rows 1..3 of the identity (16 bytes each)
inline constexpr u64 kSlotObjectBlock = 0xe15c40; ///< u32: the CUE slot of the object block
inline constexpr u64 kSlotCameraBlock = 0xe15c60; ///< u32: the CUE slot of the camera block

/// The module's callee entries (live table); false with `error` when module_base is 0.
bool Prepare(u64 module_base, std::string& error);
u64 ModuleBase();

/// 0x43d590 (pass, args = {R, scene, _, lo | hi << 32}) and 0x43d480 (pass, R, records, n, mesh), System V.
SSPC_SYSV_ABI void Loop(u64 pass, u64 args);
SSPC_SYSV_ABI void Draw(u64 pass, u64 r, u64 records, u64 n, u64 mesh);

/// The calling thread's callees: from[i] (module + rva) -> to[i]; n == 0 back to the module's entries. Returns 1.
int Redirects(const u64* from, const u64* to, u32 n);

/// SSPC_R7_SHADOWS_DEDUP's scan (tests): what the PM4 packets in [from, to) leave of a shadow draw's state - bit 0
/// primitive / index type untouched, bit 1 NUM_INSTANCES untouched; 0 when a packet is not understood.
u32 UntouchedState(u64 from, u64 to);

} // namespace SecondSon::NativeEngine::Reconstructed::ShadowCasters
