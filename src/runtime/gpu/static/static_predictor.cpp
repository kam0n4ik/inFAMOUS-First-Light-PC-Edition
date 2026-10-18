// SecondSonPC — modules of the game's material draws translated before the game meets them. See static_predictor.h.
#include "runtime/gpu/static/static_predictor.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <tuple>
#include <unordered_set>

#include <fmt/format.h>
#include <xxhash.h>

#include "runtime/gpu/shader/gcn_program.h"
#include "runtime/gpu/shader/shader_cache.h"
#include "runtime/gpu/shader/shader_disk_cache.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/gpu/shader/shader_translator.h"
#include "runtime/gpu/shader/shader_types.h"

namespace SecondSon::Gpu::Static {

namespace GS = Gpu::Shader;

namespace {

// where the synthetic memory puts what the user data point at (inside the 40-bit GPU space, 256-byte aligned)
constexpr u64 kProgramAddr = 0x10'0000'0000ull;
constexpr u64 kFetchAddr = 0x11'0000'0000ull;
constexpr u64 kEudAddr = 0x12'0000'0000ull;
constexpr u64 kVertexTableAddr = 0x13'0000'0000ull;
constexpr u32 kEudBytes = 0x1000;
constexpr u8 kStageVs = 0, kStagePs = 3; // a pass's resource pair stage codes

// Gnmx input usage slot types the game's VS/PS use
constexpr u8 kResource = 0, kSampler = 1, kConstantBuffer = 2, kRwResource = 4, kFetchShaderPtr = 18,
             kVertexTablePtr = 23, kExtendedUserDataPtr = 27;

/// Guest memory made of the regions a request needs.
class SynthMemory final : public GS::GuestReader {
public:
    void Map(u64 addr, const void* p, size_t n, size_t pad = 0) {
        Region& r = regions_.emplace_back();
        r.addr = addr;
        r.bytes.assign(static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
        r.bytes.resize(n + pad);
    }
    bool Read(u64 addr, void* dst, u64 bytes) override {
        for (const Region& r : regions_) {
            if (addr >= r.addr && addr - r.addr + bytes <= r.bytes.size()) {
                std::memcpy(dst, r.bytes.data() + (addr - r.addr), bytes);
                return true;
            }
        }
        return false;
    }

private:
    struct Region {
        u64 addr = 0;
        std::vector<u8> bytes;
    };
    std::vector<Region> regions_;
};

bool ParseHex(std::istringstream& in, u64& v) {
    std::string t;
    if (!(in >> t)) {
        return false;
    }
    const auto r = std::from_chars(t.data(), t.data() + t.size(), v, 16);
    return r.ec == std::errc{} && r.ptr == t.data() + t.size();
}

template <size_t N>
bool ParseDwords(std::istringstream& in, std::array<u32, N>& out) {
    for (u32& w : out) {
        u64 v = 0;
        if (!ParseHex(in, v)) {
            return false;
        }
        w = static_cast<u32>(v);
    }
    return true;
}

/// The user data registers and the extended user data a draw of a stage gives.
struct UserData {
    std::array<u32, 16> regs{};
    std::vector<u32> eud;
    void Put(u32 reg, const u32* w, u32 n) {
        for (u32 i = 0; i < n; ++i) {
            const u32 at = reg + i;
            if (at < 16) {
                regs[at] = w[i];
            } else {
                if (eud.size() <= at - 16) {
                    eud.resize(at - 16 + 1);
                }
                eud[at - 16] = w[i];
            }
        }
    }
    void PutAddress(u32 reg, u64 addr) {
        const u32 w[2] = {static_cast<u32>(addr), static_cast<u32>(addr >> 32)};
        Put(reg, w, 2);
    }
};

/// The engine's load-time adjustment of the PS struct's DB_SHADER_CONTROL (eboot 0x4468e0).
u32 EngineDbShaderControl(u32 v) {
    if ((v & 0x40) != 0) {
        v = 0x60;
    }
    if ((v & 1) != 0) {
        v &= 0xFFFFFFCFu;
    }
    if ((v & 0x900) != 0) {
        v = 0x920;
    }
    return v;
}

/// SPI_PS_INPUT_CNTL_i for a PS input: the VS export with its semantic (its out index), else the default value; flat
/// shading bit 10; semantic flag bit 12 -> bit 5 (checked on every material pass pair of three scenes).
u32 PsInputCntl(u16 input, const std::vector<u16>& exports) {
    const u32 sem = input & 0xFF;
    u32 v = 0x20u | (((input >> 8) & 3u) << 8);
    for (const u16 e : exports) {
        if ((e & 0xFFu) == sem) {
            v = (e >> 8) & 0x1Fu;
            break;
        }
    }
    if ((input >> 10) & 1) {
        v |= 1u << 10;
    }
    if ((input >> 12) & 1) {
        v |= 1u << 5;
    }
    return v & GS::kPsInputCntlMask;
}

struct Variant {
    std::vector<const std::array<u32, 8>*> material; ///< per material texture (nullptr: not found)
    const std::vector<std::array<u32, 4>>* streams = nullptr;
};

/// One translation to make.
struct Request {
    const StaticShader* shader = nullptr;
    GS::RuntimeParams runtime;
    GS::BindingStart start;
    UserData ud;
    const std::vector<u32>* fetch = nullptr;
    const std::vector<std::array<u32, 4>>* streams = nullptr;
};

struct Context {
    Context(const StaticDraws& d, const StaticTemplates& t, GS::ShaderTranslator& tr, const GS::DeviceProfile& p,
            GS::ShaderDiskCache* dc)
        : draws(d), templates(t), translator(tr), profile(p), disk(dc) {}
    const StaticDraws& draws;
    const StaticTemplates& templates;
    GS::ShaderTranslator& translator;
    const GS::DeviceProfile& profile;
    GS::ShaderDiskCache* disk;
    std::mutex mutex; ///< disk, report, layouts, first_error
    std::unordered_set<u64> layouts; ///< (identity, runtime) given a layout record
    std::unordered_set<u64> skip;    ///< footer hashes not to translate (static_predict_skip.txt: the translator crashed)
    std::atomic<u64> resumed{0}, skipped{0};
    std::ofstream report;
    std::FILE* trace = nullptr; ///< SSPC_STATIC_TRACE=1: each translation before it starts (a crash names its request)
    std::atomic<u64> translated{0}, failed{0}, added{0}, layout_count{0}, walk_mismatch{0};
    std::atomic<u64> unknown_textures{0}, unknown_buffers{0};
    std::string first_error;
};

u64 RequestHash(const Request& r) {
    XXH3_state_t* s = XXH3_createState();
    XXH3_64bits_reset(s);
    XXH3_64bits_update(s, &r.shader->footer_hash, 8);
    XXH3_64bits_update(s, &r.runtime, sizeof(r.runtime));
    XXH3_64bits_update(s, &r.start, sizeof(r.start));
    XXH3_64bits_update(s, r.ud.regs.data(), sizeof(r.ud.regs));
    XXH3_64bits_update(s, r.ud.eud.data(), r.ud.eud.size() * 4);
    if (r.fetch != nullptr) {
        XXH3_64bits_update(s, r.fetch->data(), r.fetch->size() * 4);
    }
    if (r.streams != nullptr) {
        XXH3_64bits_update(s, r.streams->data(), r.streams->size() * 16);
    }
    const u64 h = XXH3_64bits_digest(s);
    XXH3_freeState(s);
    return h;
}

/// The user data of a stage of `pass` (stage code kStageVs/kStagePs) with the variant's material textures.
void FillUserData(Context& c, const StaticShader& sh, const StaticPass& pass, u8 stage, const Variant& v, UserData& ud) {
    const StaticTemplates& t = c.templates;
    for (const u32 slot : sh.slots) {
        const u8 type = slot & 0xFF, api = (slot >> 8) & 0xFF, reg = (slot >> 16) & 0xFF, flags = slot >> 24;
        switch (type) {
        case kResource:
        case kRwResource: {
            const StaticSlot* e = nullptr;
            for (const StaticSlot& s : pass.textures) {
                if (s.stage == stage && s.api == api) {
                    e = &s;
                    break;
                }
            }
            if ((flags & 1) != 0) { // a T# (8 dwords)
                std::array<u32, 8> tex{};
                bool found = false;
                if (e != nullptr && e->material >= 0 && static_cast<size_t>(e->material) < v.material.size() &&
                    v.material[e->material] != nullptr) {
                    tex = *v.material[e->material];
                    found = true;
                } else if (e != nullptr) {
                    if (const auto it = t.textures.find(e->name); it != t.textures.end()) {
                        tex = it->second;
                        found = true;
                    }
                }
                if (!found) {
                    c.unknown_textures.fetch_add(1, std::memory_order_relaxed);
                    if (!t.textures.empty()) {
                        tex = t.textures.begin()->second;
                    }
                }
                if (tex[0] == 0) {
                    tex[0] = 0x1000; // (a base of 0 would read as no descriptor)
                }
                ud.Put(reg, tex.data(), 8);
            } else { // a buffer bound as a resource (4 dwords)
                std::array<u32, 4> buf{};
                const auto it = e != nullptr ? t.buffers.find(e->name) : t.buffers.end();
                if (it != t.buffers.end()) {
                    buf = it->second;
                } else {
                    c.unknown_buffers.fetch_add(1, std::memory_order_relaxed);
                    if (!t.buffers.empty()) {
                        buf = t.buffers.begin()->second;
                    }
                }
                ud.Put(reg, buf.data(), 4);
            }
            break;
        }
        case kSampler: {
            const std::array<u32, 4> s = t.samplers.empty() ? std::array<u32, 4>{} : t.samplers.front();
            ud.Put(reg, s.data(), 4);
            break;
        }
        case kConstantBuffer:
            ud.Put(reg, t.constant_buffer.data(), 4);
            break;
        case kFetchShaderPtr:
            ud.PutAddress(reg, kFetchAddr);
            break;
        case kVertexTablePtr:
            ud.PutAddress(reg, kVertexTableAddr);
            break;
        case kExtendedUserDataPtr:
            ud.PutAddress(reg, kEudAddr);
            break;
        default:
            break; // (types the game's material passes do not use: left zero)
        }
    }
}

/// Translates one request, adds the module, gives the next stage's first bindings (false: failed).
bool Translate(Context& c, const Request& r, GS::BindingStart& end) {
    const bool ps = r.shader->ps;
    const GS::HwStage hw = ps ? GS::HwStage::Ps : GS::HwStage::Vs;
    SynthMemory mem;
    mem.Map(kProgramAddr, r.shader->image.data(), r.shader->image.size(), 256);
    mem.Map(kEudAddr, r.ud.eud.data(), r.ud.eud.size() * 4, kEudBytes);
    if (r.fetch != nullptr) {
        mem.Map(kFetchAddr, r.fetch->data(), r.fetch->size() * 4, 256);
    }
    if (r.streams != nullptr) {
        mem.Map(kVertexTableAddr, r.streams->data(), r.streams->size() * 16, 64 * 16);
    }
    const auto fail = [&](std::string why) {
        c.failed.fetch_add(1, std::memory_order_relaxed);
        std::scoped_lock lock{c.mutex};
        if (c.first_error.empty()) {
            c.first_error = fmt::format("{:x}: {}", r.shader->footer_hash, why);
        }
        return false;
    };
    GS::ProgramImage img;
    std::vector<u32> scan;
    if (const GS::ProgramError pe = GS::ReadProgram(mem, kProgramAddr, hw, img, scan); pe != GS::ProgramError::None) {
        return fail(std::string(GS::ProgramErrorName(pe)));
    }
    if (c.skip.contains(r.shader->footer_hash)) {
        c.skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const GS::ProgramIdentity id{img.hash, static_cast<u32>(img.code.size() * 4), hw};
    if (c.disk != nullptr) {
        // an earlier (interrupted) run translated it: its record gives what the translation would (the next stage's
        // bindings), nothing is added again
        for (const GS::ModuleStub& st : c.disk->Stubs(id)) {
            if (!(st.start == r.start) || std::memcmp(&st.runtime, &r.runtime, sizeof(r.runtime)) != 0) {
                continue;
            }
            GS::DiskModule dm;
            std::string derr;
            if (!c.disk->LoadModule(st.record, dm, derr) || !(dm.user_data == r.ud.regs)) {
                continue;
            }
            const GS::ShaderReflection& dr = dm.result.reflection;
            GS::ResourceBindingPlan dp;
            dp.flat = dm.result.flatbuf;
            GS::DecodeResources(dr, r.start, dr.fetch.present ? &dr.fetch : nullptr, r.ud.regs, 0,
                                GS::VertexSource{nullptr, dm.result.vertex_sharps}, dp);
            end = GS::EndOf(r.start, dp);
            c.resumed.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    GS::TranslationRequest req;
    req.hw = hw;
    req.sw = ps ? GS::SwStage::Fragment : GS::SwStage::Vertex;
    req.address = kProgramAddr;
    req.code = img.code;
    req.user_data = r.ud.regs;
    req.runtime = r.runtime;
    req.profile = &c.profile;
    req.start = r.start;
    req.memory = &mem;
    req.footer_hash = img.footer.shader_hash;
    if (c.trace != nullptr) {
        std::scoped_lock lock{c.mutex};
        std::string ud;
        for (const u32 w : r.ud.regs) {
            ud += fmt::format("{:08x} ", w);
        }
        fmt::print(c.trace, "start {:x} {} {},{} ud {}\n", r.shader->footer_hash, ps ? "ps" : "vs", r.start.unified,
                   r.start.buffer, ud);
        std::fflush(c.trace);
    }
    GS::TranslationResult result;
    try {
        result = c.translator.Translate(req);
    } catch (const std::exception& e) {
        return fail(std::string("the translator threw: ") + e.what());
    } catch (...) {
        return fail("the translator threw");
    }
    if (c.trace != nullptr) {
        std::scoped_lock lock{c.mutex};
        fmt::print(c.trace, "done {:x}\n", r.shader->footer_hash);
        std::fflush(c.trace);
    }
    if (!result.Ok()) {
        return fail(fmt::format("{}: {}", GS::TranslationErrorName(result.error), result.detail));
    }
    c.translated.fetch_add(1, std::memory_order_relaxed);
    // the key, as the frontend makes a layout's from what its translation saw (ShaderFrontend::Prepare)
    const GS::ShaderReflection& refl = result.reflection;
    const GS::FetchShader* fetch = refl.fetch.present ? &refl.fetch : nullptr;
    GS::ResourceBindingPlan plan;
    plan.flat = result.flatbuf;
    GS::DecodeResources(refl, r.start, fetch, r.ud.regs, 0, GS::VertexSource{nullptr, result.vertex_sharps}, plan);
    GS::SpecializationKey key;
    GS::BuildSpecializationKey(r.runtime, r.start, refl, fetch, plan, key);
    // ... and as a draw makes it (the SRT walk over the same memory): they must agree, or the synthetic memory is not
    // what a draw's would be
    if (!refl.srt_walker.empty()) {
        GS::SrtProgram srt;
        std::string serr;
        if (srt.Compile(refl.srt_walker, serr)) {
            GS::ResourceBindingPlan wp;
            GS::ResolveResources(refl, srt, r.start, fetch, r.ud.regs, kProgramAddr, mem, wp);
            GS::SpecializationKey wk;
            GS::BuildSpecializationKey(r.runtime, r.start, refl, fetch, wp, wk);
            if (!(wk == key)) {
                c.walk_mismatch.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    end = GS::EndOf(r.start, plan);

    GS::Program prog;
    prog.id = {img.hash, static_cast<u32>(img.code.size() * 4), hw};
    prog.footer_hash = img.footer.shader_hash;
    prog.code = img.code;
    GS::Module m;
    m.key = key;
    m.keyed = true;
    m.runtime = r.runtime;
    m.start = r.start;
    m.user_data = r.ud.regs;
    m.spirv_hash = GS::HashSpirv(result.spirv);
    m.result = std::move(result);
    const u64 lh = XXH3_64bits(&prog.id, sizeof(prog.id.hash) + sizeof(prog.id.length)) ^
                   XXH3_64bits(&r.runtime, sizeof(r.runtime)) * 0x9E3779B97F4A7C15ull ^ static_cast<u64>(hw);
    std::scoped_lock lock{c.mutex};
    const bool layout = c.layouts.insert(lh).second;
    c.layout_count.fetch_add(layout ? 1 : 0, std::memory_order_relaxed);
    if (c.disk != nullptr) {
        c.disk->AddModule(prog, m, layout);
        c.added.fetch_add(1, std::memory_order_relaxed);
    }
    if (c.report.is_open()) {
        std::string rt;
        const auto* rb = reinterpret_cast<const u8*>(&r.runtime);
        for (size_t i = 0; i < sizeof(r.runtime); ++i) {
            rt += fmt::format("{:02x}", rb[i]);
        }
        std::string kw;
        for (const u32 w : key.words) {
            kw += fmt::format("{:x},", w);
        }
        c.report << fmt::format("{:016x}{:016x}\t{}\t{}\t{:x}\t{}\t{},{}\t{}\t{}\n", prog.id.hash.lo, prog.id.hash.hi,
                                prog.id.length, static_cast<u32>(hw), prog.footer_hash, layout ? 1 : 0,
                                r.start.unified, r.start.buffer, rt, kw);
    }
    return true;
}

/// Runs `n` jobs on `threads` threads.
void Parallel(u32 threads, size_t n, const std::function<void(size_t)>& fn) {
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    for (u32 t = 0; t < threads; ++t) {
        pool.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < n;) {
                fn(i);
            }
        });
    }
    for (auto& t : pool) {
        t.join();
    }
}

} // namespace

bool StaticTemplates::Load(const std::filesystem::path& file, std::string& error) {
    std::ifstream in(file);
    if (!in) {
        error = fmt::format("cannot read {}", file.string());
        return false;
    }
    std::string line;
    u32 n = 0;
    while (std::getline(in, line)) {
        ++n;
        std::istringstream ls(line);
        std::string kind;
        ls >> kind;
        bool ok = true;
        u64 name = 0;
        if (kind == "tex") {
            std::array<u32, 8> t;
            ok = ParseHex(ls, name) && ParseDwords(ls, t);
            if (ok) {
                textures[name] = t;
            }
        } else if (kind == "rbuf" || kind == "buf") {
            std::array<u32, 4> b;
            ok = ParseHex(ls, name) && ParseDwords(ls, b);
            if (ok) {
                buffers[name] = b;
            }
        } else if (kind == "cb") {
            ok = ParseDwords(ls, constant_buffer);
        } else if (kind == "smp") {
            std::array<u32, 4> s;
            ok = ParseDwords(ls, s);
            if (ok) {
                samplers.push_back(s);
            }
        } else if (kind == "state") {
            std::string role, hex;
            u64 cls = 0, col = 0;
            u32 count = 0;
            ls >> role;
            ok = ParseHex(ls, cls) && ParseHex(ls, col) && static_cast<bool>(ls >> count >> hex) && hex.size() % 2 == 0;
            if (ok) {
                State s;
                s.cls = cls;
                s.ps = role == "ps";
                s.col_format = static_cast<u32>(col);
                s.count = count;
                for (size_t i = 0; i + 1 < hex.size(); i += 2) {
                    u32 b = 0;
                    std::from_chars(hex.data() + i, hex.data() + i + 2, b, 16);
                    s.runtime.push_back(static_cast<u8>(b));
                }
                ok = s.runtime.size() == sizeof(GS::RuntimeParams);
                states.push_back(std::move(s));
            }
        } else if (!kind.empty() && kind[0] != '#') {
            ok = false;
        }
        if (!ok) {
            error = fmt::format("{} line {}: not understood", file.string(), n);
            return false;
        }
    }
    return true;
}

bool RunPrediction(const std::filesystem::path& game_dir, const std::filesystem::path& templates_file,
                   GS::ShaderTranslator& translator, const GS::DeviceProfile& profile, GS::ShaderDiskCache* disk,
                   const PredictOptions& options, std::string& summary) {
    StaticTemplates templates;
    std::string error;
    if (!templates.Load(templates_file, error)) {
        summary = error;
        return false;
    }
    StaticDraws draws;
    StaticScanStats ss;
    if (!draws.Scan(game_dir, ss, error)) {
        summary = error;
        return false;
    }
    PredictStats ps;
    PredictModules(draws, templates, translator, profile, disk, options, ps);
    summary = fmt::format(
        "static shader prediction: {} packs ({:.1f} GiB) in {:.1f} s - programs {}, passes {}, instances {}, geometry "
        "{}, textures {}, shaders {}, fetch shaders {}; instance textures not found {}\n"
        "static shader prediction: {} requests ({} duplicates) in {:.1f} s - translated {}, failed {}, added {} ({} "
        "layouts); passes {} (no state {}, no instance {}); named textures unknown {}, buffers unknown {}; SRT-walk "
        "key mismatches {}; resumed {}, skipped {}{}",
        ss.packs, static_cast<double>(ss.bytes) / (1ull << 30), ss.ms / 1000.0, ss.programs, ss.passes, ss.instances,
        ss.geometries, ss.textures, ss.shaders, ss.fetches, ss.instance_textures_missing, ps.requests,
        ps.duplicate_requests, ps.ms / 1000.0, ps.translated, ps.failed, ps.added, ps.layouts, ps.passes,
        ps.passes_no_state, ps.passes_no_instance, ps.named_textures_unknown, ps.named_buffers_unknown,
        ps.walk_key_mismatches, ps.resumed, ps.skipped, ps.first_error.empty() ? "" : "; first failure: " + ps.first_error);
    return true;
}

bool PredictModules(const StaticDraws& draws, const StaticTemplates& templates, GS::ShaderTranslator& translator,
                    const GS::DeviceProfile& profile, GS::ShaderDiskCache* disk, const PredictOptions& options,
                    PredictStats& stats) {
    const auto t0 = std::chrono::steady_clock::now();
    stats = {};
    Context c(draws, templates, translator, profile, disk);
    if (!options.report.empty()) {
        c.report.open(options.report, std::ios::app); // (a resumed run adds to an interrupted one's)
        if (const char* t = std::getenv("SSPC_STATIC_TRACE"); t != nullptr && t[0] == '1') {
            c.trace = std::fopen((options.report.parent_path() / "static_predict_trace.txt").string().c_str(), "w");
        }
    }
    if (!options.report.empty()) {
        std::ifstream sk(options.report.parent_path() / "static_predict_skip.txt");
        std::string h;
        while (sk >> h) {
            u64 v = 0;
            if (std::from_chars(h.data(), h.data() + h.size(), v, 16).ec == std::errc{}) {
                c.skip.insert(v);
            }
        }
    }
    u32 env_threads = 0;
    if (const char* t = std::getenv("SSPC_STATIC_THREADS"); t != nullptr) {
        env_threads = static_cast<u32>(std::strtoul(t, nullptr, 10));
    }
    const u32 threads = env_threads != 0 ? env_threads :
        options.threads != 0 ? options.threads : std::max(1u, std::thread::hardware_concurrency() - 1);
    // the render state of a pass class (most frequent first)
    std::map<std::tuple<bool, u64, u32>, std::vector<const StaticTemplates::State*>> states;
    for (const StaticTemplates::State& s : templates.states) {
        auto& v = states[{s.ps, s.cls, s.col_format}];
        if (v.size() < options.max_state_variants) {
            v.push_back(&s);
        }
    }
    // per program: its instances' distinct material formats and its geometry's distinct stream formats
    const size_t nprog = draws.Programs().size();
    std::vector<std::vector<Variant>> materials(nprog), vertices(nprog);
    std::deque<std::vector<std::array<u32, 4>>> skinned; // the skinned variants' streams (stable addresses)
    {
        std::vector<std::unordered_set<u64>> seen_m(nprog), seen_v(nprog);
        for (const StaticInstance& in : draws.Instances()) {
            Variant v;
            u64 h = 0;
            for (const u32 t : in.textures) {
                const std::array<u32, 8>* tex = t != 0 ? &draws.Textures()[t - 1] : nullptr;
                v.material.push_back(tex);
                const u32 sig[4] = {tex != nullptr ? ((*tex)[1] >> 20) & 0x3FF : 0xFFFFFFFFu,
                                    tex != nullptr ? (*tex)[3] >> 28 : 0, tex != nullptr ? (*tex)[4] & 0x1FFF : 0,
                                    tex != nullptr ? (*tex)[5] : 0};
                h = XXH3_64bits_withSeed(sig, sizeof(sig), h);
            }
            if (materials[in.program].size() < options.max_material_variants && seen_m[in.program].insert(h).second) {
                materials[in.program].push_back(std::move(v));
            }
        }
        for (const StaticGeometry& g : draws.Geometries()) {
            const u32 prog = draws.Instances()[g.instance].program;
            u64 h = 0;
            for (const auto& s : g.streams) {
                const u32 sig[2] = {(s[1] >> 16) & 0x3FFF, s[3] & 0x7FFFF};
                h = XXH3_64bits_withSeed(sig, sizeof(sig), h);
            }
            if (vertices[prog].size() < options.max_vertex_variants && seen_v[prog].insert(h).second) {
                Variant v;
                v.streams = &g.streams;
                vertices[prog].push_back(std::move(v));
                // a skinned draw reads its positions from the skinning dispatch's output instead of the pack's stream
                // (seen in the caches: stream 0 as 32_32_32 float, stride 12)
                std::array<u32, 4> pos = g.streams[0];
                pos[1] = (pos[1] & ~(0x3FFFu << 16)) | (12u << 16);
                pos[3] = (pos[3] & ~((0xFu << 15) | (7u << 12))) | (13u << 15) | (7u << 12);
                if (pos != g.streams[0]) {
                    auto& sk = skinned.emplace_back(g.streams);
                    sk[0] = pos;
                    Variant sv;
                    sv.streams = &sk;
                    vertices[prog].push_back(std::move(sv));
                }
            }
        }
    }
    const std::vector<Variant> none(1);
    const auto& passes = draws.Passes();
    const auto& shaders = draws.Shaders();
    std::unordered_set<u64> requested;

    // the PS requests (fragment first: its bindings decide where the VS's start)
    std::vector<Request> ps_reqs;
    std::vector<std::pair<u32, u64>> ps_of; // (pass index, request hash)
    std::vector<u8> ps_keep;
    for (u32 pi = 0; pi < nprog; ++pi) {
        const StaticProgram& prog = draws.Programs()[pi];
        for (u32 k = 0; k < prog.passes; ++k) {
            const u32 pass_index = prog.first_pass + k;
            const StaticPass& pass = passes[pass_index];
            ++stats.passes;
            if (pass.ps == 0) {
                continue;
            }
            const StaticShader& ps = shaders[pass.ps - 1];
            const StaticShader* vs = pass.vs != 0 ? &shaders[pass.vs - 1] : nullptr;
            const auto st = states.find({true, pass.cls, ps.regs[1]});
            if (st == states.end()) {
                ++stats.passes_no_state;
                continue;
            }
            std::vector<u32> clips; // the pass class's VS clip states: the PS's UCP enable follows them
            if (const auto vst = states.find({false, pass.cls, 0}); vst != states.end()) {
                for (const auto* s : vst->second) {
                    GS::RuntimeParams rt;
                    std::memcpy(&rt, s->runtime.data(), sizeof(rt));
                    if (std::ranges::find(clips, rt.clip_cntl & 0x3F) == clips.end()) {
                        clips.push_back(rt.clip_cntl & 0x3F);
                    }
                }
            }
            const auto& mats = materials[pi].empty() ? none : materials[pi];
            stats.passes_no_instance += materials[pi].empty() ? 1 : 0;
            const std::vector<u16> no_exports;
            for (const auto* s : st->second) {
                GS::RuntimeParams rt;
                std::memcpy(&rt, s->runtime.data(), sizeof(rt));
                rt.hw = GS::HwStage::Ps;
                rt.sw = GS::SwStage::Fragment;
                rt.rsrc1 = ps.rsrc1 & GS::Rsrc1TranslationMask(GS::HwStage::Ps);
                rt.rsrc2 = ps.rsrc2 & GS::Rsrc2TranslationMask(GS::HwStage::Ps);
                rt.z_format = ps.regs[0] & 0xF;
                rt.col_format = ps.regs[1];
                rt.ps_input_ena = ps.regs[2] & 0xFFFF;
                rt.ps_input_addr = ps.regs[3] & 0xFFFF;
                rt.num_interp = ps.regs[4] & 0x3F;
                rt.front_face_all_bits = (ps.regs[5] >> 24) & 1;
                rt.db_shader_control = EngineDbShaderControl(ps.regs[6]) & GS::kDbShaderMask;
                if ((rt.ps_input_ena & rt.ps_input_addr & (1u << 14)) == 0) {
                    rt.aa_samples_log2 = 0;
                }
                rt.ps_vs_out_cntl = vs != nullptr ? vs->regs[2] & 0xFF : 0;
                rt.ps_input_cntl = {};
                for (u32 i = 0; i < rt.num_interp && i < ps.ps_inputs.size() && i < 32; ++i) {
                    rt.ps_input_cntl[i] = PsInputCntl(ps.ps_inputs[i], vs != nullptr ? vs->vs_exports : no_exports);
                }
                const std::vector<u32> ucps = clips.empty() ? std::vector<u32>{rt.ps_ucp_enable} : clips;
                for (const u32 ucp : ucps) {
                    rt.ps_ucp_enable = ucp;
                    for (const Variant& mv : mats) {
                        Request r;
                        r.shader = &ps;
                        r.runtime = rt;
                        FillUserData(c, ps, pass, kStagePs, mv, r.ud);
                        const u64 h = RequestHash(r);
                        ps_of.emplace_back(pass_index, h);
                        if (requested.insert(h).second) {
                            ps_reqs.push_back(std::move(r));
                        } else {
                            ++stats.duplicate_requests;
                        }
                    }
                }
            }
        }
    }
    std::vector<GS::BindingStart> ps_end(ps_reqs.size());
    std::vector<u8> ps_ok(ps_reqs.size(), 0);
    Parallel(threads, ps_reqs.size(), [&](size_t i) { ps_ok[i] = Translate(c, ps_reqs[i], ps_end[i]) ? 1 : 0; });
    // where a pass's VS starts: the ends of its PS translations
    std::unordered_map<u64, GS::BindingStart> end_by_hash;
    for (size_t i = 0; i < ps_reqs.size(); ++i) {
        if (ps_ok[i]) {
            end_by_hash.emplace(RequestHash(ps_reqs[i]), ps_end[i]);
        }
    }
    std::vector<std::vector<GS::BindingStart>> vs_starts(passes.size());
    for (const auto& [pass_index, h] : ps_of) {
        const auto it = end_by_hash.find(h);
        if (it == end_by_hash.end()) {
            continue;
        }
        auto& v = vs_starts[pass_index];
        if (std::ranges::find(v, it->second) == v.end()) {
            v.push_back(it->second);
        }
    }
    // the VS requests
    std::vector<Request> vs_reqs;
    for (u32 pi = 0; pi < nprog; ++pi) {
        const StaticProgram& prog = draws.Programs()[pi];
        for (u32 k = 0; k < prog.passes; ++k) {
            const u32 pass_index = prog.first_pass + k;
            const StaticPass& pass = passes[pass_index];
            if (pass.vs == 0 || vs_starts[pass_index].empty()) {
                continue;
            }
            const StaticShader& vs = shaders[pass.vs - 1];
            const auto st = states.find({false, pass.cls, 0});
            if (st == states.end()) {
                ++stats.passes_no_state;
                continue;
            }
            const auto& mats = materials[pi].empty() ? none : materials[pi];
            const auto& verts = vertices[pi].empty() ? none : vertices[pi];
            const std::vector<u32>* fetch = pass.fetch != 0 ? &draws.Fetches()[pass.fetch - 1] : nullptr;
            for (const auto* s : st->second) {
                GS::RuntimeParams rt;
                std::memcpy(&rt, s->runtime.data(), sizeof(rt));
                rt.hw = GS::HwStage::Vs;
                rt.sw = GS::SwStage::Vertex;
                rt.rsrc1 = vs.rsrc1 & GS::Rsrc1TranslationMask(GS::HwStage::Vs);
                rt.rsrc2 = vs.rsrc2 & GS::Rsrc2TranslationMask(GS::HwStage::Vs);
                rt.vs_out_cntl = vs.regs[2] & 0x3FFFFFFu;
                for (const GS::BindingStart& start : vs_starts[pass_index]) {
                    for (const Variant& vv : verts) {
                        // a VS reads material textures rarely; the first material variant stands for them
                        Variant v = mats.front();
                        v.streams = vv.streams;
                        Request r;
                        r.shader = &vs;
                        r.runtime = rt;
                        r.start = start;
                        r.fetch = fetch;
                        r.streams = vv.streams;
                        FillUserData(c, vs, pass, kStageVs, v, r.ud);
                        if (requested.insert(RequestHash(r)).second) {
                            vs_reqs.push_back(std::move(r));
                        } else {
                            ++stats.duplicate_requests;
                        }
                    }
                }
            }
        }
    }
    Parallel(threads, vs_reqs.size(), [&](size_t i) {
        GS::BindingStart end;
        Translate(c, vs_reqs[i], end);
    });

    stats.requests = ps_reqs.size() + vs_reqs.size();
    stats.translated = c.translated;
    stats.failed = c.failed;
    stats.added = c.added;
    stats.layouts = c.layout_count;
    stats.walk_key_mismatches = c.walk_mismatch;
    stats.named_textures_unknown = c.unknown_textures;
    stats.named_buffers_unknown = c.unknown_buffers;
    stats.first_error = c.first_error;
    stats.resumed = c.resumed;
    stats.skipped = c.skipped;
    if (c.trace != nullptr) {
        std::fclose(c.trace);
    }
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

} // namespace SecondSon::Gpu::Static
