// SecondSonPC — the own GCN translator as the runtime's ShaderTranslator (gcn_translator.h).
#include "runtime/gpu/gcn/gcn_translator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/gcn/gcn_cfg.h"
#include "runtime/gpu/gcn/gcn_decoder.h"
#include "runtime/gpu/gcn/gcn_emit.h"
#include "runtime/gpu/gcn/gcn_resources.h"
#include "runtime/gpu/gcn/gcn_structure.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/shader/shader_translator.h"

namespace SecondSon::Gpu::Gcn {

namespace {

namespace GS = Gpu::Shader;

std::unordered_set<u64> HashList(const char* env) {
    std::unordered_set<u64> out;
    const char* v = std::getenv(env);
    if (v == nullptr) {
        return out;
    }
    std::string_view s(v);
    while (!s.empty()) {
        const size_t comma = s.find(',');
        const std::string item(s.substr(0, comma));
        if (!item.empty()) {
            out.insert(std::strtoull(item.c_str(), nullptr, 16));
        }
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
    }
    return out;
}

/// Where a descriptor of `n` dwords is in the flattened user data (consecutive: one load; else per dword).
GS::SharpLocation Location(std::span<const ScalarValue> sharp, u32 n) {
    GS::SharpLocation loc;
    loc.dwords = static_cast<u8>(n);
    bool consecutive = true;
    for (u32 i = 0; i < n; ++i) {
        consecutive = consecutive && sharp[i].kind == ScalarValue::Kind::Flat && sharp[i].value == sharp[0].value + i;
    }
    if (consecutive) {
        loc.summary = GS::SharpLocation::Summary::SingleLoad;
        loc.offsets[0] = static_cast<u16>(sharp[0].value);
        return loc;
    }
    loc.summary = GS::SharpLocation::Summary::MultiLoad;
    for (u32 i = 0; i < n; ++i) {
        if (sharp[i].kind == ScalarValue::Kind::Flat) {
            loc.load_mask |= static_cast<u8>(1u << i);
            loc.offsets[i] = static_cast<u16>(sharp[i].value);
        } else {
            loc.immediates[i] = sharp[i].value;
        }
    }
    return loc;
}
GS::SharpLocation Location(const BufferUse& b) {
    return Location(b.sharp, 4);
}

/// The stages SSPC_OWN_SHADERS names ("cs", "vs", "ps", comma-separated): bit 0 compute, 1 vertex, 2 fragment.
u32 OwnStages() {
    const char* v = std::getenv("SSPC_OWN_SHADERS");
    u32 stages = 0;
    std::string_view s = v != nullptr ? std::string_view(v) : std::string_view{};
    while (!s.empty()) {
        const size_t comma = s.find(',');
        const std::string_view item = s.substr(0, comma);
        stages |= item == "cs" ? 1u : (item == "vs" ? 2u : (item == "ps" ? 4u : 0u));
        s = comma == std::string_view::npos ? std::string_view{} : s.substr(comma + 1);
    }
    return stages;
}

/// The bridge's RemapNumberFormat / MapNumberConversion / Buffer::DstSelect (RemapSwizzle) for a vertex attribute of
/// data format `dfmt`, number format `nfmt`, the V#'s DST_SEL `dst_sel`.
EmitOptions::VertexInput VertexInputOf(u32 dfmt, u32 nfmt, u32 dst_sel) {
    EmitOptions::VertexInput in;
    const bool f32 = dfmt == 4 || dfmt == 11 || dfmt == 13 || dfmt == 14;
    u32 cls = nfmt; // the remapped number format
    switch (nfmt) {
    case 0: // unorm
        cls = f32 ? 4 : 0;
        in.conversion = f32 ? 6 : 0;
        break;
    case 2: // uscaled
        cls = 4;
        in.conversion = 1;
        break;
    case 3: // sscaled
        cls = 5;
        in.conversion = 2;
        break;
    case 6: // snorm_nz
        cls = 5;
        in.conversion = (dfmt == 1 || dfmt == 3 || dfmt == 10) ? 4 : ((dfmt == 2 || dfmt == 5 || dfmt == 12) ? 5 : 0);
        break;
    case 7: // float (8-bit: the stencil's uint)
        cls = dfmt == 1 ? 4 : 7;
        break;
    case 10: // ubnorm
        cls = 0;
        in.conversion = 3;
        break;
    default:
        break;
    }
    in.kind = cls == 4 ? 1 : (cls == 5 ? 2 : 0);
    std::array<u8, 4> s{};
    for (u32 c = 0; c < 4; ++c) {
        s[c] = static_cast<u8>((dst_sel >> (3 * c)) & 7);
    }
    std::array<u8, 4> r = s;
    switch (dfmt) {
    case 17: // 1_5_5_5
    case 7:  // 11_11_10
    case 16: // 5_6_5
        r = {s[2], s[1], s[0], s[3]};
        break;
    case 8: // 10_10_10_2
        r = {s[3], s[2], s[1], s[0]};
        break;
    case 19: // 4_4_4_4
        r = {s[1], s[2], s[3], s[0]};
        break;
    case 1:
    case 2:
    case 4: // one channel: alpha is red
        for (u8& c : r) {
            c = c == 7 ? 4 : c;
        }
        break;
    default:
        break;
    }
    in.swizzle = r;
    return in;
}

class OwnTranslator final : public GS::ShaderTranslator {
public:
    OwnTranslator(GS::ShaderTranslator* fallback, u32 stages)
        : fallback_(fallback), stages_(stages), only_(HashList("SSPC_OWN_SHADERS_ONLY")),
          skip_(HashList("SSPC_OWN_SHADERS_SKIP")) {}

