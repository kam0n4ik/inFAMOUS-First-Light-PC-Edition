// SecondSonPC — libSceLibcInternal memset/memcpy (NATIVE since Phase 2.2.3, Tier 1 of
// docs/PHASE2_NATIVE_MIGRATION_MATRIX.md). The game's libSceFios2.prx imports them (the game's own
// libc.prx has its own copies). Group "libc-mem": `--bridge libc-mem` sends both back to the oracle
// (third_party/shadPS4/src/core/libraries/libc_internal/libc_internal_memory.cpp). Like the oracle
// and C, overlapping memcpy ranges are undefined.
#include <cstring>

#include "base/abi.h"
#include "runtime/exports.h"

namespace SecondSon::Runtime::Libc {

namespace {

SSPC_SYSV_ABI void* Memset(void* s, s32 c, u64 n) {
    return std::memset(s, c, n);
}

SSPC_SYSV_ABI void* Memcpy(void* dest, const void* src, u64 n) {
    return std::memcpy(dest, src, n);
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Export kExports[] = {
    {"8zTFvBIAIN8", "libSceLibcInternal", "libSceLibcInternal", "memset", Fn(&Memset), nullptr, "libc-mem"},
    {"Q3VBxCXhUHs", "libSceLibcInternal", "libSceLibcInternal", "memcpy", Fn(&Memcpy), nullptr, "libc-mem"},
};

} // namespace

std::span<const Export> LibcMemExports() {
    return kExports;
}

} // namespace SecondSon::Runtime::Libc
