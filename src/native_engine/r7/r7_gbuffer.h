// SecondSonPC — R7-G (native G-buffer / geometry engine, branch feature/r7-native-gbuffer): the registry's extension
// point. Owned by R7-G alone (docs/NATIVE_ENGINE_R7_CORE.md §3): add the reconstructions' headers below and their specs
// to SSPC_R7_GBUFFER_SPECS ("&Reconstructed::kXxxSpec," each). native_function_registry.cpp lists them after R6's.
#pragma once

#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/gbuffer_mesh_pass.h"

// 0x1a2030 the geometry pass (the native pass, r7/gbuffer/geometry_pass.h; explicit_only until GAME_VERIFIED)
#define SSPC_R7_GBUFFER_SPECS &Reconstructed::kGBufferMeshPassSpec,
