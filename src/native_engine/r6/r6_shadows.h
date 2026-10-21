// SecondSonPC — R6-S (shadow engine, branch feature/r6-shadows): the registry's extension point.
// Owned by R6-S alone (docs/NATIVE_RENDER_R6_CORE.md §3): add the reconstructions' headers below and their specs to
// SSPC_R6_SHADOWS_SPECS ("&Reconstructed::kXxxSpec," each). native_function_registry.cpp lists them after R5's.
#pragma once

#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/shadow_loop.h"

// 0x43d590 shadow_casters_loop and 0x43d480 shadow_instanced_draw (explicit_only: verify/guarded runs only; the latter
// only below CITY-OPT's entry probe)
#define SSPC_R6_SHADOWS_SPECS                                                                                          \
    &Reconstructed::kShadowCastersLoopSpec, &Reconstructed::kShadowInstancedDrawSpec,
