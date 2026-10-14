// SecondSonPC — Native Render Interface, engine side (P5.3B; docs/native-reconstruction/NATIVE_RENDER_INTERFACE.md).
//
// SSPC_P53_NATIVE_DRAW=shadow (off by default): entry probes on the engine's draw wrappers (eboot 0x623c60 drawIndex,
// 0x623a40 drawIndexAuto; every mesh/fullscreen draw of the render passes reaches one of them right after the CUE
// commit 0x14ad0). At each call the draw is described from the engine's own objects (BuildDrawOp) - the context's pass,
// the CUE's VS/PS shader objects, the stage resource shadows and user-data layout tables, the wrapper's arguments -
// and written into the context's command buffer as a kVerify marker just before the game's draw packet. The game's
// draw runs unchanged; SecondSonPC's command processor compares its PM4-built draw with the marker
// (native_render_ops.h). Counters in the log every SSPC_P53_NATIVE_DRAW_STATS seconds (default 10).
#pragma once

#include "base/abi.h"
#include "engine/render/native_render_ops.h"

namespace SecondSon::Engine::Render::NativeDraw {

/// The loader's window before the guest runs. False (and logged) when the switch is set but nothing was installed.
bool InstallFromEnvironment(u64 eboot_base);

/// The draw that the context `r` (render context R, its DCB at R+8) is about to issue, from the engine's objects.
/// `module_base` locates the frame object (pass keys); false when `r` is not a context SecondSonPC knows.
bool BuildDrawOp(u64 module_base, u64 r, bool indexed, u32 index_count, u64 index_base, DrawOp& out);

} // namespace SecondSon::Engine::Render::NativeDraw