    std::string_view Name() const override {
        return "SecondSonPC GCN translator, the bridge's for the rest";
    }

    GS::TranslationResult Translate(const GS::TranslationRequest& req) override {
        const bool cs = req.hw == GS::HwStage::Cs && req.sw == GS::SwStage::Compute && (stages_ & 1) != 0;
        const bool vs = req.hw == GS::HwStage::Vs && req.sw == GS::SwStage::Vertex && (stages_ & 2) != 0;
        const bool ps = req.hw == GS::HwStage::Ps && req.sw == GS::SwStage::Fragment && (stages_ & 4) != 0;
        const char* stage = cs ? "compute" : (vs ? "vertex" : "fragment");
        if ((cs || vs || ps) && Wanted(req.footer_hash)) {
            GS::TranslationResult res;
            std::string why;
            u32 walk_loads = 0;
            const auto t0 = std::chrono::steady_clock::now();
            if (TryOwn(req, res, why, walk_loads)) {
                res.translate_us = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
                const u32 n = ++own_;
                if (cs) {
                    SSPC_LOG(GPU, Info,
                             "own GCN translator: compute {:016x} at {:#x} ({} dwords) translated - {} SPIR-V words, {} "
                             "buffers, flat {} dwords, walk {} loads, group {}x{}x{} [{} own so far]",
                             req.footer_hash, req.address, req.code.size(), res.spirv.size(),
                             res.reflection.buffers.size(), res.reflection.flatbuf_dwords, walk_loads,
                             req.runtime.threads[0], req.runtime.threads[1], req.runtime.threads[2], n);
                } else if (ps) {
                    SSPC_LOG(GPU, Info,
                             "own GCN translator: fragment {:016x} at {:#x} ({} dwords) translated - {} SPIR-V words, {} "
                             "buffers, {} images, MRT mask {:#x}{}, flat {} dwords, walk {} loads [{} own so far]",
                             req.footer_hash, req.address, req.code.size(), res.spirv.size(),
                             res.reflection.buffers.size(), res.reflection.images.size(), res.reflection.mrt_mask,
                             res.reflection.has_discard ? ", discards" : "", res.reflection.flatbuf_dwords, walk_loads, n);
                } else {
                    SSPC_LOG(GPU, Info,
                             "own GCN translator: vertex {:016x} at {:#x} ({} dwords) translated - {} SPIR-V words, {} "
                             "buffers, {} images, {} attributes, flat {} dwords, walk {} loads [{} own so far]",
                             req.footer_hash, req.address, req.code.size(), res.spirv.size(),
                             res.reflection.buffers.size(), res.reflection.images.size(),
                             res.reflection.fetch.attributes.size(), res.reflection.flatbuf_dwords, walk_loads, n);
                }
                return res;
            }
            if (const u32 n = ++declined_; n <= 400) {
                SSPC_LOG(GPU, Info, "own GCN translator: {} {:016x} to the bridge's ({})", stage, req.footer_hash, why);
            }
        }
        return fallback_->Translate(req);
    }

