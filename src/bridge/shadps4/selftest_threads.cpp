// SecondSonPC — self-test of the native guest thread manager (P2.4). See selftest_threads.h.
// Every check calls the manager through the exports the game binds (System V), from "guest" test
// functions running on manager threads, and compares with the oracle's documented results
// (third_party/shadPS4/src/core/libraries/kernel/threads/*.cpp) or with exact counts.
#include "bridge/shadps4/selftest_threads.h"

#include <windows.h>

#include <psapi.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"
#include "bridge/shadps4/guest_stack.h"
#include "core/guest_memory.h"
#include "runtime/exports.h"
#include "runtime/kernel/guest_errno.h"
#include "runtime/kernel/kernel.h"
#include "runtime/kernel/thread.h"

namespace SecondSon::Bridge {

namespace {

namespace Threads = Kernel::Threads;

constexpr s32 kEsrch = static_cast<s32>(0x80020003);
constexpr s32 kEdeadlk = static_cast<s32>(0x8002000b);
constexpr s32 kEinval = static_cast<s32>(0x80020016);
constexpr s32 kEagain = static_cast<s32>(0x80020023);
constexpr s32 kEnotsup = static_cast<s32>(0x8002002d);
constexpr s32 kEtimedout = static_cast<s32>(0x8002003c);

using Handle = void*;
using Entry = void*(SSPC_SYSV_ABI*)(void*);
using KeyDestructor = void(SSPC_SYSV_ABI*)(void*);
using OnceRoutine = void(SSPC_SYSV_ABI*)();

/// The manager's exports, as the game calls them.
struct Api {
    s32(SSPC_SYSV_ABI* create)(Handle*, void**, Entry, void*, const char*);
    s32(SSPC_SYSV_ABI* posix_create)(Handle*, void**, Entry, void*, const char*);
    s32(SSPC_SYSV_ABI* join)(Handle, void**);
    s32(SSPC_SYSV_ABI* posix_join)(Handle, void**);
    s32(SSPC_SYSV_ABI* detach)(Handle);
    void(SSPC_SYSV_ABI* exit)(void*);
    Handle(SSPC_SYSV_ABI* self)();
    Handle(SSPC_SYSV_ABI* posix_self)();
    s32(SSPC_SYSV_ABI* once)(void*, OnceRoutine);
    s32(SSPC_SYSV_ABI* getprio)(Handle, s32*);
    s32(SSPC_SYSV_ABI* setprio)(Handle, s32);
    s32(SSPC_SYSV_ABI* getaffinity)(Handle, u64*);
    s32(SSPC_SYSV_ABI* setaffinity)(Handle, u64);
    s32(SSPC_SYSV_ABI* attr_init)(void**);
    s32(SSPC_SYSV_ABI* posix_attr_init)(void**);
    s32(SSPC_SYSV_ABI* attr_destroy)(void**);
    s32(SSPC_SYSV_ABI* attr_setdetachstate)(void**, s32);
    s32(SSPC_SYSV_ABI* posix_attr_setdetachstate)(void**, s32);
    s32(SSPC_SYSV_ABI* attr_setinheritsched)(void**, s32);
    s32(SSPC_SYSV_ABI* attr_setschedparam)(void**, const s32*);
    s32(SSPC_SYSV_ABI* attr_getschedparam)(void**, s32*);
    s32(SSPC_SYSV_ABI* attr_setschedpolicy)(void**, s32);
    s32(SSPC_SYSV_ABI* attr_setstacksize)(void**, u64);
    s32(SSPC_SYSV_ABI* attr_setaffinity)(void**, u64);
    s32(SSPC_SYSV_ABI* attr_getaffinity)(void**, u64*);
    s32(SSPC_SYSV_ABI* attr_get)(Handle, void**);
    s32(SSPC_SYSV_ABI* key_create)(u32*, KeyDestructor);
    s32(SSPC_SYSV_ABI* key_delete)(u32);
    void*(SSPC_SYSV_ABI* getspecific)(u32);
    s32(SSPC_SYSV_ABI* setspecific)(u32, const void*);
    // Native mutexes and condition variables ("kernel-mutex") on manager threads.
    s32(SSPC_SYSV_ABI* mutex_init)(void**, void* const*, const char*);
    s32(SSPC_SYSV_ABI* mutex_destroy)(void**);
    s32(SSPC_SYSV_ABI* mutex_lock)(void**);
    s32(SSPC_SYSV_ABI* mutex_unlock)(void**);
    s32(SSPC_SYSV_ABI* cond_init)(void**, void* const*, const char*);
    s32(SSPC_SYSV_ABI* cond_destroy)(void**);
    s32(SSPC_SYSV_ABI* cond_wait)(void**, void**);
    s32(SSPC_SYSV_ABI* cond_timedwait)(void**, void**, u32);
    s32(SSPC_SYSV_ABI* cond_signal)(void**);
    s32(SSPC_SYSV_ABI* cond_broadcast)(void**);
};
Api g;

template <typename F>
bool Find(std::span<const Runtime::Export> exports, std::string_view name, std::string_view library, F& out) {
    for (const auto& e : exports) {
        if (name == e.name && library == e.library) {
            out = reinterpret_cast<F>(e.function);
            return true;
        }
    }
    return false;
}

bool LoadApi() {
    const auto t = Threads::ThreadExports();
    const auto m = Kernel::MutexExports();
    const auto c = Kernel::CondvarExports();
    return Find(t, "scePthreadCreate", "libkernel", g.create) &&
           Find(t, "pthread_create_name_np", "libScePosix", g.posix_create) &&
           Find(t, "scePthreadJoin", "libkernel", g.join) && Find(t, "pthread_join", "libScePosix", g.posix_join) &&
           Find(t, "scePthreadDetach", "libkernel", g.detach) && Find(t, "scePthreadExit", "libkernel", g.exit) &&
           Find(t, "scePthreadSelf", "libkernel", g.self) && Find(t, "pthread_self", "libScePosix", g.posix_self) &&
           Find(t, "scePthreadOnce", "libkernel", g.once) && Find(t, "scePthreadGetprio", "libkernel", g.getprio) &&
           Find(t, "scePthreadSetprio", "libkernel", g.setprio) &&
           Find(t, "scePthreadGetaffinity", "libkernel", g.getaffinity) &&
           Find(t, "scePthreadSetaffinity", "libkernel", g.setaffinity) &&
           Find(t, "scePthreadAttrInit", "libkernel", g.attr_init) &&
           Find(t, "pthread_attr_init", "libScePosix", g.posix_attr_init) &&
           Find(t, "scePthreadAttrDestroy", "libkernel", g.attr_destroy) &&
           Find(t, "scePthreadAttrSetdetachstate", "libkernel", g.attr_setdetachstate) &&
           Find(t, "pthread_attr_setdetachstate", "libScePosix", g.posix_attr_setdetachstate) &&
           Find(t, "scePthreadAttrSetinheritsched", "libkernel", g.attr_setinheritsched) &&
           Find(t, "scePthreadAttrSetschedparam", "libkernel", g.attr_setschedparam) &&
           Find(t, "scePthreadAttrGetschedparam", "libkernel", g.attr_getschedparam) &&
           Find(t, "scePthreadAttrSetschedpolicy", "libkernel", g.attr_setschedpolicy) &&
           Find(t, "scePthreadAttrSetstacksize", "libkernel", g.attr_setstacksize) &&
           Find(t, "scePthreadAttrSetaffinity", "libkernel", g.attr_setaffinity) &&
           Find(t, "scePthreadAttrGetaffinity", "libkernel", g.attr_getaffinity) &&
           Find(t, "scePthreadAttrGet", "libkernel", g.attr_get) &&
           Find(t, "scePthreadKeyCreate", "libkernel", g.key_create) &&
           Find(t, "scePthreadKeyDelete", "libkernel", g.key_delete) &&
           Find(t, "scePthreadGetspecific", "libkernel", g.getspecific) &&
           Find(t, "scePthreadSetspecific", "libkernel", g.setspecific) &&
           Find(m, "scePthreadMutexInit", "libkernel", g.mutex_init) &&
           Find(m, "scePthreadMutexDestroy", "libkernel", g.mutex_destroy) &&
           Find(m, "scePthreadMutexLock", "libkernel", g.mutex_lock) &&
           Find(m, "scePthreadMutexUnlock", "libkernel", g.mutex_unlock) &&
           Find(c, "scePthreadCondInit", "libkernel", g.cond_init) &&
           Find(c, "scePthreadCondDestroy", "libkernel", g.cond_destroy) &&
           Find(c, "scePthreadCondWait", "libkernel", g.cond_wait) &&
           Find(c, "scePthreadCondTimedwait", "libkernel", g.cond_timedwait) &&
           Find(c, "scePthreadCondSignal", "libkernel", g.cond_signal) &&
           Find(c, "scePthreadCondBroadcast", "libkernel", g.cond_broadcast);
}

thread_local void* t_tcb = nullptr; // what SetTcbBase gave this thread

/// Guest stacks from the selected guest memory manager (the bridge's or SecondSonPC's) through the
/// game's own code (guest_stack.cpp), TCBs of the test (self pointer and thread at the libkernel
/// offsets 0x00/0x10). Failures can be injected.
class TestHost final : public Threads::ThreadHost {
public:
    void* MapStack(u64 size, u64 guard) override {
        if (fail_map.exchange(false)) {
            return nullptr;
        }
        void* stack = MapGuestStack(0x7E0000000ULL, size, guard);
        if (stack == nullptr) {
            return nullptr;
        }
        ++stacks;
        last_stack = stack;
        return stack;
    }
    bool UnmapStack(void* stack, u64 size, u64 guard) override {
        if (fail_unmap.exchange(false)) {
            ++leaked; // stays mapped; the test unmaps it at the end
            std::scoped_lock lk{leak_lock};
            leaked_stack = {stack, size, guard};
            return false;
        }
        if (!UnmapGuestStack(stack, size, guard)) {
            return false;
        }
        --stacks;
        return true;
    }
    void* CreateTcb(void* thread, bool) override {
        if (fail_tcb.exchange(false)) {
            return nullptr;
        }
        auto* tcb = static_cast<u64*>(_aligned_malloc(0x40, 0x40));
        std::memset(tcb, 0, 0x40);
        tcb[0] = reinterpret_cast<u64>(tcb);
        tcb[2] = reinterpret_cast<u64>(thread);
        ++tcbs;
        return tcb;
    }
    void DestroyTcb(void* tcb) override {
        _aligned_free(tcb);
        --tcbs;
    }
    void SetTcbBase(void* tcb) override {
        t_tcb = tcb;
    }
    bool IsReadable(const void* p, u64) override {
        return p != nullptr;
    }
    void ThreadStarted() override {
        ++started;
    }
    void ThreadExiting() override {
        ++exiting;
    }

