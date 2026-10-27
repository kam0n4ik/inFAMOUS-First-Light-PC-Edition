// SecondSonPC — fixes of races in the game's own code (see game_fixes.h, docs/GAME_FIXES.md).
#include "engine/game_fixes.h"

#include <atomic>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_hooks.h"

namespace SecondSon::Engine::GameFixes {

namespace {

// The engine's reference counting (an object's strong count at +0x10 and weak count at +0x14; destroyed when both are
// 0, through its vtable slot 2). Its two releases each decrement their own count and then READ the other one:
//   strong 0x1be900: lock dec [rdi+0x10]; cmp [rdi+0x10],0; jnz ret; cmp [rdi+0x14],0; jz destroy; ret
//   weak   0x1be820 (under its manager's mutex): lock dec [r14+0x14]; cmp [r14+0x14],0; jnz done;
//          cmp [r14+0x10],0; jz destroy; call [vtable+0x18] (the last weak reference gone, strong ones left)
// The last strong and the last weak release on two threads both see (0, 0) and both destroy: the object goes back to
// its pool twice, two owners get it later and one uses it after the other freed it again (the powers menu's movies,
// thread "MovieDecoder": an execute of 0x0 through a vtable that is the pool's free-list link, 2026-10-18 11:38,
// 16:15, 17:35). Fixed: both counts change in one atomic 64-bit operation, and only the release whose operation
// leaves (0, 0) destroys. Before calling through a vtable it is checked to be inside the game's image (an object
// already back in its pool: logged, not called).
#if defined(SSPC_PRODUCT_FIRST_LIGHT)
// First Light: the same bytes, once each in its text; the weak release 0x1fe460 has Second Son's frame (rsp 16-aligned
// at the site), research/fl_sites.md §6
constexpr u64 kStrongRva = 0x1fe540;
constexpr u64 kWeakRva = 0x1fe4e2;
constexpr u64 kWeakDone = 0x1fe50d, kWeakDestroy = 0x1fe504, kWeakNotify = 0x1fe4f5;
#else
constexpr u64 kStrongRva = 0x1be900;
constexpr u64 kWeakRva = 0x1be8a2; // inside 0x1be820, after its mutex is taken
constexpr u64 kWeakDone = 0x1be8cd, kWeakDestroy = 0x1be8c4, kWeakNotify = 0x1be8b5;
#endif
constexpr u8 kStrongOriginal[23] = {0xF0, 0xFF, 0x4F, 0x10, 0x83, 0x7F, 0x10, 0x00, 0x75, 0x06, 0x83, 0x7F,
                                    0x14, 0x00, 0x74, 0x01, 0xC3, 0x48, 0x8B, 0x07, 0xFF, 0x60, 0x10};
constexpr u8 kWeakOriginal[19] = {0xF0, 0x41, 0xFF, 0x4E, 0x14, 0x41, 0x83, 0x7E, 0x14, 0x00,
                                  0x75, 0x1F, 0x41, 0x83, 0x7E, 0x10, 0x00, 0x74, 0x0F};

u64 g_base = 0;
std::atomic<u32> g_pooled{0};

bool VtableInImage(const u8* obj) {
    const u64 vt = *reinterpret_cast<const u64*>(obj);
    return vt >= GuestImageBase() && vt + 0x20 <= GuestImageEnd();
}

void ReportPooled(const char* which, const u8* obj) {
    const u32 n = g_pooled.fetch_add(1) + 1;
    if (n <= 16 || (n & (n - 1)) == 0) {
        SSPC_LOG(Engine, Error,
                 "reference counting: {} release destroys {:#x} whose vtable {:#x} is not the game's (already back "
                 "in its pool) - not called ({} so far)",
                 which, reinterpret_cast<u64>(obj), *reinterpret_cast<const u64*>(obj), n);
    }
}

std::atomic<u64>& Counts(u8* obj) {
    return *reinterpret_cast<std::atomic<u64>*>(obj + 0x10);
}

/// 0x1be900 jumps here: the strong release.
SSPC_SYSV_ABI void StrongRelease(u8* obj) {
    std::atomic<u64>& c = Counts(obj);
    u64 old = c.load(std::memory_order_relaxed);
    u64 now = 0;
    do {
        now = (old & 0xFFFFFFFF00000000ull) | static_cast<u32>(static_cast<u32>(old) - 1);
    } while (!c.compare_exchange_weak(old, now, std::memory_order_acq_rel, std::memory_order_relaxed));
    if (now != 0) {
        return;
    }
    if (!VtableInImage(obj)) {
        ReportPooled("a strong", obj);
        return;
    }
    using Destroy = SSPC_SYSV_ABI void (*)(u8*);
    reinterpret_cast<Destroy>((*reinterpret_cast<u64* const*>(obj))[2])(obj);
}

/// 0x1be820 calls here (rdi = the object): the weak release; returns where that function goes on.
SSPC_SYSV_ABI u64 WeakRelease(u8* obj) {
    std::atomic<u64>& c = Counts(obj);
    const u64 now = c.fetch_sub(1ull << 32, std::memory_order_acq_rel) - (1ull << 32);
    if ((now >> 32) != 0) {
        return g_base + kWeakDone;
    }
    if (!VtableInImage(obj)) {
        ReportPooled("a weak", obj);
        return g_base + kWeakDone;
    }
    return g_base + (static_cast<u32>(now) == 0 ? kWeakDestroy : kWeakNotify);
}

bool Write(u8* at, const u8* bytes, u32 size) {
    DWORD old = 0;
    if (!VirtualProtect(at, size, PAGE_EXECUTE_READWRITE, &old)) {
        return false;
    }
    std::memcpy(at, bytes, size);
    DWORD unused = 0;
    VirtualProtect(at, size, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, size);
    return true;
}

bool Mapped(const u8* at, u32 size) {
    MEMORY_BASIC_INFORMATION mbi{};
    return VirtualQuery(at, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT &&
           reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize >= reinterpret_cast<u64>(at) + size;
}

void InstallRefcount(u64 base) {
    const std::string name = fmt::format("the engine's reference counting (eboot {:#x} / {:#x})", kStrongRva, kWeakRva);
    const char* kName = name.c_str();
    char v[8] = {};
    if (GetEnvironmentVariableA("SSPC_FIX_REFCOUNT_RACE", v, sizeof(v)) == 1 && v[0] == '0') {
        SSPC_LOG(Engine, Warning, "game fix off (SSPC_FIX_REFCOUNT_RACE=0): {}", kName);
        return;
    }
    auto* strong = reinterpret_cast<u8*>(base + kStrongRva);
    auto* weak = reinterpret_cast<u8*>(base + kWeakRva);
    if (!Mapped(strong, sizeof(kStrongOriginal)) || !Mapped(weak, sizeof(kWeakOriginal))) {
        SSPC_LOG(Engine, Warning, "game fix not installed: {} - not mapped", kName);
        return;
    }
    if (std::memcmp(strong, kStrongOriginal, sizeof(kStrongOriginal)) != 0 ||
        std::memcmp(weak, kWeakOriginal, sizeof(kWeakOriginal)) != 0) {
        SSPC_LOG(Engine, Warning, "game fix not installed: {} - other bytes (another game version or patched code)",
                 kName);
        return;
    }
    g_base = base;
    // strong: mov rax, StrongRelease; jmp rax
    u8 s[12] = {0x48, 0xB8};
    const u64 sf = reinterpret_cast<u64>(&StrongRelease);
    std::memcpy(s + 2, &sf, 8);
    s[10] = 0xFF;
    s[11] = 0xE0;
    // weak: mov rdi, r14; mov rax, WeakRelease; call rax; jmp rax; nop; nop  (rsp is 16-aligned there)
    u8 w[19] = {0x4C, 0x89, 0xF7, 0x48, 0xB8};
    const u64 wf = reinterpret_cast<u64>(&WeakRelease);
    std::memcpy(w + 5, &wf, 8);
    const u8 tail[6] = {0xFF, 0xD0, 0xFF, 0xE0, 0x90, 0x90};
    std::memcpy(w + 13, tail, 6);
    if (!Write(weak, w, sizeof(w)) || !Write(strong, s, sizeof(s))) {
        SSPC_LOG(Engine, Error, "game fix not installed: {} - VirtualProtect failed ({})", kName, GetLastError());
        return;
    }
    SSPC_LOG(Engine, Info, "game fix installed: {}", kName);
}

} // namespace

void Install(u64 eboot_base) {
    InstallRefcount(eboot_base);
}

} // namespace SecondSon::Engine::GameFixes
