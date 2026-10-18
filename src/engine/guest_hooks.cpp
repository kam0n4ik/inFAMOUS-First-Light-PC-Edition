// SecondSonPC — replacing functions of the game's own code with native ones (see guest_hooks.h).
#include "engine/guest_hooks.h"

#include <algorithm>
#include <vector>
#include <atomic>
#include <cstring>

#include <fmt/format.h>
#include <windows.h>

namespace SecondSon::Engine {

namespace {
std::atomic<u64> g_image_base{0};
std::atomic<u64> g_image_end{0};
} // namespace

void SetGuestImage(u64 base, u64 size) {
    g_image_base.store(base, std::memory_order_release);
    g_image_end.store(base + size, std::memory_order_release);
}

u64 GuestImageBase() {
    return g_image_base.load(std::memory_order_acquire);
}

u64 GuestImageEnd() {
    return g_image_end.load(std::memory_order_acquire);
}

u64 HashBytes(const void* data, size_t size) {
    u64 h = 0xCBF29CE484222325ull;
    const auto* p = static_cast<const u8*>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

namespace {
struct Pristine {
    u64 address;
    std::vector<u8> bytes;
};
std::vector<Pristine> g_pristine; // (written by the loader before any guest thread; read-only afterwards)
} // namespace

void AddPristineCode(u64 address, const void* data, u64 size) {
    g_pristine.push_back({address, std::vector<u8>(static_cast<const u8*>(data), static_cast<const u8*>(data) + size)});
}

bool VerifyGuestFunction(u64 module_base, const GuestFunction& f, std::string& error) {
    const u64 at = module_base + f.rva;
    for (const Pristine& p : g_pristine) {
        if (at < p.address || at + f.size > p.address + p.bytes.size()) {
            continue;
        }
        const u8* code = p.bytes.data() + (at - p.address);
        if (std::memcmp(code, reinterpret_cast<const void*>(at), std::min<u32>(16, f.size)) != 0) {
            error = fmt::format("eboot+{:#x}: its entry was rewritten by the static CPU patcher", f.rva);
            return false;
        }
        const u64 h = HashBytes(code, f.size);
        if (h != f.fnv1a64) {
            error = fmt::format("eboot+{:#x} ({} bytes): hash {:#018x}, expected {:#018x} (another game version)",
                                f.rva, f.size, h, f.fnv1a64);
            return false;
        }
        return true;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_COMMIT ||
        reinterpret_cast<u64>(mbi.BaseAddress) + mbi.RegionSize < at + f.size) {
        error = fmt::format("eboot+{:#x}: not mapped", f.rva);
        return false;
    }
    const u64 h = HashBytes(reinterpret_cast<const void*>(at), f.size);
    if (h != f.fnv1a64) {
        error = fmt::format("eboot+{:#x} ({} bytes): hash {:#018x}, expected {:#018x} (another game version or "
                            "patched code)",
                            f.rva, f.size, h, f.fnv1a64);
        return false;
    }
    return true;
}

bool InstallJump(u64 at, const void* target, std::string& error) {
    u8 code[14] = {0xFF, 0x25, 0, 0, 0, 0}; // jmp qword [rip+0]
    const u64 dest = reinterpret_cast<u64>(target);
    std::memcpy(code + 6, &dest, sizeof(dest));
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(at), sizeof(code), PAGE_EXECUTE_READWRITE, &old)) {
        error = fmt::format("{:#x}: VirtualProtect failed ({})", at, GetLastError());
        return false;
    }
    std::memcpy(reinterpret_cast<void*>(at), code, sizeof(code));
    DWORD unused = 0;
    VirtualProtect(reinterpret_cast<void*>(at), sizeof(code), old, &unused);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(at), sizeof(code));
    return true;
}

} // namespace SecondSon::Engine