    struct Leaked {
        void* stack = nullptr;
        u64 size = 0;
        u64 guard = 0;
    };

    std::atomic<s64> stacks{0}, tcbs{0}, leaked{0}; // stacks: mapped and not unmapped (leaked ones too)
    std::atomic<u64> started{0}, exiting{0};
    std::atomic<bool> fail_map{false}, fail_tcb{false}, fail_unmap{false};
    std::atomic<void*> last_stack{nullptr};
    std::mutex leak_lock;
    Leaked leaked_stack;
};

TestHost& Host() {
    static TestHost host; // lives as long as any manager thread may call it
    return host;
}

// ------------------------------------------------------------------------- guest test functions

SSPC_SYSV_ABI void* ReturnArgPlusOne(void* arg) {
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(arg) + 1);
}

struct Probe {
    Handle self = nullptr;
    Handle posix_self = nullptr;
    u64 tcb_thread = 0;
    u32 mxcsr = 0;
    std::wstring name;
    s32 prio = 0;
    s32 prio_ret = -1;
    u64 affinity = 0;
    bool stack_inside = false;
};

SSPC_SYSV_ABI void* ProbeEntry(void* arg) {
    auto* p = static_cast<Probe*>(arg);
    p->self = g.self();
    p->posix_self = g.posix_self();
    p->tcb_thread = t_tcb != nullptr ? static_cast<u64*>(t_tcb)[2] : 0;
    p->mxcsr = _mm_getcsr();
    PWSTR desc = nullptr;
    if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &desc)) && desc != nullptr) {
        p->name = desc;
        LocalFree(desc);
    }
    p->prio_ret = g.getprio(p->self, &p->prio);
    g.getaffinity(p->self, &p->affinity);
    const auto sp = reinterpret_cast<uintptr_t>(&desc);
    const auto base = reinterpret_cast<uintptr_t>(Host().last_stack.load());
    p->stack_inside = base != 0 && sp > base && sp < base + 0x200000;
    return reinterpret_cast<void*>(0x5e1f);
}

[[noreturn]] void ExitDeep(int depth) {
    if (depth == 0) {
        g.exit(reinterpret_cast<void*>(0xe417));
        __builtin_unreachable();
    }
    volatile char frame[64];
    frame[0] = static_cast<char>(depth);
    ExitDeep(depth - 1);
}

SSPC_SYSV_ABI void* ExitFromNestedFrames(void*) {
    ExitDeep(12);
}

/// Blocks until its event is set (a Windows event: no manager code involved in the wait).
SSPC_SYSV_ABI void* WaitEvent(void* event) {
    WaitForSingleObject(static_cast<HANDLE>(event), INFINITE);
    return reinterpret_cast<void*>(7);
}

SSPC_SYSV_ABI void* JoinSelf(void*) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(g.join(g.self(), nullptr)));
}

// Concurrent creation: each creator makes and joins `kChildren` threads.
constexpr int kCreators = 16;
constexpr int kChildren = 50;
SSPC_SYSV_ABI void* Creator(void* arg) {
    const auto base = reinterpret_cast<uintptr_t>(arg);
    uintptr_t ok = 0;
    for (int i = 0; i < kChildren; ++i) {
        Handle h = nullptr;
        if (g.create(&h, nullptr, &ReturnArgPlusOne, reinterpret_cast<void*>(base + i), "child") != 0) {
            continue;
        }
        void* ret = nullptr;
        if (g.join(h, &ret) == 0 && reinterpret_cast<uintptr_t>(ret) == base + i + 1) {
            ++ok;
        }
    }
    return reinterpret_cast<void*>(ok);
}

// TLS keys.
std::atomic<u64> g_dtor_calls{0};
std::atomic<u64> g_dtor_sum{0};
SSPC_SYSV_ABI void CountingDestructor(void* value) {
    ++g_dtor_calls;
    g_dtor_sum += reinterpret_cast<uintptr_t>(value);
}
u32 g_key = 0;
u32 g_key2 = 0;
SSPC_SYSV_ABI void* TlsWorker(void* arg) {
    const auto mine = reinterpret_cast<uintptr_t>(arg);
    if (g.setspecific(g_key, reinterpret_cast<void*>(mine)) != 0) {
        return nullptr;
    }
    Sleep(5); // the other workers set theirs meanwhile
    return g.getspecific(g_key) == reinterpret_cast<void*>(mine) ? reinterpret_cast<void*>(1) : nullptr;
}
HANDLE g_tls_event = nullptr;
SSPC_SYSV_ABI void* TlsDeletedKey(void*) {
    g.setspecific(g_key2, reinterpret_cast<void*>(1000));
    WaitForSingleObject(g_tls_event, INFINITE); // the key is deleted meanwhile
    // The value of a deleted key is gone and a new one cannot be set (EINVAL).
    const bool gone = g.getspecific(g_key2) == nullptr;
    const s32 set = g.setspecific(g_key2, reinterpret_cast<void*>(1));
    return reinterpret_cast<void*>(static_cast<uintptr_t>(gone && set == kEinval));
}

// Once.
std::atomic<u32> g_once_runs{0};
SSPC_SYSV_ABI void OnceRoutineSlow() {
    ++g_once_runs;
    Sleep(20);
}
struct OnceControl {
    u32 state;
    u32 pad;
    void* mutex;
};
OnceControl g_once{};
SSPC_SYSV_ABI void* OnceCaller(void*) {
    return reinterpret_cast<void*>(static_cast<intptr_t>(g.once(&g_once, &OnceRoutineSlow)));
}

// Priority inheritance: sets its own priority, then creates a child with default attributes.
SSPC_SYSV_ABI void* InheritParent(void* out) {
    g.setprio(g.self(), 0x260);
    Probe child;
    Handle h = nullptr;
    if (g.create(&h, nullptr, &ProbeEntry, &child, "inherit-child") != 0 || g.join(h, nullptr) != 0) {
        return nullptr;
    }
    *static_cast<s32*>(out) = child.prio;
    return reinterpret_cast<void*>(1);
}

// Mutex contention and condition variables on manager threads.
void* g_mutex = nullptr;
void* g_cond = nullptr;
u64 g_counter = 0;
constexpr int kMutexThreads = 8;
constexpr int kMutexLoops = 20000;
SSPC_SYSV_ABI void* MutexWorker(void*) {
    for (int i = 0; i < kMutexLoops; ++i) {
        g.mutex_lock(&g_mutex);
        ++g_counter;
        g.mutex_unlock(&g_mutex);
    }
    return nullptr;
}
int g_ready = 0;
int g_woken = 0;
SSPC_SYSV_ABI void* CondWaiter(void*) {
    g.mutex_lock(&g_mutex);
    ++g_ready;
    while (g_counter == 0) {
        g.cond_wait(&g_cond, &g_mutex);
    }
    ++g_woken;
    g.mutex_unlock(&g_mutex);
    return nullptr;
}

// P3.2 stacks. A thread checks that its stack is all zeros below it (a new mapping, or a cached one the
// manager cleared) and has the size asked, then dirties it for the next thread that may get it.
constexpr u64 kExtraStack = 128 * 1024; // the manager's (and the oracle's) addition for HLE
u64 StackSizeFor(u64 attr_size) {
    return (attr_size + kExtraStack + 0x3FFF) & ~u64{0x3FFF};
}
struct StackCheck {
    u64 expect = 0; // usable bytes
    bool zero = false;
    bool size = false;
    uintptr_t base = 0;
};
SSPC_SYSV_ABI void* StackFill(void* arg) {
    auto* p = static_cast<StackCheck*>(arg);
    volatile u8 marker = 0;
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(const_cast<u8*>(&marker), &mbi, sizeof(mbi));
    const auto base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
    MEMORY_BASIC_INFORMATION whole{};
    VirtualQuery(mbi.AllocationBase, &whole, sizeof(whole));
    p->base = base;
    p->size = whole.RegionSize == p->expect;
    const uintptr_t end = (reinterpret_cast<uintptr_t>(&marker) - 4096) & ~uintptr_t{7};
    bool zero = true;
    for (uintptr_t a = base; a < end; a += 8) {
        if (*reinterpret_cast<const u64*>(a) != 0) {
            zero = false;
            break;
        }
    }
    p->zero = zero;
    std::memset(reinterpret_cast<void*>(base), 0xA5, end - base);
    return reinterpret_cast<void*>(1);
}

