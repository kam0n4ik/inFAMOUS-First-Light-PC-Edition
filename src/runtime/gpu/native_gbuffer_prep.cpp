// SecondSonPC - the Native Engine G-buffer draw path. See native_gbuffer_prep.h.
#include "runtime/gpu/native_gbuffer_prep.h"

#include <cstdlib>

#include <windows.h>

#include "base/log.h"

namespace SecondSon::Gpu {

namespace {

std::string EnvA(const char* name) {
    char v[32] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

/// A walk binding holds this V# (its raw four dwords) - a buffer, a vertex stream's read V#, anywhere in the plan.
bool PlanHasBuffer(const GS::ResourceBindingPlan& p, const std::array<u32, 4>& v) {
    for (const GS::BufferBinding& b : p.buffers) {
        if (b.desc.raw == v) {
            return true;
        }
    }
    for (const GS::VertexBinding& vb : p.vertices) {
        if (vb.read == v || vb.desc.raw == v) {
            return true;
        }
    }
    return false;
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
             "EXPERIMENT: SSPC_NATIVE_GBUFFER={} - the command processor takes 0x1a1770's per-draw descriptors from the "
             "engine's structures (native_gbuffer_prep.h){}",
             v, mode_ == Mode::Verify ? " - verify: built and compared with the SRT walk, the walk is used" : "");
}

void NativeGBuffer::Verify(const GDR::DrawDescription& d, const GS::ResourceBindingPlan& vs,
                           const GS::ResourceBindingPlan& ps) {
    bool all = true;
    for (u32 i = 0; i < d.count && i < GDR::kMaxDescResources; ++i) {
        const GDR::DescResource& e = d.res[i];
        if (e.offset == 0xffff) {
            continue; // the shader does not use this slot: nothing to compare
        }
        const u32 kind = e.kind < 3 ? e.kind : 0;
        seen_[kind].fetch_add(1, std::memory_order_relaxed);
        // the engine descriptor should be one the walk resolved on either stage it was bound to
        const bool hit = PlanHasBuffer(vs, e.desc) || PlanHasBuffer(ps, e.desc);
        (hit ? matched_ : unmatched_)[kind].fetch_add(1, std::memory_order_relaxed);
        if (!hit) {
            all = false;
            const u64 n = mismatch_logged_.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 12) {
                SSPC_LOG(GPU, Warning,
                         "SSPC_NATIVE_GBUFFER verify: engine descriptor (kind {}, stage {}, slot {}, offset {:#x}) "
                         "V# {:08x} {:08x} {:08x} {:08x} not held by the walk",
                         e.kind, e.stage, e.slot, e.offset, e.desc[0], e.desc[1], e.desc[2], e.desc[3]);
            }
        }
    }
    (all ? verified_draws_ : mismatch_draws_).fetch_add(1, std::memory_order_relaxed);
}

std::string NativeGBuffer::Report() const {
    if (mode_ == Mode::Off) {
        return {};
    }
    const auto ld = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    const char* name = mode_ == Mode::Verify ? "verify" : mode_ == Mode::Guarded ? "guarded" : "native";
    std::string out = fmt::format(
        "native G-buffer 0x1a1770 (SSPC_NATIVE_GBUFFER={}): tagged draws {}, with an engine record {}, without {}",
        name, ld(draws_), ld(with_record_), ld(no_record_));
    if (mode_ == Mode::Verify) {
        const char* kn[3] = {"buffer/instance", "vertex", "constants"};
        out += "; verify by kind (seen/matched/unmatched):";
        for (u32 k = 0; k < 3; ++k) {
            out += fmt::format(" {} {}/{}/{}", kn[k], ld(seen_[k]), ld(matched_[k]), ld(unmatched_[k]));
        }
        out += fmt::format("; draws all-matched {}, with a mismatch {}", ld(verified_draws_), ld(mismatch_draws_));
    }
    out += "\n";
    return out;
}

} // namespace SecondSon::Gpu
