// SecondSonPC — dynamic trace of the engine's render frame (P5.3A, diagnostics, SSPC_P53_RENDER_TRACE=1; off by
// default; docs/native-reconstruction/RENDER_ENGINE_PASSES.md).
//
// Entry probes (guest_probe.h) on functions the reconstruction rests on; the game's code runs unchanged, the probes
// only read its data:
//   SubmitFrame 0x1f1e30   every frame: whether each pool context's command buffer starts anew (the per-frame reset
//                          of the 254 contexts that static analysis does not show); every Nth frame: the frame's
//                          contexts in submission order, their command buffers walked (dcb_scan.h), the resources the
//                          engine bound on each, grouped into passes and linked (pass_graph.h) -> one JSON file
//   Reset 0x11f050         calls by caller and by kind of context (pool slot or not)
//   DcbReserve 0x11eed0    the DCB's out-of-space callback: chunks taken per frame
//   CUE setters 0x16d10 / 0x16de0 / 0x16b70 / 0x16c40   textures / RW textures / buffers / RW buffers bound per
//                          context, recorded only while a sampled frame is being recorded
//
//   SSPC_P53_RENDER_TRACE=1           install the probes (the eboot's functions must hash to the analysed ones)
//   SSPC_P53_RENDER_TRACE_EVERY=<n>   frames between two sampled frames (default 300)
//   SSPC_P53_RENDER_TRACE_DUMPS=<n>   sampled frames written (default 8)
//   SSPC_P53_RENDER_TRACE_DIR=<dir>   where (default logs\p53_render_trace)
//   SSPC_P53_RENDER_TRACE_STATS=<s>   a log line every <s> seconds (default 10; 0: none)
#pragma once

#include "base/abi.h"

namespace SecondSon::Engine::Render::Trace {

/// The loader's window before the guest runs. Returns false (and logs why) if the switch is set but the probes could
/// not be installed; nothing is changed then.
bool InstallFromEnvironment(u64 eboot_base);

} // namespace SecondSon::Engine::Render::Trace
