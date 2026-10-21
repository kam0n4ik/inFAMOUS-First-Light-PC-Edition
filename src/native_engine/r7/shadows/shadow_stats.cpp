// SecondSonPC — R7-S: the shadow casters loop's per-frame counts (see shadow_stats.h).
#include "native_engine/r7/shadows/shadow_stats.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <windows.h>

#include "base/log.h"
#include "engine/render/render_layout.h"

namespace SecondSon::NativeEngine::R7Shadows::Stats {

namespace {

namespace L = Engine::Render::Layout;

bool Flag(const char* name) {
    char v[8] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) && v[0] != '0';
}

enum : u32 {
    kCalls,
    kCallsInstanced,
    kSingleDraws,
    kGroups,
    kInstances,
    kGroupObjects,
    kMaterialBinds,
    kMaterialRepeats, ///< the call's first bind is the material the previous call on the context bound last
    kRecordRepeats,   ///< (item, view-projection) computed before in this frame
    kItemRepeats,     ///< item recorded before in this frame (another call: another cascade)
    kVpRepeats,       ///< loop calls whose view-projection another call of the frame used before
    kStateWritten,
    kStateSkipped,
    kTimedNative,
    kTicksNative,
    kTimedOriginal,
    kTicksOriginal,
    kCount
};
constexpr const char* kNames[kCount] = {"calls",       "instanced calls", "single draws",    "groups",
                                        "instances",   "group keys",      "material binds",  "repeat binds",
                                        "repeat records", "items again",  "vp again",        "state packets",
                                        "state skipped", "timed native",  "ticks native",    "timed original",
                                        "ticks original"};

std::atomic<u64> g_total[kCount]{};

/// The frame's sets (diagnostic: a lock per record is fine with the switch on).
struct FrameSets {
    std::mutex mu;
    u64 frame = ~0ull;
    std::unordered_set<u64> records, items, vps;
};
FrameSets& Sets() {
    static FrameSets s;
    return s;
}

std::atomic<u64> g_first_frame{0}, g_last_frame{0};
std::atomic<u64> g_next_report{0};

u64 Mix(u64 h, u64 v) {
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
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
    const u64 frames = g_last_frame.load() - g_first_frame.load();
    const double f = frames != 0 ? static_cast<double>(frames) : 1.0;
    std::string line;
    for (u32 i = 0; i < kCount; ++i) {
        const u64 v = g_total[i].load(std::memory_order_relaxed);
        line += fmt::format("{}{} {:.1f}", i == 0 ? "" : ", ", kNames[i], static_cast<double>(v) / f);
    }
    const u64 tn = g_total[kTimedNative].load(), to = g_total[kTimedOriginal].load();
    SSPC_LOG(Engine, Info, "R7-S stats: {} frames; per frame: {}; ticks/call native {:.0f} original {:.0f}", frames,
             line, tn ? static_cast<double>(g_total[kTicksNative].load()) / tn : 0.0,
             to ? static_cast<double>(g_total[kTicksOriginal].load()) / to : 0.0);
}

inline void Add(u32 c, u64 v = 1) {
    g_total[c].fetch_add(v, std::memory_order_relaxed);
}

} // namespace

struct Thread {
    u64 r = 0;
    u64 vp = 0;
    bool first_bind = true;
    std::unordered_map<u64, u64> last_material; ///< by context: the material the loop bound last
};

namespace {
thread_local Thread t_thread;
}

bool On() {
    static const bool on = Flag("SSPC_R7_SHADOWS_STATS");
    return on;
}

bool TimingOn() {
    static const bool on = Flag("SSPC_R7_SHADOWS_TIMING");
    return on;
}

Thread* Begin(u64 module, u64 r, bool instanced, const float* view_projection) {
    if (!On()) {
        return nullptr;
    }
    Thread& t = t_thread;
    t.r = r;
    t.first_bind = true;
    u64 h = 0;
    for (u32 i = 0; i < 16; ++i) {
        u32 bits;
        std::memcpy(&bits, view_projection + i, 4);
        h = Mix(h, bits);
    }
    t.vp = h;
    Add(kCalls);
    if (instanced) {
        Add(kCallsInstanced);
    }
    u64 frame = 0;
    std::memcpy(&frame, reinterpret_cast<const void*>(module + L::kFrame + L::kFrameCounter), 8);
    u64 first = g_first_frame.load(std::memory_order_relaxed);
    if (first == 0) {
        g_first_frame.compare_exchange_strong(first, frame);
    }
    g_last_frame.store(frame, std::memory_order_relaxed);
    {
        FrameSets& s = Sets();
        const std::scoped_lock lock(s.mu);
        if (s.frame != frame) {
            s.frame = frame;
            s.records.clear();
            s.items.clear();
            s.vps.clear();
        }
        if (!s.vps.insert(h).second) {
            Add(kVpRepeats);
        }
    }
    Report();
    return &t;
}

void SingleDraw(Thread&) {
    Add(kSingleDraws);
}

void Group(Thread&, u64 instances, s32 objects) {
    Add(kGroups);
    Add(kInstances, instances);
    Add(kGroupObjects, static_cast<u64>(objects));
}

void Record(Thread& t, u64 item) {
    FrameSets& s = Sets();
    const std::scoped_lock lock(s.mu);
    if (!s.records.insert(Mix(item, t.vp)).second) {
        Add(kRecordRepeats);
    }
    if (!s.items.insert(item).second) {
        Add(kItemRepeats);
    }
}

void MaterialBind(Thread& t, u64 r, u64 material) {
    Add(kMaterialBinds);
    u64& last = t.last_material[r];
    if (t.first_bind && last == material) {
        Add(kMaterialRepeats);
    }
    t.first_bind = false;
    last = material;
}

void DrawState(u32 written, u32 skipped) {
    if (!On()) {
        return;
    }
    Add(kStateWritten, written);
    Add(kStateSkipped, skipped);
}

void Time(bool native, u64 ticks) {
    Add(native ? kTimedNative : kTimedOriginal);
    Add(native ? kTicksNative : kTicksOriginal, ticks);
    if (!On()) {
        Report();
    }
}

} // namespace SecondSon::NativeEngine::R7Shadows::Stats