    std::vector<u32> EmitAuxStage(GS::AuxStage stage, const GS::RuntimeParams* fragment, const GS::DeviceProfile& profile,
                                  std::string& error) override {
        return fallback_->EmitAuxStage(stage, fragment, profile, error);
    }

    bool CompareRuntime(const GS::RuntimeParams& ours, const GS::DeviceProfile& profile, std::span<const u8> reference,
                        std::string& diff) override {
        return fallback_->CompareRuntime(ours, profile, reference, diff);
    }

private:
    bool Wanted(u64 hash) const {
        return (only_.empty() || only_.contains(hash)) && !skip_.contains(hash);
    }

    /// A vertex program's parameters (the bridge's ToRuntime / MapOutputs / translator prologue) and its fetch shader:
    /// read from the request (or guest memory), parsed, its attributes' V#s read - res.fetch_code / vertex_sharps as
    /// the bridge's adapter gives them (the runtime keys the module on them).
    static bool PrepareVertex(const GS::TranslationRequest& req, std::span<const Inst> insts,
                              EmitOptions::VertexOptions& vs, GS::FetchShader& fetch, GS::TranslationResult& res,
                              std::string& why) {
        const GS::RuntimeParams& p = req.runtime;
        const GS::DeviceProfile& prof = *req.profile;
        if (p.rect_list != 0 || p.quad_list != 0) {
            why = "a rect / quad list (drawn as patches: the auxiliary tessellation stages read the outputs)";
            return false;
        }
        vs.input_vgprs = (p.rsrc1 >> 24) & 3;
        vs.step_rate0 = p.step_rate0;
        vs.step_rate1 = p.step_rate1;
        // an indirect draw's SH offsets of base vertex / start instance: user data registers (SPI_SHADER_USER_DATA_VS_0
        // is SH offset 0x4C)
        const auto ud_reg = [&](u32 offset, s32& out) {
            out = -1;
            if (offset == 0) {
                return true;
            }
            if (offset < 0x4C || offset - 0x4C >= 16) {
                why = fmt::format("an indirect SGPR at SH offset {:#x}", offset);
                return false;
            }
            out = static_cast<s32>(offset - 0x4C);
            return true;
        };
        if (!ud_reg(p.vertex_sgpr_offset, vs.base_vertex_sgpr) || !ud_reg(p.instance_sgpr_offset, vs.base_instance_sgpr)) {
            return false;
        }
        // POS1..3 (PA_CL_VS_OUT_CNTL; MapOutputs)
        const u32 ctl = p.vs_out_cntl;
        u32 n = 0;
        if (((ctl >> 21) & 1) != 0) { // VS_OUT_MISC_VEC_ENA
            auto& misc = vs.pos_outputs[n++];
            misc[0] = ((ctl >> 16) & 1) != 0 ? 1 : 0;                                         // point size
            misc[1] = (((ctl >> 17) & 1) != 0 || ((ctl >> 25) & 1) != 0) ? 255 : 0;            // edge / cut flag
            misc[2] = (((ctl >> 20) & 1) != 0 || ((ctl >> 18) & 1) != 0) ? 255 : 0;            // kill flag / layer
            misc[3] = ((ctl >> 19) & 1) != 0 ? 255 : 0;                                        // viewport index
        }
        for (u32 vec = 0; vec < 2; ++vec) {
            if (((ctl >> (22 + vec)) & 1) == 0) { // VS_OUT_CCDIST0/1_VEC_ENA
                continue;
            }
            auto& o = vs.pos_outputs[n++];
            for (u32 i = 0; i < 4; ++i) {
                const u32 k = vec * 4 + i;
                o[i] = ((ctl >> k) & 1) != 0 ? static_cast<u8>(2 + k) : (((ctl >> (8 + k)) & 1) != 0 ? static_cast<u8>(10 + k) : 0);
            }
        }
        // clip distances: the user clip planes lowered unless the program exports its own (CLIP_DIST_ENA); emulated
        // where the device needs it - by the condition the fragment side uses (ps_vs_out_cntl, ps_ucp_enable)
        vs.ucp_mask = (ctl & 0xFF) != 0 ? 0 : (p.clip_cntl & 0x3F);
        vs.emulate_clip = prof.needs_clip_distance_emulation && ((ctl & 0xFF) != 0 || (p.clip_cntl & 0x3F) != 0);
        vs.depth_minus_one_to_one = !prof.depth_clip_control && ((p.clip_cntl >> 19) & 1) == 0;
        vs.clip_space = ((p.clip_cntl >> 16) & 1) != 0;
        vs.viewport_w = static_cast<float>(std::min<u32>(prof.max_viewport_width / 2, 8192));
        vs.viewport_h = static_cast<float>(std::min<u32>(prof.max_viewport_height / 2, 8192));
        // the fetch shader: the s_swappc_b64 at the program's start (its SSRC0 the user data pair of its address)
        const Inst* call = nullptr;
        for (const Inst& in : insts) {
            if (in.format == Format::Sop1 && Mnemonic(in.format, in.op) == "s_swappc_b64") {
                if (call != nullptr) {
                    why = "two s_swappc_b64";
                    return false;
                }
                call = &in;
            }
        }
        if (call == nullptr) {
            return true;
        }
        const u32 sgpr = call->w0 & 0xFF;
        if (sgpr + 1 >= 16) {
            why = fmt::format("the fetch shader's address in s[{}:{}] (not user data)", sgpr, sgpr + 1);
            return false;
        }
        std::vector<u32> code;
        std::string e;
        if (!req.fetch_code.empty()) {
            code.assign(req.fetch_code.begin(), req.fetch_code.end());
            if (!GS::ParseFetchShaderCode(code, sgpr, fetch, e)) {
                why = "the fetch shader: " + e;
                return false;
            }
        } else {
            // read up to its s_setpc_b64 (bounded, as the bridge's adapter)
            const u64 addr = (req.user_data[sgpr] | (u64{req.user_data[sgpr + 1]} << 32)) & 0xFFFFFFFFFFFFull;
            constexpr u32 kMaxFetchDwords = 512;
            bool parsed = false;
            for (u32 have = 0; have < kMaxFetchDwords && !parsed;) {
                const u32 chunk = std::min<u32>(64, kMaxFetchDwords - have);
                code.resize(have + chunk);
                if (req.memory == nullptr || !req.memory->Read(addr + have * 4ull, code.data() + have, chunk * 4ull)) {
                    why = fmt::format("the fetch shader at {:#x} is unreadable", addr);
                    return false;
                }
                have += chunk;
                parsed = GS::ParseFetchShaderCode(code, sgpr, fetch, e);
                if (!parsed && e.rfind("no s_setpc_b64", 0) != 0 && e.rfind("a buffer load cut", 0) != 0) {
                    break;
                }
            }
            if (!parsed) {
                why = "the fetch shader: " + e;
                return false;
            }
            code.resize(fetch.size / 4);
        }
        if (fetch.vertex_offset_sgpr >= 0 && vs.base_vertex_sgpr >= 0 && fetch.vertex_offset_sgpr != vs.base_vertex_sgpr) {
            why = "the fetch shader's vertex offset is not the indirect draw's base vertex";
            return false;
        }
        if (fetch.instance_offset_sgpr >= 0 && vs.base_instance_sgpr >= 0 &&
            fetch.instance_offset_sgpr != vs.base_instance_sgpr) {
            why = "the fetch shader's instance offset is not the indirect draw's start instance";
            return false;
        }
        vs.fetch = true;
        vs.fetch_adds_base_vertex = fetch.vertex_offset_sgpr >= 0;
        vs.fetch_adds_base_instance = fetch.instance_offset_sgpr >= 0;
        // the attributes' V#s (the vertex buffer table: a user data pair, the V# at a dword offset in it)
        const bool given = req.vertex_sharps.size() == fetch.attributes.size() && !req.vertex_sharps.empty();
        for (u32 i = 0; i < fetch.attributes.size(); ++i) {
            const GS::FetchAttribute& a = fetch.attributes[i];
            std::array<u32, 4> raw{};
            if (given) {
                raw = req.vertex_sharps[i];
            } else {
                if (a.sgpr_base + 1 >= 16) {
                    why = fmt::format("attribute {}'s vertex buffer table in s[{}:{}]", i, a.sgpr_base, a.sgpr_base + 1);
                    return false;
                }
                const u64 table =
                    (req.user_data[a.sgpr_base] | (u64{req.user_data[a.sgpr_base + 1]} << 32)) & 0xFFFFFFFFFFFFull;
                if (req.memory == nullptr || !req.memory->Read(table + u64{a.dword_offset} * 4, raw.data(), 16)) {
                    why = fmt::format("attribute {}'s V# at {:#x} is unreadable", i, table + u64{a.dword_offset} * 4);
                    return false;
                }
            }
            res.vertex_sharps.push_back(raw);
            const GS::BufferDesc d{raw};
            const u32 dfmt = a.data_format != 0 ? a.data_format : d.DataFormat();
            const u32 nfmt = a.data_format != 0 ? a.num_format : d.NumFormat();
            EmitOptions::VertexInput in = VertexInputOf(dfmt, nfmt, raw[3] & 0xFFF);
            in.dest_vgpr = a.dest_vgpr;
            in.num_elements = a.num_elements;
            vs.inputs.push_back(in);
        }
        res.fetch_code = std::move(code);
        return true;
    }

