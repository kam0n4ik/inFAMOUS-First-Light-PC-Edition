// SecondSonPC — the registry of reconstructed engine functions: installation, live state, counters (see
// native_function_registry.h). Environment handling and the reporter: native_engine_install.cpp.
#include "native_engine/native_function_registry.h"

#include <array>
#include <cstring>

#include <fmt/format.h>

#include "engine/guest_hooks.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/reconstructed/cue_set_buffers.h"
#include "native_engine/reconstructed/frame_scratch.h"
#include "native_engine/reconstructed/object_constants.h"

namespace SecondSon::NativeEngine {

namespace {

const FunctionSpec* const kFunctions[] = {
    &Reconstructed::kObjectConstantsSpec,
    &Reconstructed::kFrameScratchAllocSpec,
    &Reconstructed::kCueSetVertexBuffersSpec,
    &Reconstructed::kCueSetBuffersSpec,
};
constexpr u32 kFunctionCount = static_cast<u32>(std::size(kFunctions));
static_assert(kFunctionCount <= kMaxFunctions);

// --- per-thread counters ---------------------------------------------------------------------------------------
constexpr u32 kMaxThreads = 512;
std::array<std::atomic<ThreadCounters*>, kMaxThreads> g_blocks{};
std::atomic<u32> g_block_count{0};
ThreadCounters g_shared; ///< threads beyond kMaxThreads share one block (atomic increments)

} // namespace

const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Original:
        return "original";
    case Mode::Native:
        return "native";
    case Mode::Verify:
        return "verify";
    case Mode::Guarded:
        return "guarded";
    case Mode::Bench:
        return "bench";
    }
    return "?";
}

bool ParseMode(const std::string& text, Mode& mode) {
    if (text == "1" || text == "native" || text == "on") {
        mode = Mode::Native;
    } else if (text == "verify") {
        mode = Mode::Verify;
    } else if (text == "guarded") {
        mode = Mode::Guarded;
    } else if (text == "bench") {
        mode = Mode::Bench;
    } else if (text == "0" || text == "off" || text == "original") {
        mode = Mode::Original;
    } else {
        return false;
    }
    return true;
}

u32 FunctionCount() {
    return kFunctionCount;
}

const FunctionSpec& Function(u32 index) {
    return *kFunctions[index];
}

ThreadCounters* RegisterThreadCounters() {
    if (t_counters == nullptr) {
        const u32 i = g_block_count.fetch_add(1, std::memory_order_relaxed);
        if (i < kMaxThreads) {
            t_counters = new ThreadCounters;
            g_blocks[i].store(t_counters, std::memory_order_release);
        } else {
            g_shared.shared = true;
            t_counters = &g_shared;
        }
    }
    return t_counters;
}

u64 Total(u32 function, Counter counter) {
    u64 sum = g_shared.counters[function][counter].load(std::memory_order_relaxed);
    const u32 n = std::min(g_block_count.load(std::memory_order_acquire), kMaxThreads);
    for (u32 i = 0; i < n; ++i) {
        if (const ThreadCounters* b = g_blocks[i].load(std::memory_order_acquire)) {
            sum += b->counters[function][counter].load(std::memory_order_relaxed);
        }
    }
    return sum;
}

bool SampleThisCall(u32 function, u32 period) {
    ThreadCounters* b = t_counters != nullptr ? t_counters : RegisterThreadCounters();
    if (++b->sample[function] >= period) {
        b->sample[function] = 0;
        return true;
    }
    return false;
}

void Fallback(Slot& slot) {
    if (slot.mode.exchange(static_cast<u8>(Mode::Original), std::memory_order_relaxed) !=
        static_cast<u8>(Mode::Original)) {
        slot.fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

bool InstallFunction(u64 module_base, const FunctionSpec& spec, Mode mode, std::string& error) {
    const auto fail = [&](const std::string& why) {
        error = fmt::format("{} (eboot+{:#x}): {}", spec.name, spec.function.rva, why);
        return false;
    };
    std::string why;
    if (!Engine::VerifyGuestFunction(module_base, spec.function, why)) {
        return fail(why);
    }
    for (u32 i = 0; i < spec.assumed_count; ++i) {
        if (!Engine::VerifyGuestFunction(module_base, spec.assumed[i], why)) {
            return fail("assumed function: " + why);
        }
    }
    for (u32 i = 0; i < spec.data_count; ++i) {
        const GuestData& d = spec.data[i];
        const u64 h = Engine::HashBytes(reinterpret_cast<const void*>(module_base + d.rva), d.size);
        if (h != d.fnv1a64) {
            return fail(fmt::format("data eboot+{:#x} ({} bytes) hashes {:#018x}, expected {:#018x}", d.rva, d.size, h,
                                    d.fnv1a64));
        }
    }
    const u64 entry = module_base + spec.function.rva;
    const CloneResult clone =
        CloneGuestFunction(entry, reinterpret_cast<const u8*>(entry), spec.function.size, why);
    if (clone.code == nullptr) {
        return fail("clone: " + why);
    }
    if (spec.prepare != nullptr && !spec.prepare(module_base, why)) {
        return fail("prepare: " + why);
    }
    if (spec.self_check != nullptr && !spec.self_check(clone.code, why)) {
        return fail("self-check: " + why);
    }
    Slot& slot = *spec.slot;
    slot.original = clone.code;
    for (u32 i = 0; i < kFunctionCount; ++i) {
        if (kFunctions[i] == &spec) {
            slot.index = i;
        }
    }
    slot.mode.store(static_cast<u8>(mode), std::memory_order_release);
    if (!Engine::InstallJump(entry, spec.entry, why)) {
        return fail(why);
    }
    return true;
}

} // namespace SecondSon::NativeEngine
