// SecondSonPC — the engine's job queue, reconstructed (see job_system.h; evidence in
// docs/native-reconstruction/JOB_SYSTEM_RECONSTRUCTION.md). Status: behaviour-equivalent reconstruction from the
// eboot's disassembly (not the original source); the field names are ours.
#include "engine/job_system.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <utility>

#include <intrin.h>
#include <windows.h>

#include "base/log.h"
#include "engine/guest_hooks.h"

namespace SecondSon::Engine::Jobs {

namespace {

Environment g_env;
Options g_options;
u64 g_tsc_per_us = 0;
u64 g_tsc_per_second = 1'000'000'000;

struct Counters {
    std::atomic<u64> worker_takes{0}, worker_sleeps{0}, worker_idle_tsc{0}, worker_busy_tsc{0};
    std::atomic<u64> try_calls{0}, try_hits{0};
    std::atomic<u64> waits{0}, wait_tsc{0}, wait_max_tsc{0}, helped{0}, helped_tsc{0}, idle_spins{0};
    std::atomic<u32> worker_threads{0};
};
Counters g_counters;

thread_local u64 t_last_exit = 0; ///< this worker's last return from NextJob (0: not a worker yet)

template <typename T>
T Load(T& v) {
    return std::atomic_ref<T>(v).load(std::memory_order_acquire);
}
template <typename T>
void Store(T& v, T value) {
    std::atomic_ref<T>(v).store(value, std::memory_order_release);
}
template <typename T>
T& At(u8* manager, size_t offset) {
    return *reinterpret_cast<T*>(manager + offset);
}

void Max(std::atomic<u64>& m, u64 v) {
    u64 cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

/// The lazily created mutex and condition variable (every guest function starts with it; not atomic in the guest).
void EnsureInit(u8* m) {
    if (At<u8>(m, Field::kInit) == 0) {
        At<u8>(m, Field::kInit) = 1;
        g_env.mutex_init(m + Field::kMutex, nullptr, nullptr);
        g_env.cond_init(m + Field::kCond, nullptr, nullptr);
    }
}

/// Removes `b` from the queue (eboot 0x243fe5..0x2440c5): iterators that would read b's links move to its
/// neighbours' (or to the queue's head/tail field), then the doubly linked list is unlinked.
void Unlink(u8* m, GuestBatch* b) {
    for (GuestIterator* it = *g_env.next_iterators; it != nullptr; it = it->next) {
        if (it->slot == &b->next) {
            it->slot = b->prev != nullptr ? static_cast<void*>(&b->prev->next) : m + Field::kHead;
        }
    }
    for (GuestIterator* it = *g_env.prev_iterators; it != nullptr; it = it->next) {
        if (it->slot == &b->prev) {
            it->slot = b->next != nullptr ? static_cast<void*>(&b->next->prev) : m + Field::kTail;
        }
    }
    GuestBatch* prev = b->prev;
    GuestBatch* next = b->next;
    if (prev != nullptr) {
        prev->next = next;
    } else {
        At<GuestBatch*>(m, Field::kHead) = next;
    }
    if (next != nullptr) {
        next->prev = prev;
    } else {
        At<GuestBatch*>(m, Field::kTail) = prev;
    }
    b->next = nullptr;
    b->prev = nullptr;
}

/// Takes the head batch's next entry; the lock is held (eboot 0x243fc8..0x2440d4).
GuestEntry* TakeLocked(u8* m, GuestBatch* head) {
    const s32 index = At<s32>(m, Field::kCursor);
    GuestEntry* e = head->Entries() + index;
    s32 next = index + 1;
    if (next >= head->count) {
        head->state = 3;
        Unlink(m, head);
        next = 0;
    }
    At<s32>(m, Field::kCursor) = next;
    Store(e->state, 1u);
    return e;
}

GuestEntry* TryTake(u8* m) {
    if (Load(At<GuestBatch*>(m, Field::kHead)) == nullptr) {
        return nullptr; // the guest also looks without the lock first
    }
    EnsureInit(m);
    g_env.mutex_lock(m + Field::kMutex);
    GuestEntry* e = nullptr;
    if (GuestBatch* head = At<GuestBatch*>(m, Field::kHead); head != nullptr) {
        e = TakeLocked(m, head);
    }
    g_env.mutex_unlock(m + Field::kMutex);
    return e;
}

void IdleStep() {
    switch (g_options.wait) {
    case WaitPolicy::Guest:
        g_env.delay_us(1);
        break;
    case WaitPolicy::Pause: {
        const u64 end = __rdtsc() + g_tsc_per_us;
        while (__rdtsc() < end) {
            _mm_pause();
        }
        break;
    }
    case WaitPolicy::Yield:
        SwitchToThread();
        break;
    }
}

void Calibrate() {
    LARGE_INTEGER f{}, q0{}, q1{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    const u64 t0 = __rdtsc();
    Sleep(20);
    QueryPerformanceCounter(&q1);
    const u64 t1 = __rdtsc();
    const double seconds = static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(f.QuadPart);
    g_tsc_per_second = std::max<u64>(static_cast<u64>(static_cast<double>(t1 - t0) / seconds), 1'000'000);
    g_tsc_per_us = std::max<u64>(g_tsc_per_second / 1'000'000, 1);
}

} // namespace

const char* WaitPolicyName(WaitPolicy p) {
    switch (p) {
    case WaitPolicy::Guest:
        return "guest";
    case WaitPolicy::Pause:
        return "pause";
    case WaitPolicy::Yield:
        return "yield";
    }
    return "?";
}

void Configure(const Environment& env, const Options& options) {
    g_env = env;
    g_options = options;
    if (g_tsc_per_us == 0) {
        Calibrate();
    }
}

u64 TscPerSecond() {
    return g_tsc_per_second;
}

Stats Snapshot() {
    const auto r = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    Stats s;
    s.worker_takes = r(g_counters.worker_takes);
    s.worker_sleeps = r(g_counters.worker_sleeps);
    s.worker_idle_tsc = r(g_counters.worker_idle_tsc);
    s.worker_busy_tsc = r(g_counters.worker_busy_tsc);
    s.try_calls = r(g_counters.try_calls);
    s.try_hits = r(g_counters.try_hits);
    s.waits = r(g_counters.waits);
    s.wait_tsc = r(g_counters.wait_tsc);
    s.wait_max_tsc = r(g_counters.wait_max_tsc);
    s.helped = r(g_counters.helped);
    s.helped_tsc = r(g_counters.helped_tsc);
    s.idle_spins = r(g_counters.idle_spins);
    s.worker_threads = g_counters.worker_threads.load(std::memory_order_relaxed);
    return s;
}

// eboot 0x243f30
SSPC_SYSV_ABI GuestEntry* NextJob() {
    const u64 t0 = __rdtsc();
    if (t_last_exit != 0) {
        g_counters.worker_busy_tsc.fetch_add(t0 - t_last_exit, std::memory_order_relaxed);
    } else {
        g_counters.worker_threads.fetch_add(1, std::memory_order_relaxed);
    }
    u8* m = g_env.manager;
    EnsureInit(m);
    g_env.mutex_lock(m + Field::kMutex);
    GuestEntry* e = nullptr;
    for (;;) {
        if (GuestBatch* head = At<GuestBatch*>(m, Field::kHead); head != nullptr) {
            e = TakeLocked(m, head);
            break;
        }
        g_counters.worker_sleeps.fetch_add(1, std::memory_order_relaxed);
        g_env.cond_wait(m + Field::kCond, m + Field::kMutex);
        if (At<u8>(m, Field::kQuit) != 0) {
            break;
        }
    }
    g_env.mutex_unlock(m + Field::kMutex);
    const u64 t1 = __rdtsc();
    g_counters.worker_idle_tsc.fetch_add(t1 - t0, std::memory_order_relaxed);
    if (e != nullptr) {
        g_counters.worker_takes.fetch_add(1, std::memory_order_relaxed);
    }
    t_last_exit = t1;
    return e;
}

// eboot 0x243d70
SSPC_SYSV_ABI GuestEntry* TryNextJob() {
    g_counters.try_calls.fetch_add(1, std::memory_order_relaxed);
    GuestEntry* e = TryTake(g_env.manager);
    if (e != nullptr) {
        g_counters.try_hits.fetch_add(1, std::memory_order_relaxed);
    }
    return e;
}

// eboot 0x243c50
SSPC_SYSV_ABI void WaitBatch(u8* manager, GuestBatch* batch) {
    const u32 state = Load(batch->state);
    if ((state | 4) == 4) {
        return; // empty, or already waited for
    }
    const u64 t0 = __rdtsc();
    u8* g = g_env.manager;
    if (At<u8>(g, Field::kQuit) == 0) {
        s32 count = Load(batch->count);
        GuestEntry* e = batch->Entries();
        while (count > 0) {
            if (Load(e->state) == 2) {
                ++e;
                if (count >= 2) {
                    --count;
                    continue;
                }
                break;
            }
            // Not done: run any queued entry meanwhile (not necessarily one of this batch), else wait a moment.
            g_counters.try_calls.fetch_add(1, std::memory_order_relaxed);
            if (GuestEntry* job = TryTake(g); job != nullptr) {
                g_counters.try_hits.fetch_add(1, std::memory_order_relaxed);
                const u64 h0 = __rdtsc();
                reinterpret_cast<JobFn>(job->fn)(job->arg);
                Store(job->state, 2u);
                g_counters.helped.fetch_add(1, std::memory_order_relaxed);
                g_counters.helped_tsc.fetch_add(__rdtsc() - h0, std::memory_order_relaxed);
            } else {
                g_counters.idle_spins.fetch_add(1, std::memory_order_relaxed);
                IdleStep();
            }
            if (At<u8>(g, Field::kQuit) != 0) {
                break;
            }
            count = Load(batch->count);
            e = batch->Entries(); // the guest rescans from the first entry
        }
    }
    EnsureInit(manager);
    g_env.mutex_lock(manager + Field::kMutex);
    batch->state = 4;
    g_env.mutex_unlock(manager + Field::kMutex);
    const u64 dt = __rdtsc() - t0;
    g_counters.waits.fetch_add(1, std::memory_order_relaxed);
    g_counters.wait_tsc.fetch_add(dt, std::memory_order_relaxed);
    Max(g_counters.wait_max_tsc, dt);
}

// --- the game -------------------------------------------------------------------------------------------------

namespace {

// inFAMOUS Second Son v1.07 (CUSA00004) eboot.bin; hashes from tools/native-reconstruction/function_hashes.py.
constexpr GuestFunction kNextJob{0x243f30, 0x1c8, 0x087f4cff892a341eull};
constexpr GuestFunction kTryNextJob{0x243d70, 0x1a0, 0x0d7dc9bfa9adab7cull};
constexpr GuestFunction kWaitBatch{0x243c50, 0x11a, 0x4bb9aed4ba8deaccull};
// Functions the reconstruction assumes the behaviour of (not replaced): the worker loop, submit, advance, kick,
// delay and the five PLT stubs the replacements call.
constexpr GuestFunction kAssumed[] = {
    {0x243680, 0xea, 0x3b0d2991d56315a4ull},  // worker: NextJob, fn(arg), state = 2
    {0x243a40, 0xba, 0xa9b05e44a93a1ebbull},  // submit: batches state 2, appended under the mutex
    {0x244100, 0x118, 0x406a1c2084560051ull}, // advance the cursor (the caller holds the mutex)
    {0x244770, 0xc8, 0xe31516aeb67a1f71ull},  // kick a group: submit + broadcast, then its dependents
    {0x53b2b0, 0xa7, 0x89bfe4b7f4c237fbull},  // delay(us)
    {0x9845b0, 0x6, 0xda306bf8b615a306ull},   // scePthreadMutexInit
    {0x984590, 0x6, 0xf1680b73f12b43d6ull},   // scePthreadCondInit
    {0x9845c0, 0x6, 0xf93e5bba9de67eceull},   // scePthreadMutexLock
    {0x984d10, 0x6, 0xd0171d5e501c24eaull},   // scePthreadCondWait
    {0x9845d0, 0x6, 0x98858c839e6fec76ull},   // scePthreadMutexUnlock
};
constexpr u64 kManager = 0x1d1bfe0;
constexpr u64 kNextIterators = 0x1d1c290;
constexpr u64 kPrevIterators = 0x1d1c298;

std::string Env(const char* name) {
    char v[64] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

void Reporter(u32 seconds) {
    Stats prev = Snapshot();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        const Stats s = Snapshot();
        const double f = static_cast<double>(TscPerSecond());
        const auto ms = [&](u64 ticks) { return static_cast<double>(ticks) * 1000.0 / f; };
        const u64 busy = s.worker_busy_tsc - prev.worker_busy_tsc;
        const u64 idle = s.worker_idle_tsc - prev.worker_idle_tsc;
        const u64 waits = s.waits - prev.waits;
        SSPC_LOG(Engine, Info,
                 "jobs/{}s: workers {} took {} (busy {:.0f}%, sleeps {}), waits {} ({:.1f} ms total, mean {:.1f} us, "
                 "max ever {:.2f} ms), waiters ran {} ({:.1f} ms), idle steps {}, try {}/{} [{}]",
                 seconds, s.worker_threads, s.worker_takes - prev.worker_takes,
                 busy + idle != 0 ? 100.0 * static_cast<double>(busy) / static_cast<double>(busy + idle) : 0.0,
                 s.worker_sleeps - prev.worker_sleeps, waits, ms(s.wait_tsc - prev.wait_tsc),
                 waits != 0 ? ms(s.wait_tsc - prev.wait_tsc) * 1000.0 / static_cast<double>(waits) : 0.0,
                 ms(s.wait_max_tsc), s.helped - prev.helped, ms(s.helped_tsc - prev.helped_tsc),
                 s.idle_spins - prev.idle_spins, s.try_hits - prev.try_hits, s.try_calls - prev.try_calls,
                 WaitPolicyName(g_options.wait));
        prev = s;
    }
}

} // namespace

bool InstallFromEnvironment(u64 eboot_base) {
    if (Env("SSPC_NATIVE_JOBS") != "1") {
        return true;
    }
    Options options;
    if (const std::string w = Env("SSPC_NATIVE_JOBS_WAIT"); w == "pause") {
        options.wait = WaitPolicy::Pause;
    } else if (w == "yield") {
        options.wait = WaitPolicy::Yield;
    } else if (!w.empty() && w != "guest") {
        SSPC_LOG(Engine, Error, "SSPC_NATIVE_JOBS_WAIT={}: want guest|pause|yield; the native job queue is not used",
                 w);
        return false;
    }
    u32 report_s = 10;
    if (const std::string r = Env("SSPC_NATIVE_JOBS_STATS"); !r.empty()) {
        report_s = static_cast<u32>(std::strtoul(r.c_str(), nullptr, 10));
    }

    std::string error;
    for (const GuestFunction& f : {kNextJob, kTryNextJob, kWaitBatch}) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "native job queue not installed: {}", error);
            return false;
        }
    }
    for (const GuestFunction& f : kAssumed) {
        if (!VerifyGuestFunction(eboot_base, f, error)) {
            SSPC_LOG(Engine, Error, "native job queue not installed: {}", error);
            return false;
        }
    }

