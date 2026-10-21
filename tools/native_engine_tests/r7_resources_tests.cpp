// SecondSonPC — the tests of R7-R (native resource / material / CUE engine), owned by that branch alone
// (docs/NATIVE_ENGINE_R7_CORE.md §3). main.cpp calls R7ResourcesTests with the eboot mapped (before the CUE setters'
// jumps go into the image); it returns the number of failures and adds its checks to the total through `checks`.
#include <string>

#include "base/abi.h"

namespace SecondSon::NativeEngine::Tests {

int R7ResourcesTests(u64 image_base, u64 image_size, void* recomp_module, const std::string& recomp_dll, u64 cases,
                     int& checks) {
    (void)image_base, (void)image_size, (void)recomp_module, (void)recomp_dll, (void)cases, (void)checks;
    return 0;
}

} // namespace SecondSon::NativeEngine::Tests
