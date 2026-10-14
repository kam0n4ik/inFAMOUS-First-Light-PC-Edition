// SecondSonPC — standalone tests of the reconstructed engine code (src/engine), without the game and without the
// bridge: the guest's pthread mutex/condition variable are host SRW locks/condition variables behind the same
// System V entry points the game's PLT stubs have.
//   sspc_engine_tests [--bench]   -> stdout, exit code = failures
//     --bench   the queue's throughput with each wait policy (many tiny jobs, 5 workers + a waiting thread)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <windows.h>

#include "engine/guest_hooks.h"
#include "engine/guest_probe.h"
#include "engine/job_system.h"
#include "engine/render/dcb_scan.h"
#include "engine/render/pass_graph.h"

using namespace SecondSon;
using namespace SecondSon::Engine;
using namespace SecondSon::Engine::Jobs;

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        ++g_checks;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            ++g_failures;                                                                                              \
            std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
        }                                                                                                              \
    } while (0)

// --- host stand-ins for the guest's primitives (System V, like the PLT stubs) ---------------------------------------

SSPC_SYSV_ABI s32 MutexInit(void* object, const void*, const char*) {
    auto* lock = new SRWLOCK;
    InitializeSRWLock(lock);
    *static_cast<SRWLOCK**>(object) = lock;
    return 0;
}
SSPC_SYSV_ABI s32 CondInit(void* object, const void*, const char*) {
    auto* cv = new CONDITION_VARIABLE;
    InitializeConditionVariable(cv);
    *static_cast<CONDITION_VARIABLE**>(object) = cv;
    return 0;
}
SSPC_SYSV_ABI s32 MutexLock(void* object) {
    AcquireSRWLockExclusive(*static_cast<SRWLOCK**>(object));
    return 0;
}
SSPC_SYSV_ABI s32 MutexUnlock(void* object) {
    ReleaseSRWLockExclusive(*static_cast<SRWLOCK**>(object));
    return 0;
}
SSPC_SYSV_ABI s32 CondWait(void* cond, void* mutex) {
    SleepConditionVariableSRW(*static_cast<CONDITION_VARIABLE**>(cond), *static_cast<SRWLOCK**>(mutex), INFINITE, 0);
    return 0;
}
SSPC_SYSV_ABI void Delay(u32 us) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() < end) {
    }
}
SSPC_SYSV_ABI void CountJob(u64 arg) {
    reinterpret_cast<std::atomic<u32>*>(arg)->fetch_add(1, std::memory_order_relaxed);
}

/// A JobManager and its iterator lists in host memory.
struct Fixture {
    alignas(16) u8 manager[Field::kSize] = {};
    GuestIterator* next_iterators = nullptr;
    GuestIterator* prev_iterators = nullptr;

    explicit Fixture(WaitPolicy policy = WaitPolicy::Guest) {
        Environment env;
        env.manager = manager;
        env.next_iterators = &next_iterators;
        env.prev_iterators = &prev_iterators;
        env.mutex_init = &MutexInit;
        env.cond_init = &CondInit;
        env.mutex_lock = &MutexLock;
        env.mutex_unlock = &MutexUnlock;
        env.cond_wait = &CondWait;
        env.delay_us = &Delay;
        Options o;
        o.wait = policy;
        Configure(env, o);
    }
    template <typename T>
    T& F(size_t off) {
        return *reinterpret_cast<T*>(manager + off);
    }
    bool Quit() {
        return std::atomic_ref<u8>(F<u8>(Field::kQuit)).load(std::memory_order_acquire) != 0;
    }
    void Lock() {
        if (F<u8>(Field::kInit) == 0) {
            F<u8>(Field::kInit) = 1;
            MutexInit(manager + Field::kMutex, nullptr, nullptr);
            CondInit(manager + Field::kCond, nullptr, nullptr);
        }
        MutexLock(manager + Field::kMutex);
    }
    void Unlock() {
        MutexUnlock(manager + Field::kMutex);
    }
    void Broadcast() {
        WakeAllConditionVariable(*reinterpret_cast<CONDITION_VARIABLE**>(manager + Field::kCond));
    }
    /// What the guest's submit (eboot 0x243a40) and kick (0x244770) do: state 2, append, broadcast.
    void Submit(GuestBatch* b) {
        b->state = 2;
        Lock();
        b->next = nullptr;
        b->prev = F<GuestBatch*>(Field::kTail);
        if (b->prev != nullptr) {
            b->prev->next = b;
        } else {
            F<GuestBatch*>(Field::kHead) = b;
        }
        F<GuestBatch*>(Field::kTail) = b;
        Broadcast();
        Unlock();
    }
};

