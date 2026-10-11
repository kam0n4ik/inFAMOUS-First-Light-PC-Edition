// SecondSonPC — the backend's Vulkan pipelines (Phase 4, P4.4; docs/PHASE4_P44_NATIVE_VULKAN_RENDERING.md §D): shader
// modules from the frontend's translations (P4.3), the descriptor set and pipeline layouts from the draw's binding
// plans (one set, push descriptors when the bindings fit, the translator's push constants), graphics pipelines from
// GraphicsPipelineState as the bridge's renderer builds them (the same SPIR-V needs the same fixed function: dynamic
// rendering formats, blending with its GCN adjustments, rect/quad lists through the translator's tessellation
// stages, everything else dynamic), compute pipelines. Created on first use, kept until the backend stops.
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/shader/pipeline_state.h"
#include "runtime/gpu/shader/shader_cache.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/vulkan/vk_api.h"

namespace SecondSon::Gpu::Vk {

namespace GS = Gpu::Shader;

class DeviceContext;

/// The translator's push constants (the bridge's GS::PushData, byte for byte).
struct PushData {
    float xoffset = 0.0f, yoffset = 0.0f, xscale = 1.0f, yscale = 1.0f;
    std::array<u8, 56> buf_offsets{};
};
static_assert(sizeof(PushData) == 72);

struct PipelineObjects {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    bool push_descriptors = true;
    u32 bindings = 0;
    bool compute = false;
};

struct PipelineStats {
    u64 graphics = 0, compute = 0, modules = 0, failures = 0, lookups = 0, aux_stages = 0;
    std::string first_failure;
};

class PipelineFactory {
public:
    PipelineFactory(DeviceContext& ctx, const GS::DeviceProfile& profile);
    ~PipelineFactory();

    /// `pipeline_id`: the frontend's PipelineStateCache id; `state` its state; the stages' modules and plans as the
    /// frontend prepared them for this draw (vs required, ps may be null). nullptr with `error`.
    const PipelineObjects* Graphics(u32 pipeline_id, const GS::GraphicsPipelineState& state,
                                    const GS::Module* vs, const GS::ResourceBindingPlan& vs_plan,
                                    const GS::Module* ps, const GS::ResourceBindingPlan* ps_plan,
                                    std::string& error);
    const PipelineObjects* Compute(const GS::Module& cs, const GS::ResourceBindingPlan& plan, std::string& error);

    const PipelineStats& Stats() const { return stats_; }

private:
    VkShaderModule Module(const GS::Module& module, std::string& error);
    VkShaderModule Aux(u32 which, const GS::RuntimeParams* fragment, std::string& error);
    bool MakeLayout(std::span<const GS::ResourceBindingPlan* const> plans, bool compute, PipelineObjects& out,
                    std::string& error);

    DeviceContext& ctx_;
    const GS::DeviceProfile& profile_;
    std::unordered_map<u32, VkShaderModule> modules_;
    std::unordered_map<u64, VkShaderModule> aux_;
    std::unordered_map<u64, std::unique_ptr<PipelineObjects>> pipelines_;
    PipelineStats stats_;
};

/// The layout signature of plans (binding numbers, kinds, counts): part of a pipeline's key.
u64 LayoutSignature(std::span<const GS::ResourceBindingPlan* const> plans);

} // namespace SecondSon::Gpu::Vk
