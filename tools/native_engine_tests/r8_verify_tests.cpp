// SecondSonPC — the tests of R8-A5 Verification (differential, state and frame checks), owned by that branch
// (docs/NATIVE_ENGINE_R8_CORE.md §3). main.cpp calls R8VerifyTests with the eboot mapped (before the CUE setters' jumps
// go into the image); it returns the number of failures and adds its checks to the total through `checks`.
//
// 1. The sites of runtime/gpu/r8/r8_verify.h: SSPC_R8_VERIFY parsing (names, periods, "all", typos), Sample periods,
//    the Diff paths of the first differing field (scalars, bytes, sequences, field tables, scopes), per-site records
//    (a site flooded with mismatches - also from several threads at once - keeps its own kRecordsPerSite and never
//    takes another site's), the report.
// 2. CompareStageView / ComparePlan: equal views compare equal (padding ignored), each kind of change is named.
// 3. native_engine_verify's cases: per function (a function flooding the cases leaves another function's slots).
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include "base/abi.h"
#include "engine/render/native_render_ops.h"
#include "native_engine/native_engine_verify.h"
#include "native_engine/native_function_registry.h"
#include "runtime/gpu/r8/r8_verify.h"
#include "runtime/gpu/shader/shader_frontend.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

namespace V = Gpu::R8::Verify;
namespace GS = Gpu::Shader;

int g_fail = 0;
int* g_checks = nullptr;

