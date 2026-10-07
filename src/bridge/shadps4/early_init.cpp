// SecondSonPC — TEMPORARY Phase 0-4 bridge.
// shadPS4 computes its user directory in a namespace-scope static (common/path_util.cpp:UserPaths)
// from current_path()\user and falls back to %APPDATA%\shadPS4. `init_seg(lib)` runs this
// initializer before ordinary ("user") static initializers, so the shadPS4 core always sees
// E:\SecondSonPC\runtime\sspc\user and never touches the installed shadPS4.
#pragma init_seg(lib)

#include "app/paths.h"

namespace {

struct EarlyInit {
    EarlyInit() {
        SecondSon::App::PrepareBridgeWorkingDirectory();
    }
};

EarlyInit g_early_init;

} // namespace
