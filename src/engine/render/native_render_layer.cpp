// SecondSonPC R7 — the Native Render Layer (native_render_layer.h). R7.0: the contract and its counters; Submit carries
// nothing yet (every command falls back to the game's path) - R7-Core grows the direct path here.
#include <windows.h>

#include "engine/render/native_render_layer.h"

namespace SecondSon::Engine::Render::R7 {
namespace {

bool ReadEnabled() {
    char v[16] = {};
    const DWORD n = GetEnvironmentVariableA("SSPC_R7_NATIVE_RENDER", v, sizeof(v));
    return n > 0 && n < sizeof(v) && !(n == 1 && v[0] == '0');
}

constexpr u64 kFnvBasis = 0xcbf29ce484222325ull;
constexpr u64 kFnvPrime = 0x100000001b3ull;

void Mix(u64& h, u64 v) {
    for (u32 i = 0; i < 8; ++i) {
        h = (h ^ ((v >> (i * 8)) & 0xFF)) * kFnvPrime;
    }
}

} // namespace

bool Enabled() {
    static const bool on = ReadEnabled();
    return on;
}

LayerCounters& Counters() {
    static LayerCounters c;
    return c;
}

SubmitResult Submit(const NativeRenderContext& ctx, const NativeDrawCommand& cmd) {
    (void)ctx;
    const u32 p = static_cast<u32>(cmd.producer) < 5 ? static_cast<u32>(cmd.producer) : 0;
    LayerCounters& c = Counters();
    c.submitted[p].fetch_add(1, std::memory_order_relaxed);
    if (!Enabled()) {
        return SubmitResult::Off;
    }
    c.fallback[p].fetch_add(1, std::memory_order_relaxed);
    return SubmitResult::Fallback;
}

u64 StateKey(const NativeDrawCommand& cmd) {
    const NativeDrawState& s = cmd.state;
    if (!s.shaders) {
        return 0;
    }
    u64 h = kFnvBasis;
    for (const u32 v : s.vs) {
        Mix(h, v);
    }
    Mix(h, s.has_ps ? 1 : 0);
    for (const u32 v : s.ps) {
        Mix(h, v);
    }
    Mix(h, s.prim_type | (u64{s.index_size} << 32));
    Mix(h, s.num_instances);
    Mix(h, cmd.resources.cue);
    for (const NativeStageResources& st : cmd.resources.stage) {
        if (!st.present) {
            Mix(h, 0);
            continue;
        }
        if (st.version == 0) {
            return 0; // resources not tracked: never reused
        }
        Mix(h, st.version);
        Mix(h, st.shape.dwords | (u64{st.shape.refs} << 16));
        for (u32 i = 0; i < st.shape.refs && i < 7; ++i) {
            Mix(h, st.shape.sgpr[i] | (u64{st.shape.offset[i]} << 8));
        }
        Mix(h, st.direct_mask);
    }
    return h == 0 ? 1 : h;
}

} // namespace SecondSon::Engine::Render::R7
