// SecondSonPC — entry probes on functions of the game's own code (see guest_probe.h).
#include "engine/guest_probe.h"

#include <cstring>
#include <mutex>

#include <fmt/format.h>
#include <windows.h>

namespace SecondSon::Engine {

namespace {

/// Trampolines live in pages SecondSonPC allocates: written while the page is read-write, then made execute-read.
struct TrampolineArena {
    std::mutex lock;
    u8* page = nullptr;
    u32 used = 0;
};
TrampolineArena g_arena;
constexpr u32 kPage = 4096;

u8* Allocate(u32 bytes, std::string& error) {
    if (g_arena.page == nullptr || g_arena.used + bytes > kPage) {
        void* p = VirtualAlloc(nullptr, kPage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (p == nullptr) {
            error = fmt::format("trampoline page: VirtualAlloc failed ({})", GetLastError());
            return nullptr;
        }
        g_arena.page = static_cast<u8*>(p);
        g_arena.used = 0;
    }
    u8* at = g_arena.page + g_arena.used;
    g_arena.used += (bytes + 15) & ~15u;
    return at;
}

/// Installed probes (a handful; written in the loader's window, read by later installs in the same window).
constexpr u32 kMaxProbes = 32;
ProbeRecord g_probes[kMaxProbes];
u32 g_probe_count = 0;

bool Protect(u8* page, DWORD protection, std::string& error) {
    DWORD old = 0;
    if (!VirtualProtect(page, kPage, protection, &old)) {
        error = fmt::format("trampoline page: VirtualProtect failed ({})", GetLastError());
        return false;
    }
    return true;
}

} // namespace

u32 BuildTrampoline(const u8* entry, u32 prologue, u64 resume, u8* out) {
    std::memcpy(out, entry, prologue);
    const u8 jump[6] = {0xFF, 0x25, 0, 0, 0, 0}; // jmp qword [rip+0]
    std::memcpy(out + prologue, jump, sizeof(jump));
    std::memcpy(out + prologue + sizeof(jump), &resume, sizeof(resume));
    return TrampolineSize(prologue);
}

void* InstallEntryProbe(u64 module_base, const ProbeSite& site, const void* target, std::string& error,
                        void** original_slot) {
    if (site.prologue < 14 || site.prologue > site.function.size || site.prologue > sizeof(ProbeRecord::replaced)) {
        error = fmt::format("eboot+{:#x}: prologue of {} bytes", site.function.rva, site.prologue);
        return nullptr;
    }
    if (!VerifyGuestFunction(module_base, site.function, error)) {
        return nullptr;
    }
    const u64 entry = module_base + site.function.rva;
    std::lock_guard guard(g_arena.lock);
    u8* trampoline = Allocate(TrampolineSize(site.prologue), error);
    if (trampoline == nullptr) {
        return nullptr;
    }
    u8* page = reinterpret_cast<u8*>(reinterpret_cast<u64>(trampoline) & ~u64{kPage - 1});
    if (!Protect(page, PAGE_READWRITE, error)) {
        return nullptr;
    }
    BuildTrampoline(reinterpret_cast<const u8*>(entry), site.prologue, entry + site.prologue, trampoline);
    if (!Protect(page, PAGE_EXECUTE_READ, error)) {
        return nullptr;
    }
    FlushInstructionCache(GetCurrentProcess(), trampoline, TrampolineSize(site.prologue));
    ProbeRecord record;
    record.entry = entry;
    record.function = site.function;
    record.prologue = site.prologue;
    std::memcpy(record.replaced, reinterpret_cast<const void*>(entry), site.prologue);
    record.original_slot = original_slot;
    if (!InstallJump(entry, target, error)) {
        return nullptr;
    }
    if (g_probe_count < kMaxProbes) {
        g_probes[g_probe_count++] = record;
    }
    return trampoline;
}

const ProbeRecord* FindEntryProbe(u64 entry) {
    std::lock_guard guard(g_arena.lock);
    for (u32 i = 0; i < g_probe_count; ++i) {
        if (g_probes[i].entry == entry) {
            return &g_probes[i];
        }
    }
    return nullptr;
}

} // namespace SecondSon::Engine