/// Creates a thread with a stack of `attr_size` bytes running StackFill and joins it; true when it ran
/// and its checks passed.
bool RunStackCheck(u64 attr_size, StackCheck* out = nullptr) {
    void* attr = nullptr;
    g.attr_init(&attr);
    g.attr_setstacksize(&attr, attr_size);
    // On the heap: if the join fails, the thread may still write it (then it is left behind).
    auto* check = new StackCheck{.expect = StackSizeFor(attr_size)};
    Handle h = nullptr;
    void* r = nullptr;
    const bool created = g.create(&h, &attr, &StackFill, check, "stack-check") == 0;
    const bool joined = created && g.join(h, &r) == 0;
    g.attr_destroy(&attr);
    const StackCheck result = *check;
    if (joined || !created) {
        delete check;
    }
    if (out != nullptr) {
        *out = result;
    }
    return joined && r != nullptr && result.zero && result.size;
}

constexpr u64 kChurnSizes[] = {16 * 1024, 64 * 1024, 256 * 1024, 1ull << 20, 2ull << 20, (3ull << 20) + 16 * 1024,
                               6ull << 20};
constexpr size_t kChurnKinds = std::size(kChurnSizes);

/// One of the churn's creators: `iterations` threads of random sizes, a third detached, the joinable
/// ones joined at once or later (up to 8 waiting), in random order. Returns the failures.
struct Churn {
    u32 seed;
    u32 iterations;
};
SSPC_SYSV_ABI void* ChurnWorker(void* arg) {
    const auto* p = static_cast<const Churn*>(arg);
    std::minstd_rand rng(p->seed);
    // A running thread writes into its own StackCheck: entries never move, a slot is free again only
    // after the join.
    constexpr u32 kWaiting = 8;
    Handle waiting[kWaiting]{};
    // On the heap: a slot whose join failed keeps its StackCheck (the thread may still write it).
    StackCheck* checks[kWaiting]{};
    for (auto& ck : checks) {
        ck = new StackCheck{};
    }
    uintptr_t bad = 0;
    void* attr = nullptr;
    g.attr_init(&attr);
    const auto join_slot = [&](u32 i) {
        void* r = nullptr;
        if (g.join(waiting[i], &r) != 0) {
            ++bad;
            checks[i] = new StackCheck{}; // the old one is left to the thread
        } else if (r == nullptr || !checks[i]->zero || !checks[i]->size) {
            ++bad;
        }
        waiting[i] = nullptr;
    };
    const auto join_random = [&] {
        u32 used[kWaiting];
        u32 n = 0;
        for (u32 i = 0; i < kWaiting; ++i) {
            if (waiting[i] != nullptr) {
                used[n++] = i;
            }
        }
        if (n > 0) {
            join_slot(used[rng() % n]);
        }
        return n;
    };
    for (u32 i = 0; i < p->iterations; ++i) {
        const u64 size = kChurnSizes[rng() % kChurnKinds];
        g.attr_setstacksize(&attr, size);
        const bool detached = rng() % 3 == 0;
        g.attr_setdetachstate(&attr, detached ? 1 : 0);
        Handle h = nullptr;
        if (detached) {
            if (g.create(&h, &attr, &ReturnArgPlusOne, nullptr, "churn-detached") != 0) {
                ++bad;
            }
            continue;
        }
        u32 slot = kWaiting;
        for (u32 s = 0; s < kWaiting && slot == kWaiting; ++s) {
            slot = waiting[s] == nullptr ? s : kWaiting;
        }
        if (slot == kWaiting) {
            slot = static_cast<u32>(rng() % kWaiting);
            join_slot(slot);
        }
        *checks[slot] = StackCheck{.expect = StackSizeFor(size)};
        if (g.create(&h, &attr, &StackFill, checks[slot], "churn") != 0) {
            ++bad;
            continue;
        }
        waiting[slot] = h;
        if (rng() % 2 == 0) {
            join_slot(slot);
        }
    }
    while (join_random() > 0) {
    }
    g.attr_destroy(&attr);
    for (auto* ck : checks) {
        delete ck;
    }
    return reinterpret_cast<void*>(bad);
}

// TLS destructors under stack churn.
u32 g_churn_key = 0;
std::atomic<u64> g_churn_dtors{0};
SSPC_SYSV_ABI void ChurnDestructor(void*) {
    ++g_churn_dtors;
}
SSPC_SYSV_ABI void* SetChurnKey(void*) {
    g.setspecific(g_churn_key, reinterpret_cast<void*>(1));
    return nullptr;
}

/// Waits until `pred` holds or `ms` passed.
template <typename P>
bool WaitFor(u32 ms, P&& pred) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!pred()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        Sleep(1);
    }
    return true;
}

std::string Hex(s32 v) {
    return fmt::format("{:#x}", static_cast<u32>(v));
}

void TestAttributes(TestGroup& c) {
    void* a = nullptr;
    c.Check("AttrInit", g.attr_init(&a) == 0 && a != nullptr, "");
    s32 prio = 0;
    u64 mask = 0;
    c.Check("default priority 0x2bc (FIFO default)", g.attr_getschedparam(&a, &prio) == 0 && prio == 0x2BC,
            Hex(prio));
    c.Check("default affinity: every CPU", g.attr_getaffinity(&a, &mask) == 0 && mask == ~0ull,
            fmt::format("{:#x}", mask));
    c.Check("Setdetachstate 2 -> EINVAL", g.attr_setdetachstate(&a, 2) == kEinval, "");
    c.Check("Setdetachstate 1/0", g.attr_setdetachstate(&a, 1) == 0 && g.attr_setdetachstate(&a, 0) == 0, "");
    c.Check("POSIX pthread_attr_setdetachstate 2 -> EINVAL (22)", g.posix_attr_setdetachstate(&a, 2) == 22, "");
    c.Check("Setinheritsched 1 -> ENOTSUP", g.attr_setinheritsched(&a, 1) == kEnotsup, "");
    c.Check("Setinheritsched 0/4", g.attr_setinheritsched(&a, 0) == 0 && g.attr_setinheritsched(&a, 4) == 0, "");
    const s32 low = 0x50, ok = 0x250;
    c.Check("Setschedparam 0x50 under FIFO -> ENOTSUP", g.attr_setschedparam(&a, &low) == kEnotsup, "");
    c.Check("Setschedparam null -> ENOTSUP", g.attr_setschedparam(&a, nullptr) == kEnotsup, "");
    c.Check("Setschedparam 0x250", g.attr_setschedparam(&a, &ok) == 0 && g.attr_getschedparam(&a, &prio) == 0 &&
                                         prio == 0x250, Hex(prio));
    c.Check("Setschedpolicy 0 -> ENOTSUP", g.attr_setschedpolicy(&a, 0) == kEnotsup, "");
    c.Check("Setschedpolicy OTHER: priority 0x384", g.attr_setschedpolicy(&a, 2) == 0 &&
                                                        g.attr_getschedparam(&a, &prio) == 0 && prio == 0x384,
            Hex(prio));
    c.Check("Setschedparam 0x50 under OTHER is accepted", g.attr_setschedparam(&a, &low) == 0, "");
    c.Check("Setstacksize 8 KiB -> EINVAL", g.attr_setstacksize(&a, 8 * 1024) == kEinval, "");
    c.Check("Setstacksize 64 KiB", g.attr_setstacksize(&a, 64 * 1024) == 0, "");
    c.Check("Setaffinity 0x5", g.attr_setaffinity(&a, 0x5) == 0 && g.attr_getaffinity(&a, &mask) == 0 && mask == 0x5,
            fmt::format("{:#x}", mask));
    c.Check("Getaffinity null -> EINVAL", g.attr_getaffinity(&a, nullptr) == kEinval, "");
    c.Check("AttrDestroy", g.attr_destroy(&a) == 0 && a == nullptr, "");
    c.Check("AttrDestroy again -> EINVAL", g.attr_destroy(&a) == kEinval, "");
    struct Foreign {
        u64 words[8]{0x1111, 0x2222};
    } foreign;
    void* fp = &foreign;
    c.Check("a foreign attribute pointer -> EINVAL (not freed)",
            g.attr_destroy(&fp) == kEinval && fp == &foreign && g.attr_setstacksize(&fp, 1 << 20) == kEinval, "");
    void* pa = nullptr;
    c.Check("POSIX pthread_attr_init", g.posix_attr_init(&pa) == 0 && pa != nullptr && g.attr_destroy(&pa) == 0, "");
}

