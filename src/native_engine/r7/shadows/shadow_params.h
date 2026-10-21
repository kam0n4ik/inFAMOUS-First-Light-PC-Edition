// SecondSonPC — R7-S: the shadow loop's call of the parameter writer 0x450460, observed in the game
// (docs/NATIVE_ENGINE_R7_SHADOWS.md §4, task S2). Off by default; costs one branch per call when off.
//
// 0x450460(records, block, atlas, view, table, buffers, owner) writes up to 4 records of 18 kinds into the constants
// block R + 0xda18 (0x100 floats + the dirty byte at +0x404) through 0x11e5d0. Two of its branches stayed outside
// R6-S's verification of the recompiled 0x450460: kind 0x10 (0x4502e0: the sprite-sheet cell from a global object read
// under its pthread mutex) and kind 0xa with an object at buffers + 0x88 (a virtual method, a skeleton lookup and a
// matrix inverse). Here, at the hand-written loop's call site (live calls only):
//   SSPC_R7_SHADOWS_PARAMS_CENSUS=1   which kinds the shadow passes really give it: calls per kind, kind 0xa with an
//                                     object (and the objects' distinct vtables), kind 0x10, the largest slot used;
//   SSPC_R7_SHADOWS_PARAMS_CHECK=<recomp.dll>  every call twice: the recompiled recomp_450460 first, into the real
//                                     block, its result saved, the block restored, then the eboot's 0x450460 - whose
//                                     result the game keeps - and the two blocks compared (bit for bit, dirty byte
//                                     included). Calls whose records name a slot past the block are left unchecked
//                                     (counted). The second run repeats only reads, the mutex lock / unlock pair of
//                                     0x4502e0 and the virtual getter of kind 0xa.
// A line every 10 s ("R7-S params").
#pragma once

#include "base/abi.h"

namespace SecondSon::NativeEngine::R7Shadows::Params {

/// Census or check on (read once; the check loads its module on first use).
bool On();

/// The loop's call of 0x450460 (`original`: its entry) with the census / check around it.
void Call(u64 original, u64 records, u64 block, u64 atlas, u64 view, u64 table, u64 buffers, u64 owner);

/// The block offset (u16 field of the records) kind `kind` writes at, or 0 for kinds without a write (8, 0xb, other).
u32 SlotField(s32 kind);

} // namespace SecondSon::NativeEngine::R7Shadows::Params
