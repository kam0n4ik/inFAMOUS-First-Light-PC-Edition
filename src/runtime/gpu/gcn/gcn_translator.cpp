// SecondSonPC — the own GCN translator as the runtime's ShaderTranslator (gcn_translator.h).
#include "runtime/gpu/gcn/gcn_translator.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
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

GS::SharpLocation Location(const BufferUse& b) {
    GS::SharpLocation loc;
    loc.dwords = 4;
    bool consecutive = true;
    for (u32 i = 0; i < 4; ++i) {
        consecutive =
            consecutive && b.sharp[i].kind == ScalarValue::Kind::Flat && b.sharp[i].value == b.sharp[0].value + i;
    }
    if (consecutive) {
        loc.summary = GS::SharpLocation::Summary::SingleLoad;
        loc.offsets[0] = static_cast<u16>(b.sharp[0].value);
        return loc;
    }
    loc.summary = GS::SharpLocation::Summary::MultiLoad;
    for (u32 i = 0; i < 4; ++i) {
        if (b.sharp[i].kind == ScalarValue::Kind::Flat) {
            loc.load_mask |= static_cast<u8>(1u << i);
            loc.offsets[i] = static_cast<u16>(b.sharp[i].value);
        } else {
            loc.immediates[i] = b.sharp[i].value;
        }
    }
    return loc;
}

class OwnTranslator final : public GS::ShaderTranslator {
public:
    explicit OwnTranslator(GS::ShaderTranslator* fallback)
        : fallback_(fallback), only_(HashList("SSPC_OWN_SHADERS_ONLY")), skip_(HashList("SSPC_OWN_SHADERS_SKIP")) {}

    std::string_view Name() const override {
        return "SecondSonPC GCN translator (compute), the bridge's for the rest";
    }

    GS::TranslationResult Translate(const GS::TranslationRequest& req) override {
        if (req.hw == GS::HwStage::Cs && req.sw == GS::SwStage::Compute && Wanted(req.footer_hash)) {
            GS::TranslationResult res;
            std::string why;
            u32 walk_loads = 0;
            const auto t0 = std::chrono::steady_clock::now();
            if (TryOwn(req, res, why, walk_loads)) {
                res.translate_us = static_cast<u64>(
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count());
                const u32 n = ++own_;
                SSPC_LOG(GPU, Info,
                         "own GCN translator: compute {:016x} at {:#x} ({} dwords) translated - {} SPIR-V words, {} "
                         "buffers, flat {} dwords, walk {} loads, group {}x{}x{} [{} own so far]",
                         req.footer_hash, req.address, req.code.size(), res.spirv.size(), res.reflection.buffers.size(),
                         res.reflection.flatbuf_dwords, walk_loads, req.runtime.threads[0], req.runtime.threads[1],
                         req.runtime.threads[2], n);
                return res;
            }
            if (const u32 n = ++declined_; n <= 200) {
                SSPC_LOG(GPU, Info, "own GCN translator: compute {:016x} to the bridge's ({})", req.footer_hash, why);
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

    static bool TryOwn(const GS::TranslationRequest& req, GS::TranslationResult& res, std::string& why,
                       u32& walk_loads) {
        if (req.code.empty() || req.profile == nullptr) {
            why = "no code or no device profile";
            return false;
        }
        if (!(req.start == GS::BindingStart{})) {
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
        eo.compute = true;
        eo.threads = req.runtime.threads;
        eo.rsrc2 = rsrc2;
        eo.subgroup_size = subgroup;
        EmitResult r = EmitProgram(insts, cfg, st, eo);
        if (!r.ok) {
            why = r.error;
            return false;
        }
        const ResourceInfo& ri = r.resources;
        GS::ShaderReflection& refl = res.reflection;
        if (!BuildSrtWalker(ri, refl.srt_walker, why)) {
            return false;
        }
        refl.hw = req.hw;
        refl.sw = req.sw;
        refl.footer_hash = req.footer_hash;
        refl.buffers.push_back({.kind = GS::BufferKind::Flatbuf}); // binding 0
        for (const BufferUse& b : ri.buffers) {
            refl.buffers.push_back({.sharp = Location(b), .kind = GS::BufferKind::Guest, .is_written = b.written});
        }
        if (ri.uses_gds) {
            refl.buffers.push_back({.kind = GS::BufferKind::GdsBuffer, .is_written = true});
        }
        refl.flatbuf_dwords = ri.flat_dwords;
        refl.user_data_used = static_cast<u16>((1u << std::min<u32>(eo.user_sgprs, 16)) - 1);
        refl.uses_shared = ri.uses_lds;
        refl.uses_lane_id = true;
        refl.start = req.start;
        const u32 n = static_cast<u32>(refl.buffers.size());
        refl.end = {req.start.unified + n, req.start.buffer + n};
        // the flattened user data the translation sees (as the bridge's: the walk over the request's memory)
        res.flatbuf.assign(ri.flat_dwords, 0);
        std::copy(req.user_data.begin(), req.user_data.end(), res.flatbuf.begin());
        if (!req.flatbuf.empty()) {
            if (req.flatbuf.size() != ri.flat_dwords) {
                why = fmt::format("the given flattened user data has {} dwords, the translation's {}", req.flatbuf.size(),
                                  ri.flat_dwords);
                return false;
            }
            res.flatbuf.assign(req.flatbuf.begin(), req.flatbuf.end());
        } else if (!refl.srt_walker.empty()) {
            if (req.memory == nullptr) {
                why = "no guest memory for the SRT walk";
                return false;
            }
            GS::SrtProgram walk;
            GS::SrtProgram::RunStats stats;
            if (!walk.Compile(refl.srt_walker, why) ||
                !walk.Run(std::span<const u32, 16>(req.user_data), res.flatbuf, *req.memory, stats, why)) {
                why = "the SRT walk: " + why;
                return false;
            }
        }
        walk_loads = static_cast<u32>(ri.walk.size());
        res.spirv = std::move(r.spirv);
        return true;
    }

    GS::ShaderTranslator* fallback_;
    std::unordered_set<u64> only_, skip_;
    std::atomic<u32> own_{0}, declined_{0};
};

} // namespace

void InstallOwnTranslatorIfEnabled() {
    const char* v = std::getenv("SSPC_OWN_SHADERS");
    if (v == nullptr || std::string_view(v) != "cs") {
        return;
    }
    GS::ShaderTranslator* fallback = GS::GetShaderTranslator();
    if (fallback == nullptr) {
        SSPC_LOG(GPU, Warning, "own GCN translator: no translator installed to wrap - not enabled");
        return;
    }
    static OwnTranslator own(fallback);
    GS::InstallShaderTranslator(&own);
    SSPC_LOG(GPU, Info, "own GCN translator: enabled for compute programs (SSPC_OWN_SHADERS=cs), {} for the rest",
             fallback->Name());
}

} // namespace SecondSon::Gpu::Gcn