void TestLifecycle(TestGroup& c) {
    auto& host = Host();
    c.Check("Self on a host thread: null (as the oracle)", g.self() == nullptr && g.posix_self() == nullptr, "");

    Probe p;
    Handle h = nullptr;
    s32 r = g.create(&h, nullptr, &ProbeEntry, &p, "probe-thread");
    void* ret = nullptr;
    const s32 j = r == 0 ? g.join(h, &ret) : -1;
    c.Check("create + join", r == 0 && j == 0 && ret == reinterpret_cast<void*>(0x5e1f),
            fmt::format("create {} join {} ret {}", Hex(r), Hex(j), ret));
    c.Check("Self in the thread = its handle (sce and POSIX)", p.self == h && p.posix_self == h,
            fmt::format("{} {} {}", p.self, p.posix_self, h));
    c.Check("TCB: FS base set, tcb_thread = handle", p.tcb_thread == reinterpret_cast<u64>(h),
            fmt::format("{:#x}", p.tcb_thread));
    c.Check("MXCSR 0x9fc0 (libkernel's)", p.mxcsr == 0x9fc0, fmt::format("{:#x}", p.mxcsr));
    c.Check("host thread name", p.name == L"probe-thread", "");
    c.Check("runs on its guest stack", p.stack_inside, "");
    c.Check("Getprio in the thread: default 0x2bc", p.prio_ret == 0 && p.prio == 0x2BC, Hex(p.prio));
    c.Check("Getaffinity in the thread: every CPU", p.affinity == ~0ull, fmt::format("{:#x}", p.affinity));

    // The guard below the stack: address space without pages (P3.2: a Reserved range of its own).
    {
        const auto old = Threads::Test::SetStackCacheLimits({0, 0}); // a new stack, mapped now
        Probe q;
        Handle hq = nullptr;
        g.create(&hq, nullptr, &ProbeEntry, &q, "guard");
        const auto* stack = static_cast<u8*>(host.last_stack.load());
        MEMORY_BASIC_INFORMATION top{}, bottom{}, usable{};
        const bool queried = VirtualQuery(stack - 1, &top, sizeof(top)) == sizeof(top) &&
                             VirtualQuery(stack - 16 * 1024, &bottom, sizeof(bottom)) == sizeof(bottom) &&
                             VirtualQuery(stack, &usable, sizeof(usable)) == sizeof(usable);
        bool guard_area = false, stack_area = false;
        const auto s = reinterpret_cast<VAddr>(stack);
        for (const auto& a : Core::GetGuestVirtualMemory().Areas()) {
            if (a.base + a.size == s && a.size >= 16 * 1024 && a.type == Core::VMAType::Reserved &&
                a.name == "stack guard") {
                guard_area = true;
            }
            if (a.base == s && a.type == Core::VMAType::Stack &&
                (static_cast<u32>(a.prot) & static_cast<u32>(Core::MemoryProt::CpuWrite)) != 0) {
                stack_area = true;
            }
        }
        g.join(hq, nullptr);
        Threads::Test::SetStackCacheLimits(old);
        c.Check("16 KiB guard below the stack: no committed pages (any access faults); the stack committed RW",
                queried && top.State != MEM_COMMIT && bottom.State != MEM_COMMIT && usable.State == MEM_COMMIT &&
                    (usable.Protect == PAGE_READWRITE || usable.Protect == PAGE_EXECUTE_READWRITE),
                fmt::format("guard {:#x}/{:#x}, stack {:#x} {:#x}", top.State, bottom.State, usable.State,
                            usable.Protect));
        c.Check("the guard and the stack are two areas: Reserved \"stack guard\" + read/write Stack",
                guard_area && stack_area, fmt::format("guard {} stack {}", guard_area, stack_area));
    }

    // scePthreadExit from deep in the thread.
    h = nullptr;
    r = g.create(&h, nullptr, &ExitFromNestedFrames, nullptr, "exit");
    ret = nullptr;
    c.Check("scePthreadExit from nested frames: the joiner gets its value",
            r == 0 && g.join(h, &ret) == 0 && ret == reinterpret_cast<void*>(0xe417), fmt::format("{}", ret));

    // POSIX create/join.
    h = nullptr;
    r = g.posix_create(&h, nullptr, &ReturnArgPlusOne, reinterpret_cast<void*>(41), "posix");
    ret = nullptr;
    c.Check("POSIX pthread_create_name_np + pthread_join", r == 0 && g.posix_join(h, &ret) == 0 &&
                                                               ret == reinterpret_cast<void*>(42),
            fmt::format("{}", ret));

    // Errors.
    c.Check("create with no handle -> EINVAL", g.create(nullptr, nullptr, &ReturnArgPlusOne, nullptr, "x") == kEinval,
            "");
    struct Foreign {
        u64 words[8]{0x1234};
    } foreign;
    void* fattr = &foreign;
    h = nullptr;
    c.Check("create with a foreign attribute object -> EINVAL",
            g.create(&h, &fattr, &ReturnArgPlusOne, nullptr, "x") == kEinval, "");
    c.Check("join null -> EINVAL", g.join(nullptr, nullptr) == kEinval, "");
    c.Check("join of a handle that is not a thread -> ESRCH",
            g.join(reinterpret_cast<Handle>(0x12345678), nullptr) == kEsrch, "");
    c.Check("detach null -> EINVAL", g.detach(nullptr) == kEinval, "");
    h = nullptr;
    g.create(&h, nullptr, &JoinSelf, nullptr, "join-self");
    ret = nullptr;
    g.join(h, &ret);
    c.Check("join of itself -> EDEADLK", static_cast<s32>(reinterpret_cast<intptr_t>(ret)) == kEdeadlk,
            Hex(static_cast<s32>(reinterpret_cast<intptr_t>(ret))));
    const s32 stale = g.join(h, nullptr); // the slot is free again
    c.Check("join of a joined thread (stale handle) -> ESRCH", stale == kEsrch, Hex(stale));

    // A second joiner while the first one waits: ENOTSUP (one joiner, as the oracle).
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    h = nullptr;
    g.create(&h, nullptr, &WaitEvent, ev, "two-joiners");
    std::atomic<s32> first{-1};
    std::thread joiner([&] { first = g.join(h, nullptr); });
    Sleep(50);
    const s32 second = g.join(h, nullptr);
    const s32 det = g.detach(h);
    SetEvent(ev);
    joiner.join();
    c.Check("second joiner -> ENOTSUP, detach while joined -> EINVAL, first joiner 0",
            second == kEnotsup && det == kEinval && first == 0,
            fmt::format("second {} detach {} first {}", Hex(second), Hex(det), Hex(first.load())));
    CloseHandle(ev);

    // Detached threads: joined -> EINVAL; reaped once gone.
    const auto before = Threads::GetStats();
    void* attr = nullptr;
    g.attr_init(&attr);
    g.attr_setdetachstate(&attr, 1);
    ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    h = nullptr;
    g.create(&h, &attr, &WaitEvent, ev, "detached");
    c.Check("join of a detached thread -> EINVAL", g.join(h, nullptr) == kEinval, "");
    c.Check("detach of a detached thread -> EINVAL", g.detach(h) == kEinval, "");
    SetEvent(ev);
    const bool reaped = WaitFor(5000, [&] {
        Threads::Test::ReapDetached();
        return Threads::GetStats().slots_in_use == before.slots_in_use;
    });
    c.Check("a detached thread that ended is reaped (slot, stack, TCB)", reaped, "");
    CloseHandle(ev);
    // Detached after it ended.
    h = nullptr;
    g.attr_setdetachstate(&attr, 0);
    g.create(&h, &attr, &ReturnArgPlusOne, nullptr, "detach-late");
    Sleep(30);
    const s32 late = g.detach(h);
    const bool late_reaped = WaitFor(5000, [&] {
        Threads::Test::ReapDetached();
        return Threads::GetStats().slots_in_use == before.slots_in_use;
    });
    c.Check("detach after the thread ended: 0, then reaped", late == 0 && late_reaped, Hex(late));
    g.attr_destroy(&attr);
}

void TestPriorityAffinity(TestGroup& c) {
    void* attr = nullptr;
    g.attr_init(&attr);
    g.attr_setinheritsched(&attr, 0);
    const s32 prio = 0x250;
    g.attr_setschedparam(&attr, &prio);
    g.attr_setaffinity(&attr, 0x3);
    Probe p;
    Handle h = nullptr;
    g.create(&h, &attr, &ProbeEntry, &p, "explicit");
    g.join(h, nullptr);
    c.Check("explicit scheduling: the attribute's priority", p.prio == 0x250, Hex(p.prio));
    c.Check("the attribute's affinity", p.affinity == 0x3, fmt::format("{:#x}", p.affinity));
    g.attr_destroy(&attr);

    s32 inherited = 0;
    h = nullptr;
    g.create(&h, nullptr, &InheritParent, &inherited, "inherit-parent");
    void* ok = nullptr;
    g.join(h, &ok);
    c.Check("inherited priority (scePthreadSetprio of the creator)", ok != nullptr && inherited == 0x260,
            Hex(inherited));

    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    h = nullptr;
    g.create(&h, nullptr, &WaitEvent, ev, "prio");
    s32 got = 0;
    u64 mask = 0;
    const bool set = g.setprio(h, 0x2F0) == 0 && g.getprio(h, &got) == 0 && got == 0x2F0;
    const bool aff = g.setaffinity(h, 0x10) == 0 && g.getaffinity(h, &mask) == 0 && mask == 0x10;
    void* tattr = nullptr;
    g.attr_init(&tattr);
    s32 tprio = 0;
    const bool attr_get = g.attr_get(h, &tattr) == 0 && g.attr_getschedparam(&tattr, &tprio) == 0 && tprio == 0x2F0;
    g.attr_destroy(&tattr);
    c.Check("Setprio/Getprio of another thread", set, Hex(got));
    c.Check("Setaffinity/Getaffinity of another thread", aff, fmt::format("{:#x}", mask));
    c.Check("AttrGet: the thread's current attributes", attr_get, Hex(tprio));
    c.Check("Getprio null -> EINVAL", g.getprio(h, nullptr) == kEinval, "");
    SetEvent(ev);
    g.join(h, nullptr);
    CloseHandle(ev);
    c.Check("Getprio of a joined thread -> ESRCH (as the oracle)", g.getprio(h, &got) == kEsrch, "");
    c.Check("Setprio/Getaffinity of no thread -> ESRCH", g.setprio(reinterpret_cast<Handle>(0x1000), 1) == kEsrch &&
                                                              g.getaffinity(reinterpret_cast<Handle>(0x1000), &mask) == kEsrch,
            "");
    c.Check("Getaffinity null thread -> EINVAL", g.getaffinity(nullptr, &mask) == kEinval, "");
}

