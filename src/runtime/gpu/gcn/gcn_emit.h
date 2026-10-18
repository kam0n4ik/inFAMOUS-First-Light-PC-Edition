// SecondSonPC — GCN -> SPIR-V emission of SecondSonPC's own shader translator (docs/OWN_SHADER_TRANSLATOR.md, step 2):
// a decoded program (gcn_decoder), its graph (gcn_cfg) and structure (gcn_structure) into a SPIR-V module. The model is
// the oracle's: a lane is an invocation; SGPRs, VGPRs, M0 and VCC are 32-bit function variables; a 64-bit mask (VCC,
// an SGPR pair from a compare, EXEC as a source) is the subgroup ballot of the lanes' bits and a lane's bit is the
// inverse ballot; EXEC and SCC are the lane's booleans; vector instructions run under `if (exec)` when the program
// writes EXEC at all.
//
// Step 2 emits every program as a compute module (local size 64): the control flow, the registers, the constants and
// the ALU. Memory, interpolation, exports and resources come next; an instruction not handled yet ends the emission with
// its mnemonic (the --gcn-translate-test report counts them).
#pragma once

#include <span>
#include <string>
#include <vector>

#include "base/abi.h"
#include "runtime/gpu/gcn/gcn_cfg.h"
#include "runtime/gpu/gcn/gcn_decoder.h"
#include "runtime/gpu/gcn/gcn_resources.h"
#include "runtime/gpu/gcn/gcn_structure.h"

namespace SecondSon::Gpu::Gcn {

struct EmitResult {
    bool ok = false;
    std::string error;       ///< the first instruction not handled ("unsupported: <mnemonic>") or a structure problem
    std::string unsupported; ///< its mnemonic alone (empty: none)
    std::vector<u32> spirv;
    /// the resources (bindings: buffers[i] at i, the flattened user data at buffers.size(), GDS after it; push
    /// constants: PushData, buf_offsets[i] for buffers[i])
    ResourceInfo resources;
    u8 mrt_mask = 0;          ///< a fragment program: the MRTs it exports to (the bridge's info.mrt_mask)
    bool has_discard = false; ///< ... an export with VM
};

struct EmitOptions {
    /// tests only: memory, interpolation and export instructions do nothing and their destinations get zeros (the
    /// control flow, the registers and the ALU of every program can be checked before resources exist)
    bool stub_memory = false;
    /// SGPRs holding user data at the start (SPI_SHADER_PGM_RSRC2.USER_SGPR)
    u32 user_sgprs = 16;
    /// a compute program (else the tests' form: one group of 64, no thread / group ids): its group size and
    /// COMPUTE_PGM_RSRC2 (after the user data the enabled group ids X/Y/Z and TG_SIZE; v0-v2 the thread ids)
    bool compute = false;
    std::array<u32, 3> threads{64, 1, 1};
    u32 rsrc2 = 0;
    /// the device's subgroup size (<= 64). A wave of 64 is then 64 / size subgroups, each run as a wave of its own
    /// (masks, ballots, ds_append, readfirstlane per subgroup - consistent within it); what needs the whole wave's
    /// lane numbering - v_mbcnt of a constant mask (a lane's index in the wave) - uses LocalInvocationIndex & 63 in a
    /// compute program; v_readlane / v_writelane of a constant lane beyond the subgroup are not translated.
    u32 subgroup_size = 64;
    /// the flattened user data at the translation (the user data, then the SRT walk's copies): the V#s' fields the
    /// module is specialized on (the runtime's key holds them) - buffer_*_format's DATA/NUM_FORMAT and DST_SEL.
    /// Empty: those instructions are not translated.
    std::span<const u32> flat;
    /// the stage's first descriptor binding and its first buffer's index in PushData.buf_offsets (a graphics
    /// pipeline numbers its stages' bindings one after another, the fragment stage's first)
    u32 binding_base = 0, buffer_base = 0;

