// SecondSonPC — the tests of R7-G (native G-buffer / geometry engine), owned by that branch alone
// (docs/NATIVE_ENGINE_R7_CORE.md §3). main.cpp calls R7GBufferTests with the eboot mapped (before the CUE setters'
// jumps go into the image); it returns the number of failures and adds its checks to the total through `checks`.
//
// 0x1a2030 (the geometry pass): the native pass (r7/gbuffer/geometry_pass.h) against the eboot's own code as a
// recording clone, on generated cases of both variants (gbuffer_mesh_pass.h SelfCheck); two wrong implementations
// must be caught.
#include <cstdio>
#include <string>

#include "base/abi.h"
#include "native_engine/reconstructed/gbuffer_mesh_pass.h"
#include "native_engine/reconstructed/gbuffer_trace.h"

namespace SecondSon::NativeEngine::Tests {

namespace {

namespace MP = Reconstructed::GBufferMeshPass;

int g_fail = 0;
int* g_checks = nullptr;

#define R7G_CHECK(cond)                                                                                                \
    do {                                                                                                               \
        ++*g_checks;                                                                                                   \
        if (!(cond)) {                                                                                                 \
            ++g_fail;                                                                                                  \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                           \
        }                                                                                                              \
    } while (0)

} // namespace

int R7GBufferTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                   int& checks) {
    (void)image_size, (void)recomp_module, (void)recomp_dll;
    g_fail = 0;
    g_checks = &checks;
    std::printf("R7-G geometry pass 0x1a2030 (gbuffer_mesh_pass.h):\n");
    if (image_base == 0) {
        std::printf("  (no eboot: skipped)\n");
        return 0;
    }
    std::string error;
    const bool prepared = MP::Prepare(image_base, error);
    R7G_CHECK(prepared);
    if (!prepared) {
        std::printf("  prepare: %s\n", error.c_str());
        return g_fail;
    }
    const u32 n = cases > 0 ? static_cast<u32>(cases) : 2000;
    const std::string a = MP::SelfCheck(n, 0x1a2030 ^ cases);
    std::printf("  native pass vs eboot 0x1a2030 (recording clone): %u cases%s%s\n", n, a.empty() ? ", equal" : ": ",
                a.c_str());
    R7G_CHECK(a.empty());
    MP::EnableStats(true);
    const std::string s = MP::SelfCheck(200, 11);
    MP::EnableStats(false);
    std::printf("  with the branch counters on: %s\n", s.empty() ? "equal" : s.c_str());
    R7G_CHECK(s.empty());
    std::printf("  branch coverage of the generated cases (per case):%s\n", MP::StatsText(200, true).c_str());
    MP::InjectFault(1);
    const std::string f1 = MP::SelfCheck(300, 7);
    MP::InjectFault(2);
    const std::string f2 = MP::SelfCheck(300, 7);
    MP::InjectFault(0);
    std::printf("  wrong implementations seen: no stencil cache \"%s\", a wrong record field \"%s\"\n", f1.c_str(),
                f2.c_str());
    R7G_CHECK(!f1.empty());
    R7G_CHECK(!f2.empty());
    std::printf("  orphan thunk calls: %llu\n",
                static_cast<unsigned long long>(Reconstructed::GTrace::OrphanCalls()));
    return g_fail;
}

} // namespace SecondSon::NativeEngine::Tests
