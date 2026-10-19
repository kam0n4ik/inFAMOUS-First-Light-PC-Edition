// SecondSonPC — the own GCN translator as the runtime's ShaderTranslator (docs/OWN_SHADER_TRANSLATOR.md). It takes
// the compute, vertex and pixel programs it translates whole (decode, graph, structure, SPIR-V, resources: the
// reflection in the bridge's form - the V#s, then the flattened user data, then GDS; the SRT walk as x86 of the
// bridge's walker forms) and hands every other program, and each it does not take, to the installed translator (the
// bridge's). SSPC_OWN_SHADERS: its stages (cs,vs,ps when unset; "bridge": none - every program the bridge's).
// SSPC_OWN_SHADERS_ONLY / SSPC_OWN_SHADERS_SKIP: comma-separated footer hashes (hex) to take only / never (bisecting
// a difference in the game).
#pragma once

namespace SecondSon::Gpu::Gcn {

/// Wraps the installed translator (after InstallShaderTranslatorAdapter) for the stages SSPC_OWN_SHADERS names; with
/// the variable unset, for cs,vs,ps when `by_default` (the game, --predict-shaders), else not (the self-test: the
/// bridge's adapter is what it tests).
void InstallOwnTranslatorIfEnabled(bool by_default);

/// Whether the own translator was installed (the native backend's shader cache: its own folder, native_own).
bool OwnTranslatorInstalled();

} // namespace SecondSon::Gpu::Gcn
