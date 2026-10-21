// SecondSonPC — R7-R (native resource / material / CUE engine, branch feature/r7-native-resources): the registry's
// extension point. Owned by R7-R alone (docs/NATIVE_ENGINE_R7_CORE.md §3): add the reconstructions' headers below and
// their specs to SSPC_R7_RESOURCES_SPECS ("&Reconstructed::kXxxSpec," each). native_function_registry.cpp lists them
// after R6's.
#pragma once

#include "native_engine/native_function_registry.h"
#include "native_engine/reconstructed/cue_set_buffers.h"

// the read-write resource setters 0x16c40 / 0x16de0 (the Native Resource State observes every shadow writer)
#define SSPC_R7_RESOURCES_SPECS                                                                                       \
    &Reconstructed::kCueSetRwBuffersSpec,                                                                              \
    &Reconstructed::kCueSetRwTexturesSpec,