/// A batch with `n` entries running CountJob on per-entry counters.
struct Batch {
    std::vector<u8> storage;
    std::vector<std::atomic<u32>> runs;
    explicit Batch(int n) : storage(sizeof(GuestBatch) + n * sizeof(GuestEntry)), runs(n) {
        auto* b = Get();
        b->state = 0;
        b->count = n;
        for (int i = 0; i < n; ++i) {
            GuestEntry& e = b->Entries()[i];
            e = {};
            e.fn = reinterpret_cast<u64>(&CountJob);
            e.arg = reinterpret_cast<u64>(&runs[i]);
        }
    }
    GuestBatch* Get() {
        return reinterpret_cast<GuestBatch*>(storage.data());
    }
};

void TestHash() {
    std::printf("hash\n");
    CHECK(HashBytes("", 0) == 0xcbf29ce484222325ull);
    CHECK(HashBytes("a", 1) == 0xaf63dc4c8601ec8cull);
    CHECK(HashBytes("foobar", 6) == 0x85944171f73967e8ull);
}

void TestOrder() {
    std::printf("order: entries in submission order, batches unlinked when every entry is taken\n");
    Fixture fx;
    Batch a(3), b(1);
    fx.Submit(a.Get());
    fx.Submit(b.Get());
    GuestEntry* e[5] = {};
    for (auto& x : e) {
        x = TryNextJob();
    }
    CHECK(e[0] == &a.Get()->Entries()[0] && e[1] == &a.Get()->Entries()[1] && e[2] == &a.Get()->Entries()[2]);
    CHECK(e[3] == &b.Get()->Entries()[0]);
    CHECK(e[4] == nullptr);
    CHECK(a.Get()->state == 3 && b.Get()->state == 3);
    CHECK(e[0]->state == 1 && e[3]->state == 1);
    CHECK(fx.F<GuestBatch*>(Field::kHead) == nullptr && fx.F<GuestBatch*>(Field::kTail) == nullptr);
    CHECK(fx.F<s32>(Field::kCursor) == 0);
    CHECK(a.Get()->next == nullptr && a.Get()->prev == nullptr);
}

void TestIterators() {
    std::printf("iterators: unlinking the head moves the iterators that point into it\n");
    Fixture fx;
    Batch a(1), b(2);
    fx.Submit(a.Get());
    fx.Submit(b.Get());
    GuestIterator to_a_next{&a.Get()->next, nullptr};
    GuestIterator other{&b.Get()->next, &to_a_next};
    GuestIterator to_a_prev{&a.Get()->prev, nullptr};
    fx.next_iterators = &other;
    fx.prev_iterators = &to_a_prev;
    CHECK(TryNextJob() == &a.Get()->Entries()[0]);
    CHECK(to_a_next.slot == fx.manager + Field::kHead); // a had no previous batch
    CHECK(to_a_prev.slot == &b.Get()->prev);            // a's next is b
    CHECK(other.slot == &b.Get()->next);                // untouched
    CHECK(fx.F<GuestBatch*>(Field::kHead) == b.Get() && b.Get()->prev == nullptr);
    // b is the last batch: a prev-iterator on it goes to the tail field.
    GuestIterator to_b_prev{&b.Get()->prev, nullptr};
    fx.prev_iterators = &to_b_prev;
    fx.next_iterators = nullptr;
    CHECK(TryNextJob() == &b.Get()->Entries()[0]);
    CHECK(TryNextJob() == &b.Get()->Entries()[1]);
    CHECK(to_b_prev.slot == fx.manager + Field::kTail);
}

void TestWaitHelps() {
    std::printf("wait: no workers - the waiting thread runs every entry itself\n");
    Fixture fx;
    Batch a(4);
    fx.Submit(a.Get());
    const Stats before = Snapshot();
    WaitBatch(fx.manager, a.Get());
    const Stats after = Snapshot();
    for (auto& r : a.runs) {
        CHECK(r.load() == 1);
    }
    for (int i = 0; i < 4; ++i) {
        CHECK(a.Get()->Entries()[i].state == 2);
    }
    CHECK(a.Get()->state == 4);
    CHECK(after.helped - before.helped == 4);
    CHECK(after.waits - before.waits == 1);
    // Waiting again, or for an empty batch: returns at once.
    WaitBatch(fx.manager, a.Get());
    Batch empty(0);
    WaitBatch(fx.manager, empty.Get());
    CHECK(Snapshot().waits == after.waits);
}

