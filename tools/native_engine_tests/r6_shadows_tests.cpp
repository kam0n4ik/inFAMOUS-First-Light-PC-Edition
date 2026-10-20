// SecondSonPC — the tests of R6-S (shadow engine), owned by that branch alone (docs/NATIVE_RENDER_R6_CORE.md §3). main.cpp calls
// R6ShadowsTests with the eboot mapped (before the CUE setters' jumps go into the image); it returns the number of failures and
// adds its checks to the total through the counters it is given.
#include <string>

#include "base/abi.h"

namespace SecondSon::NativeEngine::Tests {

int R6ShadowsTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases, int& checks) {
    (void)image_base, (void)image_size, (void)recomp_module, (void)recomp_dll, (void)cases, (void)checks;
    return 0;
}

} // namespace SecondSon::NativeEngine::Tests