    /// A fragment program's parameters (the bridge's ToRuntime: the inputs' SPI_PS_INPUT_CNTL, the colour targets'
    /// export format, number class, conversion and COMP_SWAP, the depth export's mask).
    static bool PrepareFragment(const GS::TranslationRequest& req, EmitOptions::FragmentOptions& fs, std::string& why) {
        const GS::RuntimeParams& p = req.runtime;
        const GS::DeviceProfile& prof = *req.profile;
        const bool ls = (p.stages_en & 3) != 0, gs = ((p.stages_en >> 5) & 1) != 0;
        fs.clip_emulation =
            ((p.ps_vs_out_cntl != 0 && !ls) || (p.ps_ucp_enable != 0 && !gs)) && prof.needs_clip_distance_emulation;
        fs.input_addr = p.ps_input_addr;
        fs.input_ena = p.ps_input_ena;
        fs.front_face_all_bits = p.front_face_all_bits != 0;
        fs.manual_interpolation = prof.needs_manual_interpolation;
        fs.dual_source = p.dual_source != 0;
        fs.num_inputs = std::min<u32>(p.num_interp, 32);
        for (u32 i = 0; i < fs.num_inputs; ++i) {
            const u32 c = p.ps_input_cntl[i];
            fs.inputs[i] = {.location = c & 0x1F,
                            .flat = ((c >> 10) & 1) != 0,
                            .is_default = ((c >> 5) & 1) != 0 && ((c >> 10) & 1) == 0, // (with FLAT_SHADE: an input)
                            .default_value = (c >> 8) & 3};
        }
        // MRTZ: DB_SHADER_CONTROL's exports, then SPI_SHADER_Z_FORMAT's components (MaskFromExportFormat)
        const u32 d = p.db_shader_control;
        const u32 stencil = ((d >> 2) & 1) | ((d >> 1) & 1);
        u32 mrtz = (d & 1) | (stencil << 1) | (((d >> 8) & 1) << 2) | (((d >> 7) & 1) << 3);
        switch (p.z_format) {
        case 0: mrtz = 0; break;
        case 1: mrtz &= 1; break;
        case 2: mrtz &= 3; break;
        case 3: mrtz &= 9; break;
        case 9: break;
        default:
            if (mrtz != 0) {
                why = fmt::format("SPI_SHADER_Z_FORMAT {}", p.z_format);
                return false;
            }
        }
        fs.mrtz_mask = mrtz;
        fs.early_tests = ((d >> 12) & 1) != 0;
        // colour targets (CB_COLORn_INFO: FORMAT 2-6, NUMBER_TYPE 8-10, COMP_SWAP 11-12)
        static constexpr u8 kComponents[23] = {0, 1, 1, 2, 1, 2, 3, 3, 4, 4, 4, 2, 4, 3, 4, 0, 3, 4, 4, 4, 2, 2, 2};
        // the bridge's mrt_swizzles[swap][components - 1]: CompSwizzle per output component (0 zero, 4-7 r-a)
        static constexpr u8 kSwizzle[4][4][4] = {
            {{4, 0, 0, 0}, {4, 5, 0, 0}, {4, 5, 6, 0}, {4, 5, 6, 7}},
            {{5, 0, 0, 0}, {4, 7, 0, 0}, {4, 5, 7, 0}, {6, 5, 4, 7}},
            {{6, 0, 0, 0}, {5, 4, 0, 0}, {6, 5, 4, 0}, {7, 6, 5, 4}},
            {{7, 0, 0, 0}, {7, 4, 0, 0}, {7, 5, 4, 0}, {7, 4, 5, 6}},
        };
        for (u32 cb = 0; cb < 8; ++cb) {
            const GS::ColorTargetParams& t = p.targets[cb];
            if (t.bound == 0) {
                continue; // (no export: the bridge's empty colour buffer)
            }
            EmitOptions::ColorTarget& ct = fs.targets[cb];
            const u32 dfmt = (t.info >> 2) & 0x1F, nt = (t.info >> 8) & 7, swap = (t.info >> 11) & 3;
            const u32 nfmt = nt == 6 ? 9 : nt; // (the CB's 6 is sRGB)
            const bool f32 = dfmt == 4 || dfmt == 11 || dfmt == 13 || dfmt == 14;
            u32 cls = nfmt;
            switch (nfmt) {
            case 0:
                cls = f32 ? 4 : 0;
                ct.conversion = f32 ? 6 : 0;
                break;
            case 2:
                cls = 4;
                ct.conversion = 1;
                break;
            case 3:
                cls = 5;
                ct.conversion = 2;
                break;
            case 7:
                cls = dfmt == 1 ? 4 : 7;
                break;
            case 10:
                cls = 0;
                ct.conversion = 3;
                break;
            default:
                break;
            }
            ct.kind = cls == 4 ? 1 : (cls == 5 ? 2 : 0);
            ct.export_format = (p.col_format >> (cb * 4)) & 0xF;
            ct.self_scale = t.min_max_self != 0;
            const u32 n = dfmt < 23 ? kComponents[dfmt] : 0;
            if (n == 0) {
                why = fmt::format("MRT{} of data format {}", cb, dfmt);
                return false;
            }
            std::array<u8, 4> s{};
            for (u32 c = 0; c < 4; ++c) {
                s[c] = kSwizzle[swap][n - 1][c];
            }
            std::array<u8, 4> r = s; // RemapSwizzle
            switch (dfmt) {
            case 17:
            case 7:
            case 16:
                r = {s[2], s[1], s[0], s[3]};
                break;
            case 8:
                r = {s[3], s[2], s[1], s[0]};
                break;
            case 19:
                r = {s[1], s[2], s[3], s[0]};
                break;
            case 1:
            case 2:
            case 4:
                for (u8& c : r) {
                    c = c == 7 ? 4 : c;
                }
                break;
            default:
                break;
            }
            for (u32 c = 0; c < 4; ++c) {
                ct.map[c] = static_cast<u8>(r[c] >= 4 ? r[c] - 4 : c); // (CompMapping::Map)
            }
        }
        return true;
    }

