// SecondSonPC R7-G — the native geometry pass: eboot 0x1a2030 "GBuffer::MeshPass" rebuilt from components
// (docs/NATIVE_ENGINE_R7_GBUFFER.md §2). Our names; offsets are the game's.
//
// 0x1a2030(M, chunk) [A]: M is the pass's material/setup object, chunk = {R (render context), S (the pass's object
// list), -, {lo, hi} (s32 pair: the range of sorted keys this job draws)}. S: *S the view, S + 0x4db8 the instances
// (0x80 bytes each), S + 0x4dc0 the sorted u64 keys (low u16: the instance; keys equal above bit 16: one mesh and
// material), S + 0x44f0 the pass's lighting data. M + 0x1f selects the variant:
//
//   per-object (M + 0x1f == 0; every object its own draw, 0x1a1960):
//     cull (dither != 0: 0x612110 the global random table, % 1025 < dither -> skipped) -> the object's mesh
//     (mesh + 0x18 == 0 -> skipped) -> material (0x448640 when it changes) -> light (M + 0x135: the light's rows,
//     0x1a0a80) -> skin V#s (M + 0x125 / 0x126: 0x61e260 + 0x16b70) -> pass 0x1d transform (0x1a1b90 + three streams)
//     -> object constants (M + 0x11c: 0x11c890 into R + 0xd190; upload 0x11be90 + copy 0x140 bytes, bind 0x11bed0 x2
//     when its changed byte says so) -> second constant block (M + 0x120: 0x11cde0 into R + 0xd2f0, 0x80 bytes) ->
//     0x446490, 0x445e00 -> draw constants (M + 0x128: 0x11e5d0; M + 0x90: 0x450460; M + 0x144: 0x1742f0) -> 0x120280
//     -> stencil (R + 0xde64 cache, 0x6229a0) -> [M + 0x124: render control 0x6270e0 around an extra 0x1a1960] ->
//     0x1a1960
//   instanced (M + 0x1f != 0; runs of equal keys are one 0x1a1770 draw):
//     group the run -> 0x1f0870 frame allocation of n * 0xD4 -> per member: cull as above, the instance record
//     (instance_records.h) and 0x451440 (M + 0x94) -> material (0x448640 on change) -> 0x451290 (M + 0x90) ->
//     [M + 0x124: as above around an extra 0x1a1770] -> 0x1a1770(M, R, records, n, mesh)
//
// Writes of its own: R + 0xd2e8 / 0xd388 (cleared after a bind), R + 0xde48 / 0xde60 (render control), R + 0xde64..67
// (stencil cache), the upload copies, the records. Everything else is calls; the template below makes them through a
// callee table - the game's functions (native) or the trace thunks (verification, gbuffer_mesh_pass.cpp).
#pragma once

#include <immintrin.h>

#include "base/abi.h"
#include "native_engine/r7/gbuffer/instance_records.h"