void TestKeysOnce(TestGroup& c) {
    g_dtor_calls = 0;
    g_dtor_sum = 0;
    c.Check("KeyCreate", g.key_create(&g_key, &CountingDestructor) == 0, "");
    constexpr int kWorkers = 8;
    std::vector<Handle> hs(kWorkers);
    for (int i = 0; i < kWorkers; ++i) {
        g.create(&hs[i], nullptr, &TlsWorker, reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1)), "tls");
    }
    u64 ok = 0;
    for (auto h : hs) {
        void* r = nullptr;
        g.join(h, &r);
        ok += r != nullptr ? 1 : 0;
    }
    c.Check("TLS values are per thread", ok == kWorkers, fmt::format("{}/{}", ok, kWorkers));
    c.Check("destructors ran at exit with each thread's value", g_dtor_calls == kWorkers && g_dtor_sum == 36,
            fmt::format("calls {} sum {}", g_dtor_calls.load(), g_dtor_sum.load()));
    c.Check("the host thread does not see them", g.getspecific(g_key) == nullptr, "");

    // A key deleted while a thread holds a value: the value is gone, no destructor, set -> EINVAL.
    g.key_create(&g_key2, &CountingDestructor);
    g_tls_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Handle h = nullptr;
    g.create(&h, nullptr, &TlsDeletedKey, nullptr, "tls-deleted");
    Sleep(30);
    const u64 calls_before = g_dtor_calls;
    const s32 del = g.key_delete(g_key2);
    SetEvent(g_tls_event);
    void* r = nullptr;
    g.join(h, &r);
    CloseHandle(g_tls_event);
    c.Check("deleted key: value gone, no destructor, Setspecific -> EINVAL",
            del == 0 && r != nullptr && g_dtor_calls == calls_before, fmt::format("delete {}", Hex(del)));
    c.Check("KeyDelete again / out of range -> EINVAL",
            g.key_delete(g_key2) == kEinval && g.key_delete(256) == kEinval, "");

    // Exhaustion: 256 keys at most (EAGAIN).
    std::vector<u32> keys;
    u32 k = 0;
    s32 last = 0;
    while ((last = g.key_create(&k, nullptr)) == 0 && keys.size() < 300) {
        keys.push_back(k);
    }
    const size_t total = keys.size() + 1; // + g_key
    c.Check("256 keys, then EAGAIN", last == kEagain && total == 256, fmt::format("{} keys, {}", total, Hex(last)));
    for (const u32 key : keys) {
        g.key_delete(key);
    }
    g.key_delete(g_key);

    // Once under contention: one call of the routine, every caller returns after it.
    g_once = {};
    g_once_runs = 0;
    constexpr int kCallers = 16;
    std::vector<Handle> callers(kCallers);
    for (auto& ch : callers) {
        g.create(&ch, nullptr, &OnceCaller, nullptr, "once");
    }
    u64 zero = 0;
    for (auto ch : callers) {
        void* rv = reinterpret_cast<void*>(1);
        g.join(ch, &rv);
        zero += rv == nullptr ? 1 : 0;
    }
    c.Check("scePthreadOnce: the routine ran once, 16 callers returned 0, state done",
            g_once_runs == 1 && zero == kCallers && g_once.state == 1,
            fmt::format("runs {} ok {} state {}", g_once_runs.load(), zero, g_once.state));
}

