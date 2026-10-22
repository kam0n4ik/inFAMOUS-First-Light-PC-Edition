// SecondSonPC - the Native Engine G-buffer draw path. See native_gbuffer_prep.h.
#include "runtime/gpu/native_gbuffer_prep.h"

#include <algorithm>
#include <cstring>

#include <windows.h>

#include "base/log.h"

namespace SecondSon::Gpu {

namespace {

std::string EnvA(const char* name) {
    char v[32] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

constexpr size_t kVs = static_cast<size_t>(Cp::Stage::Vs), kPs = static_cast<size_t>(Cp::Stage::Ps);
constexpr u8 kVsPs = (1u << static_cast<u32>(Cp::Stage::Vs)) | (1u << static_cast<u32>(Cp::Stage::Ps));

// The stage map's walk: register k (the pair k, k+1) of the CUE copy points at kFakeBase + k MiB, and a load from
// there answers each dword with its tag - 0xA5 | k | the dword - instead of memory. (2^39: inside the 40-bit space the
// walk accepts, far from anything a register of the copy can hold.)
constexpr u64 kFakeBase = 0x80'0000'0000ull;
constexpr u32 kTag = 0xA5u << 24;
constexpr u32 kNotPointer = 0xA6u << 24; ///< a register that is no pointer into the copy

class TagReader final : public GS::GuestReader {
public:
    bool Read(u64 addr, void* dst, u64 bytes) override {
        const u64 rel = addr - kFakeBase;
        if (addr < kFakeBase || rel >= (16ull << 20) || (rel & 3) != 0 || (bytes & 3) != 0 ||
            ((rel & 0xFFFFF) >> 2) + bytes / 4 > 0xFFFF) {
            foreign = true; // memory that is not a register's copy (another table, a pointer read from the copy)
            std::memset(dst, 0, bytes);
            return true;
        }
        const u32 reg = static_cast<u32>(rel >> 20);
        const u32 dw = static_cast<u32>((rel & 0xFFFFF) >> 2);
        for (u64 i = 0; i < bytes / 4; ++i) {
            const u32 tag = kTag | (reg << 16) | static_cast<u32>(dw + i);
            std::memcpy(static_cast<u8*>(dst) + i * 4, &tag, 4);
        }
        return true;
    }
    bool foreign = false;
};

/// The layout's first module of a prepared view - the one whose walk and reflection PrepareStage resolves with
/// (RedecodeStage does the same). nullptr: none, or not ready.
const GS::Module* FirstModule(const GS::StageView& v) {
    if (v.program == nullptr || v.module == nullptr) {
        return nullptr;
    }
    const GS::Layout* layout = v.program->FindLayout(v.runtime);
    if (layout == nullptr || layout->module == nullptr) {
        return nullptr;
    }
    const GS::Module& first = *layout->module;
    if (first.state.load(std::memory_order_acquire) != GS::ModuleState::Ready || !first.prepared) {
        return nullptr;
    }
    return &first;
}

/// What the executor reads of a view besides its plan (CopyStage's fields).
void CopyHead(GS::StageView& d, const GS::StageView& s) {
    d.status = s.status;
    d.address = s.address;
    d.program = s.program;
    d.footer_hash = s.footer_hash;
    d.module = s.module;
    d.runtime = s.runtime;
    d.start = s.start;
    d.end = s.end;
    d.program_error = s.program_error;
    d.native_shared = false;
}

/// A stage writes memory (a written buffer, GDS / LDS / BDA, a storage image): the frontend notes its writes at each
/// preparation - such a stage is left to the full path (GEOFAST's rule).
bool Writes(const GS::StageView& v) {
    for (const GS::BufferBinding& b : v.plan.buffers) {
        if (b.written || (b.kind != GS::BufferKind::Guest && b.kind != GS::BufferKind::Flatbuf &&
                          b.kind != GS::BufferKind::ClipPlanes)) {
            return true;
        }
    }
    return std::ranges::any_of(v.plan.images, [](const GS::ImageBinding& im) { return im.storage; });
}

u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h * 0xff51afd7ed558ccdull;
}

} // namespace

NativeGBuffer::NativeGBuffer() {
    const std::string v = EnvA("SSPC_NATIVE_GBUFFER");
    if (v.empty() || v == "0" || v == "off") {
        return;
    }
    if (v == "verify") {
        mode_ = Mode::Verify;
    } else if (v == "guarded") {
        mode_ = Mode::Guarded;
    } else if (v == "native" || v == "1") {
        mode_ = Mode::Native;
    } else {
        SSPC_LOG(GPU, Warning, "SSPC_NATIVE_GBUFFER={} not understood (verify|guarded|native|off) - off", v);
        return;
    }
    SSPC_LOG(GPU, Warning,
             "EXPERIMENT: SSPC_NATIVE_GBUFFER={} - 0x1a1770's draws prepared from the engine's state (its descriptors "
             "and CUE copy, no SRT walk; native_gbuffer_prep.h){}",
             v,
             mode_ == Mode::Verify    ? " - verify: each also prepared in full, the full one used, the two compared"
             : mode_ == Mode::Guarded ? " - guarded: every 64th checked against the full preparation"
                                      : "");
}

NativeGBuffer::Key NativeGBuffer::KeyOf(const Cp::DrawDescriptor& d, const Engine::Render::DrawRuns::EngineDraw& e,
                                        u64 targets) {
    return {.vs = d.shaders[kVs].address,
            .ps = d.shaders[kPs].address,
            .pipeline_key = d.pipeline_key,
            .targets = targets,
            .vs_layout = e.vs.layout,
            .ps_layout = e.ps.layout};
}

u64 NativeGBuffer::HashOf(const Key& k) {
    u64 h = Mix(0, k.vs);
    h = Mix(h, k.ps);
    h = Mix(h, k.pipeline_key);
    h = Mix(h, k.targets);
    h = Mix(h, k.vs_layout);
    return Mix(h, k.ps_layout);
}

u64 NativeGBuffer::FetchOf(const GS::StageView& vs, std::span<const u32, 16> ud, u32& reg) {
    reg = 0xff;
    const GS::Module* first = FirstModule(vs);
    if (first == nullptr || !first->result.reflection.fetch.present) {
        return 0;
    }
    const u32 b = first->result.reflection.fetch.sgpr_base;
    if (b + 1 >= 16) {
        return ~0ull;
    }
    reg = b;
    return (ud[b] | (u64{ud[b + 1]} << 32)) & 0xFFFFFFFFFFFFull; // (PrepareStage's fetch shader address)
}

const NativeGBuffer::StageMap* NativeGBuffer::MapOf(const GS::StageView& v, const GDR::StageShadow& sh) {
    const GS::Module* first = FirstModule(v);
    if (first == nullptr) {
        return nullptr;
    }
    const u64 h = Mix(Mix(0, reinterpret_cast<u64>(first)), sh.layout);
    auto& slot = maps_[h];
    if (slot != nullptr && slot->first == first && slot->layout == sh.layout) {
        return slot.get();
    }
    if (slot != nullptr) {
        return nullptr; // (a hash collision: this pair is left to the full path)
    }
    slot = std::make_unique<StageMap>();
    slot->first = first;
    slot->layout = sh.layout;
    MakeMap(*slot, *first, sh);
    Bump(map_why_[slot->ok ? 0 : std::min<u32>(slot->why, 7)]);
    if (!slot->ok && logged_.load(std::memory_order_relaxed) < 24) {
        Bump(logged_);
        SSPC_LOG(GPU, Info, "SSPC_NATIVE_GBUFFER: stage {} program {:#x} (CUE layout {:#x}) not from the engine's CUE "
                            "copy: reason {} (native_gbuffer_prep.h StageMap::why)",
                 static_cast<u32>(first->result.reflection.hw), v.address, sh.layout, slot->why);
    }
    return slot.get();
}

void NativeGBuffer::MakeMap(StageMap& m, const GS::Module& first, const GDR::StageShadow& sh) {
    const GS::ShaderReflection& r = first.result.reflection;
    const auto no = [&](u8 why) {
        m.ok = false;
        m.why = why;
    };
    if (!first.srt_error.empty() || (first.srt.Empty() && r.flatbuf_dwords > 16)) {
        return no(1);
    }
    // the registers: those of the copy point at their fake tables, the rest are no pointers
    std::array<u32, 16> ud{};
    for (u32 k = 0; k < 16; ++k) {
        ud[k] = kNotPointer | k;
    }
    for (u32 i = 0; i < GDR::kShadowPointers; ++i) {
        const u32 reg = sh.reg[i];
        if (reg != 0xff && reg + 1 < 16) {
            const u64 base = kFakeBase + (u64{reg} << 20);
            ud[reg] = static_cast<u32>(base);
            ud[reg + 1] = static_cast<u32>(base >> 32);
        }
    }
    std::vector<u32> flat(std::max<u32>(r.flatbuf_dwords, 16));
    std::copy(ud.begin(), ud.end(), flat.begin());
    TagReader tags;
    GS::SrtProgram::RunStats st;
    std::string error;
    if (!first.srt.Empty() && !first.srt.Run(ud, flat, tags, st, error)) {
        return no(2);
    }
    if (tags.foreign) {
        return no(3);
    }
    if (st.user_data_written) {
        return no(4);
    }
    const auto at = [&](u32 reg, u32 dw, u16& out) {
        for (u32 i = 0; i < GDR::kShadowPointers; ++i) {
            if (sh.reg[i] == reg) {
                const u32 idx = u32{sh.off[i]} + dw;
                if (idx >= sh.dwords) {
                    return false;
                }
                out = static_cast<u16>(idx);
                return true;
            }
        }
        return false;
    };
    m.ud_read = st.user_data_read;
    m.flat_size = static_cast<u32>(flat.size());
    m.flat.resize(flat.size() - 16);
    for (size_t pos = 16; pos < flat.size(); ++pos) {
        const u32 v = flat[pos];
        u16& idx = m.flat[pos - 16];
        if (v == 0) {
            idx = kZero;
        } else if ((v >> 20) == (kTag >> 20)) {
            if (!at((v >> 16) & 0xF, v & 0xFFFF, idx)) {
                return no(6);
            }
        } else {
            return no(5); // (a value the walk did not load from a register's copy)
        }
    }
    if (r.fetch.present) {
        for (const GS::FetchAttribute& a : r.fetch.attributes) {
            u16 idx = 0;
            if (a.sgpr_base + 1 >= 16 || !at(a.sgpr_base, a.dword_offset, idx) || u32{idx} + 4 > sh.dwords) {
                return no(7);
            }
            m.vertex.push_back(idx);
        }
    }
    m.ok = true;
}

bool NativeGBuffer::BuildStage(const GS::StageView& head, const StageMap& m, const GDR::StageShadow& sh,
                               std::span<const u32, 16> ud, GS::StageView& out, GS::SpecializationKey& key) {
    if (head.module == nullptr || head.module->state.load(std::memory_order_acquire) != GS::ModuleState::Ready ||
        m.first == nullptr) {
        return false;
    }
    const GS::ShaderReflection& r = m.first->result.reflection;
    const GS::ShaderReflection& mr = head.module->result.reflection;
    const GS::FetchShader* fetch = r.fetch.present ? &mr.fetch : nullptr; // (the fetch shader at the same address)
    if (fetch != nullptr && (!mr.fetch.present || m.vertex.size() != mr.fetch.attributes.size())) {
        return false;
    }
    CopyHead(out, head);
    // the flattened user data: the draw's registers, then the CUE copy's dwords the walk would have loaded
    std::vector<u32>& flat = out.plan.flat;
    flat.resize(m.flat_size);
    std::copy(ud.begin(), ud.end(), flat.begin());
    for (size_t i = 0; i < m.flat.size(); ++i) {
        flat[16 + i] = m.flat[i] == kZero ? 0u : sh.shadow[m.flat[i]];
    }
    vreads_.resize(m.vertex.size());
    for (size_t i = 0; i < m.vertex.size(); ++i) {
        std::memcpy(vreads_[i].data(), &sh.shadow[m.vertex[i]], 16);
    }
    GS::DecodeResources(r, head.start, fetch, ud, head.address, GS::VertexSource{nullptr, vreads_}, out.plan);
    out.plan.srt = {};
    out.plan.srt.user_data_read = m.ud_read; // (the registers the walk reads: the next draw's checks)
    out.plan.error.clear();
    GS::BuildSpecializationKey(head.runtime, head.start, r, fetch, out.plan, key); // (the module it selects)
    return true;
}

bool NativeGBuffer::Prepare(const Cp::DrawDescriptor& d, const Engine::Render::DrawRuns::EngineDraw& e, u64 epoch,
                            u64 targets, GS::DrawPreparation& prep, const GS::GraphicsPipelineState*& state,
                            GS::StageView& vs, GS::StageView& ps) {
    const auto refuse = [&](u32 why) {
        Bump(refused_[why]);
        return false;
    };
    if (d.indirect || d.active_stages != kVsPs) {
        return refuse(1);
    }
    if (e.vs.dwords == 0 || e.ps.dwords == 0) {
        return refuse(2);
    }
    const Key k = KeyOf(d, e, targets);
    const auto it = templates_.find(HashOf(k));
    if (it == templates_.end() || !(it->second.key == k) || it->second.n == 0) {
        return refuse(3);
    }
    Template& t = it->second;
    if (t.epoch != epoch) {
        templates_.erase(it); // the programs may have changed since
        return refuse(4);
    }
    const std::span<const u32, 16> vud(d.user_data[kVs]), pud(d.user_data[kPs]);
    // the plans from the engine's CUE copy, on any variant's heads (the same programs, runtime and bindings)
    const Variant& base = t.v[0];
    u32 freg = 0xff;
    const u64 fetch = FetchOf(base.vs, vud, freg);
    const StageMap* vm = MapOf(base.vs, e.vs);
    const StageMap* pm = MapOf(base.ps, e.ps);
    if (vm == nullptr || pm == nullptr || !vm->ok || !pm->ok) {
        return refuse(6);
    }
    if (!BuildStage(base.ps, *pm, e.ps, pud, ps, key_ps_) || !BuildStage(base.vs, *vm, e.vs, vud, vs, key_vs_)) {
        return refuse(7);
    }
    // the variant with the modules these plans select (and this draw's fetch shader)
    const Variant* hit = nullptr;
    bool fetch_seen = false;
    for (u32 i = 0; i < t.n && hit == nullptr; ++i) {
        const Variant& v = t.v[i];
        if (v.fetch != fetch || v.fetch_reg != freg) {
            continue;
        }
        fetch_seen = true;
        if (v.ps.module->key == key_ps_ && v.vs.module->key == key_vs_) {
            hit = &v;
        }
    }
    if (hit == nullptr) {
        return refuse(fetch_seen ? 8 : 5);
    }
    if (!(GS::EndOf(hit->ps.start, ps.plan) == hit->ps.end) || !(GS::EndOf(hit->vs.start, vs.plan) == hit->vs.end) ||
        !(hit->vs.start == hit->ps.end)) {
        return refuse(9);
    }
    CopyHead(ps, hit->ps);
    CopyHead(vs, hit->vs);
    prep = hit->prep;
    state = hit->state;
    Bump(taken_);
    return true;
}

bool NativeGBuffer::Compare(const GS::StageView& x, const GS::StageView& y, u32 stage) {
    const auto bufs = [](const GS::BufferBinding& a, const GS::BufferBinding& b) {
        return a.binding == b.binding && a.kind == b.kind && a.desc.raw == b.desc.raw && a.address == b.address &&
               a.size == b.size && a.written == b.written && a.formatted == b.formatted && a.status == b.status;
    };
    const auto imgs = [](const GS::ImageBinding& a, const GS::ImageBinding& b) {
        return a.binding == b.binding && a.count == b.count && a.desc.raw == b.desc.raw && a.view_type == b.view_type &&
               a.storage == b.storage && a.depth == b.depth && a.array == b.array && a.atomic == b.atomic &&
               a.fmask == b.fmask && a.status == b.status;
    };
    const auto smps = [](const GS::SamplerBinding& a, const GS::SamplerBinding& b) {
        return a.binding == b.binding && a.desc.raw == b.desc.raw && a.status == b.status;
    };
    const auto vtxs = [](const GS::VertexBinding& a, const GS::VertexBinding& b) {
        return a.attribute == b.attribute && a.fetch == b.fetch && a.read == b.read && a.desc.raw == b.desc.raw &&
               a.address == b.address && a.size == b.size && a.status == b.status;
    };
    const std::array<bool, 6> same = {
        x.plan.flat == y.plan.flat,
        std::ranges::equal(x.plan.buffers, y.plan.buffers, bufs),
        std::ranges::equal(x.plan.images, y.plan.images, imgs),
        std::ranges::equal(x.plan.samplers, y.plan.samplers, smps),
        std::ranges::equal(x.plan.vertices, y.plan.vertices, vtxs),
        x.status == y.status && x.address == y.address && x.program == y.program && x.footer_hash == y.footer_hash &&
            x.module == y.module && std::memcmp(&x.runtime, &y.runtime, sizeof(x.runtime)) == 0 &&
            x.start == y.start && x.end == y.end && x.program_error == y.program_error &&
            x.plan.stage == y.plan.stage && x.plan.status_counts == y.plan.status_counts &&
            x.plan.error == y.plan.error};
    Bump(compared_);
    bool all = true;
    for (u32 i = 0; i < same.size(); ++i) {
        if (!same[i]) {
            Bump(diff_[i]);
            all = false;
        }
    }
    if (all) {
        Bump(same_);
        return true;
    }
    if (logged_.load(std::memory_order_relaxed) < 24) {
        Bump(logged_);
        std::string what;
        static constexpr const char* kPart[] = {"flat", "buffers", "images", "samplers", "vertices", "head"};
        for (u32 i = 0; i < same.size(); ++i) {
            what += same[i] ? "" : fmt::format(" {}", kPart[i]);
        }
        if (!same[0]) {
            for (size_t i = 0; i < std::min(x.plan.flat.size(), y.plan.flat.size()); ++i) {
                if (x.plan.flat[i] != y.plan.flat[i]) {
                    what += fmt::format(" (flat[{}] walk {:08x} engine {:08x})", i, x.plan.flat[i], y.plan.flat[i]);
                    break;
                }
            }
            if (x.plan.flat.size() != y.plan.flat.size()) {
                what += fmt::format(" (flat size walk {} engine {})", x.plan.flat.size(), y.plan.flat.size());
            }
        }
        SSPC_LOG(GPU, Warning, "SSPC_NATIVE_GBUFFER check: stage {} program {:#x}: the engine build differs from the "
                               "walk:{}", stage, x.address, what);
    }
    return false;
}

void NativeGBuffer::AfterFull(const Cp::DrawDescriptor& d, const Engine::Render::DrawRuns::EngineDraw& e, u64 epoch,
                              u64 targets, const GS::DrawPreparation& prep, const GS::GraphicsPipelineState* state,
                              const GS::StageView& vs, const GS::StageView& ps, bool check) {
    if (!prep.ready || state == nullptr || d.indirect || d.active_stages != kVsPs ||
        vs.status != GS::StageStatus::Ready || ps.status != GS::StageStatus::Ready || e.vs.dwords == 0 ||
        e.ps.dwords == 0 || Writes(vs) || Writes(ps)) {
        return;
    }
    if (check) {
        // the engine path as it would prepare this draw (its template), else the plans alone on this draw's heads
        GS::DrawPreparation eprep;
        const GS::GraphicsPipelineState* estate = nullptr;
        bool built = Prepare(d, e, epoch, targets, eprep, estate, check_vs_, check_ps_);
        if (built) {
            if (eprep.pipeline != prep.pipeline || estate != state) {
                Bump(diff_[5]);
            }
        } else {
            const StageMap* vm = MapOf(vs, e.vs);
            const StageMap* pm = MapOf(ps, e.ps);
            built = vm != nullptr && pm != nullptr && vm->ok && pm->ok &&
                    BuildStage(ps, *pm, e.ps, std::span<const u32, 16>(d.user_data[kPs]), check_ps_, key_ps_) &&
                    BuildStage(vs, *vm, e.vs, std::span<const u32, 16>(d.user_data[kVs]), check_vs_, key_vs_) &&
                    key_ps_ == ps.module->key && key_vs_ == vs.module->key;
        }
        if (!built) {
            Bump(built_failed_);
        } else {
            const bool a = Compare(ps, check_ps_, 1);
            const bool b = Compare(vs, check_vs_, 2);
            if ((!a || !b) && mode_ == Mode::Guarded && !tripped_) {
                tripped_ = true;
                SSPC_LOG(GPU, Error, "SSPC_NATIVE_GBUFFER=guarded: an engine build differed from the full "
                                     "preparation - the engine path is off for the rest of the run");
            }
        }
    }
    // the draw's key keeps this preparation as a variant (its modules, fetch shader and pipeline)
    const Key k = KeyOf(d, e, targets);
    if (templates_.size() >= kMaxTemplates && !templates_.contains(HashOf(k))) {
        templates_.clear();
        Bump(templates_dropped_);
    }
    auto [it, made] = templates_.try_emplace(HashOf(k));
    Template& t = it->second;
    if (made || !(t.key == k) || t.epoch != epoch) {
        Bump(templates_made_);
        t.key = k;
        t.epoch = epoch;
        t.n = t.next = 0;
    }
    u32 freg = 0xff;
    const u64 fetch = FetchOf(vs, std::span<const u32, 16>(d.user_data[kVs]), freg);
    u32 slot = t.n;
    for (u32 i = 0; i < t.n; ++i) {
        if (t.v[i].vs.module == vs.module && t.v[i].ps.module == ps.module && t.v[i].fetch == fetch &&
            t.v[i].fetch_reg == freg) {
            slot = i; // (the same variant: its newest preparation)
            break;
        }
    }
    if (slot == t.n) {
        Bump(variants_made_);
        if (t.n < kVariants) {
            ++t.n;
        } else {
            slot = t.next;
            t.next = (t.next + 1) % kVariants;
        }
    }
    Variant& v = t.v[slot];
    v.prep = prep;
    v.state = state;
    CopyHead(v.vs, vs);
    CopyHead(v.ps, ps);
    v.fetch = fetch;
    v.fetch_reg = freg;
}

std::string NativeGBuffer::Report() const {
    if (mode_ == Mode::Off) {
        return {};
    }
    const auto ld = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    const char* name = mode_ == Mode::Verify ? "verify" : mode_ == Mode::Guarded ? "guarded" : "native";
    std::string out = fmt::format(
        "native G-buffer 0x1a1770 (SSPC_NATIVE_GBUFFER={}{}): tagged draws {}, with an engine record {}, without {}; "
        "prepared from the engine {}{}; refused (1 indirect/stages, 2 no CUE state, 3 no template, 4 programs changed, "
        "5 another fetch shader, 6 a stage not of the CUE copy, 7 not ready, 8 no variant of these modules, 9 other "
        "bindings):",
        name, tripped_ ? ", tripped: off" : "", ld(draws_), ld(with_record_), ld(no_record_), ld(taken_),
        mode_ == Mode::Verify ? " (verify: would be; each checked)" : "");
    for (u32 i = 1; i <= 9; ++i) {
        out += fmt::format(" {}:{}", i, ld(refused_[i]));
    }
    out += fmt::format("; templates {} (variants {}, all dropped {} times); stage maps (0 ok, 1 no walk, 2 failed, 3 other memory, 4 wrote registers, "
                       "5 unknown dword, 6 past the copy, 7 vertex V#):",
                       ld(templates_made_), ld(variants_made_), ld(templates_dropped_));
    for (u32 i = 0; i < 8; ++i) {
        out += fmt::format(" {}:{}", i, ld(map_why_[i]));
    }
    if (mode_ != Mode::Native) {
        out += fmt::format("; checked stages {} (same {}), differing by part flat {} buffers {} images {} samplers {} "
                           "vertices {} head/pipeline {}; draws not built {}",
                           ld(compared_), ld(same_), ld(diff_[0]), ld(diff_[1]), ld(diff_[2]), ld(diff_[3]),
                           ld(diff_[4]), ld(diff_[5]), ld(built_failed_));
    }
    out += "\n";
    return out;
}

} // namespace SecondSon::Gpu