    Environment env;
    env.manager = reinterpret_cast<u8*>(eboot_base + kManager);
    env.next_iterators = reinterpret_cast<GuestIterator**>(eboot_base + kNextIterators);
    env.prev_iterators = reinterpret_cast<GuestIterator**>(eboot_base + kPrevIterators);
    env.mutex_init = reinterpret_cast<InitFn>(eboot_base + 0x9845b0);
    env.cond_init = reinterpret_cast<InitFn>(eboot_base + 0x984590);
    env.mutex_lock = reinterpret_cast<LockFn>(eboot_base + 0x9845c0);
    env.mutex_unlock = reinterpret_cast<LockFn>(eboot_base + 0x9845d0);
    env.cond_wait = reinterpret_cast<CondWaitFn>(eboot_base + 0x984d10);
    env.delay_us = reinterpret_cast<DelayFn>(eboot_base + 0x53b2b0);
    Configure(env, options);

    const std::pair<GuestFunction, const void*> hooks[] = {
        {kNextJob, reinterpret_cast<const void*>(&NextJob)},
        {kTryNextJob, reinterpret_cast<const void*>(&TryNextJob)},
        {kWaitBatch, reinterpret_cast<const void*>(&WaitBatch)},
    };
    for (const auto& [f, target] : hooks) {
        if (!InstallJump(eboot_base + f.rva, target, error)) {
            // Verified bytes, but the host refused to write the page: nothing ran yet, an earlier hook may be in -
            // stop rather than run half-replaced.
            SSPC_LOG(Engine, Critical, "native job queue: {}", error);
            std::abort();
        }
    }
    SSPC_LOG(Engine, Warning,
             "EXPERIMENT: SSPC_NATIVE_JOBS=1 - the engine's job queue (eboot 0x243f30 NextJob, 0x243d70 TryNextJob, "
             "0x243c50 WaitBatch) runs SecondSonPC's reconstruction; wait policy {}, TSC {:.3f} GHz, stats every {} s",
             WaitPolicyName(options.wait), static_cast<double>(TscPerSecond()) / 1e9, report_s);
    if (report_s != 0) {
        std::thread(Reporter, report_s).detach();
    }
    return true;
}

} // namespace SecondSon::Engine::Jobs