#define R8V_CHECK(cond)                                                                                                \
    do {                                                                                                               \
        ++*g_checks;                                                                                                   \
        if (!(cond)) {                                                                                                 \
            ++g_fail;                                                                                                  \
            std::printf("  FAILED: %s (%s:%d)\n", #cond, __FILE__, __LINE__);                                          \
        }                                                                                                              \
    } while (0)

bool FieldIs(const V::Diff& d, const char* name) {
    if (std::strcmp(d.FirstField(), name) != 0) {
        std::printf("    first field '%s', expected '%s'\n", d.FirstField(), name);
        return false;
    }
    return true;
}

void SitesAndSpec() {
    const V::SiteId a = V::Register("r8v_test_a");
    const V::SiteId b = V::Register("r8v_test_b");
    const V::SiteId c = V::Register("r8v_test_c");
    R8V_CHECK(a != 0 && b != 0 && c != 0 && a != b && b != c);
    R8V_CHECK(V::Register("r8v_test_a") == a); // the same name: the same site
    R8V_CHECK(V::Name(b) == "r8v_test_b");

    V::ResetForTests("");
    R8V_CHECK(!V::Enabled(a) && !V::Enabled(b) && !V::Enabled(c) && !V::Enabled(0));
    R8V_CHECK(V::Report().empty()); // not set: no section in gpu_native.txt

    V::ResetForTests(" r8v_test_a:4 , r8v_test_b,r8v_typo ");
    R8V_CHECK(V::Enabled(a) && V::Enabled(b) && !V::Enabled(c));
    int sampled = 0;
    for (int i = 0; i < 40; ++i) {
        sampled += V::Sample(a) ? 1 : 0;
    }
    R8V_CHECK(sampled == 10);
    bool every = true;
    for (int i = 0; i < 10; ++i) {
        every &= V::Sample(b);
    }
    R8V_CHECK(every);
    const std::string report = V::Report();
    R8V_CHECK(report.find("r8v_test_a [verify every 4]") != std::string::npos);
    R8V_CHECK(report.find("r8v_test_c [off]") != std::string::npos);
    R8V_CHECK(report.find("requested but no such site registered: r8v_typo") != std::string::npos);

    V::ResetForTests("all:2");
    R8V_CHECK(V::Enabled(a) && V::Enabled(b) && V::Enabled(c));
    V::ResetForTests("all,r8v_test_c:3"); // a name's own period wins over "all"
    R8V_CHECK(V::Enabled(a) && V::Enabled(c));
    sampled = 0;
    for (int i = 0; i < 9; ++i) {
        sampled += V::Sample(c) ? 1 : 0;
    }
    R8V_CHECK(sampled == 3);
    V::ResetForTests("off");
    R8V_CHECK(!V::Enabled(a));
}

struct Packed {
    u32 x;
    u8 y; // 3 bytes of padding after it
    u64 z;
    u32 w[4];
};

void Diffs() {
    {
        V::Diff d;
        R8V_CHECK(d.Field("count", 5u, 5u) && d.Same());
        R8V_CHECK(!d.Field("count", 5u, 6u) && !d.Same() && FieldIs(d, "count") && d.Expected() == 5 && d.Actual() == 6);
        R8V_CHECK(!d.Field("other", 1, 2) && d.Differences() == 2 && FieldIs(d, "count")); // the first one stays
        const float f0 = 0.0f, f1 = -0.0f;
        V::Diff e;
        R8V_CHECK(!e.Field("f", f0, f1)); // floats by their bits: +0 and -0 differ
    }
    {
        u8 x[40] = {}, y[40] = {};
        V::Diff d;
        R8V_CHECK(d.Bytes("mem", x, y, sizeof(x)));
        y[19] = 0xAB;
        R8V_CHECK(!d.Bytes("mem", x, y, sizeof(x)) && FieldIs(d, "mem+0x13") && d.Expected() == 0 &&
                  d.Actual() == (u64{0xAB} << 24));
        u8 s[3] = {1, 2, 3}, t[3] = {1, 2, 4};
        V::Diff e;
        R8V_CHECK(!e.Bytes("tail", s, t, 3) && FieldIs(e, "tail+0x2")); // a window shorter than 8 bytes
    }
    {
        const u64 e[] = {1, 2, 3, 4}, a[] = {1, 2, 9, 4}, b[] = {1, 2, 3};
        V::Diff d, f, g;
        R8V_CHECK(d.Sequence("cmds", e, e));
        R8V_CHECK(!d.Sequence("cmds", e, a) && FieldIs(d, "cmds[2]") && d.Expected() == 3 && d.Actual() == 9);
        R8V_CHECK(!f.Sequence("cmds", e, b) && FieldIs(f, "cmds.size") && f.Expected() == 4 && f.Actual() == 3);
        {
            V::Diff::Scope s1(g, "run", 7);
            V::Diff::Scope s2(g, "barriers");
            R8V_CHECK(!g.Sequence("", a, e) && FieldIs(g, "run[7].barriers[2]"));
        }
        V::Diff h;
        {
            V::Diff::Scope s(h, "outer");
        }
        R8V_CHECK(!h.Field("x", 1, 2) && FieldIs(h, "x")); // the scope ended: no prefix
    }
    {
        static constexpr V::Diff::FieldDesc kFields[] = {SSPC_R8_FIELD(Packed, x), SSPC_R8_FIELD(Packed, y),
                                                         SSPC_R8_FIELD(Packed, z), SSPC_R8_FIELD(Packed, w)};
        Packed p, q;
        std::memset(&p, 0x11, sizeof(p)); // different padding bytes, equal fields
        std::memset(&q, 0x22, sizeof(q));
        p.x = q.x = 1;
        p.y = q.y = 2;
        p.z = q.z = 3;
        for (int i = 0; i < 4; ++i) {
            p.w[i] = q.w[i] = 10u + static_cast<u32>(i);
        }
        V::Diff d;
        R8V_CHECK(d.Fields("cmd", kFields, &p, &q) && d.Same());
        q.w[2] = 99;
        R8V_CHECK(!d.Fields("cmd", kFields, &p, &q) && FieldIs(d, "cmd.w+0x8"));
        V::Diff e;
        q.w[2] = p.w[2];
        q.z = 4;
        R8V_CHECK(!e.Fields("cmd", kFields, &p, &q) && FieldIs(e, "cmd.z") && e.Expected() == 3 && e.Actual() == 4);
    }
}

void RecordsPerSite() {
    const V::SiteId flood = V::Register("r8v_flood");
    const V::SiteId quiet = V::Register("r8v_quiet");
    V::ResetForTests("r8v_flood,r8v_quiet");
    // one site with a mass of mismatches, from 4 threads at once
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([flood, t] {
            for (u64 i = 0; i < 5000; ++i) {
                V::Diff d;
                d.Field("value", i, i + static_cast<u64>(i % 3 != 0));
                V::Check(flood, {static_cast<u64>(t), i, 0x1234}, d);
            }
        });
    }
    for (std::thread& th : threads) {
        th.join();
    }
    const V::SiteStats f = V::Stats(flood);
    R8V_CHECK(f.checks == 20000);
    R8V_CHECK(f.mismatches == 4 * (5000 - 1667)); // i % 3 == 0 matches: 1667 of 0..4999
    R8V_CHECK(f.records == V::kRecordsPerSite);
    // the other site still keeps its first mismatch, whole
    R8V_CHECK(!V::Check(quiet, {77, 12, 0xBEEF}, false, "plan.buffers[1].address", 0x1000, 0x2000));
    R8V_CHECK(V::Check(quiet, {78, 1, 0}, true, "x", 0, 0));
    V::Unverifiable(quiet, 3);
    V::Unverifiable(quiet, 99); // clamped to the last reason
    const V::SiteStats q = V::Stats(quiet);
    R8V_CHECK(q.checks == 2 && q.mismatches == 1 && q.records == 1 && q.unverifiable == 2);
    const std::string rec = V::Records(quiet);
    R8V_CHECK(rec.find("#1 frame 77 draw 12 key 0xbeef: plan.buffers[1].address expected 0x1000 actual 0x2000") !=
              std::string::npos);
    const std::string report = V::Report();
    R8V_CHECK(report.find("r8v_quiet [verify every 1]: checks 2, mismatches 1, not verifiable 2 (by reason: r3 1 r7 1)"
                          ", records kept 1") != std::string::npos);
    // a reset clears the counters and the records
    V::ResetForTests("");
    R8V_CHECK(V::Stats(flood).checks == 0 && V::Stats(flood).records == 0);
}

