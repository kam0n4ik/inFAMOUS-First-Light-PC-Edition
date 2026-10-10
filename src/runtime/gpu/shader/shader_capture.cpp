// SecondSonPC — captured translations and their offline comparison (P4.3). See shader_capture.h.
#include "runtime/gpu/shader/shader_capture.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#include <fmt/format.h>
#include <xxhash.h>

#include "base/log.h"
#include "runtime/gpu/cp_registers.h"
#include "runtime/gpu/shader/runtime_params.h"
#include "runtime/gpu/shader/shader_frontend.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "runtime/gpu/vulkan/vk_foundation.h"

namespace SecondSon::Gpu::Shader {

namespace {

constexpr char kMagic[8] = {'S', 'S', 'P', 'C', 'S', 'H', 'D', '1'};

class Out {
public:
    std::vector<u8> b;
    void U8(u8 v) { b.push_back(v); }
    void U32(u32 v) { Raw(&v, 4); }
    void U64(u64 v) { Raw(&v, 8); }
    void Raw(const void* p, size_t n) {
        const auto* c = static_cast<const u8*>(p);
        b.insert(b.end(), c, c + n);
    }
    void Words(std::span<const u32> w) {
        U32(static_cast<u32>(w.size()));
        Raw(w.data(), w.size_bytes());
    }
    void Bytes(std::span<const u8> v) {
        U32(static_cast<u32>(v.size()));
        Raw(v.data(), v.size());
    }
};

class In {
public:
    In(std::span<const u8> d) : data(d) {}
    std::span<const u8> data;
    size_t pos = 0;
    bool ok = true;
    bool Raw(void* p, size_t n) {
        if (!ok || pos + n > data.size()) {
            ok = false;
            return false;
        }
        std::memcpy(p, data.data() + pos, n);
        pos += n;
        return true;
    }
    u8 U8() { u8 v = 0; Raw(&v, 1); return v; }
    u32 U32() { u32 v = 0; Raw(&v, 4); return v; }
    u64 U64() { u64 v = 0; Raw(&v, 8); return v; }
    void Words(std::vector<u32>& w) {
        const u32 n = U32();
        if (!ok || n > (data.size() - pos) / 4) {
            ok = false;
            return;
        }
        w.resize(n);
        Raw(w.data(), n * 4ull);
    }
    void Bytes(std::vector<u8>& v) {
        const u32 n = U32();
        if (!ok || n > data.size() - pos) {
            ok = false;
            return;
        }
        v.resize(n);
        Raw(v.data(), n);
    }
};

#define SSPC_PROFILE_U32(X) X(max_viewport_width) X(max_viewport_height) X(max_shared_memory_size) X(supported_spirv) \
    X(subgroup_size) X(sparse_page_shift)
#define SSPC_PROFILE_BOOL(X)                                                                                           \
    X(support_int8) X(support_int16) X(support_int64) X(support_float16) X(support_float64)                            \
    X(supports_denorm_behavior_independence) X(supports_rounding_mode_independence) X(support_fp16_denorm_preserve)   \
    X(support_fp16_denorm_flush) X(support_fp16_round_to_zero) X(support_fp32_denorm_preserve)                        \
    X(support_fp32_denorm_flush) X(support_fp32_round_to_zero) X(support_fp64_denorm_preserve)                        \
    X(support_fp64_denorm_flush) X(support_fp64_round_to_zero) X(support_fp16_signed_zero_inf_nan_preserve)           \
    X(support_fp32_signed_zero_inf_nan_preserve) X(support_fp64_signed_zero_inf_nan_preserve)                         \
    X(supports_image_load_store_lod) X(supports_native_cube_calc) X(supports_trinary_minmax)                          \
    X(supports_buffer_fp32_atomic_min_max) X(supports_image_fp32_atomic_min_max) X(supports_buffer_int64_atomics)    \
    X(supports_shared_int64_atomics) X(supports_workgroup_explicit_memory_layout)                                    \
    X(supports_amd_shader_explicit_vertex_parameter) X(supports_fragment_shader_barycentric)                          \
    X(supports_shader_subgroup_clock) X(has_broken_spirv_clamp) X(lower_left_origin_mode)                             \
    X(needs_manual_interpolation) X(needs_lds_barriers) X(needs_buffer_offsets) X(needs_unorm_fixup)                  \
    X(needs_clip_distance_emulation) X(supports_shader_stencil_export) X(depth_clip_control)

std::atomic<BridgePipelineCallback> g_callback{nullptr};
CaptureWriter g_writer;
std::atomic<bool> g_writer_open{false};

} // namespace

void SerializeProfile(const DeviceProfile& p, std::vector<u8>& out) {
    Out o;
#define X(f) o.U32(p.f);
    SSPC_PROFILE_U32(X)
#undef X
#define X(f) o.U8(p.f ? 1 : 0);
    SSPC_PROFILE_BOOL(X)
#undef X
    o.Bytes(std::span<const u8>(reinterpret_cast<const u8*>(p.source.data()), p.source.size()));
    out = std::move(o.b);
}

bool DeserializeProfile(std::span<const u8> in, size_t& pos, DeviceProfile& p) {
    In r(in);
    r.pos = pos;
#define X(f) p.f = r.U32();
    SSPC_PROFILE_U32(X)
#undef X
#define X(f) p.f = r.U8() != 0;
    SSPC_PROFILE_BOOL(X)
#undef X
    std::vector<u8> s;
    r.Bytes(s);
    p.source.assign(s.begin(), s.end());
    pos = r.pos;
    return r.ok;
}

std::string DiffProfiles(const DeviceProfile& a, const DeviceProfile& b) {
    std::string d;
#define X(f)                                                                                                           \
    if (a.f != b.f) {                                                                                                  \
        d += fmt::format("{}{}: {} / {}", d.empty() ? "" : ", ", #f, a.f, b.f);                                        \
    }
    SSPC_PROFILE_U32(X)
    SSPC_PROFILE_BOOL(X)
#undef X
    return d;
}

bool CaptureWriter::Open(const std::filesystem::path& path, std::string_view info) {
    std::scoped_lock lock{mutex_};
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_) {
        return false;
    }
    file_.write(kMagic, sizeof(kMagic));
    const u32 type = 'I', bytes = static_cast<u32>(info.size());
    file_.write(reinterpret_cast<const char*>(&type), 4);
    file_.write(reinterpret_cast<const char*>(&bytes), 4);
    file_.write(info.data(), info.size());
    bytes_ = sizeof(kMagic) + 8 + info.size();
    return true;
}

void CaptureWriter::Write(const CapturedModule& m) {
    Out o;
    o.U8(static_cast<u8>(m.hw));
    o.U8(static_cast<u8>(m.sw));
    o.U8(m.has_fetch ? 1 : 0);
    o.U8(0);
    o.U32(m.perm_idx);
    o.U64(m.footer_hash);
    o.U64(m.address);
    o.U32(m.start.unified);
    o.U32(m.start.buffer);
    o.U32(m.vertex_sgpr_offset);
    o.U32(m.instance_sgpr_offset);
    o.U32(m.fetch_sgpr_base);
    o.U32(m.primitive_type);
    o.Raw(m.user_data.data(), 64);
    o.Raw(m.cs.data(), sizeof(m.cs));
    o.Words(m.code);
    o.Words(m.flat);
    o.Words(m.fetch_code);
    o.U32(static_cast<u32>(m.vertex_sharps.size()));
    for (const auto& v : m.vertex_sharps) {
        o.Raw(v.data(), 16);
    }
    o.Words(m.sh);
    o.Words(m.context);
    o.Words(m.spirv);
    std::vector<u8> prof;
    SerializeProfile(m.profile, prof);
    o.Bytes(prof);
    o.Bytes(m.bridge_runtime);
    std::scoped_lock lock{mutex_};
    if (!file_) {
        return;
    }
    const u32 type = 'M', bytes = static_cast<u32>(o.b.size());
    file_.write(reinterpret_cast<const char*>(&type), 4);
    file_.write(reinterpret_cast<const char*>(&bytes), 4);
    file_.write(reinterpret_cast<const char*>(o.b.data()), static_cast<std::streamsize>(o.b.size()));
    file_.flush(); // a capture cut by the process's end keeps every module written so far
    ++records_;
    bytes_ += 8 + o.b.size();
}

void CaptureWriter::Close() {
    std::scoped_lock lock{mutex_};
    if (file_.is_open()) {
        const u32 type = 'E', bytes = 0;
        file_.write(reinterpret_cast<const char*>(&type), 4);
        file_.write(reinterpret_cast<const char*>(&bytes), 4);
        file_.close();
    }
}

bool LoadCapture(const std::filesystem::path& path, std::vector<CapturedModule>& out, std::string& info,
                 std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path.string();
        return false;
    }
    std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < sizeof(kMagic) || std::memcmp(data.data(), kMagic, sizeof(kMagic)) != 0) {
        error = "not a shader capture (magic)";
        return false;
    }
    size_t pos = sizeof(kMagic);
    while (pos + 8 <= data.size()) {
        u32 type = 0, bytes = 0;
        std::memcpy(&type, data.data() + pos, 4);
        std::memcpy(&bytes, data.data() + pos + 4, 4);
        pos += 8;
        if (pos + bytes > data.size()) {
            error = fmt::format("record at {} cut short", pos - 8);
            return false; // a record cut by the end: the modules before it are kept
        }
        const std::span<const u8> rec(data.data() + pos, bytes);
        pos += bytes;
        if (type == 'I') {
            info.assign(rec.begin(), rec.end());
        } else if (type == 'E') {
            break;
        } else if (type == 'M') {
            In r(rec);
            CapturedModule m;
            m.hw = static_cast<HwStage>(r.U8());
            m.sw = static_cast<SwStage>(r.U8());
            m.has_fetch = r.U8() != 0;
            r.U8();
            m.perm_idx = r.U32();
            m.footer_hash = r.U64();
            m.address = r.U64();
            m.start.unified = r.U32();
            m.start.buffer = r.U32();
            m.vertex_sgpr_offset = r.U32();
            m.instance_sgpr_offset = r.U32();
            m.fetch_sgpr_base = r.U32();
            m.primitive_type = r.U32();
            r.Raw(m.user_data.data(), 64);
            r.Raw(m.cs.data(), sizeof(m.cs));
            r.Words(m.code);
            r.Words(m.flat);
            r.Words(m.fetch_code);
            const u32 nv = r.U32();
            for (u32 i = 0; i < nv && r.ok && i < 64; ++i) {
                std::array<u32, 4> v{};
                r.Raw(v.data(), 16);
                m.vertex_sharps.push_back(v);
            }
            r.Words(m.sh);
            r.Words(m.context);
            r.Words(m.spirv);
            std::vector<u8> prof;
            r.Bytes(prof);
            size_t pp = 0;
            const bool prof_ok = DeserializeProfile(prof, pp, m.profile);
            r.Bytes(m.bridge_runtime);
            if (!r.ok || !prof_ok || m.sh.size() != 0x400 || m.context.size() != 0x400 ||
                static_cast<u8>(m.hw) >= static_cast<u8>(HwStage::Count)) {
                error = fmt::format("module record {} is malformed", out.size());
                return false;
            }
            out.push_back(std::move(m));
        }
    }
    return true;
}