namespace SecondSon::NativeEngine::R7G {

/// The callees, System V as 0x1a2030 calls them (the registers it passes; garbage it leaves in others is not passed).
struct PassFns {
    SSPC_SYSV_ABI u32 (*random)(u64 table);                                     // 0x612110
    SSPC_SYSV_ABI void (*material)(u64 m, u64 r, u64 mat);                       // 0x448640
    SSPC_SYSV_ABI void (*light)(u64 m134, u64 r, u64 unused, u64 light);        // 0x1a0a80
    SSPC_SYSV_ABI void (*set_buffers)(u64 cue, u64 stage, u64 slot, u64 count, u64 src); // 0x16b70
    SSPC_SYSV_ABI void (*pass1d)(__m128 offset, u64 out, u64 md, u64 mi, u64 mesh);      // 0x1a1b90
    SSPC_SYSV_ABI void (*object_constants)(u64 world, u64 prev, u64 view, u64 mi, u64 light, u64 value, u64 flags,
                                           u64 block);                          // 0x11c890
    SSPC_SYSV_ABI void (*upload)(u64 slot, u64 r, u64 bytes);                    // 0x11be90
    SSPC_SYSV_ABI void (*bind)(u64 slot, u64 stage, u64 index, u64 r);          // 0x11bed0
    SSPC_SYSV_ABI void (*constants2)(__m128 a, __m128 b, u64 world, u64 cam, u64 value, u64 block); // 0x11cde0
    SSPC_SYSV_ABI void (*setup_446490)(u64 m140, u64 m, u64 r, u64 md, u64 mi); // 0x446490
    SSPC_SYSV_ABI void (*setup_445e00)(__m128 offset, u64 m12c, u64 m, u64 r, u64 world, u64 lighting, u64 md, u64 mi,
                                       u64 bounds);                              // 0x445e00
    SSPC_SYSV_ABI void (*draw_constants)(u64 block, u64 offset, u64 src, u64 count); // 0x11e5d0
    SSPC_SYSV_ABI void (*constants_450460)(u64 m90, u64 block, u64 m28, u64 lighting, u64 mat28, u64 md, u64 mi); // 0x450460
    SSPC_SYSV_ABI void (*constants_1742f0)(u64 m144, u64 r, u64 md, u64 block); // 0x1742f0
    SSPC_SYSV_ABI void (*commit_constants)(u64 r);                               // 0x120280
    SSPC_SYSV_ABI void (*stencil)(u64 gfx, u64 value);                           // 0x6229a0
    SSPC_SYSV_ABI void (*render_control)(u64 gfx, u64 value);                    // 0x6270e0
    SSPC_SYSV_ABI void (*mesh_draw)(u64 m, u64 r, u64 inst);                     // 0x1a1960
    SSPC_SYSV_ABI u64 (*frame_alloc)(u64 frame, u64 bytes, u64 align);           // 0x1f0870
    SSPC_SYSV_ABI void (*record_451440)(u64 m90, u64 rec, u64 m28, u64 lighting, u64 mat28); // 0x451440
    SSPC_SYSV_ABI void (*group_451290)(u64 m90, u64 block, u64 m28, u64 lighting, u64 mat28, u64 mesh); // 0x451290
    SSPC_SYSV_ABI void (*instanced_draw)(u64 m, u64 r, u64 records, u64 n, u64 mesh); // 0x1a1770
    // not recorded (pure: they write only memory the pass gives them)
    SSPC_SYSV_ABI void (*bounds_init)(u64 out);                                  // 0x26ac70
    SSPC_SYSV_ABI void (*init_buffer)(u64 vsharp, u64 base, u64 stride, u64 count); // 0x61e260
};

/// Module-relative data the loop reads.
inline constexpr u64 kFrameObject = 0xf1bcd0;
inline constexpr u64 kRandomTable = 0x7f92020;
inline constexpr u64 kDitherTable = 0xda08c0;  ///< + 0x10 + 0x14 * frame[+0x1dca8]: s32 cull threshold of 1025
inline constexpr u64 kFrameDitherIndex = 0x1dca8;
inline constexpr u64 kLightTable = 0x7dff1e0 + 0x1c0; ///< light pointers by the instance's s32 +0x28
inline constexpr u64 kObjectSlot = 0xe15c40;    ///< u32: the object constants' buffer slot
inline constexpr u64 kObjectSlot2 = 0xe15c60;   ///< u32: the second block's slot

/// Coverage / measurement counters of one pass call (summed by the caller).
enum PassStat : u32 {
    kPsCalls,
    kPsCallsInstanced,
    kPsObjects,        ///< keys visited
    kPsCulled,         ///< the dither dropped it
    kPsEmptyMesh,      ///< mesh + 0x18 == 0
    kPsDrawn,          ///< 0x1a1960 objects / instanced records
    kPsMaterials,      ///< 0x448640 calls
    kPsLight,          ///< 0x1a0a80 calls
    kPsLightRows,      ///< ... with a light (index >= 0)
    kPsSkin,           ///< M + 0x125 path
    kPsSkin2,
    kPsPass1d,
    kPsPass1dSkip,
    kPsPass1dStreams,
    kPsObjConst,       ///< 0x11c890 calls
    kPsObjUpload,      ///< changed: upload + bind
    kPsObjRebind,      ///< only rebind
    kPsConst2,
    kPsConst2Upload,
    kPsConst2Rebind,
    kPsDrawConst,      ///< 0x11e5d0 (M + 0x128)
    kPs450460,
    kPs1742f0,
    kPsStencil,
    kPsDouble,         ///< M + 0x124: two draws
    kPsGroups,         ///< instanced groups drawn
    kPsGroupsEmpty,    ///< groups whose members were all culled
    kPsAllocFail,
    kPs451440,
    kPs451290,
    kPsSameWorld,      ///< G3: an object whose world rows equal the previous object's
    kPsMaterialRepeat, ///< G3: a material set again that this call set before (not the previous one)
    kPsStats
};
inline constexpr const char* kPassStatNames[kPsStats] = {
    "calls", "calls_instanced", "objects", "culled", "empty_mesh", "drawn", "materials", "light", "light_rows",
    "skin", "skin2", "pass1d", "pass1d_skip", "pass1d_streams", "objconst", "objconst_upload", "objconst_rebind",
    "const2", "const2_upload", "const2_rebind", "drawconst", "c450460", "c1742f0", "stencil", "double", "groups",
    "groups_empty", "alloc_fail", "r451440", "g451290", "same_world", "material_repeat"};

struct PassCounters {
    u64 v[kPsStats] = {};
    void Add(PassStat k, u64 n = 1) {
        v[k] += n;
    }
};

/// Options of the native implementation that change how (not what) it does its work (G3/G4; off: the original's
/// order of memory accesses).
struct PassOptions {
    u32 prefetch = 0; ///< > 0: prefetch the instance, its mesh instance and mesh this many keys ahead
    u32 fault = 0;    ///< tests: 1 no stencil cache write, 2 a wrong record field
};

/// The pass. `module`: the eboot base (constants, globals). `c`: LoadPassConstants(module). Counters when `stats`.
void RunPass(const PassFns& f, u64 module, const PassConstants& c, u64 m, u64 chunk, const PassOptions& o,
             PassCounters* stats);

} // namespace SecondSon::NativeEngine::R7G