void Stages() {
    GS::StageView e;
    e.status = GS::StageStatus::Ready;
    e.address = 0x12345000;
    e.footer_hash = 0xabcdef;
    e.runtime.rsrc1 = 0x77;
    e.start = {1, 2};
    e.end = {3, 4};
    e.plan.stage = GS::HwStage::Ps;
    e.plan.flat = {1, 2, 3, 4, 5, 6};
    e.plan.buffers.resize(3);
    e.plan.buffers[1].address = 0x8000;
    e.plan.buffers[1].desc.raw = {1, 2, 3, 4};
    e.plan.images.resize(2);
    e.plan.images[1].desc.raw[5] = 9;
    e.plan.samplers.resize(1);
    e.plan.vertices.resize(2);
    e.plan.vertices[0].fetch.data_format = 13;
    GS::StageView a = e;
    // padding bytes of the bindings differ (built twice): not a difference
    static_assert(sizeof(GS::BufferBinding) > offsetof(GS::BufferBinding, status) + 1);
    std::memset(reinterpret_cast<u8*>(&a.plan.buffers[0]) + offsetof(GS::BufferBinding, status) + 1, 0x5A, 1);
    {
        V::Diff d;
        R8V_CHECK(V::CompareStageView(e, a, d) && d.Same());
    }
    const auto expect = [&](auto change, const char* field) {
        GS::StageView x = e;
        change(x);
        V::Diff d;
        const bool same = V::CompareStageView(e, x, d);
        R8V_CHECK(!same && FieldIs(d, field));
    };
    expect([](GS::StageView& x) { x.plan.buffers[1].address = 0x8040; }, "plan.buffers[1].address");
    expect([](GS::StageView& x) { x.plan.buffers[1].desc.raw[2] = 7; }, "plan.buffers[1].desc[2]");
    expect([](GS::StageView& x) { x.plan.images[1].desc.raw[5] = 8; }, "plan.images[1].desc[5]");
    expect([](GS::StageView& x) { x.plan.vertices[0].fetch.data_format = 14; }, "plan.vertices[0].fetch.data_format");
    expect([](GS::StageView& x) { x.plan.flat[4] = 50; }, "plan.flat[4]");
    expect([](GS::StageView& x) { x.plan.samplers.clear(); }, "plan.samplers.size");
    expect([](GS::StageView& x) { x.end.buffer = 5; }, "end.buffer");
    expect([](GS::StageView& x) { x.runtime.rsrc1 = 0x78; }, "runtime+0x4");
    expect([](GS::StageView& x) { x.module = reinterpret_cast<GS::Module*>(0x10); }, "module");
    expect([](GS::StageView& x) { x.plan.error = "walk"; }, "plan.error");
    {
        V::Diff d;
        GS::ResourceBindingPlan p = e.plan;
        p.status_counts[2] = 1;
        R8V_CHECK(!V::ComparePlan(e.plan, p, d) && FieldIs(d, "status_counts[2]"));
    }
    // draw commands of the native stream
    Engine::Render::DrawOp op;
    op.pass = 14;
    op.indexed = true;
    op.index_count = 300;
    op.index_base = 0x10000;
    op.shaders = op.has_ps = true;
    op.vs[2] = 0x1234;
    op.table[1] = {true, 24, 0xfeed, 2, {{4, 0}, {6, 8}}};
    {
        V::Diff d;
        R8V_CHECK(V::CompareDrawOp(op, op, d) && d.Same());
        Engine::Render::DrawOp x = op;
        x.table[1].ref[1].offset = 9;
        R8V_CHECK(!V::CompareDrawOp(op, x, d) && FieldIs(d, "table[1].ref[1].offset"));
        V::Diff f;
        x = op;
        x.ps[11] = 0xF;
        R8V_CHECK(!V::CompareDrawOp(op, x, f) && FieldIs(f, "ps[11]"));
    }
}