void TestQuit() {
    std::printf("quit: a sleeping worker leaves NextJob with nullptr\n");
    Fixture fx;
    std::atomic<bool> got_null{false};
    std::thread w([&] { got_null = NextJob() == nullptr; });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    fx.Lock();
    fx.F<u8>(Field::kQuit) = 1;
    fx.Broadcast();
    fx.Unlock();
    w.join();
    CHECK(got_null.load());
}

/// 5 workers (like JobWorker1..5) + this thread submitting and waiting: every entry runs exactly once.
double Stress(WaitPolicy policy, int batches, int max_entries, bool quiet) {
    Fixture fx(policy);
    std::vector<std::thread> workers;
    std::atomic<int> stopped{0};
    for (int i = 0; i < 5; ++i) {
        workers.emplace_back([&] {
            while (!fx.Quit()) { // the guest worker loop (eboot 0x243680)
                if (GuestEntry* e = NextJob(); e != nullptr) {
                    reinterpret_cast<JobFn>(e->fn)(e->arg);
                    std::atomic_ref<u32>(e->state).store(2, std::memory_order_release);
                }
            }
            stopped.fetch_add(1);
        });
    }
    std::mt19937 rng(12345);
    std::vector<std::unique_ptr<Batch>> all;
    all.reserve(batches);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < batches; ++i) {
        all.push_back(std::make_unique<Batch>(1 + static_cast<int>(rng() % max_entries)));
        fx.Submit(all.back()->Get());
        if (i % 3 == 2) { // waits lag behind submissions, as frames do
            WaitBatch(fx.manager, all[i - 2]->Get());
            WaitBatch(fx.manager, all[i - 1]->Get());
            WaitBatch(fx.manager, all[i]->Get());
        }
    }
    for (auto& b : all) {
        WaitBatch(fx.manager, b->Get());
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    // Shutdown like eboot 0x243770: set quit, broadcast until every worker is out.
    fx.Lock();
    std::atomic_ref<u8>(fx.F<u8>(Field::kQuit)).store(1, std::memory_order_release);
    fx.Unlock();
    while (stopped.load() != 5) {
        fx.Lock();
        fx.Broadcast();
        fx.Unlock();
        Sleep(1);
    }
    for (auto& w : workers) {
        w.join();
    }
    u64 bad = 0, entries = 0;
    for (auto& b : all) {
        bad += b->Get()->state != 4;
        for (auto& r : b->runs) {
            bad += r.load() != 1;
            ++entries;
        }
    }
    if (!quiet) {
        std::printf("  %s: %d batches, %llu entries in %.3f s, wrong %llu\n", WaitPolicyName(policy), batches,
                    static_cast<unsigned long long>(entries), seconds, static_cast<unsigned long long>(bad));
    }
    CHECK(bad == 0);
    CHECK(fx.F<GuestBatch*>(Field::kHead) == nullptr);
    return seconds;
}

void TestStress() {
    std::printf("stress: 5 workers + a waiting thread, every entry exactly once\n");
    for (WaitPolicy p : {WaitPolicy::Guest, WaitPolicy::Pause, WaitPolicy::Yield}) {
        Stress(p, 20000, 16, false);
    }
}

void Bench() {
    std::printf("bench: 100000 batches of 1..8 tiny entries, 5 runs each\n");
    for (WaitPolicy p : {WaitPolicy::Guest, WaitPolicy::Pause, WaitPolicy::Yield}) {
        std::vector<double> t;
        for (int i = 0; i < 5; ++i) {
            t.push_back(Stress(p, 100000, 8, true));
        }
        std::sort(t.begin(), t.end());
        std::printf("  %-5s median %.3f s (min %.3f, max %.3f)\n", WaitPolicyName(p), t[2], t[0], t[4]);
    }
}

// --- P5.3A: entry probe, command buffer walk, pass graph -------------------------------------------------------------

using AddFn = SSPC_SYSV_ABI s32 (*)(s32, s32);
AddFn g_add_original = nullptr;
int g_add_probe_calls = 0;
SSPC_SYSV_ABI s32 AddProbe(s32 a, s32 b) {
    ++g_add_probe_calls;
    return g_add_original(a, b) + 1000;
}

