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
        eo.compute = true;
        eo.threads = req.runtime.threads;
        eo.rsrc2 = rsrc2;
        eo.subgroup_size = subgroup;
        EmitResult r = EmitProgram(insts, cfg, st, eo);
        const ResourceInfo& ri = r.ok ? r.resources : pre;
        GS::ShaderReflection& refl = res.reflection;
        if (HashList("SSPC_OWN_SHADERS_DUMP").contains(req.footer_hash)) {
            // SSPC_OWN_SHADERS_DUMP=<footer hashes>: the program, its buffers' V#s as the translation saw them, the SPIR-V
            // (a program not translated too: its code and why)
            std::string text = fmt::format("compute {:016x} at {:#x}, rsrc2 {:#x}, group {}x{}x{}{}\n", req.footer_hash,
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
            const std::string base = fmt::format("logs/own_cs_{:016x}", req.footer_hash);
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
        // the bridge's order (the executor's game fixes, e.g. the particle sort's split, know the buffers by it): the
        // V#s, the flattened user data, GDS
        for (const BufferUse& b : ri.buffers) {
            refl.buffers.push_back({.sharp = Location(b), .kind = GS::BufferKind::Guest, .is_written = b.written});
        }
        refl.buffers.push_back({.kind = GS::BufferKind::Flatbuf});
        if (ri.uses_gds) {
            refl.buffers.push_back({.kind = GS::BufferKind::GdsBuffer, .is_written = true});
        }
        for (const ImageUse& im : ri.images) {
            refl.images.push_back({.sharp = Location(im.sharp, im.r128 ? 4 : 8),
                                   .is_array = im.array,
                                   .is_written = im.written,
                                   .is_r128 = im.r128});
        }
        for (const SamplerUse& s : ri.samplers) {
            refl.samplers.push_back({.sharp = Location(s.sharp, 4)});
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
