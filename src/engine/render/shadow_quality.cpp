// SecondSonPC — the game's shadow map sizes (see shadow_quality.h).
#include "engine/render/shadow_quality.h"

#include <cstring>
#include <string>

#include <fmt/format.h>

#include "base/log.h"
#include "engine/guest_hooks.h"
#include "engine/guest_probe.h"

namespace SecondSon::Engine::Render::ShadowQuality {

namespace {

constexpr u64 kSunSize = 0xdf9d2c, kRainSize = 0xdf9d30, kCascade0Size = 0xdf9d38;
// The functions reading them (FNV-1a 64 of the body, tools/native-reconstruction/function_hashes.py).
constexpr GuestFunction kReaders[] = {
    {0x18e4c0, 0x1512, 0x98f7ee4645cf906cull}, // makes the targets at start
    {0x43b7b0, 0x99c, 0x013a31573e4f5b53ull},  // the cascades' update (rain_shadows probes it)
    {0x439830, 0x1c9, 0x868050b9e77e9ce9ull},  // a cascade's view of the target
    {0x439ab0, 0x1de, 0x80362b58f322413cull},  // its viewport
    {0x43e3e0, 0x12cc, 0xb6f81f3b01388b87ull}, // the sampling constants (size, 1 / size)
    {0x441080, 0x1182, 0x3e7d4e52f40dd90cull}, // the cascade matrices (texel snap)
};

u32 Read(u64 base, u64 at) {
    u32 v = 0;
    std::memcpy(&v, reinterpret_cast<const void*>(base + at), 4);
    return v;
}

void Write(u64 base, u64 at, u32 v) {
    std::memcpy(reinterpret_cast<void*>(base + at), &v, 4);
}

} // namespace

bool Install(u64 eboot_base, std::string_view level) {
    u32 sun = 2048, rain = 1024;
    if (level == "low") {
        sun = 1024, rain = 512;
    } else if (level == "high") {
        sun = 4096, rain = 2048;
    } else {
        return true; // "original" (and anything else): nothing written, the game's own
    }
    std::string error;
    bool verified = true;
    for (const GuestFunction& f : kReaders) {
        // (another module's entry probe on it verified the same bytes)
        const ProbeRecord* probe = FindEntryProbe(eboot_base + f.rva);
        verified = verified && ((probe != nullptr && probe->function.fnv1a64 == f.fnv1a64 &&
                                 probe->function.size == f.size) ||
                                VerifyGuestFunction(eboot_base, f, error));
    }
    const u32 was_sun = Read(eboot_base, kSunSize), was_rain = Read(eboot_base, kRainSize),
              was_c0 = Read(eboot_base, kCascade0Size);
    if (!verified || was_sun != 2048 || was_rain != 1024 || was_c0 != 2048) {
        SSPC_LOG(Engine, Error, "Shadow quality {} not applied (the game's own sizes): {}", level,
                 verified ? fmt::format("the sizes are {} / {} / {}, not 2048 / 1024 / 2048", was_sun, was_rain, was_c0)
                          : error);
        return false;
    }
    Write(eboot_base, kSunSize, sun);
    Write(eboot_base, kCascade0Size, sun);
    Write(eboot_base, kRainSize, rain);
    SSPC_LOG(Engine, Warning, "Shadow quality {}: the sun's maps {}x{}, the rain's {}x{} (the game's 2048 / 1024)", level,
             sun, sun, rain, rain);
    return true;
}

} // namespace SecondSon::Engine::Render::ShadowQuality