void TestStress(TestGroup& c) {
    auto& host = Host();
    const auto base = Threads::GetStats();
    const auto handle_count = [] {
        DWORD n = 0;
        GetProcessHandleCount(GetCurrentProcess(), &n);
        return n;
    };
    const DWORD handles_before = handle_count();
    const auto t0 = std::chrono::steady_clock::now();
    u64 ok = 0;
    constexpr int kCycles = 1000;
    for (int i = 0; i < kCycles; ++i) {
        Handle h = nullptr;
        void* r = nullptr;
        const auto arg = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        if (g.create(&h, nullptr, &ReturnArgPlusOne, arg, "short") == 0 && g.join(h, &r) == 0 &&
            r == reinterpret_cast<void*>(static_cast<uintptr_t>(i + 1))) {
            ++ok;
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const DWORD handles_after = handle_count();
    const auto after = Threads::GetStats();
    c.Check("1000 create+join in a row", ok == kCycles, fmt::format("{} ok", ok));
    // One stack size: at most one more stack held (the cache's), reused every time after it.
    c.Check("no leaked slots, stacks, TCBs (one stack size: reused, at most one more held)",
            after.slots_in_use == base.slots_in_use && after.stacks_mapped <= base.stacks_mapped + 1 &&
                after.reuses >= base.reuses + kCycles - 1 &&
                host.tcbs.load() == static_cast<s64>(base.slots_in_use),
            fmt::format("slots {}->{}, stacks {}->{}, reused {}, tcbs {}", base.slots_in_use, after.slots_in_use,
                        base.stacks_mapped, after.stacks_mapped, after.reuses - base.reuses, host.tcbs.load()));
    c.Check("no leaked host handles", handles_after <= handles_before + 4,
            fmt::format("{} -> {}", handles_before, handles_after));
    c.Info(fmt::format("1000 short threads: {:.1f} ms ({:.0f} us per create+join)", ms, ms * 1000 / kCycles));

    // Concurrent creation from 16 manager threads.
    std::vector<Handle> creators(kCreators);
    for (int i = 0; i < kCreators; ++i) {
        g.create(&creators[i], nullptr, &Creator, reinterpret_cast<void*>(static_cast<uintptr_t>(i * 1000)), "creator");
    }
    u64 children = 0;
    for (auto h : creators) {
        void* r = nullptr;
        g.join(h, &r);
        children += reinterpret_cast<uintptr_t>(r);
    }
    const auto after2 = Threads::GetStats();
    c.Check("16 threads creating and joining 50 each at once", children == kCreators * kChildren &&
                                                                   after2.slots_in_use == base.slots_in_use,
            fmt::format("{} of {}, slots {}", children, kCreators * kChildren, after2.slots_in_use));

    // 300 detached threads ending in random order.
    std::mt19937 rng(1234);
    void* attr = nullptr;
    g.attr_init(&attr);
    g.attr_setdetachstate(&attr, 1);
    std::vector<HANDLE> events;
    for (int i = 0; i < 300; ++i) {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        events.push_back(ev);
        Handle h = nullptr;
        g.create(&h, &attr, &WaitEvent, ev, "detached-stress");
    }
    std::shuffle(events.begin(), events.end(), rng);
    for (HANDLE ev : events) {
        SetEvent(ev);
        if (rng() % 4 == 0) {
            Sleep(0);
        }
    }
    const bool all_reaped = WaitFor(10000, [&] {
        Threads::Test::ReapDetached();
        return Threads::GetStats().slots_in_use == base.slots_in_use;
    });
    for (HANDLE ev : events) {
        CloseHandle(ev);
    }
    g.attr_destroy(&attr);
    const auto after3 = Threads::GetStats();
    c.Check("300 detached threads ending in random order are all reaped", all_reaped,
            fmt::format("slots {} pending {}", after3.slots_in_use, after3.pending_reap));
    const auto limits = Threads::DefaultStackCacheLimits();
    c.Check("stacks: held = in use + cached, the cache within its limits, the rest unmapped",
            after3.stacks_mapped == static_cast<u64>(host.stacks.load() - host.leaked.load()) &&
                after3.stacks_cached == after3.stacks_mapped - after3.slots_in_use &&
                after3.stacks_cached <= limits.count && after3.cache_bytes <= limits.bytes &&
                after3.evictions > base.evictions,
            fmt::format("held {} cached {} ({} KiB) in use {}, unmapped for the limit {}", after3.stacks_mapped,
                        after3.stacks_cached, after3.cache_bytes >> 10, after3.slots_in_use,
                        after3.evictions - base.evictions));
}

void TestFailures(TestGroup& c) {
    auto& host = Host();
    // A stack size no kept stack has, so that the manager maps a new one; an empty cache.
    Threads::Test::DrainStackCache();
    void* attr = nullptr;
    g.attr_init(&attr);
    g.attr_setstacksize(&attr, 2u << 20);
    const auto base = Threads::GetStats();
    host.fail_map = true;
    Handle h = reinterpret_cast<Handle>(1);
    const s32 r1 = g.create(&h, &attr, &ReturnArgPlusOne, nullptr, "no-stack");
    const auto c1 = Threads::GetStats();
    c.Check("no memory for the stack -> EAGAIN, nothing left behind",
            r1 == kEagain && c1.slots_in_use == base.slots_in_use && c1.stacks_mapped == base.stacks_mapped,
            Hex(r1));
    host.fail_tcb = true;
    const s32 r2 = g.create(&h, &attr, &ReturnArgPlusOne, nullptr, "no-tcb");
    const auto c2 = Threads::GetStats();
    c.Check("no TCB -> EAGAIN, its new stack cached for reuse, no slot taken",
            r2 == kEagain && c2.slots_in_use == base.slots_in_use && c2.stacks_mapped == base.stacks_mapped + 1 &&
                c2.stacks_cached == base.stacks_cached + 1,
            fmt::format("{} mapped {} kept {}", Hex(r2), c2.stacks_mapped, c2.stacks_cached));
    // The kept 2 MiB stack serves the next thread of that size.
    void* ret = nullptr;
    const s32 r3 = g.create(&h, &attr, &ReturnArgPlusOne, reinterpret_cast<void*>(9), "reuse");
    const bool joined = r3 == 0 && g.join(h, &ret) == 0;
    const auto c3 = Threads::GetStats();
    c.Check("a kept stack is reused (no new mapping)", joined && ret == reinterpret_cast<void*>(10) &&
                                                           c3.stacks_mapped == c2.stacks_mapped,
            fmt::format("mapped {}", c3.stacks_mapped));
    g.attr_destroy(&attr);
}

/// P3.2: the lifecycle of guest stacks (docs/PHASE3_INDEPENDENT_RUNTIME.md §10) on the selected memory
/// manager: unmapped for real, reused only at the exact size and cleared, the cache bounded, nothing left
/// behind (stacks, areas, commit, handles) after thousands of threads.
void TestStacks(TestGroup& c) {
    auto& host = Host();
    auto& memory = Core::GetGuestVirtualMemory();
    const auto handle_count = [] {
        DWORD n = 0;
        GetProcessHandleCount(GetCurrentProcess(), &n);
        return n;
    };
    const auto private_bytes = [] {
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc));
        return static_cast<s64>(pmc.PrivateUsage);
    };
    const auto defaults = Threads::DefaultStackCacheLimits();
    Threads::Test::DrainStackCache();
    const size_t areas0 = memory.Areas().size();
    const auto s0 = Threads::GetStats();
    const s64 priv0 = private_bytes();
    const DWORD h0 = handle_count();
    const auto t0 = std::chrono::steady_clock::now();
    std::string step_private; // private commit after each step (diagnostics)
    s64 step_last = priv0;
    // TEMP diagnostics: committed private memory of the process by allocation size.
    const auto census = [] {
        std::map<u64, std::pair<u64, u64>> by_alloc; // allocation base -> (committed bytes, reserved bytes)
        MEMORY_BASIC_INFORMATION mbi{};
        for (uintptr_t a = 0; VirtualQuery(reinterpret_cast<void*>(a), &mbi, sizeof(mbi)) == sizeof(mbi);
             a = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize) {
            if (mbi.Type == MEM_PRIVATE && mbi.State != MEM_FREE) {
                auto& e = by_alloc[reinterpret_cast<u64>(mbi.AllocationBase)];
                e.second += mbi.RegionSize;
                if (mbi.State == MEM_COMMIT) {
                    e.first += mbi.RegionSize;
                }
            }
            if (reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize < a) {
                break;
            }
        }
        std::map<u64, std::pair<u64, u64>> hist; // reserved size of the allocation -> (count, committed)
        for (const auto& [base, e] : by_alloc) {
            auto& h = hist[e.second];
            ++h.first;
            h.second += e.first;
        }
        return hist;
    };
    const auto census0 = census();
    s64 step_delta[2] = {0, 0};
    /// Private commit since the last mark; the allocations that grew by more than 8 MiB after step 8a.
    const auto mark = [&](const char* step) {
        const s64 now = private_bytes();
        const s64 delta = now - step_last;
        step_private += fmt::format(" {}:{:+}MiB", step, delta >> 20);
        step_last = now;
        if (std::string_view(step) == "8a") {
            for (const auto& [size, h] : census()) {
                const auto it = census0.find(size);
                const u64 n0 = it == census0.end() ? 0 : it->second.first;
                const u64 c0 = it == census0.end() ? 0 : it->second.second;
                if (h.second > c0 + (8ull << 20)) {
                    step_private += fmt::format(" [allocations of {} KiB: {} -> {}, committed {:+} MiB]", size >> 10,
                                                n0, h.first, (static_cast<s64>(h.second) - static_cast<s64>(c0)) >> 20);
                }
            }
        }
        return delta;
    };

    // 1. No cache: the stack and its guard go back to the memory manager (D17: one call, no split).
    Threads::Test::SetStackCacheLimits({0, 0});
    StackCheck first;
    const bool ran = RunStackCheck(3ull << 20, &first);
    const auto s1 = Threads::GetStats();
    MEMORY_BASIC_INFORMATION gone{};
    VirtualQuery(reinterpret_cast<void*>(first.base), &gone, sizeof(gone));
    c.Check("no cache: the stack + guard are unmapped at once (D17), the area is free again",
            ran && s1.stacks_mapped == s0.stacks_mapped && s1.unmap_calls == s0.unmap_calls + 1 &&
                s1.unmap_failures == 0 && memory.Areas().size() == areas0 && gone.State != MEM_COMMIT,
            fmt::format("ran {} held {}->{} unmapped {} areas {}->{} state {:#x}", ran, s0.stacks_mapped,
                        s1.stacks_mapped, s1.unmap_calls - s0.unmap_calls, areas0, memory.Areas().size(), gone.State));

    mark("1");
    // 2. Reuse at the same size, cleared; another size is a new mapping.
    Threads::Test::SetStackCacheLimits({4, 16ull << 20});
    StackCheck a, b, other;
    RunStackCheck(1ull << 20, &a);
    const auto s2a = Threads::GetStats();
    const bool b_ok = RunStackCheck(1ull << 20, &b);
    const auto s2b = Threads::GetStats();
    const bool o_ok = RunStackCheck(512 * 1024, &other);
    const auto s2c = Threads::GetStats();
    c.Check("a cached stack is reused at exactly its size, all zeros again (the last thread dirtied it)",
            b_ok && b.base == a.base && s2b.reuses == s2a.reuses + 1 && s2b.map_calls == s2a.map_calls,
            fmt::format("same base {} zeros {} reused {}", b.base == a.base, b.zero, s2b.reuses - s2a.reuses));
    c.Check("another size: a new mapping, not the cached stack", o_ok && other.base != a.base &&
                                                                     s2c.map_calls == s2b.map_calls + 1,
            fmt::format("maps {}", s2c.map_calls - s2b.map_calls));

    mark("2");
    // 3. A burst over the limits: 12 threads alive at once, then all gone; at most 4 stacks / 8 MiB stay.
    Threads::Test::SetStackCacheLimits({4, 8ull << 20});
    {
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::vector<Handle> hs(12);
        for (auto& h : hs) {
            g.create(&h, nullptr, &WaitEvent, ev, "burst");
        }
        const auto during = Threads::GetStats();
        SetEvent(ev);
        for (auto h : hs) {
            g.join(h, nullptr);
        }
        CloseHandle(ev);
        const auto after = Threads::GetStats();
        c.Check("burst of 12: the cache keeps 4 (limit), 8 unmapped; held = cached",
                during.slots_in_use >= 12 && after.stacks_cached == 4 && after.cache_bytes <= (8ull << 20) &&
                    after.stacks_mapped == s0.stacks_mapped + after.stacks_cached &&
                    static_cast<u64>(host.stacks.load() - host.leaked.load()) == after.stacks_mapped,
                fmt::format("cached {} ({} KiB) held {} evicted {}", after.stacks_cached, after.cache_bytes >> 10,
                            after.stacks_mapped, after.evictions - during.evictions));
    }

    mark("3");
    // 4. 1200 threads of 7 sizes one after another, default limits: a plateau, never above the limits.
    Threads::Test::SetStackCacheLimits(defaults);
    std::minstd_rand rng(4321);
    u64 bad = 0;
    s64 priv_mid = 0; // private commit without the held stacks' (the cache's sizes change)
    u64 held_mid = 0;
    for (int i = 0; i < 1200; ++i) {
        if (!RunStackCheck(kChurnSizes[rng() % kChurnKinds])) {
            ++bad;
        }
        if (i == 399) {
            const auto s = Threads::GetStats();
            priv_mid = private_bytes() - static_cast<s64>(s.stack_bytes);
            held_mid = s.stacks_mapped;
        }
    }
    const auto s4 = Threads::GetStats();
    const s64 priv_end = private_bytes() - static_cast<s64>(s4.stack_bytes);
    c.Check("1200 threads of 7 stack sizes in a row: zeros + size every time, held <= the cache limit",
            bad == 0 && s4.stacks_mapped - s0.stacks_mapped <= defaults.count &&
                s4.peak_cache_bytes <= defaults.bytes && s4.reuses > s0.reuses,
            fmt::format("bad {} held {} peak cache {} KiB reused {} mapped {}", bad, s4.stacks_mapped,
                        s4.peak_cache_bytes >> 10, s4.reuses - s0.reuses, s4.map_calls - s0.map_calls));
    c.Check("... a plateau: no growth of held stacks or of private commit (less the held stacks') from thread 400 "
            "to 1200",
            s4.stacks_mapped <= std::max<u64>(held_mid, s0.stacks_mapped + defaults.count) &&
                priv_end - priv_mid < (8ll << 20),
            fmt::format("held {} -> {}, private {:+} KiB", held_mid, s4.stacks_mapped, (priv_end - priv_mid) >> 10));

    mark("4");
    // 5. Churn: 8 manager threads create 250 threads each (random sizes, a third detached, joins now or
    // later in random order) at once.
    {
        constexpr u32 kCreators = 8;
        constexpr u32 kEach = 250;
        std::vector<Churn> params(kCreators);
        std::vector<Handle> creators(kCreators);
        for (u32 i = 0; i < kCreators; ++i) {
            params[i] = {1000u + i, kEach};
            g.create(&creators[i], nullptr, &ChurnWorker, &params[i], "churn-creator");
        }
        u64 failures = 0;
        for (auto h : creators) {
            void* r = nullptr;
            g.join(h, &r);
            failures += reinterpret_cast<uintptr_t>(r);
        }
        const bool reaped = Threads::Test::WaitDetached(10000);
        const auto s5 = Threads::GetStats();
        c.Check("churn: 8 creators x 250 threads (sizes, detached, joins in random order) without a failure",
                failures == 0 && reaped && s5.slots_in_use == s0.slots_in_use && s5.rejected == 0,
                fmt::format("failures {} reaped {} slots {} rejected {}", failures, reaped, s5.slots_in_use,
                            s5.rejected));
        c.Check("... held = cached, within the limits; the manager's count = the memory manager's",
                s5.stacks_mapped == s0.stacks_mapped + s5.stacks_cached && s5.stacks_cached <= defaults.count &&
                    s5.cache_bytes <= defaults.bytes &&
                    static_cast<u64>(host.stacks.load() - host.leaked.load()) == s5.stacks_mapped,
                fmt::format("held {} cached {} ({} KiB) host {}", s5.stacks_mapped, s5.stacks_cached,
                            s5.cache_bytes >> 10, host.stacks.load() - host.leaked.load()));
    }

    mark("5");
    // 6. 500 detached threads at once.
    {
        void* attr = nullptr;
        g.attr_init(&attr);
        g.attr_setdetachstate(&attr, 1);
        u64 created = 0;
        for (int i = 0; i < 500; ++i) {
            Handle h = nullptr;
            created += g.create(&h, &attr, &ReturnArgPlusOne, nullptr, "mass-detached") == 0 ? 1 : 0;
        }
        g.attr_destroy(&attr);
        const bool reaped = Threads::Test::WaitDetached(10000);
        const auto s6 = Threads::GetStats();
        c.Check("500 detached threads: all reaped, the cache within its limits",
                created == 500 && reaped && s6.slots_in_use == s0.slots_in_use &&
                    s6.stacks_cached <= defaults.count && s6.stacks_mapped == s0.stacks_mapped + s6.stacks_cached,
                fmt::format("created {} reaped {} held {} cached {}", created, reaped, s6.stacks_mapped,
                            s6.stacks_cached));
    }

    mark("6");
    // 7. TLS destructors of 200 threads while stacks come and go (a small cache).
    {
        Threads::Test::SetStackCacheLimits({2, 4ull << 20});
        g_churn_dtors = 0;
        g.key_create(&g_churn_key, &ChurnDestructor);
        void* attr = nullptr;
        g.attr_init(&attr);
        for (int i = 0; i < 200; ++i) {
            g.attr_setstacksize(&attr, kChurnSizes[i % kChurnKinds]);
            Handle h = nullptr;
            if (g.create(&h, &attr, &SetChurnKey, nullptr, "tls-churn") == 0) {
                g.join(h, nullptr);
            }
        }
        g.attr_destroy(&attr);
        g.key_delete(g_churn_key);
        c.Check("200 threads with a TLS value: 200 destructor calls, before the stack went away",
                g_churn_dtors == 200, fmt::format("{}", g_churn_dtors.load()));
        Threads::Test::SetStackCacheLimits(defaults);
    }

    mark("7");
    // 8. Every slot taken: EAGAIN for the next, nothing held for it; then all gone.
    for (int round = 0; round < 2; ++round) { // twice: is the memory kept for reuse, or does it grow?
        Threads::Test::DrainStackCache(); // every stack below is a new one
        HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        void* attr = nullptr;
        g.attr_init(&attr);
        g.attr_setstacksize(&attr, 16 * 1024);
        const auto before = Threads::GetStats();
        std::vector<Handle> hs;
        s32 last = 0;
        for (;;) {
            Handle h = nullptr;
            last = g.create(&h, &attr, &WaitEvent, ev, "fill");
            if (last != 0) {
                break;
            }
            hs.push_back(h);
        }
        const auto full = Threads::GetStats();
        SetEvent(ev);
        for (auto h : hs) {
            g.join(h, nullptr);
        }
        g.attr_destroy(&attr);
        CloseHandle(ev);
        const auto after = Threads::GetStats();
        c.Check("all 1024 slots taken: the next create -> EAGAIN, no stack held for it; all joined after",
                last == kEagain && full.slots_in_use == 1024 && hs.size() == 1024 - before.slots_in_use &&
                    full.stacks_mapped == before.stacks_mapped + hs.size() &&
                    after.slots_in_use == before.slots_in_use,
                fmt::format("{} threads, {}, held {}", hs.size(), Hex(last), full.stacks_mapped));
        step_delta[round] = mark(round == 0 ? "8a" : "8b");
    }
    // 1024 host threads alive at once leave a high-water mark that is not the thread manager's: the
    // bridge's profiler (Tracy, on in every build but Release: its rpmalloc maps 4 MiB per thread and
    // keeps it for later threads). Repeating it must not add another one.
    c.Check("1024 threads at once, twice: the second time adds no memory (a high-water mark, not a leak)",
            step_delta[1] < std::max<s64>(step_delta[0] / 4, 64ll << 20),
            fmt::format("{:+} MiB, then {:+} MiB", step_delta[0] >> 20, step_delta[1] >> 20));

    // 9. The memory manager refuses an unmap: the stack is forgotten (never handed out again), the next
    // thread gets a new one.
    {
        Threads::Test::SetStackCacheLimits({0, 0});
        const auto before = Threads::GetStats();
        host.fail_unmap = true;
        StackCheck x, y;
        RunStackCheck(5ull << 20, &x);
        const auto mid = Threads::GetStats();
        const bool y_ok = RunStackCheck(5ull << 20, &y);
        const auto after = Threads::GetStats();
        c.Check("unmap refused: counted, forgotten (not reused); the next thread maps a new stack",
                mid.unmap_failures == before.unmap_failures + 1 && mid.stacks_mapped == before.stacks_mapped &&
                    y_ok && y.base != x.base && after.map_calls == mid.map_calls + 1,
                fmt::format("failures {} held {} new base {}", mid.unmap_failures - before.unmap_failures,
                            mid.stacks_mapped, y.base != x.base));
        TestHost::Leaked l;
        {
            std::scoped_lock lk{host.leak_lock};
            l = host.leaked_stack;
        }
        if (l.stack != nullptr && UnmapGuestStack(l.stack, l.size, l.guard)) {
            --host.leaked;
            --host.stacks;
        }
        Threads::Test::SetStackCacheLimits(defaults);
    }

    mark("9");
    // 10. Everything back: drained cache, the areas, private commit and handles of the start.
    Threads::Test::DrainStackCache();
    const auto s10 = Threads::GetStats();
    const size_t areas10 = memory.Areas().size();
    const s64 priv10 = private_bytes();
    const DWORD h10 = handle_count();
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    c.Check("afterwards: no stack held, the memory manager's areas as at the start, nothing rejected",
            s10.stacks_mapped == s0.stacks_mapped && s10.stacks_cached == 0 && areas10 == areas0 &&
                s10.rejected == 0 && host.leaked.load() == 0,
            fmt::format("held {} areas {}->{} rejected {} leaked {}", s10.stacks_mapped, areas0, areas10, s10.rejected,
                        host.leaked.load()));
    mark("10");
    const s64 outside_step8 = priv10 - priv0 - step_delta[0] - step_delta[1];
    c.Check("afterwards: private commit (outside the 1024-thread step) and handles back at the start (16 MiB, +8)",
            outside_step8 < (16ll << 20) && h10 <= h0 + 8,
            fmt::format("private {:+} KiB, handles {} -> {}", outside_step8 >> 10, h0, h10));
    c.Info("private commit per step:" + step_private);
    c.Info(fmt::format("{} threads in {:.1f} s; stacks mapped {}, unmapped {} ({} for the limit), reused {}, "
                       "peak held {} ({} KiB), peak cache {} KiB",
                       s10.created - s0.created, secs, s10.map_calls - s0.map_calls, s10.unmap_calls - s0.unmap_calls,
                       s10.evictions - s0.evictions, s10.reuses - s0.reuses, s10.peak_stacks_mapped,
                       s10.peak_stack_bytes >> 10, s10.peak_cache_bytes >> 10));
}

