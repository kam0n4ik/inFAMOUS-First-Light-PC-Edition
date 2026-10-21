// SecondSonPC — R6-G (G-buffer / geometry engine, branch feature/r6-gbuffer): the registry's extension point.
// Owned by R6-G alone (docs/NATIVE_RENDER_R6_CORE.md §3): add the reconstructions' headers below and their specs to
// SSPC_R6_GBUFFER_SPECS ("&Reconstructed::kXxxSpec," each). native_function_registry.cpp lists them after R5's.
#pragma once

#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/gbuffer_draw.h"

// 0x1a1770 the mesh pass's instanced draw, 0x1a1960 its mesh draw (explicit_only until GAME_VERIFIED)
#define SSPC_R6_GBUFFER_SPECS &Reconstructed::kGBufferInstancedDrawSpec, &Reconstructed::kGBufferMeshDrawSpec,
