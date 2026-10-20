// SecondSonPC — reconstructions of the mesh pass's two draw functions, eboot 0x1a1770 and 0x1a1960 (R6-G;
// docs/NATIVE_RENDER_R6_GBUFFER.md). No symbols in the eboot; our names. Analysis: docs/NATIVE_RECOMP_R5_E_GBUFFER.md.
//
// 0x1a1770 "GBuffer::InstancedDraw" (rdi material M, rsi context R, rdx instance records, ecx n, r8 mesh) [A]:
//   M+0x1c (u16) low byte != 0: v = lo << 16 | (hi | mesh+0x1d) << 24 | 0xff00; v != u32 R+0xde64:
//       bytes R+0xde64.. = {0, 0xff, lo, hi | mesh+0x1d}; 0x6229a0(R+8, v)       DB_STENCILREFMASK (SET_CONTEXT_REG)
//   0x120280(R)                                                                  draw constants (when dirty)
//   M+0x135 != 0xff: V# = 0x45f590(eboot+0x7dff738, ~(mesh+0x18 >> 27) & 1)   (RAX:RDX; for a table entry 0 it
//       returns the stack guard and the caller's RDX - not reproducible: such a call runs the original)
//       M+0x13c != 0xff: 0x16b70(R+0x128, 2, M+0x13c, 1, &V#)
//   V# = 0; 0x61e260(&V#, records, 0xd4, n); 0x16b70(R+0x128, 0x27530(0), M+0x127, 1, &V#)   instance records' V#
//   0x16aa0(R+0x128, 2, 0, mesh+0x6c (u8), mesh+0x60)                              vertex streams
//   0x6250b0(R+8, n); 0x623740(R+8, 4); 0x624f80(R+8, 0, 2)                       NUM_INSTANCES, primitive, index type
//   u8 eboot+0x1a0cbd5 != 0: 0x628ae0(R+8, mesh+0x58 << 16)
//   count = mesh+0x68, indices = mesh+0x58 (read before the commit); 0x14ad0(R+0x128); 0x623c60(R+8, count, indices, 0)
//
// 0x1a1960 "GBuffer::MeshDraw" (rdi material M, rsi context R, rdx instance I (0x80-byte object record)) [A]:
//   mesh = **(I+0x18); skin = I+0x20; n = mesh+0x6c (u8)
//   skin && (u8 skin & 7): the n stream V#s copied to the stack (memmove n*16 into 0x200 bytes) and replaced by the
//       skin's V#s skin+0x28 / +0x38 / +0x48 at s8 indices k0, k1, k2 = *(*(skin+8)+0x58) [0..2] (k1, k2 < 0: kept);
//       table = the copy; else table = mesh+0x60
//   0x16aa0(R+0x128, 2, 0, n, table)
//   M+0xd8 != 0xff: 0x16b70(R+0x128, 2, M+0xd8, 1, skin ? skin+0x58 : *(GOT 0xd9dec8)+0x1f6c)
//   (*(M+0xf0))+0xf & 2: 0x16aa0(R+0x128, 2, (s8) (*(*(M+0x10)+0x58))+0x22, 1,
//                                skin && skin+0x20 ? skin+0xc0 : *(GOT 0xd9dec8)+0x1f6c)
//   0x623740(R+8, 4); 0x624f80(R+8, 0, 2); [0x628ae0 as above]; 0x14ad0(R+0x128); 0x623c60(R+8, mesh+0x68, mesh+0x58, 0)
//
// Writes of their own: 0x1a1770 the four stencil bytes R+0xde64..0xde67; 0x1a1960 only its stack. Everything else is
// calls. The stack guard (GOT 0xd9de00) is read and checked as in any function with a stack buffer.
//
// Implementations: the hand-written one below (Native), or the recompiled recomp_1a1770 / recomp_1a1960
// (SSPC_R6G_IMPL=recomp with SSPC_NATIVE_ENGINE_RECOMP). A call the hand-written one does not follow (more than 32
// streams, a skin index outside the copy, an empty vertex-format table entry) runs the original.
//
// Verification (gbuffer_trace.h): the original as a clone whose recorded callees go to the trace thunks, the
// implementation with the same thunks, both on a shadow context (the stencil bytes copied in); the traces and the
// stencil bytes are compared, then the original runs (verify) - or the implementation (guarded).
//
// Measurement (SSPC_R6G_STATS=1, docs/NATIVE_RENDER_R6_GBUFFER.md §G2): the hand-written path also counts per call
// what it does and what the CUE commits (which stages are dirty, how large their tables are, whether a committed
// table equals the previous one) and times the call, the commit and the draw; a reporter logs it every 10 s.
#pragma once

#include <string>

#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/gbuffer_trace.h"