void TestSync(TestGroup& c) {
    c.Check("mutex/cond init", g.mutex_init(&g_mutex, nullptr, "t-mutex") == 0 &&
                                    g.cond_init(&g_cond, nullptr, "t-cond") == 0, "");
    g_counter = 0;
    std::vector<Handle> hs(kMutexThreads);
    for (auto& h : hs) {
        g.create(&h, nullptr, &MutexWorker, nullptr, "mutex");
    }
    for (auto h : hs) {
        g.join(h, nullptr);
    }
    c.Check("8 manager threads x 20000 locked increments", g_counter == static_cast<u64>(kMutexThreads) * kMutexLoops,
            fmt::format("{}", g_counter));

    // Broadcast wakes every waiter.
    g_counter = 0;
    g_ready = 0;
    g_woken = 0;
    constexpr int kWaiters = 4;
    std::vector<Handle> ws(kWaiters);
    for (auto& h : ws) {
        g.create(&h, nullptr, &CondWaiter, nullptr, "cond");
    }
    const bool all_waiting = WaitFor(5000, [] {
        g.mutex_lock(&g_mutex);
        const bool r = g_ready == kWaiters;
        g.mutex_unlock(&g_mutex);
        return r;
    });
    Sleep(20);
    g.mutex_lock(&g_mutex);
    g_counter = 1;
    const s32 b = g.cond_broadcast(&g_cond);
    g.mutex_unlock(&g_mutex);
    for (auto h : ws) {
        g.join(h, nullptr);
    }
    c.Check("broadcast wakes all 4 waiters", all_waiting && b == 0 && g_woken == kWaiters,
            fmt::format("woken {}", g_woken));

    // Timed wait with nothing signalled: ETIMEDOUT after at least the time asked.
    g.mutex_lock(&g_mutex);
    const auto t0 = std::chrono::steady_clock::now();
    const s32 tw = g.cond_timedwait(&g_cond, &g_mutex, 30'000);
    const double waited = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    g.mutex_unlock(&g_mutex);
    c.Check("timed wait: ETIMEDOUT after >= 30 ms", tw == kEtimedout && waited >= 29.0,
            fmt::format("{} after {:.1f} ms", Hex(tw), waited));
    c.Check("mutex/cond destroy", g.cond_destroy(&g_cond) == 0 && g.mutex_destroy(&g_mutex) == 0, "");
}

// ------------------------------------------------------------------------- P3.5.1: the guest errno

using ErrorFn = s32*(SSPC_SYSV_ABI*)();
ErrorFn g_error = nullptr; // SecondSonPC's __error (runtime/kernel/guest_errno.h), called as the game calls it
constexpr int kErrnoThreads = 8;
std::atomic<int> g_errno_written{0};
s32* g_errno_cells[kErrnoThreads];

/// arg: the thread's index. Result bits: 1 not 0 at the start, 2 another address later, 4 the value changed under it.
SSPC_SYSV_ABI void* ErrnoWorker(void* arg) {
    const auto index = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
    const s32 mine = 1000 + index;
    s32* const cell = g_error();
    g_errno_cells[index] = cell;
    uintptr_t bad = *cell != 0 ? 1 : 0;
    *cell = mine;
    g_errno_written.fetch_add(1);
    WaitFor(5000, [] { return g_errno_written.load() == kErrnoThreads; }); // every thread has written its own
    for (int i = 0; i < 20000 && bad == 0; ++i) {
        s32* const again = g_error();
        if (again != cell) {
            bad |= 2;
        } else if (*again != mine) {
            bad |= 4;
        }
        *again = mine;
    }
    *cell = 0x5A5A; // left behind at exit: no later thread may see it
    return reinterpret_cast<void*>(bad);
}

/// 1: the thread's errno was 0 at its start.
SSPC_SYSV_ABI void* ErrnoFresh(void*) {
    s32* const cell = g_error();
    const bool fresh = *cell == 0;
    *cell = 0x5A5A;
    return reinterpret_cast<void*>(static_cast<uintptr_t>(fresh ? 1 : 0));
}

void TestGuestErrno(TestGroup& c) {
    for (const auto& e : Kernel::GuestErrno::Exports()) {
        if (std::string_view(e.name) == "__error") {
            g_error = reinterpret_cast<ErrorFn>(e.function);
        }
    }
    if (g_error == nullptr) {
        c.Check("SecondSonPC's __error export", false, "missing");
        return;
    }
    s32* const own = g_error();
    const s32 own_before = *own;
    g_errno_written = 0;
    std::vector<Handle> hs(kErrnoThreads);
    for (int i = 0; i < kErrnoThreads; ++i) {
        g.create(&hs[i], nullptr, &ErrnoWorker, reinterpret_cast<void*>(static_cast<uintptr_t>(i)), "errno");
    }
    uintptr_t bad = 0;
    for (auto h : hs) {
        void* r = nullptr;
        g.join(h, &r);
        bad |= reinterpret_cast<uintptr_t>(r);
    }
    std::vector<s32*> cells(g_errno_cells, g_errno_cells + kErrnoThreads);
    std::ranges::sort(cells);
    const bool distinct = std::ranges::adjacent_find(cells) == cells.end() &&
                          std::ranges::find(cells, own) == cells.end();
    c.Check("8 manager threads at once: each starts at 0, keeps one cell (its own address) for its life and sees only "
            "its own value through 20000 calls",
            bad == 0 && distinct, fmt::format("result bits {:#x}, distinct cells {}", bad, distinct));

    u32 fresh = 0;
    for (int i = 0; i < 64; ++i) {
        Handle h = nullptr;
        void* r = nullptr;
        g.create(&h, nullptr, &ErrnoFresh, nullptr, "errno-reuse");
        g.join(h, &r);
        fresh += r != nullptr ? 1 : 0;
    }
    c.Check("64 manager threads one after another, each after one that left 0x5A5A behind: each starts at 0",
            fresh == 64, fmt::format("{}/64", fresh));
    c.Check("the creating thread's errno is untouched by its threads", g_error() == own && *own == own_before,
            fmt::format("{} -> {}", own_before, *own));
}

} // namespace

