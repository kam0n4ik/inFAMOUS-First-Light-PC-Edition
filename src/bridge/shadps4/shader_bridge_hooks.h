// SPDX-License-Identifier: GPL-2.0-or-later
//
// SecondSonPC — what the bridge's pipeline cache reports to SecondSonPC's shader frontend (Phase 4, P4.3, patch 0031;
// docs/PHASE4_P43_SHADER_TRANSLATION.md §10). Adapter layer: bridge types in, SecondSonPC types out
// (shader_translator_host.cpp). Off unless --gpu-shader-capture (every module the bridge translates, with its inputs,
// written for offline comparison) or --gpu-shaders (the pipeline the bridge picks at each draw/dispatch, compared on
// its GPU thread with what SecondSonPC's frontend prepared for the same packet).
#pragma once

#include <array>
#include <atomic>
#include <span>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace SecondSon::Bridge {
/// The adapter over the bridge's recompiler as the process's ShaderTranslator (start-up, before any frontend).
void InstallShaderTranslatorAdapter();
/// P4.4: the bridge's built tiling kernels for SecondSonPC's backend (host_shaders_host.cpp).
void InstallHostShaders();
} // namespace SecondSon::Bridge

namespace SecondSon::Bridge::ShaderHooks {

extern std::atomic<bool> g_capture;
extern std::atomic<bool> g_view;

inline bool CaptureEnabled() {
    return g_capture.load(std::memory_order_relaxed);
}
inline bool ViewEnabled() {
    return g_view.load(std::memory_order_relaxed);
}

/// The bridge translated a module (its CompileModule, the GPU thread): inputs, SPIR-V, the registers of the draw.
void OnModuleCompiled(const Shader::Info& info, const Shader::RuntimeInfo& runtime, std::span<const u32> code,
                      size_t perm_idx, const Shader::Backend::Bindings& start, std::span<const u32> spirv,
                      const Shader::Profile& profile, const AmdGpu::Regs& regs, const AmdGpu::ComputeProgram& cs,
                      u16 vertex_sgpr_offset, u16 instance_sgpr_offset);

/// The pipeline the bridge picked for a draw (compute: a dispatch): its stages' programs and modules.
void OnPipeline(bool compute, const std::array<const Shader::Info*, 6>& infos,
                const std::array<vk::ShaderModule, 6>& modules, const std::array<Shader::RuntimeInfo, 6>& runtimes,
                const void* pipeline);

} // namespace SecondSon::Bridge::ShaderHooks
