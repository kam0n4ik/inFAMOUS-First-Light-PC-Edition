// SecondSonPC — the auxiliary tessellation stages of SecondSonPC's own shader translator (docs/OWN_SHADER_TRANSLATOR.md):
// rect and quad lists are drawn as patches of 3 / 4 vertices (the vertex program's outputs as for triangles); a
// tessellation control stage makes the four corners of each (a rect's fourth one interpolated from the three given, the
// quad's reordered) and a pass-through evaluation stage puts them out as a quad. The conventions are the bridge's
// (emit_spirv_quad_rect.cpp), whose fragment programs read the outputs: the position, and the fragment program's inputs
// - SPI_PS_INPUT_CNTL_i.OFFSET's locations, a vec4 each.
#pragma once

#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/shader_translator.h"

namespace SecondSon::Gpu::Gcn {

namespace GS = SecondSon::Gpu::Shader;

/// The SPIR-V of `stage` for a pipeline whose fragment stage has `fragment` (nullptr: none - no varyings carried).
std::vector<u32> EmitAuxTessStage(GS::AuxStage stage, const GS::RuntimeParams* fragment, std::string& error);

} // namespace SecondSon::Gpu::Gcn
