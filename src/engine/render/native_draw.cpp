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
constexpr ProbeSite kCommitSite{{0x14ad0, 0x1184, 0x8bb6e7985e565584ull}, 20};
constexpr GuestFunction kAssumed[] = {
    {0x14ad0, 0x1184, 0x8bb6e7985e565584ull}, // CUE commit: shaders from +0xC0A8/+0xC0B0, snapshot = shadow, user data
    {0x1f2c60, 0x1f, 0xef1991cc4587b6dfull},  // AcquireContext: the pool's layout (pass keys)
    // The native commit (apply mode) does what these do, or calls them:
    {0x123d0, 0x94, 0x7bf8f59b6455f910ull},   // CUE linear allocator (snapshots), called
    {0x638540, 0x229, 0xe7ccd229525c9081ull}, // PS input mapping (SPI_PS_INPUT_CNTL values), called
    {0x626400, 0x89, 0xc135b5f904cb94d7ull},  // setPointerInUserData: SET_SH_REG USER_DATA+sgpr {lo, hi}
    {0x626520, 0x143, 0x1e04ce784703751bull}, // setUserDataRegion: NOP 0x6875000D + SET_SH_REG USER_DATA+sgpr
    {0x624cb0, 0x15f, 0x79dea00a63f200beull}, // SET_CONTEXT_REG SPI_PS_INPUT_CNTL_0 x n
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

using CommitFn = SSPC_SYSV_ABI void (*)(u8* cue);

u64 g_base = 0;
u64 g_canary = 0;        ///< SSPC_P53_NATIVE_DRAW_CANARY=<n>: every n-th marker is spoiled on purpose (0: none)
u64 g_apply_passes = 0;  ///< apply:<passes>: bit per pass whose commits and draws run natively
DrawIndexFn g_draw_index = nullptr;
DrawAutoFn g_draw_auto = nullptr;
CommitFn g_commit = nullptr;
CommitEnv g_env;

struct EngineCounters {
    std::atomic<u64> draws{0};
    std::atomic<u64> marked{0};
    std::atomic<u64> foreign{0}; ///< the command buffer is not a render context SecondSonPC knows
    std::atomic<u64> no_space{0};
    std::atomic<u64> native_draws{0};
    std::atomic<u64> draw_fallbacks{0}; ///< apply pass, but predication/flags: the game's wrapper ran
    std::atomic<u64> native_commits{0};
    std::atomic<u64> commit_fallbacks[4] = {}; ///< CommitCheck reasons: the game's commit ran
};
EngineCounters g_engine;

bool Applies(s32 pass) {
    return pass >= 0 && pass < 64 && ((g_apply_passes >> pass) & 1) != 0;
}

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

/// Room for `dwords` at the buffer's write pointer, asked the way the game's own wrappers ask (the buffer's callback:
/// 0x11eed0 for a render context). The write pointer, or null.
u32* Room(u8* dcb, u32 dwords) {
    const u64 cb = reinterpret_cast<u64>(dcb);
    if ((At<u64>(cb + L::kCbEnd) - At<u64>(cb + L::kCbCmdPtr)) / 4 < dwords) {
        const auto reserve = At<ReserveFn>(cb + 0x18);
        if (reserve == nullptr || !reserve(dcb, dwords, At<u64>(cb + 0x20))) {
            return nullptr;
        }
    }
    return reinterpret_cast<u32*>(At<u64>(cb + L::kCbCmdPtr));
}

void Advance(u8* dcb, u32* at, u32 dwords) {
    const u64 next = reinterpret_cast<u64>(at + dwords);
    std::memcpy(dcb + L::kCbCmdPtr, &next, sizeof(next));
}

/// Shadow: a kVerify marker before the game's draw packet. Apply (a listed pass): the draw itself as a kApply|kVerify
/// marker; returns true when the game's wrapper must not run.
bool Mark(u8* dcb, bool indexed, u32 count, u64 indices, u32 flags, u32 packet_dwords) {
    g_engine.draws.fetch_add(1, std::memory_order_relaxed);
    DrawOp op;
    if (!BuildDrawOp(g_base, reinterpret_cast<u64>(dcb) - L::kContextGfx, indexed, count, indices, op)) {
        g_engine.foreign.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // The predication bit of the buffer (+0x28) and the wrapper's flags reach the packet; a native draw has neither.
    const bool apply = Applies(op.pass) && flags == 0 && (At<u32>(reinterpret_cast<u64>(dcb) + 0x28) & 1) == 0;
    if (Applies(op.pass) && !apply) {
        g_engine.draw_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
    if (!apply && g_canary != 0 && g_engine.draws.load(std::memory_order_relaxed) % g_canary == 0) {
        op.canary = true; // the processor must see the count differ
        op.index_count += 1;
    }
    u32* at = Room(dcb, kDrawMarkerDwords + (apply ? 0 : packet_dwords));
    if (at == nullptr) {
        g_engine.no_space.fetch_add(1, std::memory_order_relaxed);
        return false; // nothing written: the game's wrapper draws (it reports its own failure if it has no room)
    }
    Advance(dcb, at, EncodeDrawMarker(op, apply ? u16(kApply | kVerify) : u16(kVerify), at));
    g_engine.marked.fetch_add(1, std::memory_order_relaxed);
    if (apply) {
        g_engine.native_draws.fetch_add(1, std::memory_order_relaxed);
    }
    return apply;
}

SSPC_SYSV_ABI void DrawIndexProbe(u8* dcb, u32 count, u64 indices, u32 flags) {
    if (!Mark(dcb, true, count, indices, flags, 10)) {
        g_draw_index(dcb, count, indices, flags);
    }
}

SSPC_SYSV_ABI void DrawAutoProbe(u8* dcb, u32 count, u32 flags) {
    if (!Mark(dcb, false, count, 0, flags, 7)) {
        g_draw_auto(dcb, count, flags);
    }
}

/// Apply mode: the CUE commit of a listed pass's context runs natively (CheckNativeCommit), otherwise the game's.
SSPC_SYSV_ABI void CommitProbe(u8* cue_ptr) {
    const u64 cue = reinterpret_cast<u64>(cue_ptr);
    const u64 r = cue - L::kContextCue;
    s32 pass = -1;
    u32 sub = 0;
    if (!ContextPass(g_base, r, pass, sub) || !Applies(pass) || At<u64>(cue + L::kCueDcb) != r + L::kContextGfx) {
        g_commit(cue_ptr);
        return;
    }
    if (const CommitCheck check = CheckNativeCommit(cue); check != CommitCheck::Native) {
        g_engine.commit_fallbacks[static_cast<u32>(check)].fetch_add(1, std::memory_order_relaxed);
        g_commit(cue_ptr);
        return;
    }
    u8* dcb = reinterpret_cast<u8*>(r + L::kContextGfx);
    u32* at = Room(dcb, RegistersMarkerDwords(kMaxRegWrites));
    if (at == nullptr) {
        g_engine.no_space.fetch_add(1, std::memory_order_relaxed);
        g_commit(cue_ptr);
        return;
    }
    RegWrite writes[kMaxRegWrites];
    const u32 n = NativeCommit(cue, g_env, writes);
    if (n != 0) {
        Advance(dcb, at, EncodeRegistersMarker({writes, n}, kApply, at));
    }
    g_engine.native_commits.fetch_add(1, std::memory_order_relaxed);
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
        if (g_apply_passes != 0) {
            SSPC_LOG(Engine, Info,
                     "native draw (apply): engine native draws {}, draw fallbacks {}, native commits {}, commit "
                     "fallbacks (stage mode {}, alt shader {}, missing {}) | processor applied draws {}, commits {}, "
                     "register writes {}",
                     g_engine.native_draws.load(), g_engine.draw_fallbacks.load(), g_engine.native_commits.load(),
                     g_engine.commit_fallbacks[1].load(), g_engine.commit_fallbacks[2].load(),
                     g_engine.commit_fallbacks[3].load(), c.applied_draws.load(), c.applied_commits.load(),
                     c.applied_registers.load());
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

namespace {

// CUE fields of the commit's VS/PS part (0x14ad0, after its ES/GS and LS/HS parts).
constexpr u64 kCueModeFlags = 0xC1E0;    ///< u32: 0x20 ES/GS mode, 0x1 LS/HS mode
constexpr u64 kCueChangedBits = 0xC1E4;  ///< u16: |= 4 VS set, |= 2 PS set
constexpr u64 kCuePsDirty = 0xC1C9;      ///< PS shader to set
constexpr u64 kCueVsDirty = 0xC1CA;      ///< VS shader to set
constexpr u64 kCuePsTableDirty = 0xC1D1; ///< PS resources changed: new snapshot + user data
constexpr u64 kCueVsTableDirty = 0xC1D2;
constexpr u64 kCuePsInputCache = 0xC1C0; ///< u32*: precomputed SPI_PS_INPUT_CNTL values, or 0
constexpr u64 kCueGlobalTable = 0x78;    ///< u64 pointer every stage may get (layout byte 0xC)
constexpr u32 kUserDataPs = 0x2C0C;      ///< SPI_SHADER_USER_DATA_PS_0 (table 0xaf0070)
constexpr u32 kUserDataVs = 0x2C4C;
constexpr u32 kSpiPsInputCntl0 = 0xA191;

struct Writer {
    RegWrite* out;
    u32 n = 0;
    void Reg(u32 reg, u32 value) {
        if (n < kMaxRegWrites) {
            out[n++] = RegWrite{reg, value};
        }
    }
    void Pointer(u32 user_data, u8 sgpr, u64 value) { // setPointerInUserData 0x626400
        Reg(user_data + sgpr, static_cast<u32>(value));
        Reg(user_data + sgpr + 1, static_cast<u32>(value >> 32));
    }
};

/// One stage's part of the commit: snapshot of the shadow, then the layout's user-data slots (0x14ad0, in its order).
void CommitStage(u64 cue, u32 stage, u32 user_data, const CommitEnv& env, Writer& w) {
    const u64 layout = At<u64>(cue + L::kCueLayout + u64{stage} * 8);
    const u32 dwords = At<u16>(layout);
    const u64 snapshot = env.alloc(cue, dwords);
    std::memcpy(reinterpret_cast<void*>(snapshot),
                reinterpret_cast<const void*>(cue + L::kCueShadow + u64{stage} * L::kCueShadowStride), u64{dwords} * 4);
    const auto byte = [&](u32 at) { return At<u8>(layout + at); };
    const auto table = [&](u32 sgpr_at, u32 offset_at) {
        if (const u8 sgpr = byte(sgpr_at); sgpr != 0xFF) {
            w.Pointer(user_data, sgpr, snapshot + (offset_at == 0 ? 0 : u64{At<u16>(layout + offset_at)} * 4));
        }
    };
    table(7, 0);
    table(8, 0x16);
    table(9, 0x1A);
    table(0xA, 0x1C);
    table(0xB, 0x1E);
    if (const u8 sgpr = byte(0xC); sgpr != 0xFF) {
        w.Pointer(user_data, sgpr, At<u64>(cue + kCueGlobalTable));
    }
    const bool vertex = stage == kCueStageVs;
    if (const u8 sgpr = byte(0xD); sgpr != 0xFF) { // setUserDataRegion(.., 1): one dword
        w.Reg(user_data + sgpr, At<u32>(cue + (vertex ? 0xC0E8 : 0xC0E4)));
    }
    if (const u8 sgpr = byte(0xE); sgpr != 0xFF) {
        w.Reg(user_data + sgpr, At<u32>(cue + (vertex ? 0xC108 : 0xC104)));
    }
    if (!vertex) {
        return;
    }
    if (const u8 sgpr = byte(4); sgpr != 0xFF) {
        w.Pointer(user_data, sgpr, At<u64>(cue + 0xC130));
    }
    table(5, 0x18);
    table(6, 0x20);
    if (const u8 sgpr = byte(0xF); sgpr != 0xFF) {
        const u32 v = At<u32>(cue + 0xC6E8);
        w.Reg(user_data + sgpr, (v >> 16) * (v & 0xFFFF));
    }
}

} // namespace

CommitCheck CheckNativeCommit(u64 cue) {
    if ((At<u32>(cue + kCueModeFlags) & 0x21) != 0) {
        return CommitCheck::StageMode;
    }
    if (At<u8>(cue + kCuePsAltPath) != 0 || At<u8>(cue + kCueVsAltPath) != 0) {
        return CommitCheck::AltShader;
    }
    if ((At<u8>(cue + kCueVsDirty) != 0 && At<u64>(cue + kCueVsShader) == 0) ||
        (At<u8>(cue + kCueVsTableDirty) != 0 && At<u64>(cue + L::kCueLayout + u64{kCueStageVs} * 8) == 0) ||
        (At<u8>(cue + kCuePsTableDirty) != 0 && At<u64>(cue + L::kCueLayout + u64{kCueStagePs} * 8) == 0)) {
        return CommitCheck::Missing;
    }
    return CommitCheck::Native;
}

u32 NativeCommit(u64 cue, const CommitEnv& env, RegWrite* out) {
    Writer w{out};
    const bool vs_dirty = At<u8>(cue + kCueVsDirty) != 0;
    const bool ps_dirty = At<u8>(cue + kCuePsDirty) != 0;
    const u64 vs = At<u64>(cue + kCueVsShader);
    const u64 ps = At<u64>(cue + kCuePsShader);
    u16 changed = At<u16>(cue + kCueChangedBits);
    if (vs_dirty) { // sceGnmSetVsShader(dcb, vs + 8, modifier) -> gnm_driver SetVsRegs
        changed |= 4;
        const auto* v = reinterpret_cast<const u32*>(vs + 8);
        const u32 modifier = At<u32>(cue + kCueVsModifier);
        w.Reg(0x2C48, v[0]);
        w.Reg(0x2C49, 0);
        w.Reg(0x2C4A, modifier == 0 ? v[2] : (v[2] & 0xFCFFFC3Fu) | modifier);
        w.Reg(0x2C4B, v[3]);
        w.Reg(0xA207, v[6]);
        w.Reg(0xA1B1, v[4]);
        w.Reg(0xA1C3, v[5]);
    }
    if (ps_dirty) { // sceGnmSetPsShader350(dcb, ps ? ps + 8 : null) -> SetPsRegs / the no-PS form
        changed |= 2;
        if (ps == 0) {
            w.Reg(0x2C08, 0);
            w.Reg(0x2C09, 0);
            w.Reg(0xA203, 0);
            w.Reg(0xA08F, 0xF);
        } else {
            const auto* p = reinterpret_cast<const u32*>(ps + 8);
            w.Reg(0x2C08, p[0]);
            w.Reg(0x2C09, 0);
            w.Reg(0x2C0A, p[2]);
            w.Reg(0x2C0B, p[3]);
            w.Reg(0xA1C4, p[4]);
            w.Reg(0xA1C5, p[5]);
            w.Reg(0xA1B3, p[6]);
            w.Reg(0xA1B4, p[7]);
            w.Reg(0xA1B6, p[8]);
            w.Reg(0xA1B8, p[9]);
            w.Reg(0xA203, p[10]);
            w.Reg(0xA08F, p[11]);
        }
    }
    std::memcpy(reinterpret_cast<void*>(cue + kCueChangedBits), &changed, sizeof(changed));
    if (At<u8>(cue + kCueVsTableDirty) != 0) {
        CommitStage(cue, kCueStageVs, kUserDataVs, env, w);
    }
    if (At<u8>(cue + kCuePsTableDirty) != 0) {
        CommitStage(cue, kCueStagePs, kUserDataPs, env, w);
    }
    if ((vs_dirty || ps_dirty) && vs != 0 && ps != 0) {
        const u32 n = At<u8>(ps + 0x38); // (char)ps[0xe]: PS inputs
        if (n != 0) {
            u32 local[32] = {};
            const u32* values = reinterpret_cast<const u32*>(At<u64>(cue + kCuePsInputCache));
            if (values == nullptr) {
                const u32 vs9 = At<u32>(vs + 9 * 4);
                const u64 vs_semantics = vs + 4 * (u64{At<u8>(vs + 3)} + (vs9 & 0xFF) + 10);
                const u64 ps_semantics = ps + 4 * (u64{At<u8>(ps + 3)} + 0xF);
                env.ps_inputs(local, vs_semantics, (vs9 >> 8) & 0xFF, ps_semantics, static_cast<s32>(n));
                values = local;
            }
            for (u32 i = 0; i < std::min(n, 32u); ++i) {
                w.Reg(kSpiPsInputCntl0 + i, values[i]);
            }
        }
    }
    for (const u64 flag : {kCuePsTableDirty, kCuePsDirty, kCuePsAltPath, kCueVsTableDirty, kCueVsDirty, kCueVsAltPath}) {
        *reinterpret_cast<u8*>(cue + flag) = 0;
    }
    return w.n;
}

bool InstallFromEnvironment(u64 eboot_base, bool native_backend) {
    const std::string mode = Env("SSPC_P53_NATIVE_DRAW");
    if (mode.empty() || mode == "0" || mode == "off") {
        return true;
    }
    if (mode.rfind("apply:", 0) == 0) {
        for (size_t at = 6; at < mode.size();) {
            char* end = nullptr;
            const unsigned long pass = std::strtoul(mode.c_str() + at, &end, 10);
            if (end == mode.c_str() + at || pass > 40) {
                SSPC_LOG(Engine, Error, "SSPC_P53_NATIVE_DRAW={}: want apply:<pass 0..40>[,<pass>...]; nothing installed",
                         mode);
                return false;
            }
            g_apply_passes |= u64{1} << pass;
            at = static_cast<size_t>(end - mode.c_str()) + (*end == ',' ? 1 : 0);
        }
        if (!native_backend) {
            SSPC_LOG(Engine, Error,
                     "SSPC_P53_NATIVE_DRAW={}: apply needs --gpu-backend sspc (the bridge does not execute native "
                     "operations); shadow mode instead",
                     mode);
            g_apply_passes = 0;
        }
    } else if (mode != "shadow") {
        SSPC_LOG(Engine, Error, "SSPC_P53_NATIVE_DRAW={}: want shadow or apply:<passes>; nothing installed", mode);
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
    if (g_apply_passes != 0) {
        g_env.alloc = reinterpret_cast<AllocFn>(eboot_base + 0x123d0);
        g_env.ps_inputs = reinterpret_cast<PsInputsFn>(eboot_base + 0x638540);
        void* commit = InstallEntryProbe(eboot_base, kCommitSite, reinterpret_cast<const void*>(&CommitProbe), error);
        if (commit == nullptr) {
            SSPC_LOG(Engine, Critical, "native draw: {}", error);
            std::abort();
        }
        g_commit = reinterpret_cast<CommitFn>(commit);
        std::string passes;
        for (u32 p = 0; p <= 40; ++p) {
            if (Applies(static_cast<s32>(p))) {
                passes += fmt::format(" {}", p);
            }
        }
        SSPC_LOG(Engine, Warning,
                 "EXPERIMENT: SSPC_P53_NATIVE_DRAW={} - passes{}: CUE commit 0x14ad0 and draws run natively (register "
                 "writes and draws handed to the command processor, no PM4 of their own); other passes in shadow mode",
                 mode, passes);
    }
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