void TestEntryProbe() {
    // mov eax, edi; add eax, esi; 10 x nop; ret - a System V add(a, b) whose 14-byte prologue moves to the trampoline.
    u8 code[15] = {0x89, 0xF8, 0x01, 0xF0};
    std::memset(code + 4, 0x90, 10);
    code[14] = 0xC3;
    auto* page = static_cast<u8*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    CHECK(page != nullptr);
    std::memcpy(page, code, sizeof(code));
    const u64 base = reinterpret_cast<u64>(page);
    CHECK(reinterpret_cast<AddFn>(page)(2, 3) == 5);

    u8 t[TrampolineSize(14)] = {};
    CHECK(BuildTrampoline(code, 14, 0x1122334455667788ull, t) == 28);
    CHECK(std::memcmp(t, code, 14) == 0 && t[14] == 0xFF && t[15] == 0x25 && t[16] == 0 && t[19] == 0);
    u64 resume = 0;
    std::memcpy(&resume, t + 20, 8);
    CHECK(resume == 0x1122334455667788ull);

    std::string error;
    const ProbeSite wrong{{0, sizeof(code), HashBytes(code, sizeof(code)) ^ 1}, 14};
    CHECK(InstallEntryProbe(base, wrong, reinterpret_cast<const void*>(&AddProbe), error) == nullptr);
    CHECK(!error.empty());
    CHECK(reinterpret_cast<AddFn>(page)(2, 3) == 5); // untouched
    const ProbeSite short_prologue{{0, sizeof(code), HashBytes(code, sizeof(code))}, 13};
    CHECK(InstallEntryProbe(base, short_prologue, reinterpret_cast<const void*>(&AddProbe), error) == nullptr);

    const ProbeSite site{{0, sizeof(code), HashBytes(code, sizeof(code))}, 14};
    void* original = InstallEntryProbe(base, site, reinterpret_cast<const void*>(&AddProbe), error);
    CHECK(original != nullptr);
    g_add_original = reinterpret_cast<AddFn>(original);
    CHECK(g_add_original(20, 22) == 42);                         // the trampoline is the original function
    CHECK(reinterpret_cast<AddFn>(page)(2, 3) == 1005);          // callers of the entry reach the probe
    CHECK(reinterpret_cast<AddFn>(page)(-7, 7) == 1000 && g_add_probe_calls == 2);
}

struct Pm4Writer {
    std::vector<u32> dw;
    void Packet(u8 op, std::initializer_list<u32> body) {
        dw.push_back(0xC0000000u | (static_cast<u32>(body.size() - 1) << 16) | (u32{op} << 8));
        dw.insert(dw.end(), body);
    }
    void Context(u32 reg, u32 value) {
        Packet(0x69, {reg, value});
    }
};

void TestDcbScan() {
    using namespace Render;
    Pm4Writer w;
    // Colour target 0: 1920x1080 at 0x1234500, format 0xA, number type 4.
    w.Packet(0x69, {Reg::CbColor0Base, 0x12345, 1920 / 8 - 1, 1920 * 1080 / 64 - 1, 0, (0xAu << 2) | (4u << 8)});
    w.Context(Reg::CbTargetMask, 0xF);
    w.Context(Reg::CbColorControl, 1u << 4);
    // Depth 1920x1080 at 0x20000, test + write.
    w.Context(Reg::DbZInfo, 3);
    w.Context(Reg::DbZReadBase, 0x200);
    w.Context(Reg::DbZWriteBase, 0x200);
    w.Context(Reg::DbDepthSize, (1920 / 8 - 1) | ((1080 / 8 - 1) << 11));
    w.Context(Reg::DbDepthControl, 0x6);
    w.Packet(0x76, {0x008, 0x100000, 0}); // SPI_SHADER_PGM_LO/HI_PS: program at 0x10000000
    w.Packet(0x2F, {4});                  // NUM_INSTANCES
    w.Packet(0x2D, {36, 2});              // DRAW_INDEX_AUTO 36 vertices
    w.Packet(0x10, {0, 0, 0});            // NOP
    w.Packet(0x2F, {1});
    w.Context(Reg::CbTargetMask, 0);      // depth-only, test without write
    w.Context(Reg::DbDepthControl, 0x2);
    w.Packet(0x27, {100, 0, 0, 30, 0});   // DRAW_INDEX_2: 30 indices
    w.Packet(0x76, {0x20C, 0x200000, 0}); // COMPUTE_PGM_LO/HI: 0x20000000
    w.Packet(0x15, {8, 8, 1, 1});         // DISPATCH_DIRECT
    w.Packet(0x58, {0, 0, 0, 0, 0, 0});   // ACQUIRE_MEM
    w.dw.push_back(0xC0050000u | (0x10u << 8)); // NOP claiming 6 body dwords, truncated

    ScanState state;
    ScanResult r;
    ScanCommands(w.dw, state, r);
    CHECK(r.draws == 2 && r.draws_instanced == 1 && r.indices == 36 * 4 + 30);
    CHECK(r.dispatches == 1 && r.cache_actions == 1 && r.malformed == 1);
    CHECK(r.draws_depth_test == 2 && r.draws_depth_write == 1 && r.draws_no_color == 1);
    const TargetDesc color{0x1234500, 1920, 1080, 0xA | (4u << 8), false};
    const TargetDesc depth{0x20000, 1920, 1080, 3, true};
    CHECK(r.targets.size() == 2 && r.targets[0] == color && r.targets[1] == depth);
    CHECK(r.depth_read.size() == 1 && r.depth_read[0] == depth);
    CHECK(r.shaders.size() == 2 && r.shaders[0] == 0x10000000 && r.shaders[1] == 0x20000000);
    CHECK(state.num_instances == 1 && state.context[Reg::CbTargetMask] == 0);
}

