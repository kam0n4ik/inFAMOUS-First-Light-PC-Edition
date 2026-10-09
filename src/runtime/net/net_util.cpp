// SecondSonPC — libSceNet byte-order helpers (NATIVE since Phase 2.2.3, Tier 1 of
// docs/PHASE2_NATIVE_MIGRATION_MATRIX.md). Group "net-util": `--bridge net-util` sends both back
// to the oracle (third_party/shadPS4/src/core/libraries/net/net.cpp sceNetHtons/sceNetNtohs).
// The rest of libSceNet stays BRIDGE: sceNetInetNtop/Pton set the library's errno (read through
// sceNetErrnoLoc, mirrored into the kernel errno by a bridge hook), so they move with the group.
#include "base/abi.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::Net {

namespace {

u16 Swap16(u16 v) {
    return static_cast<u16>((v << 8) | (v >> 8));
}

SSPC_SYSV_ABI u16 Htons(u16 host16) {
    return Swap16(host16);
}

SSPC_SYSV_ABI u16 Ntohs(u16 net16) {
    return Swap16(net16);
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    {"iWQWrwiSt8A", "libSceNet", "libSceNet", "sceNetHtons", Fn(&Htons), nullptr, "net-util"},
    {"Rbvt+5Y2iEw", "libSceNet", "libSceNet", "sceNetNtohs", Fn(&Ntohs), nullptr, "net-util"},
};

} // namespace

std::span<const Export> NetUtilExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Net
