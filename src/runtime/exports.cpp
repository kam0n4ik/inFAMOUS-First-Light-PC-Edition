// SecondSonPC — export table. NIDs/library/module strings are the game's own import keys
// (docs/generated/imports.csv, prx_imports.csv; the loader checks them against its import table).
#include "runtime/exports.h"

#include <vector>

#include "runtime/filesystem/file_system.h"
#include "runtime/filesystem/fs_observer.h"
#include "runtime/kernel/kernel.h"
#include "runtime/pad/pad.h"
#include "runtime/system/system_service.h"
#include "runtime/user/user_service.h"

namespace SecondSon::Loader {
std::span<const Runtime::Export> RtldExports(); // loader/rtld.cpp
}

namespace SecondSon::Runtime {

namespace Sysmodule {
std::span<const Export> SysmoduleExports(); // runtime/sysmodule/sysmodule.cpp
}
namespace PlayGo {
std::span<const Export> PlayGoExports(); // runtime/playgo/playgo.cpp
}
namespace AppContent {
std::span<const Export> AppContentExports(); // runtime/app_content/app_content.cpp
}
namespace Net {
std::span<const Export> NetUtilExports(); // runtime/net/net_util.cpp
}
namespace Libc {
std::span<const Export> LibcMemExports(); // runtime/libc/libc_mem.cpp
}

namespace {

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kSystemExports[] = {
    {"fZo48un7LK4", "libSceSystemService", "libSceSystemService", "sceSystemServiceParamGetInt",
     Fn(&SystemService::ParamGetInt), nullptr, "system", true},
    {"SsC-m-S9JTA", "libSceSystemService", "libSceSystemService", "sceSystemServiceParamGetString",
     Fn(&SystemService::ParamGetString), nullptr, "system", true},
    {"rPo6tV8D9bM", "libSceSystemService", "libSceSystemService", "sceSystemServiceGetStatus",
     Fn(&SystemService::GetStatus), nullptr, "system"},
    {"656LMQSrg6U", "libSceSystemService", "libSceSystemService", "sceSystemServiceReceiveEvent",
     Fn(&SystemService::ReceiveEvent), nullptr, "system"},
    {"Vo5V8KAwCmk", "libSceSystemService", "libSceSystemService", "sceSystemServiceHideSplashScreen",
     Fn(&SystemService::HideSplashScreen), nullptr, "system"},
    {"1n37q1Bvc5Y", "libSceSystemService", "libSceSystemService", "sceSystemServiceGetDisplaySafeAreaInfo",
     Fn(&SystemService::GetDisplaySafeAreaInfo), nullptr, "system"},
};

// Observers in front of the BRIDGE file functions (called by the game's libSceFios2.prx). They
// stay active while the file functions are BRIDGE.
const Export kFilesystemObservers[] = {
    {"1G3lF1Gg1k8", "libkernel", "libkernel", "sceKernelOpen", Fn(&Filesystem::ObserveOpen),
     &Filesystem::g_open_original, "fs", true},
    {"eV9wAD2riIA", "libkernel", "libkernel", "sceKernelStat", Fn(&Filesystem::ObserveStat),
     &Filesystem::g_stat_original, "fs", true},
};

std::vector<Export> Collect() {
    std::vector<Export> all;
    // Order matters: the first entry for an import key wins (native FileSystem before the Phase 0
    // observers of the same functions, which stay for --loader shadps4 / --bridge fs).
    for (const auto group : {std::span<const Export>(kSystemExports), Loader::RtldExports(),
                             Sysmodule::SysmoduleExports(), Filesystem::FileSystemExports(),
                             UserService::UserServiceExports(), PlayGo::PlayGoExports(),
                             AppContent::AppContentExports(), Pad::PadExports(),
                             Kernel::KernelExports(), Kernel::SemaphoreExports(),
                             Kernel::EventFlagExports(), Kernel::KernelUtilExports(),
                             Kernel::MutexExports(), Kernel::CondvarExports(), Kernel::RwlockExports(),
                             Net::NetUtilExports(), Libc::LibcMemExports(),
                             std::span<const Export>(kFilesystemObservers)}) {
        all.insert(all.end(), group.begin(), group.end());
    }
    return all;
}

} // namespace

std::span<const Export> Exports() {
    static const std::vector<Export> all = Collect();
    return all;
}

} // namespace SecondSon::Runtime