namespace SecondSon::NativeEngine::Reconstructed {

extern const FunctionSpec kGBufferInstancedDrawSpec; ///< 0x1a1770, key gbuffer_instanced_draw
extern const FunctionSpec kGBufferMeshDrawSpec;      ///< 0x1a1960, key gbuffer_mesh_draw

namespace GBufferDraw {

using InstancedFn = SSPC_SYSV_ABI void (*)(u64 material, u64 r, u64 records, u64 n, u64 mesh);
using MeshFn = SSPC_SYSV_ABI void (*)(u64 material, u64 r, u64 instance);
using RedirectsFn = int (*)(const u64* from, const u64* to, u32 n); ///< recomp_set_redirects

inline constexpr u64 kCue = 0x128;          ///< R + 0x128: the context's CUE
inline constexpr u64 kGfx = 0x8;            ///< R + 8: the context's Gnm DCB
inline constexpr u64 kStencilState = 0xde64; ///< R: the last stencil value written (u32)
inline constexpr u64 kShadowBytes = 0xde80; ///< a shadow context (what the functions address in R, rounded up)
inline constexpr u32 kMaxStreams = 32;      ///< 0x1a1960's stack copy (0x200 bytes)

/// The callees in the order of the trace's callee table.
enum Callee : u32 {
    kStencil = 0,   ///< 0x6229a0 (gfx, value)
    kDrawConstants, ///< 0x120280 (R)
    kSetBuffers,    ///< 0x16b70 (cue, stage, slot, count, src)
    kSetVertex,     ///< 0x16aa0 (cue, stage, slot, count, src)
    kNumInstances,  ///< 0x6250b0 (gfx, n)
    kPrimitive,     ///< 0x623740 (gfx, type)
    kIndexType,     ///< 0x624f80 (gfx, a, b)
    kDebugMarker,   ///< 0x628ae0 (gfx, value)
    kCommit,        ///< 0x14ad0 (cue)
    kDrawIndex,     ///< 0x623c60 (gfx, count, indices, flags)
    kStackFail,     ///< 0x984360 __stack_chk_fail
    kMemmove,       ///< 0x984430 memmove (not recorded: performed)
    kVsharp,        ///< 0x45f590 (table, which) -> a V# in RAX:RDX (pure: recorded, returns a fixed sequence)
    kCallees
};
extern const GTrace::CalleeSpec kCalleeSpecs[kCallees];

/// The hand-written reconstructions (System V; counted by the dispatcher).
SSPC_SYSV_ABI void InstancedNative(u64 material, u64 r, u64 records, u64 n, u64 mesh);
SSPC_SYSV_ABI void MeshNative(u64 material, u64 r, u64 instance);

/// For tests and the install: callee addresses at module_base, the recording clones, the recompiled implementations
/// when selected. False with `error` when a clone cannot be made.
bool Prepare(u64 module_base, std::string& error);
InstancedFn RecordingInstanced();
MeshFn RecordingMesh();
/// The recompiled implementations (nullptr: the hand-written ones).
void UseRecompiled(InstancedFn instanced, MeshFn mesh, RedirectsFn redirects);

/// One comparison of the original (recording clone) with the implementation on the calling thread: "" when equal,
/// else the first difference; `skipped` when the call cannot be compared (a case the implementation does not follow,
/// a trace overflow).
std::string CheckInstanced(u64 material, u64 r, u64 records, u64 n, u64 mesh, bool& skipped);
std::string CheckMesh(u64 material, u64 r, u64 instance, bool& skipped);

/// Generated cases, as the install's self-check (also the offline tests): "" or the first failure.
std::string SelfCheckInstanced(u32 cases, u64 seed);
std::string SelfCheckMesh(u32 cases, u64 seed);

/// The GOT slots the functions read, while a check runs in the loader's window or on an unrelocated image (before
/// relocation they are 0 - cue_commit.cpp): the stack guard (-> a guard value), the defaults' global (-> a zeroed
/// block), the vertex-format table's two entries (-> 1). Restored on destruction; slots already set are left alone.
class GotForChecks {
public:
    explicit GotForChecks(u64 module);
    ~GotForChecks();
    GotForChecks(const GotForChecks&) = delete;
    GotForChecks& operator=(const GotForChecks&) = delete;
    bool ok() const {
        return ok_;
    }

private:
    static constexpr u32 kSlots = 4;
    u64* at_[kSlots] = {};
    u64 saved_[kSlots] = {};
    bool ok_ = false;
};

/// Tests only: 1 - the hand-written 0x1a1770 passes a wrong index type; 2 - the hand-written 0x1a1960 skips the
/// primitive type; 0 - none.
void InjectFault(u32 fault);

/// SSPC_R6G_STATS: per-call measurement in the hand-written path.
void EnableStats(bool on);
bool StatsEnabled();
/// The measurement so far, as log text (summed over threads; per frame when `frames` > 0).
std::string StatsText(double frames, bool reset);

} // namespace GBufferDraw

/// The current frame (VideoOut flip id) in the game; 0 in the tests (gbuffer_frames*.cpp).
u64 R6GFrameId();

} // namespace SecondSon::NativeEngine::Reconstructed