CaptureWriter* GetCaptureWriter() {
    return g_writer_open.load(std::memory_order_acquire) ? &g_writer : nullptr;
}

bool StartCapture(const std::filesystem::path& path) {
    if (!g_writer.Open(path, "SecondSonPC shader capture (P4.3): the bridge's translations with their inputs")) {
        return false;
    }
    g_writer_open.store(true, std::memory_order_release);
    return true;
}

void StopCapture() {
    g_writer_open.store(false, std::memory_order_release);
    g_writer.Close();
}

void SetBridgePipelineCallback(BridgePipelineCallback callback) {
    g_callback.store(callback, std::memory_order_release);
}

BridgePipelineCallback GetBridgePipelineCallback() {
    return g_callback.load(std::memory_order_acquire);
}

// ---- offline comparison

namespace {

/// The register file of a captured module's draw (the SH and context windows, the primitive type).
void LoadRegisters(const CapturedModule& m, Cp::RegisterFile& regs) {
    std::array<u32, Cp::kNumRegs> flat{};
    std::copy(m.sh.begin(), m.sh.end(), flat.begin() + Cp::kSpaceFirst[static_cast<u32>(Cp::Space::Sh)]);
    std::copy(m.context.begin(), m.context.end(), flat.begin() + Cp::kSpaceFirst[static_cast<u32>(Cp::Space::Context)]);
    u32 index = 0;
    if (Cp::FlatIndex(Reg::VgtPrimitiveType, index)) {
        flat[index] = m.primitive_type;
    }
    regs.Load(flat);
}

struct Tally {
    u64 n = 0;
    std::vector<std::string> examples;
    void Add(std::string text) {
        ++n;
        if (examples.size() < 8) {
            examples.push_back(std::move(text));
        }
    }
};

} // namespace

