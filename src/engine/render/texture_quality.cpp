// SecondSonPC — the game's high-resolution city textures (see texture_quality.h).
#include "engine/render/texture_quality.h"

#include <cstring>
#include <string>

#include <windows.h>

#include "base/log.h"
#include "engine/guest_hooks.h"

namespace SecondSon::Engine::Render::TextureQuality {

namespace {

// The streaming update (FNV-1a 64 of the body, tools/native-reconstruction/function_hashes.py).
constexpr GuestFunction kStreamUpdate{0xfd890, 0x13ee, 0xa516f863457b77c3ull};
// 0xfe9a0: mov r12d, 4 ; mov dword [rbp-0x26c], 0 ; cmp eax, 4 ; cmovle r12d, eax
constexpr u64 kSite = 0xfe9a0;
constexpr u8 kBytes[] = {0x41, 0xBC, 0x04, 0x00, 0x00, 0x00, 0xC7, 0x85, 0x94, 0xFD, 0xFF, 0xFF,
                         0x00, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x04, 0x44, 0x0F, 0x4E, 0xE0};
constexpr u32 kMovImm = 2, kCmpImm = 18; // (offsets in kBytes: the mov's imm32, the cmp's imm8)

} // namespace

bool Install(u64 eboot_base, std::string_view level) {
    u8 count = 4;
    if (level == "medium") {
        count = 2;
    } else if (level == "low") {
        count = 0;
    } else {
        return true; // "high" (and anything else): nothing written, the game's own
    }
    std::string error;
    auto* at = reinterpret_cast<u8*>(eboot_base + kSite);
    if (!VerifyGuestFunction(eboot_base, kStreamUpdate, error) || std::memcmp(at, kBytes, sizeof(kBytes)) != 0) {
        SSPC_LOG(Engine, Error, "Texture quality {} not applied (the game's own): {}", level,
                 error.empty() ? "eboot+0xfe9a0 is not the analysed code" : error);
        return false;
    }
    DWORD old = 0;
    if (!VirtualProtect(at, sizeof(kBytes), PAGE_EXECUTE_READWRITE, &old)) {
        SSPC_LOG(Engine, Error, "Texture quality {} not applied: VirtualProtect failed ({})", level, GetLastError());
        return false;
    }
    const u32 imm = count;
    std::memcpy(at + kMovImm, &imm, 4);
    at[kCmpImm] = count;
    DWORD unused = 0;
    VirtualProtect(at, sizeof(kBytes), old, &unused);
    FlushInstructionCache(GetCurrentProcess(), at, sizeof(kBytes));
    SSPC_LOG(Engine, Warning, "Texture quality {}: high-resolution textures for the {} nearest city blocks (the game's 4)",
             level, count);
    return true;
}

} // namespace SecondSon::Engine::Render::TextureQuality
