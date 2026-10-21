// SecondSonPC — R7-S: the census / check of the shadow loop's 0x450460 calls (see shadow_params.h).
#include "native_engine/r7/shadows/shadow_params.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>

#include <windows.h>

#include "base/log.h"

namespace SecondSon::NativeEngine::R7Shadows::Params {

namespace {

using F7 = SSPC_SYSV_ABI void (*)(u64, u64, u64, u64, u64, u64, u64);

constexpr u32 kBlockBytes = 0x408; // 0x100 floats + the dirty byte at +0x404
constexpr u32 kMaxFloats = 16;     // the most one record writes (kind 6: 4 rows)

enum : u32 {
    kCalls,
    kRecords,
    kKindOther = 2,             // kinds 0..0x11 at 3..0x14
    kKind0 = 3,
    kKindAObject = kKind0 + 18, ///< kind 0xa with an object at buffers + 0x88
    kKindAEmpty,
    kChecked,
    kDifferent,
    kUnchecked,
    kCount
};

std::atomic<u64> g_count[kCount]{};
std::atomic<u32> g_max_slot{0};
std::atomic<u64> g_next_report{0};
std::mutex g_mu;
std::unordered_set<u64> g_vtables; // kind 0xa objects' vtables (distinct)
std::unordered_set<u64> g_passes;  // records blocks seen (distinct passes)

int Mode() { // 0 off, 1 census, 2 check (+ census)
    static const int mode = [] {
        char v[300] = {};
        const DWORD c = GetEnvironmentVariableA("SSPC_R7_SHADOWS_PARAMS_CHECK", v, sizeof(v));
        if (c > 0 && c < sizeof(v)) {
            return 2;
        }
        const DWORD n = GetEnvironmentVariableA("SSPC_R7_SHADOWS_PARAMS_CENSUS", v, sizeof(v));
        return n > 0 && n < sizeof(v) && v[0] != '0' ? 1 : 0;
    }();
    return mode;
}

/// recomp_450460 of SSPC_R7_SHADOWS_PARAMS_CHECK's module (its image base set to the eboot's), nullptr on failure.
F7 Recompiled(u64 module_base) {
    static F7 fn = [module_base]() -> F7 {
        char path[300] = {};
        const DWORD n = GetEnvironmentVariableA("SSPC_R7_SHADOWS_PARAMS_CHECK", path, sizeof(path));
        HMODULE m = n > 0 && n < sizeof(path) ? GetModuleHandleA(path) : nullptr;
        if (m == nullptr && n > 0 && n < sizeof(path)) {
            m = LoadLibraryA(path);
        }
        auto* base = m != nullptr ? reinterpret_cast<u64*>(GetProcAddress(m, "recomp_image_base")) : nullptr;
        auto* f = m != nullptr ? reinterpret_cast<F7>(GetProcAddress(m, "recomp_450460")) : nullptr;
        if (base == nullptr || f == nullptr) {
            SSPC_LOG(Engine, Error, "R7-S params: {} has no recomp_450460 / recomp_image_base: check off", path);
            return nullptr;
        }
        if (*base != 0 && *base != module_base) {
            SSPC_LOG(Engine, Error, "R7-S params: the module's image base is {:#x}, not {:#x}: check off", *base,
                     module_base);
            return nullptr;
        }
        *base = module_base;
        SSPC_LOG(Engine, Warning, "R7-S params: checking recomp_450460 of {} against 0x450460 at every shadow call",
                 path);
        return f;
    }();
    return fn;
}

template <typename T>
T At(u64 a) {
    T v;
    std::memcpy(&v, reinterpret_cast<const void*>(a), sizeof(T));
    return v;
}

void Report() {
    const u64 now = GetTickCount64();
    u64 due = g_next_report.load(std::memory_order_relaxed);
    if (due == 0) {
        g_next_report.compare_exchange_strong(due, now + 10000);
        return;
    }
    if (now < due || !g_next_report.compare_exchange_strong(due, now + 10000)) {
        return;
    }
    std::string kinds;
    for (u32 k = 0; k < 18; ++k) {
        if (const u64 v = g_count[kKind0 + k].load(); v != 0) {
            kinds += fmt::format("{}{:#x}:{}", kinds.empty() ? "" : " ", k, v);
        }
    }
    size_t vtables = 0, passes = 0;
    {
        const std::scoped_lock lock(g_mu);
        vtables = g_vtables.size();
        passes = g_passes.size();
    }
    SSPC_LOG(Engine, Info,
             "R7-S params: {} calls, {} records, {} record blocks; kinds {} other:{}; 0xa with object {} ({} vtables), "
             "0xa without {}; largest slot {:#x}; checked {} different {} unchecked {}",
             g_count[kCalls].load(), g_count[kRecords].load(), passes, kinds, g_count[kKindOther].load(),
             g_count[kKindAObject].load(), vtables, g_count[kKindAEmpty].load(), g_max_slot.load(),
             g_count[kChecked].load(), g_count[kDifferent].load(), g_count[kUnchecked].load());
}

inline void Add(u32 c, u64 v = 1) {
    g_count[c].fetch_add(v, std::memory_order_relaxed);
}

} // namespace

u32 SlotField(s32 kind) {
    switch (kind) {
    case 1: case 2: case 5: return 0x28;
    case 0: case 3: return 0x2a;
    case 4: return 0x2c;
    case 7: return 0x2e;
    case 6: return 0x30;
    case 9: return 0x32;
    case 0xa: return 0x36;
    case 0xc: return 0x38;
    case 0xd: return 0x3a;
    case 0xe: return 0x3c;
    case 0xf: return 0x3e;
    case 0x10: return 0x40;
    case 0x11: return 0x42;
    default: return 0;
    }
}

bool On() {
    return Mode() != 0;
}

void Call(u64 original, u64 records, u64 block, u64 atlas, u64 view, u64 table, u64 buffers, u64 owner) {
    const auto run = [&](F7 f) { f(records, block, atlas, view, table, buffers, owner); };
    // --- census (the records as 0x450460 reads them: count at +0, kinds at +8) ---
    const s32 count = At<s32>(records);
    const u32 n = count > 0 ? (count < 4 ? static_cast<u32>(count) : 4u) : 0u;
    bool in_block = true;
    Add(kCalls);
    Add(kRecords, n);
    for (u32 i = 0; i < n; ++i) {
        const s32 kind = At<s32>(records + 8 + 4 * i);
        Add(kind >= 0 && kind < 18 ? kKind0 + static_cast<u32>(kind) : kKindOther);
        if (kind == 0xa) {
            const u64 object = At<u64>(buffers + 0x88);
            Add(object != 0 ? kKindAObject : kKindAEmpty);
            if (object != 0) {
                const std::scoped_lock lock(g_mu);
                g_vtables.insert(At<u64>(object));
            }
        }
        if (const u32 field = SlotField(kind); field != 0) {
            u32 slot = At<u16>(records + field);
            if (kind == 0xc || kind == 0xd) {
                slot += 8; // three at slot, three at slot + 4, the rest from slot + 8
            }
            u32 seen = g_max_slot.load(std::memory_order_relaxed);
            while (slot > seen && !g_max_slot.compare_exchange_weak(seen, slot)) {
            }
            in_block = in_block && slot + kMaxFloats <= 0x100;
        }
    }
    {
        const std::scoped_lock lock(g_mu);
        g_passes.insert(records);
    }
    F7 const recompiled = Mode() == 2 ? Recompiled(original - 0x450460) : nullptr;
    if (recompiled == nullptr) {
        run(reinterpret_cast<F7>(original));
        Report();
        return;
    }
    if (!in_block) {
        Add(kUnchecked);
        run(reinterpret_cast<F7>(original));
        Report();
        return;
    }
    // --- check: the recompiled first, then the original (its result stays) ---
    alignas(16) u8 before[kBlockBytes], recomp_out[kBlockBytes];
    std::memcpy(before, reinterpret_cast<const void*>(block), kBlockBytes);
    run(recompiled);
    std::memcpy(recomp_out, reinterpret_cast<const void*>(block), kBlockBytes);
    std::memcpy(reinterpret_cast<void*>(block), before, kBlockBytes);
    run(reinterpret_cast<F7>(original));
    Add(kChecked);
    if (std::memcmp(recomp_out, reinterpret_cast<const void*>(block), kBlockBytes) != 0) {
        const u64 bad = g_count[kDifferent].fetch_add(1, std::memory_order_relaxed);
        if (bad < 8) {
            u32 at = 0;
            while (at < kBlockBytes && recomp_out[at] == At<u8>(block + at)) {
                ++at;
            }
            s32 kinds[4] = {};
            std::memcpy(kinds, reinterpret_cast<const void*>(records + 8), sizeof(kinds));
            SSPC_LOG(Engine, Error,
                     "R7-S params: recomp_450460 differs from 0x450460 at block +{:#x} (records {} kinds {} {} {} {}, "
                     "object {:#x})",
                     at, count, kinds[0], kinds[1], kinds[2], kinds[3], At<u64>(buffers + 0x88));
        }
    }
    Report();
}

} // namespace SecondSon::NativeEngine::R7Shadows::Params