    /// a vertex program (the hardware VS stage; the bridge's conventions, which its fragment programs read):
    /// the fetch shader's attributes are vertex inputs at locations 0.., read where the program calls it
    /// (s_swappc_b64), converted and swizzled as the V# says; v0 the vertex index, v1-v3 the instance ids; POS0 is
    /// gl_Position, PARAMn an output vec4 at location n
    struct VertexInput {
        u32 dest_vgpr = 0, num_elements = 0;
        u32 kind = 0;       ///< the input's type: 0 float, 1 uint, 2 sint (the format's number class, remapped)
        u32 conversion = 0; ///< 0 none, 1 uscaled, 2 sscaled, 3 ubnorm, 4 snorm_nz 8-bit, 5 snorm_nz 16-bit, 6 unorm 32
        std::array<u8, 4> swizzle{4, 5, 6, 7}; ///< per register component: 0 zero, 1 one, 4-7 the input's x-w
    };
    struct VertexOptions {
        bool fetch = false;              ///< the program calls a fetch shader
        std::vector<VertexInput> inputs; ///< (in the fetch shader's order: location i)
        u32 input_vgprs = 0;             ///< SPI_SHADER_PGM_RSRC1_VS.VGPR_COMP_CNT (v1..v3 given)
        u32 step_rate0 = 0, step_rate1 = 0;
        s32 base_vertex_sgpr = -1, base_instance_sgpr = -1; ///< indirect draws: the SGPRs given BaseVertex / BaseInstance
        bool fetch_adds_base_vertex = false, fetch_adds_base_instance = false; ///< (else v0 / v3 have them taken off)
        /// POS1..POS3's components: 0 none, 1 point size, 2 + k clip distance k, 10 + k cull distance k, 255 not
        /// translated (edge / kill / cut flag, render target / viewport index)
        std::array<std::array<u8, 4>, 3> pos_outputs{};
        bool depth_minus_one_to_one = false; ///< z = (z + w) / 2 at the end (no VK_EXT_depth_clip_control)
        bool clip_space = false;             ///< CLIP_DISABLE / a rect list: the position is in screen space
        float viewport_w = 0, viewport_h = 0; ///< (clip_space: half the device's largest viewport, at most 8 K)
        /// PA_CL_CLIP_CNTL.UCP_ENA lowered (the bridge's LowerUserClipPlanes): clip distance p = dot(position, plane p),
        /// the planes (PA_CL_UCP_*) a buffer after the stage's others (BufferKind::ClipPlanes); 0 when the program
        /// exports clip distances of its own
        u32 ucp_mask = 0;
        /// the device emulates clip distances (the bridge's needs_clip_distance_emulation): distances 0-3 are a vec4
        /// output at location 0, the PARAMs at n + 1 (the fragment program discards where one is negative)
        bool emulate_clip = false;
    };
    bool vertex = false;
    VertexOptions vs;

    /// a fragment program (the hardware PS stage; the bridge's conventions): the VGPRs SPI_PS_INPUT_ADDR lays out
    /// (the barycentrics' I/J only name the interpolation: v_interp_p2 reads the interpolated input, p1 does nothing),
    /// the position / front face SPI_PS_INPUT_ENA gives; input slot i (v_interp's ATTR) is the vec4 at location
    /// SPI_PS_INPUT_CNTL_i.OFFSET; MRTn an output at location n, MRTZ the depth; an export with VM discards the lanes
    /// EXEC does not hold
    struct FragmentInput {
        u32 location = 0;
        bool flat = false, is_default = false;
        u32 default_value = 0; ///< (0, 0, 0, 0), (0, 0, 0, 1), (1, 1, 1, 0), (1, 1, 1, 1)
    };
    struct ColorTarget {
        u32 export_format = 0; ///< SPI_SHADER_COL_FORMAT's field (0: none)
        u32 kind = 0;          ///< the output's type: 0 float, 1 uint, 2 sint (the target's number class, remapped)
        u32 conversion = 0;    ///< as VertexInput's, in the write direction
        std::array<u8, 4> map{0, 1, 2, 3}; ///< output component i = the exported component map[i] (CB COMP_SWAP)
        bool self_scale = false; ///< a MIN/MAX blend with SRC/DST colour factors: RGB squared (the bridge's emulation)
    };
    struct FragmentOptions {
        u32 input_addr = 0, input_ena = 0; ///< SPI_PS_INPUT_ADDR / ENA
        bool front_face_all_bits = false;
        u32 num_inputs = 0;
        std::array<FragmentInput, 32> inputs{};
        std::array<ColorTarget, 8> targets{};
        u32 mrtz_mask = 0; ///< bit 0 depth, 1 stencil, 2 sample mask (DB_SHADER_CONTROL, SPI_SHADER_Z_FORMAT)
        bool early_tests = false; ///< DEPTH_BEFORE_SHADER: EarlyFragmentTests
        /// the bridge's needs_manual_interpolation (NVIDIA): the inputs are per-vertex (PerVertexKHR), the I/J VGPRs
        /// BaryCoordKHR's y/z, v_interp_p1/p2 the hardware's P0 + I * (P1 - P0) + J * (P2 - P0) - exact for an input
        /// constant over the primitive (an index), which the device's interpolation is not
        bool manual_interpolation = false;
        /// CB0 blends with a second source (SRC1 factors): MRT1 exports it - with MRT0's parameters, to location 0
        /// index 1 (MRT0: index 0), as the bridge's
        bool dual_source = false;
        /// clip distances emulated (the vertex side's emulate_clip): the inputs at SPI_PS_INPUT_CNTL.OFFSET + 1, the
        /// distances 0-3 a vec4 at location 0 - a fragment where one is negative is discarded
        bool clip_emulation = false;
    };
    bool fragment = false;
    FragmentOptions fs;
};

/// What a fragment program's emission found (the reflection's): the MRTs it exports to, whether it discards.
struct FragmentFacts {
    u8 mrt_mask = 0;
    bool has_discard = false;
};

EmitResult EmitProgram(std::span<const Inst> insts, const Cfg& cfg, const Structure& st,
                       const EmitOptions& options = {});

} // namespace SecondSon::Gpu::Gcn
