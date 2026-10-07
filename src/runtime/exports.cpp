// SecondSonPC — export table. NIDs/library/module strings are the game's own import keys
// (docs/generated/imports.csv, prx_imports.csv).
#include "runtime/exports.h"

#include "runtime/filesystem/fs_observer.h"
#include "runtime/system/system_service.h"

namespace SecondSon::Runtime {

namespace {

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    // NATIVE
    {"fZo48un7LK4", "libSceSystemService", "libSceSystemService", "sceSystemServiceParamGetInt",
     Fn(&SystemService::ParamGetInt), nullptr},
    {"SsC-m-S9JTA", "libSceSystemService", "libSceSystemService", "sceSystemServiceParamGetString",
     Fn(&SystemService::ParamGetString), nullptr},
    // Observers (implementation stays BRIDGE in Phase 0; called by the game's libSceFios2.prx)
    {"1G3lF1Gg1k8", "libkernel", "libkernel", "sceKernelOpen", Fn(&Filesystem::ObserveOpen),
     &Filesystem::g_open_original},
    {"eV9wAD2riIA", "libkernel", "libkernel", "sceKernelStat", Fn(&Filesystem::ObserveStat),
     &Filesystem::g_stat_original},
};

} // namespace

std::span<const Export> Exports() {
    return kExports;
}

} // namespace SecondSon::Runtime