void NativeCasesPerFunction() {
    // the two last registry slots are past the registered functions (never flushed to a file)
    const u32 noisy = kMaxFunctions - 1, other = kMaxFunctions - 2;
    R8V_CHECK(FunctionCount() <= other);
    const u8 bytes[16] = {1, 2, 3};
    const u64 total = CasesRecorded(), dropped = CasesDropped();
    for (int i = 0; i < 100; ++i) {
        RecordCase(noisy, bytes);
    }
    R8V_CHECK(CasesRecorded(noisy) == kCasesPerFunction);
    R8V_CHECK(CasesDropped(noisy) == 100 - kCasesPerFunction);
    const u64 before = CasesRecorded(other);
    RecordCase(other, bytes);
    R8V_CHECK(CasesRecorded(other) == before + 1); // before R8: dropped once the shared ring of 64 was full
    R8V_CHECK(CasesRecorded() == total + kCasesPerFunction + 1);
    R8V_CHECK(CasesDropped() == dropped + 100 - kCasesPerFunction);
    std::vector<u8> big(kCaseBytes + 1, 0);
    RecordCase(other, big);
    R8V_CHECK(CasesRecorded(other) == before + 1 && CasesDropped(other) == 1);
}

/// The deterministic mode's premise on the eboot's own clock 0x12a160 (r8_verify_frame.h): with the object's "last
/// TSC" (+0x1b0) set to 0 before the call, the dt it stores (+0x00) is exactly its clamp (1/30 s; 1/15 s under the flag
/// byte), whatever the TSC; without that, a call right after the previous one measures a short dt. The clock is run
/// on the mapped image with its own object (eboot+0xe16290: the code reads it directly too), saved and put back.
void DetClock(u64 image_base, u64 image_size) {
    constexpr u64 kClock = 0x12a160, kObject = 0xe16290, kFreq = 0x7f91f88, kFlag = 0x7d3bd72, kClamp30 = 0x9944f0,
                  kClamp15 = 0x9944ec;
    if (image_base == 0 || image_size <= kFreq + 8) {
        std::printf("  (no mapped eboot: the clock check is skipped)\n");
        return;
    }
    using ClockFn = SSPC_SYSV_ABI void (*)(u64);
    const auto clock = reinterpret_cast<ClockFn>(image_base + kClock);
    u8* obj = reinterpret_cast<u8*>(image_base + kObject);
    u8 saved[0x200];
    u64 saved_freq = 0;
    u8 saved_flag = 0;
    std::memcpy(saved, obj, sizeof(saved));
    std::memcpy(&saved_freq, reinterpret_cast<void*>(image_base + kFreq), 8);
    std::memcpy(&saved_flag, reinterpret_cast<void*>(image_base + kFlag), 1);
    const u64 freq = 3'000'000'000ull; // the TSC frequency the game would have read (any: the clamp does not depend on it)
    std::memcpy(reinterpret_cast<void*>(image_base + kFreq), &freq, 8);
    float clamp30 = 0, clamp15 = 0, dt = -1;
    std::memcpy(&clamp30, reinterpret_cast<const void*>(image_base + kClamp30), 4);
    std::memcpy(&clamp15, reinterpret_cast<const void*>(image_base + kClamp15), 4);
    bool exact = true;
    for (u8 flag : {u8{0}, u8{1}}) {
        std::memcpy(reinterpret_cast<void*>(image_base + kFlag), &flag, 1);
        for (int i = 0; i < 100; ++i) {
            std::memset(obj + 0x1b0, 0, 8);
            clock(image_base + kObject);
            std::memcpy(&dt, obj, 4);
            exact &= dt == (flag ? clamp15 : clamp30);
        }
    }
    R8V_CHECK(clamp30 == 1.0f / 30.0f || std::fabs(clamp30 - 1.0f / 30.0f) < 1e-7f);
    R8V_CHECK(clamp15 == 1.0f / 15.0f || std::fabs(clamp15 - 1.0f / 15.0f) < 1e-7f);
    R8V_CHECK(exact);
    // SSPC_R8_VERIFY_DET_STEP (Frame::SetClampStep): one step in both constants - dt is that step under either flag
    {
        DWORD old = 0;
        void* page = reinterpret_cast<void*>(image_base + kClamp15);
        const bool writable = VirtualProtect(page, 8, PAGE_READWRITE, &old) != 0;
        R8V_CHECK(writable);
        if (writable) {
            bool step_exact = true;
            for (float step : {1.0f / 30.0f, 1.0f / 15.0f}) {
                std::memcpy(reinterpret_cast<void*>(image_base + kClamp15), &step, 4);
                std::memcpy(reinterpret_cast<void*>(image_base + kClamp30), &step, 4);
                for (u8 flag : {u8{0}, u8{1}}) {
                    std::memcpy(reinterpret_cast<void*>(image_base + kFlag), &flag, 1);
                    std::memset(obj + 0x1b0, 0, 8);
                    clock(image_base + kObject);
                    std::memcpy(&dt, obj, 4);
                    step_exact &= dt == step;
                }
            }
            R8V_CHECK(step_exact);
            std::memcpy(reinterpret_cast<void*>(image_base + kClamp15), &clamp15, 4);
            std::memcpy(reinterpret_cast<void*>(image_base + kClamp30), &clamp30, 4);
            DWORD unused = 0;
            VirtualProtect(page, 8, old, &unused);
        }
    }
    // the game's own way: two calls in a row measure the time between them (well under the clamp)
    const u8 zero = 0;
    std::memcpy(reinterpret_cast<void*>(image_base + kFlag), &zero, 1);
    clock(image_base + kObject);
    clock(image_base + kObject);
    std::memcpy(&dt, obj, 4);
    R8V_CHECK(dt >= 0.0f && dt < clamp30 * 0.5f);
    std::memcpy(obj, saved, sizeof(saved));
    std::memcpy(reinterpret_cast<void*>(image_base + kFreq), &saved_freq, 8);
    std::memcpy(reinterpret_cast<void*>(image_base + kFlag), &saved_flag, 1);
}

} // namespace

int R8VerifyTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                  int& checks) {
    (void)recomp_module, (void)recomp_dll, (void)cases;
    g_fail = 0;
    g_checks = &checks;
    const int before = checks;
    std::printf("R8 verify: sites, diffs, records, stage views, native cases per function, deterministic clock\n");
    SitesAndSpec();
    Diffs();
    RecordsPerSite();
    Stages();
    NativeCasesPerFunction();
    DetClock(image_base, image_size);
    V::ResetForTests(""); // leave nothing enabled for the tests after these
    std::printf("R8 verify: %d checks, %d failures\n", checks - before, g_fail);
    return g_fail;
}

} // namespace SecondSon::NativeEngine::Tests
