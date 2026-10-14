// SecondSonPC — Native Render Interface, engine side (see native_draw.h).
#include "engine/render/native_draw.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_probe.h"
#include "engine/render/render_layout.h"

namespace SecondSon::Engine::Render::NativeDraw {

namespace {

namespace L = Layout;

// eboot.bin of CUSA00004 (tools/native-reconstruction/function_hashes.py); prologues checked with queries/prologue.py.
constexpr ProbeSite kDrawIndexSite{{0x623c60, 0x7a, 0x90df1869fca20135ull}, 14};
constexpr ProbeSite kDrawAutoSite{{0x623a40, 0x6f, 0x1e6017d7a3f4de6cull}, 16};
constexpr GuestFunction kAssumed[] = {
    {0x14ad0, 0x1184, 0x8bb6e7985e565584ull}, // CUE commit: shaders from +0xC0A8/+0xC0B0, snapshot = shadow, user data
    {0x1f2c60, 0x1f, 0xef1991cc4587b6dfull},  // AcquireContext: the pool's layout (pass keys)
};

// CUE fields the commit 0x14ad0 reads (GFX_CONTEXT_LAYOUT.md §2, RENDER_ENGINE_ARCHITECTURE.md §4).
constexpr u64 kCuePsShader = 0xC0A8;  ///< PS shader object; its register block at +8
constexpr u64 kCueVsShader = 0xC0B0;  ///< VS shader object
constexpr u64 kCueVsModifier = 0xC168;
constexpr u64 kCuePsAltPath = 0xC1D9; ///< nonzero: the commit sets the PS another way (0x6251c0)
constexpr u64 kCueVsAltPath = 0xC1DA; ///< nonzero: ... the VS (0x625300)
constexpr u32 kCueStagePs = 1;        ///< CUE stage index = Gnm::ShaderStage
constexpr u32 kCueStageVs = 2;

using DrawIndexFn = SSPC_SYSV_ABI void (*)(u8* dcb, u32 count, u64 indices, u32 flags);
using DrawAutoFn = SSPC_SYSV_ABI void (*)(u8* dcb, u32 count, u32 flags);
using ReserveFn = SSPC_SYSV_ABI bool (*)(u8* cb, u32 dwords, u64 user);

u64 g_base = 0;
u64 g_canary = 0; ///< SSPC_P53_NATIVE_DRAW_CANARY=<n>: every n-th marker is spoiled on purpose (0: none)
DrawIndexFn g_draw_index = nullptr;
DrawAutoFn g_draw_auto = nullptr;

struct EngineCounters {
    std::atomic<u64> draws{0};
    std::atomic<u64> marked{0};
    std::atomic<u64> foreign{0}; ///< the command buffer is not a render context SecondSonPC knows
    std::atomic<u64> no_space{0};
};
EngineCounters g_engine;

template <typename T>
T At(u64 address) {
    T value;
    std::memcpy(&value, reinterpret_cast<const void*>(address), sizeof(T));
    return value;
}

/// The pass key of a context, or false if `r` is none of the frame's contexts.
bool ContextPass(u64 base, u64 r, s32& pass, u32& sub) {
    const u64 frame = base + L::kFrame;
    const u64 pool = frame + L::kFramePool;
    if (r >= pool && r < pool + u64{L::kPoolSlots} * L::kPoolSlotStride && (r - pool) % L::kPoolSlotStride == 0) {
        pass = At<s32>(r + L::kSlotPass);
        sub = At<u32>(r + L::kSlotSub);
        return true;
    }
    sub = 0;
    if (r == frame + L::kFramePreContext) {
        pass = -2;
        return true;
    }
    if (r == frame + L::kFramePostContext) {
        pass = -3;
        return true;
    }
    const u64 deform = base + L::kDeformContexts;
    if (r >= deform && r < deform + u64{L::kDeformContextCount} * L::kContextSize &&
        (r - deform) % L::kContextSize == 0) {
        pass = -4;
        return true;
    }
    return false;
}

void DescribeTable(u64 cue, u32 stage, bool vertex, StageTable& t) {
    const u64 layout = At<u64>(cue + L::kCueLayout + u64{stage} * 8);
    if (layout == 0) {
        return;
    }
    t.present = true;
    t.dwords = std::min<u32>(At<u16>(layout), 0x600);
    t.hash = HashBytes(reinterpret_cast<const void*>(cue + L::kCueShadow + u64{stage} * L::kCueShadowStride),
                       u64{t.dwords} * 4);
    // Commit 0x14ad0: SGPR byte -> pointer to snapshot + u16 offset (dwords). 0xFF: not used by the shader.
    struct Ref {
        u32 sgpr_byte;
        u32 offset_field; ///< 0: the snapshot's start
    };
    static constexpr Ref kRefs[] = {{7, 0}, {8, 0x16}, {9, 0x1A}, {0xA, 0x1C}, {0xB, 0x1E}};
    static constexpr Ref kVertexRefs[] = {{5, 0x18}, {6, 0x20}};
    const auto add = [&](const Ref& r) {
        const u8 sgpr = At<u8>(layout + r.sgpr_byte);
        if (sgpr != 0xFF && sgpr <= 14 && t.refs < 7) {
            t.ref[t.refs++] = TableRef{sgpr, r.offset_field == 0 ? u16{0} : At<u16>(layout + r.offset_field)};
        }
    };
    for (const Ref& r : kRefs) {
        add(r);
    }
    if (vertex) {
        for (const Ref& r : kVertexRefs) {
            add(r);
        }
    }
}

void Mark(u8* dcb, bool indexed, u32 count, u64 indices, u32 packet_dwords) {
    g_engine.draws.fetch_add(1, std::memory_order_relaxed);
    DrawOp op;
    if (!BuildDrawOp(g_base, reinterpret_cast<u64>(dcb) - L::kContextGfx, indexed, count, indices, op)) {
        g_engine.foreign.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_canary != 0 && g_engine.draws.load(std::memory_order_relaxed) % g_canary == 0) {
        op.canary = true; // the processor must see the count differ
        op.index_count += 1;
    }
    const u64 cb = reinterpret_cast<u64>(dcb);
    const u32 need = kDrawMarkerDwords + packet_dwords;
    if ((At<u64>(cb + L::kCbEnd) - At<u64>(cb + L::kCbCmdPtr)) / 4 < need) {
        // As the game's own wrappers do: ask the buffer's callback for room (0x11eed0 for a render context).
        const auto reserve = At<ReserveFn>(cb + 0x18);
        if (reserve == nullptr || !reserve(dcb, need, At<u64>(cb + 0x20))) {
            g_engine.no_space.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    auto* at = reinterpret_cast<u32*>(At<u64>(cb + L::kCbCmdPtr));
    const u32 written = EncodeDrawMarker(op, kVerify, at);
    const u64 next = reinterpret_cast<u64>(at + written);
    std::memcpy(reinterpret_cast<void*>(cb + L::kCbCmdPtr), &next, sizeof(next));
    g_engine.marked.fetch_add(1, std::memory_order_relaxed);
}

SSPC_SYSV_ABI void DrawIndexProbe(u8* dcb, u32 count, u64 indices, u32 flags) {
    Mark(dcb, true, count, indices, 10);
    g_draw_index(dcb, count, indices, flags);
}

SSPC_SYSV_ABI void DrawAutoProbe(u8* dcb, u32 count, u32 flags) {
    Mark(dcb, false, count, 0, 7);
    g_draw_auto(dcb, count, flags);
}

void Reporter(u32 seconds) {
    static constexpr const char* kFields[kMismatchFields] = {"kind",    "index count", "index base", "VS regs",
                                                             "PS regs", "VS table",    "PS table"};
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        DrawCounters& c = Counters();
        std::string fields;
        for (u32 i = 0; i < kMismatchFields; ++i) {
            if (const u64 n = c.field[i].load(); n != 0) {
                fields += fmt::format(" {} {}", kFields[i], n);
            }
        }
        std::string passes;
        for (u32 p = 0; p < kPassSlots; ++p) {
            if (const u64 n = c.pass_mismatched[p].load(); n != 0) {
                passes += fmt::format(" p{}:{}/{}", p, n, c.pass_verified[p].load());
            }
        }
        SSPC_LOG(Engine, Info,
                 "native draw (shadow): engine draws {}, marked {}, foreign {}, no space {} | processor markers {}, "
                 "malformed {}, orphaned {}, verified {}, matched {}, mismatched {}{}{} shaders unknown {}, tables "
                 "checked {}, canaries caught {}/{}",
                 g_engine.draws.load(), g_engine.marked.load(), g_engine.foreign.load(), g_engine.no_space.load(),
                 c.markers.load(), c.malformed.load(), c.orphaned.load(), c.verified.load(), c.matched.load(),
                 c.mismatched.load(), fields.empty() ? "" : " (fields:" + fields + ")",
                 passes.empty() ? "" : " (mismatched/verified by pass slot:" + passes + ")", c.shaders_unknown.load(),
                 c.tables_checked.load(), c.canaries_caught.load(), c.canaries.load());
    }
}

std::string Env(const char* name) {
    char v[256] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

} // namespace

bool BuildDrawOp(u64 base, u64 r, bool indexed, u32 index_count, u64 index_base, DrawOp& op) {
    if (!ContextPass(base, r, op.pass, op.sub)) {
        return false;
    }
    const u64 cue = r + L::kContextCue;
    if (At<u64>(cue + L::kCueDcb) != r + L::kContextGfx) {
        return false;
    }
    op.indexed = indexed;
    op.index_count = index_count;
    op.index_base = indexed ? index_base : 0;

    const u64 vs = At<u64>(cue + kCueVsShader);
    const u64 ps = At<u64>(cue + kCuePsShader);
    op.shaders = vs != 0 && At<u8>(cue + kCueVsAltPath) == 0 && At<u8>(cue + kCuePsAltPath) == 0;
    if (op.shaders) {
        // sceGnmSetVsShader(cb, vs + 8, modifier) / sceGnmSetPsShader350(cb, ps + 8): the block's values as written.
        const auto* v = reinterpret_cast<const u32*>(vs + 8);
        const u32 modifier = At<u32>(cue + kCueVsModifier);
        const u32 rsrc1 = modifier == 0 ? v[2] : (v[2] & 0xFCFFFC3Fu) | modifier;
        const u32 vs_values[kVsRegs] = {v[0], 0u, rsrc1, v[3], v[4], v[5], v[6]};
        std::memcpy(op.vs, vs_values, sizeof(op.vs));
        op.has_ps = ps != 0;
        if (op.has_ps) {
            const auto* p = reinterpret_cast<const u32*>(ps + 8);
            const u32 ps_values[kPsRegs] = {p[0], 0u, p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]};
            std::memcpy(op.ps, ps_values, sizeof(op.ps));
        }
    }
    DescribeTable(cue, kCueStageVs, true, op.table[kStageVs]);
    DescribeTable(cue, kCueStagePs, false, op.table[kStagePs]);
    return true;
}

bool InstallFromEnvironment(u64 eboot_base) {
    const std::string mode = Env("SSPC_P53_NATIVE_DRAW");
    if (mode.empty() || mode == "0" || mode == "off") {
        return true;
    }
    if (mode != "shadow") {
        SSPC_LOG(Engine, Error, "SSPC_P53_NATIVE_DRAW={}: want shadow; nothing installed", mode);
        return false;
    }
    std::string error;
    for (const GuestFunction& f : kAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "native draw not installed: {}", error);
            return false;
        }
    }
    for (const ProbeSite* s : {&kDrawIndexSite, &kDrawAutoSite}) {
        if (!VerifyGuestFunction(eboot_base, s->function, error)) {
            SSPC_LOG(Engine, Error, "native draw not installed: {}", error);
            return false;
        }
    }
    g_base = eboot_base;
    void* index = InstallEntryProbe(eboot_base, kDrawIndexSite, reinterpret_cast<const void*>(&DrawIndexProbe), error);
    void* autod = index ? InstallEntryProbe(eboot_base, kDrawAutoSite, reinterpret_cast<const void*>(&DrawAutoProbe),
                                            error)
                        : nullptr;
    if (index == nullptr || autod == nullptr) {
        SSPC_LOG(Engine, Critical, "native draw: {}", error);
        std::abort(); // verified bytes, but the host refused the write: stop rather than run half-probed
    }
    g_draw_index = reinterpret_cast<DrawIndexFn>(index);
    g_draw_auto = reinterpret_cast<DrawAutoFn>(autod);
    if (const std::string s = Env("SSPC_P53_NATIVE_DRAW_CANARY"); !s.empty()) {
        g_canary = std::strtoull(s.c_str(), nullptr, 10);
    }
    u32 stats = 10;
    if (const std::string s = Env("SSPC_P53_NATIVE_DRAW_STATS"); !s.empty()) {
        stats = static_cast<u32>(std::strtoul(s.c_str(), nullptr, 10));
    }
    SSPC_LOG(Engine, Warning,
             "EXPERIMENT: SSPC_P53_NATIVE_DRAW=shadow - the engine's draws (0x623c60, 0x623a40) are described from its "
             "objects and verified by the command processor against the PM4 path (kVerify markers in the DCB)");
    if (stats != 0) {
        std::thread(Reporter, stats).detach();
    }
    return true;
}

} // namespace SecondSon::Engine::Render::NativeDraw