int RunShaderVerify(const std::filesystem::path& capture, const std::filesystem::path& logs_dir) {
    std::vector<CapturedModule> modules;
    std::string info, error;
    const bool loaded = LoadCapture(capture, modules, info, error);
    std::string t = fmt::format("SecondSonPC shader verification (--gpu-shader-verify, P4.3): {}\n", capture.string());
    if (!loaded && modules.empty()) {
        t += "cannot load: " + error + "\n";
        std::ofstream(logs_dir / "gpu_shader_verify.txt", std::ios::binary) << t;
        std::fwrite(t.data(), 1, t.size(), stdout);
        return 2;
    }
    if (!loaded) {
        t += fmt::format("load stopped early ({}); {} modules read\n", error, modules.size());
    }
    ShaderTranslator* translator = GetShaderTranslator();
    if (translator == nullptr) {
        t += "no translator installed\n";
        std::ofstream(logs_dir / "gpu_shader_verify.txt", std::ios::binary) << t;
        return 2;
    }
    t += fmt::format("{}\ntranslator: {}\nmodules captured: {}\n", info, translator->Name(), modules.size());

    std::map<HwStage, u64> by_stage;
    std::set<u64> addresses, footers;
    std::set<std::pair<u64, u64>> contents;
    Tally runtime_diff, translate_fail, spirv_diff, fetch_diff, srt_fail, coarser, profile_diff;
    u64 runtime_equal = 0, translated = 0, spirv_equal = 0, fetch_equal = 0, srt_ok = 0, srt_none = 0;
    u64 translate_us = 0, emit_us = 0, spirv_words = 0;
    std::array<u64, static_cast<size_t>(ResourceStatus::Count)> statuses{};
    // the specialization keys SecondSonPC computes for the bridge's modules of one program: two of the bridge's
    // permutations must never get one key (a key coarser than the bridge's would merge them)
    // (footer, content lo) -> key hash -> (bridge perm, its SPIR-V hash)
    std::map<std::pair<u64, u64>, std::map<u64, std::pair<u32, u64>>> keys;
    u64 bridge_duplicates = 0;
    u64 ps_programs = 0, ps_user4_loaded = 0, ps_user4_read = 0;
    std::set<u64> ps_seen;
    const DeviceProfile* first_profile = modules.empty() ? nullptr : &modules[0].profile;
    std::vector<std::vector<u32>> spirv_for_vulkan;

    for (size_t i = 0; i < modules.size(); ++i) {
        const CapturedModule& m = modules[i];
        ++by_stage[m.hw];
        addresses.insert(m.address);
        footers.insert(m.footer_hash);
        const ContentHash ch = HashCode(m.code);
        contents.insert({ch.lo, ch.hi});
        if (first_profile != nullptr) {
            const std::string d = DiffProfiles(*first_profile, m.profile);
            if (!d.empty()) {
                profile_diff.Add(fmt::format("module {}: {}", i, d));
            }
        }
        // SecondSonPC's runtime parameters from the draw's registers, against the bridge's runtime information
        Cp::RegisterFile regs;
        LoadRegisters(m, regs);
        RuntimeParams rp;
        bool rp_ok = true;
        if (m.hw == HwStage::Cs) {
            BuildComputeRuntime(std::span<const u32, Cp::kComputeCount>(m.cs.data(), Cp::kComputeCount), rp);
        } else {
            rp_ok = BuildGraphicsRuntime(regs, m.hw, m.sw, m.vertex_sgpr_offset, m.instance_sgpr_offset, rp);
        }
        std::string diff;
        if (!rp_ok) {
            runtime_diff.Add(fmt::format("module {} ({} {:#018x}): stage not prepared", i, HwStageName(m.hw),
                                         m.footer_hash));
        } else if (translator->CompareRuntime(rp, m.profile, m.bridge_runtime, diff)) {
            ++runtime_equal;
        } else {
            runtime_diff.Add(fmt::format("module {} ({} {:#018x} perm {}): {}", i, HwStageName(m.hw), m.footer_hash,
                                         m.perm_idx, diff));
        }
        if (!rp_ok) {
            continue;
        }
        // the translation through SecondSonPC's interface, with the bridge's inputs
        TranslationRequest req;
        req.hw = m.hw;
        req.sw = m.sw;
        req.address = m.address;
        req.code = m.code;
        req.user_data = m.user_data;
        req.runtime = rp;
        req.profile = &m.profile;
        req.start = m.start;
        req.flatbuf = m.flat;
        req.fetch_code = m.fetch_code;
        req.vertex_sharps = m.vertex_sharps;
        req.footer_hash = m.footer_hash;
        const TranslationResult res = translator->Translate(req);
        if (!res.Ok()) {
            translate_fail.Add(fmt::format("module {} ({} {:#018x}): {}: {}", i, HwStageName(m.hw), m.footer_hash,
                                           TranslationErrorName(res.error), res.detail));
            continue;
        }
        ++translated;
        translate_us += res.translate_us;
        emit_us += res.emit_us;
        spirv_words += res.spirv.size();
        if (res.spirv == m.spirv) {
            ++spirv_equal;
        } else {
            size_t at = 0;
            while (at < res.spirv.size() && at < m.spirv.size() && res.spirv[at] == m.spirv[at]) {
                ++at;
            }
            spirv_diff.Add(fmt::format("module {} ({} {:#018x} perm {}): {} words vs the bridge's {}, first difference "
                                       "at word {}",
                                       i, HwStageName(m.hw), m.footer_hash, m.perm_idx, res.spirv.size(),
                                       m.spirv.size(), at));
        }
        spirv_for_vulkan.push_back(res.spirv);
        const ShaderReflection& r = res.reflection;
        // SecondSonPC's fetch shader parser against the recompiler's
        if (m.has_fetch) {
            FetchShader ours;
            std::string fe;
            if (!ParseFetchShaderCode(m.fetch_code, m.fetch_sgpr_base, ours, fe)) {
                fetch_diff.Add(fmt::format("module {}: SecondSonPC's parser: {}", i, fe));
            } else if (ours.attributes != r.fetch.attributes || ours.vertex_offset_sgpr != r.fetch.vertex_offset_sgpr ||
                       ours.instance_offset_sgpr != r.fetch.instance_offset_sgpr || ours.size != r.fetch.size) {
                fetch_diff.Add(fmt::format("module {}: {} attributes (ours) vs {} (recompiler), size {} vs {}", i,
                                           ours.attributes.size(), r.fetch.attributes.size(), ours.size, r.fetch.size));
            } else {
                ++fetch_equal;
            }
        }
        // SecondSonPC's decoding of the walker the recompiler generated
        if (r.srt_walker.empty()) {
            ++srt_none;
        } else {
            SrtProgram srt;
            std::string se;
            if (srt.Compile(r.srt_walker, se)) {
                ++srt_ok;
            } else {
                srt_fail.Add(fmt::format("module {} ({:#018x}): {}", i, m.footer_hash, se));
            }
        }
        // descriptors from the bridge's flattened user data and V#s; the specialization key
        ResourceBindingPlan plan;
        plan.flat = m.flat;
        const FetchShader* fetch = r.fetch.present ? &r.fetch : nullptr;
        DecodeResources(r, m.start, fetch, m.user_data, m.address, VertexSource{nullptr, m.vertex_sharps}, plan);
        for (size_t s = 0; s < statuses.size(); ++s) {
            statuses[s] += plan.status_counts[s];
        }
        SpecializationKey key;
        BuildSpecializationKey(rp, m.start, r, fetch, plan, key);
        auto& per_program = keys[{m.footer_hash, ch.lo}];
        const u64 spv_hash = XXH3_64bits(m.spirv.data(), m.spirv.size() * 4);
        if (const auto it = per_program.find(key.hash); it != per_program.end() && it->second.first != m.perm_idx) {
            if (it->second.second == spv_hash) {
                // the bridge translated the same specialization twice (identical SPIR-V): its own duplicate
                ++bridge_duplicates;
            } else {
                coarser.Add(fmt::format("program {:#018x}: the bridge's permutations {} and {} (different SPIR-V) get "
                                        "one key",
                                        m.footer_hash, it->second.first, m.perm_idx));
            }
        } else {
            per_program.emplace(key.hash, std::pair<u32, u64>{m.perm_idx, spv_hash});
        }
        // USER_DATA_4 of fragment programs (docs §8): does the hardware load it (RSRC2.USER_SGPR > 4), does the
        // program read s4 (the IR after optimisation)?
        if (m.hw == HwStage::Ps && ps_seen.insert(ch.lo ^ ch.hi).second) {
            ++ps_programs;
            const u32 user_sgprs = (m.sh[Reg::kPgmLo[0] + 3 - 0x2C00] >> 1) & 0x1F;
            ps_user4_loaded += user_sgprs > 4 ? 1 : 0;
            ps_user4_read += (r.user_data_used >> 4) & 1;
        }
    }

    // the device's acceptance of the SPIR-V (SecondSonPC's Vulkan foundation: vkCreateShaderModule)
    std::string vk_line = "Vulkan: not checked";
    {
        u32 accepted = 0, refused = 0;
        std::string vk_error;
        if (Vk::CheckShaderModules(spirv_for_vulkan, accepted, refused, vk_error)) {
            vk_line = fmt::format("Vulkan (vkCreateShaderModule on the device): {} accepted, {} refused{}", accepted,
                                  refused, vk_error.empty() ? "" : " (" + vk_error + ")");
        } else {
            vk_line = "Vulkan: " + vk_error;
        }
    }

    t += "by stage:";
    for (const auto& [stage, n] : by_stage) {
        t += fmt::format(" {}={}", HwStageName(stage), n);
    }
    t += fmt::format("\nprogram addresses {}, programs by footer hash {}, distinct GCN contents {}\n", addresses.size(),
                     footers.size(), contents.size());
    const auto tally = [&](std::string_view name, u64 good, const Tally& bad) {
        t += fmt::format("{}: {} equal, {} different\n", name, good, bad.n);
        for (const auto& e : bad.examples) {
            t += "  " + e + "\n";
        }
    };
    tally("runtime information (SecondSonPC's registers -> runtime parameters vs the bridge's)", runtime_equal,
          runtime_diff);
    t += fmt::format("translations through SecondSonPC's interface: {} done, {} failed\n", translated, translate_fail.n);
    for (const auto& e : translate_fail.examples) {
        t += "  " + e + "\n";
    }
    tally("SPIR-V (word for word with the bridge's)", spirv_equal, spirv_diff);
    tally("fetch shaders (SecondSonPC's parser vs the recompiler's)", fetch_equal, fetch_diff);
    t += fmt::format("SRT walkers decoded by SecondSonPC: {} ok, {} not interpretable, {} programs without one\n",
                     srt_ok, srt_fail.n, srt_none);
    for (const auto& e : srt_fail.examples) {
        t += "  " + e + "\n";
    }
    t += "descriptors from the bridge's flattened user data:";
    for (size_t s = 0; s < statuses.size(); ++s) {
        t += fmt::format(" {}={}", ResourceStatusName(static_cast<ResourceStatus>(s)), statuses[s]);
    }
    t += fmt::format("\nspecialization keys coarser than the bridge's (two of its permutations with different SPIR-V, "
                     "one key): {}; the bridge's own duplicate permutations (same key, same SPIR-V): {}\n",
                     coarser.n, bridge_duplicates);
    for (const auto& e : coarser.examples) {
        t += "  " + e + "\n";
    }
    t += fmt::format("device profiles differing between modules: {}\n", profile_diff.n);
    t += fmt::format("fragment programs: {}; the hardware loads USER_DATA_4 into s4 (RSRC2.USER_SGPR > 4) in {}; the "
                     "program reads s4 in {}\n",
                     ps_programs, ps_user4_loaded, ps_user4_read);
    t += vk_line + "\n";
    t += fmt::format("time: translate {} ms, emit {} ms ({:.0f} us per module), SPIR-V {} KiB\n", translate_us / 1000,
                     emit_us / 1000,
                     translated == 0 ? 0.0 : static_cast<double>(translate_us + emit_us) / static_cast<double>(translated),
                     spirv_words * 4 / 1024);
    const bool pass = loaded && runtime_diff.n == 0 && translate_fail.n == 0 && spirv_diff.n == 0 &&
                      fetch_diff.n == 0 && srt_fail.n == 0 && coarser.n == 0;
    t += fmt::format("RESULT {}\n", pass ? "PASS" : "DIFFERENCES");
    std::ofstream(logs_dir / "gpu_shader_verify.txt", std::ios::binary) << t;
    std::fwrite(t.data(), 1, t.size(), stdout);
    SSPC_LOG(Shader, Info, "--gpu-shader-verify: {} modules, {} translated, SPIR-V equal {}, report {}",
             modules.size(), translated, spirv_equal, (logs_dir / "gpu_shader_verify.txt").string());
    return pass ? 0 : 1;
}

} // namespace SecondSon::Gpu::Shader
