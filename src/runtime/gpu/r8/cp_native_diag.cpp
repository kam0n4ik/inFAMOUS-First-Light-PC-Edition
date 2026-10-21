// SecondSonPC R8-A2 — the upper bound of reusing stage preparations by the native engine's knowledge (cp_native_diag.h).
#include <windows.h>

#include "runtime/gpu/r8/cp_native_diag.h"

#include <intrin.h>

#include <algorithm>
#include <cstring>
#include <string_view>

#include <fmt/format.h>

#include "base/log.h"
#include "runtime/gpu/native_videoout.h"
#include "runtime/gpu/r8/cp_native_table.h"
#include "runtime/gpu/shader/shader_resources.h"
#include "runtime/kernel/clock.h"

namespace SecondSon::Gpu::Native::R8 {

namespace {

inline void Bump(std::atomic<u64>& a, u64 v = 1) {
    a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
inline u64 Ld(const std::atomic<u64>& a) {
    return a.load(std::memory_order_relaxed);
}

constexpr u32 kCueStage[2] = {2, 1}; ///< (VS, PS) -> the CUE's stage (Gnm: 2 VS, 1 PS)
constexpr GS::HwStage kHw[2] = {GS::HwStage::Vs, GS::HwStage::Ps};
constexpr const char* kCat[4] = {"untagged", "mesh 0x1a1960", "shadow 0x43d480", "G-buffer 0x1a1770"};
constexpr const char* kKeyName[PrepDiag::kKeys] = {"V", "V0", "C", "C0"};

bool SameBuffer(const GS::BufferBinding& a, const GS::BufferBinding& b) {
    return a.binding == b.binding && a.kind == b.kind && a.desc.raw == b.desc.raw && a.address == b.address &&
           a.size == b.size && a.written == b.written && a.formatted == b.formatted && a.status == b.status;
}
bool SameImage(const GS::ImageBinding& a, const GS::ImageBinding& b) {
    return a.binding == b.binding && a.count == b.count && a.desc.raw == b.desc.raw && a.view_type == b.view_type &&
           a.storage == b.storage && a.depth == b.depth && a.array == b.array && a.atomic == b.atomic &&
           a.fmask == b.fmask && a.status == b.status;
}
bool SameSampler(const GS::SamplerBinding& a, const GS::SamplerBinding& b) {
    return a.binding == b.binding && a.desc.raw == b.desc.raw && a.status == b.status;
}
bool SameVertex(const GS::VertexBinding& a, const GS::VertexBinding& b) {
    return a.attribute == b.attribute && a.fetch == b.fetch && a.read == b.read && a.desc.raw == b.desc.raw &&
           a.address == b.address && a.size == b.size && a.status == b.status;
}

/// A binding of the plan lies in [lo, hi) (the snapshot): its address moves with the snapshot - not reusable.
bool Touches(const GS::ResourceBindingPlan& p, u64 lo, u64 hi) {
    const auto in = [&](u64 a, u64 n) { return a < hi && a + std::max<u64>(n, 1) > lo; };
    return std::ranges::any_of(p.buffers, [&](const GS::BufferBinding& b) { return in(b.address, b.size); }) ||
           std::ranges::any_of(p.vertices, [&](const GS::VertexBinding& v) { return in(v.address, v.size); });
}

/// The walk's reads, watched: any outside [lo, hi).
class Watch final : public GS::GuestReader {
public:
    Watch(GS::GuestReader& inner, u64 lo, u64 hi) : inner_(inner), lo_(lo), hi_(hi) {}
    bool Read(u64 addr, void* dst, u64 bytes) override {
        outside_ = outside_ || addr < lo_ || addr + bytes > hi_;
        return inner_.Read(addr, dst, bytes);
    }
    bool Outside() const { return outside_; }

private:
    GS::GuestReader& inner_;
    u64 lo_, hi_;
    bool outside_ = false;
};

} // namespace

bool PrepDiag::On() {
    static const bool on = [] {
        char v[8] = {};
        const bool o = GetEnvironmentVariableA("SSPC_R8_CP_DIAG", v, sizeof(v)) == 1 && v[0] == '1';
        if (o) {
            SSPC_LOG(GPU, Warning, "DIAGNOSTIC: SSPC_R8_CP_DIAG=1 - the stage preparations a reuse by the native "
                                   "engine's snapshot versions would take (gpu_native.txt, 'r8 cp diag' lines)");
        }
        return o;
    }();
    return on;
}

PrepDiag::PrepDiag() {
    log_period_ = Kernel::Clock::TscFrequency() * 10;
    t_start_ = __rdtsc();
    last_log_ = t_start_;
    bytes_.reserve(0x600);
}

void PrepDiag::Before(const GS::ShaderFrontend& f) {
    gen_before_[0] = f.StageGeneration(GS::HwStage::Vs);
    gen_before_[1] = f.StageGeneration(GS::HwStage::Ps);
    memory_gen_ = f.MemoryGeneration();
}

bool PrepDiag::SamePlan(const GS::StageView& v, const Entry& e, u32 pointer_mask) {
    const GS::ResourceBindingPlan& x = v.plan;
    const GS::ResourceBindingPlan& y = e.plan;
    if (v.module != e.module || !(v.end == e.end) || x.flat.size() != y.flat.size() || x.flat.size() < 16 ||
        x.status_counts != y.status_counts || !x.error.empty() || !y.error.empty()) {
        return false;
    }
    for (u32 i = 0; i < 16; ++i) {
        if (((pointer_mask >> i) & 1) == 0 && x.flat[i] != y.flat[i]) {
            return false;
        }
    }
    return std::equal(x.flat.begin() + 16, x.flat.end(), y.flat.begin() + 16) &&
           std::ranges::equal(x.buffers, y.buffers, SameBuffer) && std::ranges::equal(x.images, y.images, SameImage) &&
           std::ranges::equal(x.samplers, y.samplers, SameSampler) &&
           std::ranges::equal(x.vertices, y.vertices, SameVertex);
}

u64 PrepDiag::ResourceCost(const GS::StageView& v, std::span<const u32, 16> ud, GS::GuestReader& memory, u64 lo,
                           u64 hi, bool& inside) {
    inside = false;
    if (v.program == nullptr || v.module == nullptr) {
        return 0;
    }
    const GS::Layout* layout = v.program->FindLayout(v.runtime);
    if (layout == nullptr || layout->module == nullptr) {
        return 0;
    }
    const GS::Module& first = *layout->module; // (PrepareStage resolves with the layout's first module)
    if (first.state.load(std::memory_order_acquire) != GS::ModuleState::Ready || !first.prepared ||
        !first.srt_error.empty()) {
        return 0;
    }
    const GS::ShaderReflection& r = first.result.reflection;
    const GS::FetchShader* fetch = r.fetch.present ? &v.module->result.reflection.fetch : nullptr;
    Watch watch(memory, lo, hi);
    const u64 t0 = __rdtsc();
    const bool ok = GS::ResolveResources(r, first.srt, v.start, fetch, ud, v.address, watch, scratch_);
    GS::BuildSpecializationKey(v.runtime, v.start, r, fetch, scratch_, key_);
    const u64 t = __rdtsc() - t0;
    inside = ok && lo < hi && !watch.Outside() && !scratch_.srt.user_data_written && scratch_.srt.faults == 0;
    return t;
}

void PrepDiag::After(const Cp::DrawDescriptor& d, const GS::ShaderFrontend& f, GS::GuestReader& memory, u64 ticks) {
    const u64 gen_after = f.MemoryGeneration(); // (before this diagnostic's own reads)
    const u32 cat = d.engine_run & 3;
    DrawC& dc = dr_[cat];
    Bump(dc.calls);
    Bump(dc.ticks, ticks);
    std::array<std::array<bool, kKeys>, 2> covered{};
    ++stamp_;
    for (u32 s = 0; s < 2; ++s) {
        const GS::HwStage hw = kHw[s];
        StageC& c = st_[cat][s];
        if (f.StageGeneration(hw) == gen_before_[s]) {
            Bump(c.memo); // (the frontend's P4.7 memo took it: nothing to save there)
            covered[s].fill(true);
            continue;
        }
        Bump(c.full);
        const GS::StageView& v = f.Stage(hw);
        if (f.SharedStage(hw) != nullptr || v.status != GS::StageStatus::Ready || v.module == nullptr ||
            v.plan.flat.size() < 16) {
            Bump(c.unready);
            continue;
        }
        const size_t cpst = static_cast<size_t>(hw); // (Cp::Stage has the graphics stages in HwStage's order)
        const std::span<const u32, 16> ud(d.user_data[cpst]);
        ::SecondSon::Gpu::R8::SnapshotInfo sn;
        u32 pmask = 0;
        const bool snap = ::SecondSon::Gpu::R8::Find(ud, kCueStage[s], sn, pmask);
        bool bytes_ok = false;
        bytes_.clear();
        if (snap) {
            Bump(c.snap);
            Bump(c.versioned, sn.version != 0 ? 1 : 0);
            if (sn.dwords != 0 && sn.dwords <= 0x600) {
                bytes_.resize(sn.dwords);
                bytes_ok = memory.Read(sn.base, bytes_.data(), u64{sn.dwords} * 4);
                if (!bytes_ok) {
                    bytes_.clear();
                }
            }
        }
        const bool touches = snap && Touches(v.plan, sn.base, sn.base + u64{sn.dwords} * 4);
        Bump(c.touches, touches ? 1 : 0);
        std::array<u32, 16> key_ud;
        for (u32 i = 0; i < 16; ++i) {
            key_ud[i] = ((pmask >> i) & 1) != 0 ? 0u : ud[i];
        }
        const Cp::ShaderRef& sr = d.shaders[cpst];
        const u64 pk = (sr.address * 0x9E3779B97F4A7C15ull) ^ (u64{sr.rsrc1} << 32 | sr.rsrc2);
        Ways& ways = cache_[s][pk];
        // the stored preparations of this program that every key shares the base of
        bool prev = false;
        std::array<bool, kKeys> hit{}, mru{}, bad{};
        bool stale = false;
        Entry* same_base = nullptr;
        u64 newest = 0;
        for (Entry& e : ways.e) {
            newest = std::max(newest, e.valid ? e.stamp : 0);
        }
        for (Entry& e : ways.e) {
            if (!e.valid || e.address != sr.address || e.rsrc1 != sr.rsrc1 || e.rsrc2 != sr.rsrc2) {
                continue;
            }
            prev = true;
            if (std::memcmp(&e.runtime, &v.runtime, sizeof(v.runtime)) != 0 || !(e.start == v.start) ||
                e.pointer_mask != pmask || e.ud != key_ud) {
                continue;
            }
            same_base = &e;
            if (!snap) {
                continue;
            }
            const bool same_bytes = bytes_ok && e.bytes.size() == bytes_.size() &&
                                    std::memcmp(e.bytes.data(), bytes_.data(), bytes_.size() * 4) == 0;
            const bool same_ver = sn.version != 0 && e.version == sn.version;
            const bool same_mem = e.memory_gen == memory_gen_;
            const std::array<bool, kKeys> k = {same_ver && same_mem, same_ver, same_bytes && same_mem, same_bytes};
            stale = stale || (same_ver && bytes_ok && !same_bytes);
            const bool same_plan = (k[kV] || k[kV0] || k[kC] || k[kC0]) && SamePlan(v, e, pmask);
            for (u32 q = 0; q < kKeys; ++q) {
                if (k[q] && !hit[q]) {
                    hit[q] = true;
                    mru[q] = e.stamp == newest;
                    bad[q] = !same_plan;
                }
            }
        }
        Bump(c.prev, prev ? 1 : 0);
        Bump(c.stale, stale ? 1 : 0);
        for (u32 q = 0; q < kKeys; ++q) {
            Bump(c.hit[q], hit[q] ? 1 : 0);
            Bump(c.mru[q], mru[q] ? 1 : 0);
            Bump(c.bad[q], hit[q] && bad[q] ? 1 : 0);
            covered[s][q] = hit[q] && !bad[q] && !touches;
        }
        if ((++sample_ & 7) == 0) {
            bool inside = false;
            const u64 lo = snap ? sn.base : 0, hi = snap ? sn.base + u64{sn.dwords} * 4 : 0;
            const u64 t = ResourceCost(v, ud, memory, lo, hi, inside);
            if (t != 0) {
                Bump(c.cost_n);
                Bump(c.cost_t, t);
                if (hit[kV0]) {
                    Bump(c.cost_hit_n);
                    Bump(c.cost_hit_t, t);
                }
                if (inside) {
                    Bump(c.inside_n);
                    if (hit[kC0] && !bad[kC0] && !touches) {
                        Bump(c.inside_c0_n);
                        Bump(c.inside_c0_t, t);
                    }
                }
            }
        }
        // this preparation stored: over the one with the same base, else over the oldest
        Entry* slot = same_base;
        if (slot == nullptr) {
            slot = &ways.e[0];
            for (Entry& e : ways.e) {
                if (!e.valid) {
                    slot = &e;
                    break;
                }
                if (e.stamp < slot->stamp) {
                    slot = &e;
                }
            }
        }
        slot->valid = true;
        slot->address = sr.address;
        slot->rsrc1 = sr.rsrc1;
        slot->rsrc2 = sr.rsrc2;
        slot->runtime = v.runtime;
        slot->start = v.start;
        slot->end = v.end;
        slot->ud = key_ud;
        slot->pointer_mask = pmask;
        slot->version = snap ? sn.version : 0;
        slot->memory_gen = gen_after;
        slot->bytes.assign(bytes_.begin(), bytes_.end());
        slot->module = v.module;
        slot->plan.flat.assign(v.plan.flat.begin(), v.plan.flat.end());
        slot->plan.buffers.assign(v.plan.buffers.begin(), v.plan.buffers.end());
        slot->plan.images.assign(v.plan.images.begin(), v.plan.images.end());
        slot->plan.samplers.assign(v.plan.samplers.begin(), v.plan.samplers.end());
        slot->plan.vertices.assign(v.plan.vertices.begin(), v.plan.vertices.end());
        slot->plan.status_counts = v.plan.status_counts;
        slot->plan.error = v.plan.error;
        slot->stamp = stamp_;
    }
    for (u32 q = 0; q < kKeys; ++q) {
        const bool vs = covered[0][q], ps = covered[1][q];
        if (vs && ps) {
            Bump(dc.both[q]);
            Bump(dc.both_t[q], ticks);
        } else if (ps) {
            Bump(dc.ps_only[q]);
            Bump(dc.ps_only_t[q], ticks);
        } else if (vs) {
            Bump(dc.vs_only[q]);
            Bump(dc.vs_only_t[q], ticks);
        }
    }
    if ((++calls_ & 1023) == 0) {
        const u64 now = __rdtsc();
        if (now - last_log_ >= log_period_) {
            last_log_ = now;
            SSPC_LOG(GPU, Info, "r8 cp diag: flip {} {}", VideoOut::NextFlipId(), Line());
        }
    }
}

std::string PrepDiag::Line() const {
    std::string out = fmt::format("tsc_hz {}", Kernel::Clock::TscFrequency());
    const auto t = ::SecondSon::Gpu::R8::Stats();
    out += fmt::format(" | table {} {} {} {}", t.published, t.lookups, t.found, t.torn);
    for (u32 cat = 0; cat < 4; ++cat) {
        const DrawC& dc = dr_[cat];
        out += fmt::format(" | d{} {} {}", cat, Ld(dc.calls), Ld(dc.ticks));
        for (u32 q = 0; q < kKeys; ++q) {
            out += fmt::format(" {} {} {} {} {} {}", Ld(dc.both[q]), Ld(dc.both_t[q]), Ld(dc.ps_only[q]),
                               Ld(dc.ps_only_t[q]), Ld(dc.vs_only[q]), Ld(dc.vs_only_t[q]));
        }
        for (u32 s = 0; s < 2; ++s) {
            const StageC& c = st_[cat][s];
            out += fmt::format(" | s{}{} {} {} {} {} {} {} {} {} {} {} {} {}", cat, s == 0 ? "vs" : "ps", Ld(c.full),
                               Ld(c.memo), Ld(c.unready), Ld(c.snap), Ld(c.versioned), Ld(c.prev), Ld(c.touches),
                               Ld(c.stale), Ld(c.cost_n), Ld(c.cost_t), Ld(c.cost_hit_n), Ld(c.cost_hit_t));
            for (u32 q = 0; q < kKeys; ++q) {
                out += fmt::format(" {} {} {}", Ld(c.hit[q]), Ld(c.mru[q]), Ld(c.bad[q]));
            }
            out += fmt::format(" {} {} {}", Ld(c.inside_n), Ld(c.inside_c0_n), Ld(c.inside_c0_t));
        }
    }
    return out;
}

std::string PrepDiag::Report() const {
    const double tpu = static_cast<double>(std::max<u64>(Kernel::Clock::TscFrequency(), 1)) / 1e6;
    const auto pct = [](u64 a, u64 b) { return b != 0 ? 100.0 * static_cast<double>(a) / static_cast<double>(b) : 0.0; };
    const auto t = ::SecondSon::Gpu::R8::Stats();
    std::string out = fmt::format(
        "  R8 CP diag (SSPC_R8_CP_DIAG=1; keys: V = program + RSRC + runtime + start + user data past the snapshot "
        "pointers + R7 binding version + memory generation, V0 = without the generation, C / C0 = the snapshot's bytes "
        "instead of the version): snapshot table published {}, lookups {}, found {}, torn {}\n",
        t.published, t.lookups, t.found, t.torn);
    for (u32 cat = 0; cat < 4; ++cat) {
        const DrawC& dc = dr_[cat];
        const u64 calls = Ld(dc.calls);
        if (calls == 0) {
            continue;
        }
        out += fmt::format("    {}: frontend calls {} ({:.0f} ms, {:.2f} us each); covered (both stages memo or key) / "
                           "PS only / VS only:", kCat[cat], calls, Ld(dc.ticks) / tpu / 1e3,
                           Ld(dc.ticks) / tpu / static_cast<double>(calls));
        for (u32 q = 0; q < kKeys; ++q) {
            out += fmt::format(" {} {:.1f}% ({:.0f} ms) / {:.1f}% ({:.0f} ms) / {:.1f}%;", kKeyName[q],
                               pct(Ld(dc.both[q]), calls), Ld(dc.both_t[q]) / tpu / 1e3, pct(Ld(dc.ps_only[q]), calls),
                               Ld(dc.ps_only_t[q]) / tpu / 1e3, pct(Ld(dc.vs_only[q]), calls));
        }
        out += "\n";
        for (u32 s = 0; s < 2; ++s) {
            const StageC& c = st_[cat][s];
            const u64 full = Ld(c.full);
            out += fmt::format("      {}: memo {}, prepared {} (not ready {}, snapshot found {}, versioned {}, program "
                               "seen {}, a binding inside the snapshot {}, same version other bytes {}); resource part "
                               "{:.2f} us ({} sampled; of key V0 hits {:.2f} us); hits / most recent / bad:",
                               s == 0 ? "VS" : "PS", Ld(c.memo), full, Ld(c.unready), Ld(c.snap), Ld(c.versioned),
                               Ld(c.prev), Ld(c.touches), Ld(c.stale),
                               Ld(c.cost_t) / tpu / static_cast<double>(std::max<u64>(Ld(c.cost_n), 1)), Ld(c.cost_n),
                               Ld(c.cost_hit_t) / tpu / static_cast<double>(std::max<u64>(Ld(c.cost_hit_n), 1)));
            for (u32 q = 0; q < kKeys; ++q) {
                out += fmt::format(" {} {} ({:.1f}%) / {} / {};", kKeyName[q], Ld(c.hit[q]), pct(Ld(c.hit[q]), full),
                                   Ld(c.mru[q]), Ld(c.bad[q]));
            }
            out += fmt::format(" sampled walks inside the snapshot {} of {}, of them C0 matches {}\n", Ld(c.inside_n),
                               Ld(c.cost_n), Ld(c.inside_c0_n));
        }
    }
    out += "    line: " + Line() + "\n";
    return out;
}

} // namespace SecondSon::Gpu::Native::R8
