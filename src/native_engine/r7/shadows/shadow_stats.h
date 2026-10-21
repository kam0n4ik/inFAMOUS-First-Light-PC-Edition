// SecondSonPC — R7-S: what the shadow casters loop does per frame, measured from the hand-written reconstruction
// (docs/NATIVE_ENGINE_R7_SHADOWS.md §5). SSPC_R7_SHADOWS_STATS=1, off by default; costs nothing when off (one load and
// branch per loop call). Only the live calls count: a verification's recording runs (callees redirected) are skipped.
//
// The questions of the S3 task, answered with counts rather than assumptions:
//   - repeated matrices: (item, cascade view-projection) pairs computed more than once in a frame; items whose
//     records are computed in more than one loop call (cascades); view-projections shared by several calls;
//   - repeated material preparation: 0x448640 calls for the material the previous loop call on the same context
//     bound last (the loop forgets its "previous material" between calls);
//   - draw state: the instanced draws' primitive / index type / instance count packets that repeat the state the
//     previous shadow draw of the same loop call left (nothing written to the DCB in between: SSPC_R7_SHADOWS_DEDUP);
//   - the instancing itself: groups, draws, instances, objects dropped by the quality generator;
//   - CPU: the loop's inclusive time per call through the registry's dispatcher (native: the reconstruction; bench:
//     the original), SSPC_R7_SHADOWS_TIMING=1.
// A line every 10 s in the log ("R7-S stats"), per frame (the frame object's fence counter).
#pragma once

#include "base/abi.h"

namespace SecondSon::NativeEngine::R7Shadows::Stats {

struct Thread;

/// SSPC_R7_SHADOWS_STATS=1 (read once).
bool On();
/// SSPC_R7_SHADOWS_TIMING=1 (read once).
bool TimingOn();

/// The calling thread's block for one live loop call (counts the call); nullptr when off.
Thread* Begin(u64 module, u64 r, bool instanced, const float* view_projection);
void SingleDraw(Thread& t);
/// A drawn group: `instances` records of `objects` keys.
void Group(Thread& t, u64 instances, s32 objects);
/// The record of `item` (its address) for the call's view-projection.
void Record(Thread& t, u64 item);
/// A material bind of the loop (`first`: the call's first).
void MaterialBind(Thread& t, u64 r, u64 material);
/// The instanced draw's state packets: written / skipped (SSPC_R7_SHADOWS_DEDUP).
void DrawState(u32 written, u32 skipped);
/// The dispatcher's timed paths (TimingOn): one call of the loop, `ticks` rdtsc inclusive.
void Time(bool native, u64 ticks);

} // namespace SecondSon::NativeEngine::R7Shadows::Stats
