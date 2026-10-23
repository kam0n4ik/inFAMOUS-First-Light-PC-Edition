// SecondSonPC — the registry of reconstructed engine functions: installation, live state, counters (see
// native_function_registry.h). Environment handling and the reporter: native_engine_install.cpp.
#include "native_engine/native_function_registry.h"

#include <array>
#include <cstring>

#include <fmt/format.h>

#include "engine/guest_hooks.h"
#include "engine/guest_probe.h"
#include "native_engine/guest_code_clone.h"
#include "native_engine/native_dispatch.h"
#include "native_engine/reconstructed/cue_commit.h"
#include "native_engine/reconstructed/cue_set_buffers.h"
#include "native_engine/reconstructed/frame_scratch.h"
#include "native_engine/reconstructed/gbuffer_draw_constants.h"
#include "native_engine/reconstructed/material_bind.h"
#include "native_engine/reconstructed/object_constants.h"
#include "native_engine/reconstructed/shadow_instance_uv.h"
// R6: one extension point per subsystem, each owned by one branch (docs/NATIVE_RENDER_R6_CORE.md §3)
#include "native_engine/r6/r6_core.h"
#include "native_engine/r6/r6_gbuffer.h"
#include "native_engine/r6/r6_shadows.h"
// R7: the same per R7 branch (docs/NATIVE_ENGINE_R7_CORE.md §3)
#include "native_engine/r7/r7_core.h"
#include "native_engine/r7/r7_gbuffer.h"
#include "native_engine/r7/r7_resources.h"
#include "native_engine/r7/r7_shadows.h"
// R8: the same per R8 branch that adds functions (docs/NATIVE_ENGINE_R8_CORE.md §3)
#include "native_engine/r8/r8_core.h"
#include "native_engine/r8/r8_recomp.h"
#include "native_engine/r8/r8_render.h"

namespace SecondSon::NativeEngine {

namespace {

const FunctionSpec* const kFunctions[] = {
    &Reconstructed::kObjectConstantsSpec,
    &Reconstructed::kFrameScratchAllocSpec,
    &Reconstructed::kCueSetVertexBuffersSpec,
    &Reconstructed::kCueSetBuffersSpec,
    // R5-D: the CUE setters of the material setup, and the material setup 0x448640 itself
    &Reconstructed::kCueSetConstantBuffersSpec,
    &Reconstructed::kCueSetTexturesSpec,
    &Reconstructed::kCueSetSamplersSpec,
    &Reconstructed::kMaterialBindSpec,
    // R5-D: the CUE commit 0x14ad0 (its recompiled implementation only; explicit_only)
    &Reconstructed::kCueCommitSpec,
    // R5-E: the G-buffer draws' constants 0x11e5d0
    &Reconstructed::kDrawConstantsSetSpec,
    // R5-F: the shadow instances' atlas UV 0x451700
    &Reconstructed::kShadowInstanceUvSpec,
    // R6 (r6/r6_*.h): the geometry engine, the shadow engine, the render bridge
    SSPC_R6_GBUFFER_SPECS
    SSPC_R6_SHADOWS_SPECS
    SSPC_R6_CORE_SPECS
    // R7 (r7/r7_*.h): the native geometry, shadow and resource engines, the Native Render Layer
    SSPC_R7_GBUFFER_SPECS
    SSPC_R7_SHADOWS_SPECS
    SSPC_R7_RESOURCES_SPECS
    SSPC_R7_CORE_SPECS
    // R8 (r8/r8_*.h): the reconstructions the CP and the Native Render Layer need
    SSPC_R8_RECOMP_SPECS
    SSPC_R8_RENDER_SPECS
    SSPC_R8_CORE_SPECS
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

static const Engine::ProbeRecord* ProbeBelow(u64 module_base, const Engine::GuestFunction& f) {
    const Engine::ProbeRecord* p = Engine::FindEntryProbe(module_base + f.rva);
    return p != nullptr && p->function.rva == f.rva && p->function.size == f.size &&
                   p->function.fnv1a64 == f.fnv1a64
               ? p
               : nullptr;
}

const u8* OriginalBytes(u64 module_base, const Engine::GuestFunction& f, std::vector<u8>& storage) {
    const u8* at = reinterpret_cast<const u8*>(module_base + f.rva);
    const Engine::ProbeRecord* p = ProbeBelow(module_base, f);
    if (p == nullptr) {
        return at;
    }
    storage.assign(at, at + f.size);
    std::memcpy(storage.data(), p->replaced, p->prologue);
    return storage.data();
}

bool InstalledBelowProbe(const FunctionSpec& spec) {
    return spec.slot != nullptr && spec.slot->below_probe;
}

bool InstallFunction(u64 module_base, const FunctionSpec& spec, Mode mode, std::string& error) {
    const auto fail = [&](const std::string& why) {
        error = fmt::format("{} (eboot+{:#x}): {}", spec.name, spec.function.rva, why);
        return false;
    };
    std::string why;
    // an entry probe on the function (verified when it went in) takes the reconstruction below it
    const Engine::ProbeRecord* probe = ProbeBelow(module_base, spec.function);
    if (probe != nullptr && probe->original_slot == nullptr) {
        return fail("an entry probe is on it without a slot for the original (guest_probe.h)");
    }
    if (probe == nullptr && !Engine::VerifyGuestFunction(module_base, spec.function, why)) {
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
    std::vector<u8> bytes;
    const CloneResult clone =
        CloneGuestFunction(entry, OriginalBytes(module_base, spec.function, bytes), spec.function.size, why);
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
    if (probe != nullptr) {
        // the loader's window (no guest thread yet): the probe calls the dispatcher from now on
        *probe->original_slot = const_cast<void*>(spec.entry);
        slot.below_probe = true;
        return true;
    }
    if (!Engine::InstallJump(entry, spec.entry, why)) {
        return fail(why);
    }
    return true;
}

} // namespace SecondSon::NativeEngine