std::vector<TestGroup> TestNativeThreads(bool guest_stacks) {
    std::vector<TestGroup> groups;
    TestGroup c;
    c.name = "NativeThreads";
    if (!LoadApi()) {
        c.Check("exports", false, "a thread/mutex export is missing");
        groups.push_back(std::move(c));
        return groups;
    }
    Threads::SetThreadHost(&Host());
    c.Info(fmt::format("guest stacks from the {} memory manager",
                       guest_stacks ? "SecondSonPC (--native memory)" : "bridge's"));

    // Hang watchdog: a test that never ends fails the self-test instead of hanging it.
    static std::atomic<bool> done{false};
    std::thread([] {
        for (int i = 0; i < 1200 && !done; ++i) {
            Sleep(100);
        }
        if (!done) {
            SSPC_LOG(App, Critical, "NativeThreads self-test: no progress for 120 s (a deadlock?), stopping");
            Log::Flush();
            std::quick_exit(5);
        }
    }).detach();

    TestGroup attrs{.name = "NativeThreads(attr)"};
    TestAttributes(attrs);
    groups.push_back(std::move(attrs));

    TestLifecycle(c);
    TestPriorityAffinity(c);
    groups.push_back(std::move(c));

    TestGroup keys{.name = "NativeThreads(tls)"};
    TestKeysOnce(keys);
    groups.push_back(std::move(keys));

    // Thousands of threads below: without the line per created / released thread.
    const bool event_log = Threads::Test::SetEventLog(false);
    TestGroup stress{.name = "NativeThreads(mt)"};
    TestStress(stress);
    TestFailures(stress);
    groups.push_back(std::move(stress));

    TestGroup stacks{.name = "NativeThreads(stacks)"};
    TestStacks(stacks);
    groups.push_back(std::move(stacks));

    TestGroup err{.name = "NativeThreads(errno)"}; // P3.5.1
    TestGuestErrno(err);
    groups.push_back(std::move(err));
    Threads::Test::SetEventLog(event_log);

    TestGroup sync{.name = "NativeThreads(sync)"};
    TestSync(sync);

    // Threads still running when the process ends (as the game's own threads at exit): left to the
    // system, nothing waits for them.
    static HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (int i = 0; i < 4; ++i) {
        Handle h = nullptr;
        g.create(&h, nullptr, &WaitEvent, never, "left-running");
    }
    const bool all_started = WaitFor(5000, [] { return Host().started.load() == Threads::GetStats().created; });
    const auto counts = Threads::GetStats();
    sync.Info(fmt::format("4 threads left running at exit; manager: created {}, reaped {}, slots in use {}, "
                          "stacks mapped {}; host: started {} exited {}",
                          counts.created, counts.reaped, counts.slots_in_use, counts.stacks_mapped,
                          Host().started.load(), Host().exiting.load()));
    sync.Check("every thread started; exits seen by the host + live threads = created",
               all_started && Host().exiting.load() + counts.slots_in_use == counts.created,
               fmt::format("started {} exiting {} created {} in use {}", Host().started.load(),
                           Host().exiting.load(), counts.created, counts.slots_in_use));
    groups.push_back(std::move(sync));
    done = true;
    return groups;
}

} // namespace SecondSon::Bridge