void TestPassGraph() {
    using namespace Render;
    const u32 tsharp[8] = {0x12345, 0x01u | (0xAu << 20) | (7u << 26), 1919u | (1079u << 14), 0, 0, 0, 0, 0};
    const ResourceUse tex = DecodeTexture(tsharp, ResourceKind::Texture);
    CHECK(tex.base == 0x10001234500ull && tex.width == 1920 && tex.height == 1080 && tex.format == (0xA | (7u << 8)));
    const u32 vsharp[4] = {0x5000, 0x2u | (16u << 16), 64, 0};
    const ResourceUse buf = DecodeBuffer(vsharp, ResourceKind::RwBuffer);
    CHECK(buf.base == 0x200005000ull && buf.width == 16 && buf.height == 64);

    std::vector<ContextRecord> ctx(5);
    ctx[0].kind = NodeKind::Pre;
    ctx[0].scan.targets.push_back({0xA000, 64, 64, 1, false}); // pre writes A
    ctx[1].kind = NodeKind::Pass;
    ctx[1].pass = 1;
    ctx[1].uses.push_back({0xA000, 64, 64, 1, ResourceKind::Texture}); // pass 1 reads A ...
    ctx[2].kind = NodeKind::Pass;
    ctx[2].pass = 1;
    ctx[2].sub = 1;
    ctx[2].uses.push_back({0xB000, 4, 16, 0, ResourceKind::RwBuffer}); // ... and writes B (second context)
    ctx[3].kind = NodeKind::Pass;
    ctx[3].pass = 2;
    ctx[3].uses.push_back({0xB000, 4, 16, 0, ResourceKind::Buffer});  // pass 2 reads B
    ctx[3].uses.push_back({0xC000, 8, 8, 1, ResourceKind::Texture});  // and C, written later (previous frame's)
    ctx[3].uses.push_back({0xD000, 8, 8, 1, ResourceKind::Texture});  // and D, never written (static)
    ctx[4].kind = NodeKind::Post;
    ctx[4].scan.targets.push_back({0xC000, 8, 8, 1, false});

    const Graph g = BuildGraph(ctx);
    CHECK(g.nodes.size() == 4 && g.nodes[1].contexts == 2 && NodeName(g.nodes[1]) == "pass 1");
    CHECK(NodeName(g.nodes[0]) == "pre" && NodeName(g.nodes[3]) == "post");
    CHECK(g.edges.size() == 3);
    const auto has = [&](u32 from, u32 to, bool prev) {
        return std::any_of(g.edges.begin(), g.edges.end(),
                           [&](const Edge& e) { return e.from == from && e.to == to && e.prev_frame == prev; });
    };
    CHECK(has(0, 1, false) && has(1, 2, false) && has(3, 2, true));
    CHECK(g.lifetimes.size() == 3);
    const auto life = std::find_if(g.lifetimes.begin(), g.lifetimes.end(), [](const Lifetime& l) { return l.base == 0xA000; });
    CHECK(life != g.lifetimes.end() && life->target && life->first_write == 0 && life->last_read == 1 &&
          life->readers == 1);
    const auto b = std::find_if(g.lifetimes.begin(), g.lifetimes.end(), [](const Lifetime& l) { return l.base == 0xB000; });
    CHECK(b != g.lifetimes.end() && !b->target && b->first_write == 1 && b->first_read == 2 && b->readers == 1);
}

} // namespace

int main(int argc, char** argv) {
    const bool bench = argc > 1 && std::strcmp(argv[1], "--bench") == 0;
    TestHash();
    TestOrder();
    TestIterators();
    TestWaitHelps();
    TestQuit();
    TestStress();
    TestEntryProbe();
    TestDcbScan();
    TestPassGraph();
    if (bench) {
        Bench();
    }
    std::printf("TOTAL %d checks, %d failures - %s\n", g_checks, g_failures, g_failures == 0 ? "PASS" : "FAIL");
    return g_failures;
}
