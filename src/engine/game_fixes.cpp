// SecondSonPC — fixes of races in the game's own code (see game_fixes.h, docs/GAME_FIXES.md).
#include "engine/game_fixes.h"

#include <cstring>

#include <windows.h>

#include "base/log.h"

namespace SecondSon::Engine::GameFixes {

namespace {

/// A byte patch: `size` bytes at eboot + rva must be exactly `original`; `offset`.. of them become `patch`.
struct Fix {
    const char* name;
    const char* env; // "<name>=0" turns it off
    u64 rva;
    u8 original[32];
    u32 size;
    u32 offset;
    u8 patch[16];
    u32 patch_size;
};

// The engine's reference release (eboot 0x1be900, 131 callers - resources, movies, ...):
//   lock dec dword [rdi+0x10]; cmp dword [rdi+0x10], 0; jnz ret; cmp dword [rdi+0x14], 0; jz destroy; ret;
//   destroy: mov rax, [rdi]; jmp [rax+0x10]
// The count is read again after the atomic decrement: two threads releasing the last two references can both read
// 0 and destroy the object twice - the second time through the freed object's overwritten vtable pointer (the powers
// menu's movies, thread "MovieDecoder": an execute of 0x0, 2026-10-18 twice). Fixed by deciding on the decrement's
// own zero flag (`jnz ret` right after `lock dec`), as an atomic release does; the rest of the function is unchanged.
constexpr Fix kRefcountRelease{
    "the engine's reference release (eboot 0x1be900)",
    "SSPC_FIX_REFCOUNT_RACE",
    0x1be900,
    {0xF0, 0xFF, 0x4F, 0x10, 0x83, 0x7F, 0x10, 0x00, 0x75, 0x06, 0x83, 0x7F,
     0x14, 0x00, 0x74, 0x01, 0xC3, 0x48, 0x8B, 0x07, 0xFF, 0x60, 0x10},
    23,
    4,
    {0x75, 0x0A, 0x90, 0x90, 0x90, 0x90}, // jnz +0x0a (-> ret at +0x10); nop x4
    6,
};

bool Enabled(const Fix& f) {
    char v[8] = {};
    return !(GetEnvironmentVariableA(f.env, v, sizeof(v)) == 1 && v[0] == '0');
}

void Apply(u64 base, const Fix& f) {
    if (!Enabled(f)) {
        SSPC_LOG(Engine, Warning, "game fix off ({}=0): {}", f.env, f.name);
        return;
    }
    auto* at = reinterpret_cast<u8*>(base + f.rva);
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(at, &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize < base + f.rva + f.size) {
        SSPC_LOG(Engine, Warning, "game fix not installed: {} - not mapped", f.name);
        return;
    }
    if (std::memcmp(at, f.original, f.size) != 0) {
        SSPC_LOG(Engine, Warning, "game fix not installed: {} - other bytes (another game version or patched code)",
                 f.name);
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(at, f.size, PAGE_EXECUTE_READWRITE, &old)) {
        SSPC_LOG(Engine, Error, "game fix not installed: {} - VirtualProtect failed ({})", f.name, GetLastError());
        return;
    }
    std::memcpy(at + f.offset, f.patch, f.patch_size);
    DWORD unused = 0;
    VirtualProtect(at, f.size, old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, f.size);
    SSPC_LOG(Engine, Info, "game fix installed: {}", f.name);
}

} // namespace

void Install(u64 eboot_base) {
    Apply(eboot_base, kRefcountRelease);
}

} // namespace SecondSon::Engine::GameFixes
