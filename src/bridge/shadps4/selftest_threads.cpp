// SecondSonPC — self-test of the native guest thread manager (P2.4). See selftest_threads.h.
// Every check calls the manager through the exports the game binds (System V), from "guest" test
// functions running on manager threads, and compares with the oracle's documented results
// (third_party/shadPS4/src/core/libraries/kernel/threads/*.cpp) or with exact counts.
#include "bridge/shadps4/selftest_threads.h"

#include <windows.h>

#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
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
#include "core/guest_memory.h"
#include "runtime/exports.h"
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

/// Guest stacks from the selected guest memory manager (the bridge's or SecondSonPC's), TCBs of
/// the test (self pointer and thread at the libkernel offsets 0x00/0x10). Failures can be injected.
class TestHost final : public Threads::ThreadHost {
public:
    void* MapStack(u64 size, u64 guard) override {
        if (fail_map.exchange(false)) {
            return nullptr;
        }
        auto& memory = Core::GetGuestVirtualMemory();
        void* addr = nullptr;
        if (memory.Map(&addr, {.addr = 0x7E0000000ULL,
                               .size = size + guard,
                               .prot = Core::MemoryProt::CpuReadWrite,
                               .flags = Core::MemoryMapFlags::NoFlags,
                               .type = Core::VMAType::Stack}) != 0) {
            return nullptr;
        }
        const auto base = reinterpret_cast<VAddr>(addr);
        if (guard != 0 && memory.Protect(base, guard, Core::MemoryProt::NoAccess) != 0) {
            memory.Unmap(base, size + guard);
            return nullptr;
        }
        ++stacks;
        last_stack = reinterpret_cast<void*>(base + guard);
        return last_stack.load();
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

    std::atomic<s64> stacks{0}, tcbs{0};
    std::atomic<u64> started{0}, exiting{0};
    std::atomic<bool> fail_map{false}, fail_tcb{false};
    std::atomic<void*> last_stack{nullptr};
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

    // The guard below the stack: no access.
    {
        Probe q;
        Handle hq = nullptr;
        g.create(&hq, nullptr, &ProbeEntry, &q, "guard");
        MEMORY_BASIC_INFORMATION mbi{};
        const void* below = static_cast<u8*>(host.last_stack.load()) - 1;
        const bool queried = VirtualQuery(below, &mbi, sizeof(mbi)) == sizeof(mbi);
        g.join(hq, nullptr);
        c.Check("16 KiB no-access guard below the stack", queried && mbi.Protect == PAGE_NOACCESS,
                fmt::format("protect {:#x}", mbi.Protect));
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
    const auto before = Threads::Test::GetCounts();
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
        return Threads::Test::GetCounts().slots_in_use == before.slots_in_use;
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
        return Threads::Test::GetCounts().slots_in_use == before.slots_in_use;
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
    const auto base = Threads::Test::GetCounts();
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
    const auto after = Threads::Test::GetCounts();
    c.Check("1000 create+join in a row", ok == kCycles, fmt::format("{} ok", ok));
    c.Check("no leaked slots, stacks, TCBs", after.slots_in_use == base.slots_in_use &&
                                                 after.stacks_mapped == base.stacks_mapped &&
                                                 host.tcbs.load() == static_cast<s64>(base.slots_in_use),
            fmt::format("slots {}->{}, stacks {}->{}, tcbs {}", base.slots_in_use, after.slots_in_use,
                        base.stacks_mapped, after.stacks_mapped, host.tcbs.load()));
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
    const auto after2 = Threads::Test::GetCounts();
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
        return Threads::Test::GetCounts().slots_in_use == base.slots_in_use;
    });
    for (HANDLE ev : events) {
        CloseHandle(ev);
    }
    g.attr_destroy(&attr);
    const auto after3 = Threads::Test::GetCounts();
    c.Check("300 detached threads ending in random order are all reaped", all_reaped,
            fmt::format("slots {} pending {}", after3.slots_in_use, after3.pending_reap));
    c.Check("stacks are kept and reused: mapped = in use + kept, never more than the peak",
            after3.stacks_mapped == static_cast<u64>(host.stacks.load()) &&
                after3.stacks_cached == after3.stacks_mapped - after3.slots_in_use && after3.stacks_mapped <= 300 + 40,
            fmt::format("mapped {} kept {} in use {}", after3.stacks_mapped, after3.stacks_cached,
                        after3.slots_in_use));
}

void TestFailures(TestGroup& c) {
    auto& host = Host();
    // A stack size no kept stack has, so that the manager maps a new one.
    void* attr = nullptr;
    g.attr_init(&attr);
    g.attr_setstacksize(&attr, 2u << 20);
    const auto base = Threads::Test::GetCounts();
    host.fail_map = true;
    Handle h = reinterpret_cast<Handle>(1);
    const s32 r1 = g.create(&h, &attr, &ReturnArgPlusOne, nullptr, "no-stack");
    const auto c1 = Threads::Test::GetCounts();
    c.Check("no memory for the stack -> EAGAIN, nothing left behind",
            r1 == kEagain && c1.slots_in_use == base.slots_in_use && c1.stacks_mapped == base.stacks_mapped,
            Hex(r1));
    host.fail_tcb = true;
    const s32 r2 = g.create(&h, &attr, &ReturnArgPlusOne, nullptr, "no-tcb");
    const auto c2 = Threads::Test::GetCounts();
    c.Check("no TCB -> EAGAIN, its new stack kept for reuse, no slot taken",
            r2 == kEagain && c2.slots_in_use == base.slots_in_use && c2.stacks_mapped == base.stacks_mapped + 1 &&
                c2.stacks_cached == base.stacks_cached + 1,
            fmt::format("{} mapped {} kept {}", Hex(r2), c2.stacks_mapped, c2.stacks_cached));
    // The kept 2 MiB stack serves the next thread of that size.
    void* ret = nullptr;
    const s32 r3 = g.create(&h, &attr, &ReturnArgPlusOne, reinterpret_cast<void*>(9), "reuse");
    const bool joined = r3 == 0 && g.join(h, &ret) == 0;
    const auto c3 = Threads::Test::GetCounts();
    c.Check("a kept stack is reused (no new mapping)", joined && ret == reinterpret_cast<void*>(10) &&
                                                           c3.stacks_mapped == c2.stacks_mapped,
            fmt::format("mapped {}", c3.stacks_mapped));
    g.attr_destroy(&attr);
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

    TestGroup stress{.name = "NativeThreads(mt)"};
    TestStress(stress);
    TestFailures(stress);
    groups.push_back(std::move(stress));

    TestGroup sync{.name = "NativeThreads(sync)"};
    TestSync(sync);

    // Threads still running when the process ends (as the game's own threads at exit): left to the
    // system, nothing waits for them.
    static HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (int i = 0; i < 4; ++i) {
        Handle h = nullptr;
        g.create(&h, nullptr, &WaitEvent, never, "left-running");
    }
    const bool all_started = WaitFor(5000, [] { return Host().started.load() == Threads::Test::GetCounts().created; });
    const auto counts = Threads::Test::GetCounts();
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
