// SecondSonPC — the own GCN translator as the runtime's ShaderTranslator (docs/OWN_SHADER_TRANSLATOR.md). With
// SSPC_OWN_SHADERS=cs it takes the compute programs it translates whole (decode, graph, structure, SPIR-V, resources:
// the reflection in the bridge's form - buffer 0 the flattened user data, then the V#s, then GDS; the SRT walk as x86
// of the bridge's walker forms) and hands every other program, and each it does not take, to the installed translator
// (the bridge's). SSPC_OWN_SHADERS_ONLY / SSPC_OWN_SHADERS_SKIP: comma-separated footer hashes (hex) to take only /
// never (bisecting a difference in the game).
#pragma once

namespace SecondSon::Gpu::Gcn {

/// Wraps the installed translator when SSPC_OWN_SHADERS is set (after InstallShaderTranslatorAdapter); else nothing.
void InstallOwnTranslatorIfEnabled();

} // namespace SecondSon::Gpu::Gcn