    static bool TryOwn(const GS::TranslationRequest& req, GS::TranslationResult& res, std::string& why,
                       u32& walk_loads) {
        if (req.code.empty() || req.profile == nullptr) {
            why = "no code or no device profile";
            return false;
        }
        const bool vertex = req.hw == GS::HwStage::Vs;
        const bool fragment = req.hw == GS::HwStage::Ps;
        if (!vertex && !fragment && !(req.start == GS::BindingStart{})) {
            why = "its bindings do not start at 0";
            return false;
        }
        const u32 rsrc2 = req.runtime.rsrc2;
        if ((rsrc2 & 1) != 0) {
            why = "scratch memory";
            return false;
        }
        std::vector<Inst> insts;
        Cfg cfg;
        if (!Decode(req.code, insts, why, true) || !BuildCfg(insts, cfg, why)) {
            return false;
        }
        if (!cfg.Reducible()) {
            why = "an irreducible graph";
            return false;
        }
        const u32 subgroup = req.profile->subgroup_size;
        if (subgroup == 0 || subgroup > 64 || (subgroup & (subgroup - 1)) != 0) {
            why = fmt::format("subgroups of {}", subgroup);
            return false;
        }
        const Structure st = AnalyzeStructure(cfg);
        EmitOptions eo;
        eo.user_sgprs = (rsrc2 >> 1) & 0x1F;
        // the flattened user data the translation sees (as the bridge's: the walk over the request's memory), before
        // the emission (formatted buffers are specialized on their V#'s format; the runtime's key holds it)
        ResourceInfo pre;
        AnalyzeResources(insts, cfg, eo.user_sgprs, pre); // (an unknown V# fails in the emission, at its instruction)
        std::vector<u8> walker;
        if (!BuildSrtWalker(pre, walker, why)) {
            return false;
        }
        res.flatbuf.assign(pre.flat_dwords, 0);
        std::copy(req.user_data.begin(), req.user_data.end(), res.flatbuf.begin());
        if (!req.flatbuf.empty()) {
            if (req.flatbuf.size() != pre.flat_dwords) {
                why = fmt::format("the given flattened user data has {} dwords, the translation's {}", req.flatbuf.size(),
                                  pre.flat_dwords);
                return false;
            }
            res.flatbuf.assign(req.flatbuf.begin(), req.flatbuf.end());
        } else if (!walker.empty()) {
            if (req.memory == nullptr) {
                why = "no guest memory for the SRT walk";
                return false;
            }
            GS::SrtProgram walk;
            GS::SrtProgram::RunStats stats;
            if (!walk.Compile(walker, why) ||
                !walk.Run(std::span<const u32, 16>(req.user_data), res.flatbuf, *req.memory, stats, why)) {
                why = "the SRT walk: " + why;
                return false;
            }
        }
        eo.flat = res.flatbuf;
        eo.rsrc2 = rsrc2;
        eo.subgroup_size = subgroup;
        eo.binding_base = req.start.unified;
        eo.buffer_base = req.start.buffer;
        GS::FetchShader fetch;
        if (vertex) {
            eo.vertex = true;
            if (!PrepareVertex(req, insts, eo.vs, fetch, res, why)) {
                return false;
            }
        } else if (fragment) {
            eo.fragment = true;
            if (!PrepareFragment(req, eo.fs, why)) {
                return false;
            }
        } else {
            eo.compute = true;
            eo.threads = req.runtime.threads;
        }
        EmitResult r = EmitProgram(insts, cfg, st, eo);
        const ResourceInfo& ri = r.ok ? r.resources : pre;
        GS::ShaderReflection& refl = res.reflection;
        const char* dump_env = std::getenv("SSPC_OWN_SHADERS_DUMP");
        if (dump_env != nullptr &&
            (std::string_view(dump_env) == "all" || HashList("SSPC_OWN_SHADERS_DUMP").contains(req.footer_hash))) {
            // SSPC_OWN_SHADERS_DUMP=<footer hashes>: the program, its buffers' V#s as the translation saw them, the SPIR-V
            // (a program not translated too: its code and why)
            std::string text = fmt::format("{} {:016x} at {:#x}, rsrc2 {:#x}, group {}x{}x{}{}\n",
                                           vertex ? "vertex" : (fragment ? "fragment" : "compute"), req.footer_hash,
                                           req.address, rsrc2, eo.threads[0], eo.threads[1], eo.threads[2],
                                           r.ok ? "" : " - not translated: " + r.error);
            for (u32 b = 0; b < ri.buffers.size(); ++b) {
                text += fmt::format("buffer {} (binding {}){}:", b, b + 1, ri.buffers[b].written ? " written" : "");
                for (const ScalarValue& v : ri.buffers[b].sharp) {
                    const bool flat = v.kind == ScalarValue::Kind::Flat;
                    text += fmt::format(" {}{}={:08x}", flat ? "flat" : "const", v.value,
                                        flat && v.value < res.flatbuf.size() ? res.flatbuf[v.value] : v.value);
                }
                text += "\n";
            }
            for (const Inst& in : insts) {
                const Operands ops = GetOperands(in);
                std::string regs;
                for (u32 k = 0; k < ops.num_dst; ++k) {
                    regs += fmt::format(" d{}", ops.dst[k]);
                }
                for (u32 k = 0; k < ops.num_src; ++k) {
                    regs += fmt::format(" s{}", ops.src[k]);
                }
                text += fmt::format("{:5} {:<28} {:08x} {:08x}{}{}\n", in.offset, Mnemonic(in.format, in.op), in.w0, in.w1,
                                    in.literal ? fmt::format(" lit {:08x}", in.lit) : "", regs);
            }
            const std::string base = fmt::format("logs/own_{}_{:016x}", vertex ? "vs" : (fragment ? "ps" : "cs"), req.footer_hash);
            std::ofstream(base + ".txt", std::ios::binary) << text;
            std::ofstream(base + ".spv", std::ios::binary)
                .write(reinterpret_cast<const char*>(r.spirv.data()), static_cast<std::streamsize>(r.spirv.size() * 4));
        }
        if (!r.ok) {
            why = r.error;
            return false;
        }
        if (!(ri.walk == pre.walk) || ri.flat_dwords != pre.flat_dwords) {
            why = "the emission's resources differ from the analysis'";
            return false;
        }
        refl.srt_walker = std::move(walker);
        refl.hw = req.hw;
        refl.sw = req.sw;
        refl.footer_hash = req.footer_hash;
        if (fetch.present) {
            refl.fetch = std::move(fetch);
        }
        refl.mrt_mask = r.mrt_mask;
        refl.has_discard = r.has_discard;
        // the bridge's order (the executor's game fixes, e.g. the particle sort's split, know the buffers by it): the
        // V#s, the flattened user data, GDS
        for (const BufferUse& b : ri.buffers) {
            refl.buffers.push_back({.sharp = Location(b), .kind = GS::BufferKind::Guest, .is_written = b.written});
        }
        refl.buffers.push_back({.kind = GS::BufferKind::Flatbuf});
        if (ri.uses_gds) {
            refl.buffers.push_back({.kind = GS::BufferKind::GdsBuffer, .is_written = true});
        }
        if (eo.vertex && eo.vs.ucp_mask != 0) {
            refl.buffers.push_back({.kind = GS::BufferKind::ClipPlanes});
        }
        for (const ImageUse& im : ri.images) {
            refl.images.push_back({.sharp = Location(im.sharp, im.r128 ? 4 : 8),
                                   .is_depth = im.depth,
                                   .is_array = im.array,
                                   .is_written = im.written,
                                   .is_r128 = im.r128});
        }
        for (const SamplerUse& s : ri.samplers) {
            refl.samplers.push_back({.sharp = Location(s.sharp, 4), .is_depth = s.depth});
        }
        refl.has_storage_images = std::ranges::any_of(ri.images, [](const ImageUse& im) { return im.written; });
        refl.flatbuf_dwords = ri.flat_dwords;
        refl.user_data_used = static_cast<u16>((1u << std::min<u32>(eo.user_sgprs, 16)) - 1);
        refl.uses_shared = ri.uses_lds;
        refl.uses_lane_id = true;
        refl.start = req.start;
        const u32 n = static_cast<u32>(refl.buffers.size());
        const u32 unified = n + static_cast<u32>(refl.images.size() + refl.samplers.size());
        refl.end = {req.start.unified + unified, req.start.buffer + n};
        walk_loads = static_cast<u32>(ri.walk.size());
        res.spirv = std::move(r.spirv);
        return true;
    }

    GS::ShaderTranslator* fallback_;
    u32 stages_ = 0; ///< SSPC_OWN_SHADERS: bit 0 compute, 1 vertex, 2 fragment
    std::unordered_set<u64> only_, skip_;
    std::atomic<u32> own_{0}, declined_{0};
};

} // namespace

void InstallOwnTranslatorIfEnabled() {
    const u32 stages = OwnStages();
    if (stages == 0) {
        return;
    }
    GS::ShaderTranslator* fallback = GS::GetShaderTranslator();
    if (fallback == nullptr) {
        SSPC_LOG(GPU, Warning, "own GCN translator: no translator installed to wrap - not enabled");
        return;
    }
    static OwnTranslator own(fallback, stages);
    GS::InstallShaderTranslator(&own);
    SSPC_LOG(GPU, Info, "own GCN translator: enabled for{}{}{} programs (SSPC_OWN_SHADERS={}), {} for the rest",
             (stages & 1) != 0 ? " compute" : "", (stages & 2) != 0 ? " vertex" : "", (stages & 4) != 0 ? " fragment" : "", std::getenv("SSPC_OWN_SHADERS"),
             fallback->Name());
}

} // namespace SecondSon::Gpu::Gcn
