// SecondSonPC — native guest threads (Phase 2, P2.4). See thread.h and
// docs/PHASE2_P24_NATIVE_THREADS.md. The oracle is the bridge's thread manager
// (third_party/shadPS4/src/core/libraries/kernel/threads/{pthread,pthread_attr,pthread_spec,stack}.cpp):
// the same attribute defaults, ranges and error numbers; where it is unsafe this file differs, and
// says so.
//
// A guest thread is a slot of a table that is never freed: the ScePthread the game holds is the
// slot's address, so a stale handle can never point at freed memory, and slots come back into use in
// FIFO order (as late as possible). A thread's resources — its TCB and host thread handle freed,
// its guest stack kept for the next thread of the same size — are released only once its host
// thread has ended (the joiner waits for the handle; a detached thread is reaped by the next
// scePthreadCreate), never while it may still run on them (the bridge returns the stack of an
// exiting thread to its cache before the thread is gone).
// Priorities and affinity are kept and reported but not applied to Windows threads (as the oracle);
// there is no cancellation and no signal delivery to guest threads (the game imports neither).
#include <windows.h>

#include <xmmintrin.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/log.h"
#include "runtime/exports.h"
#include "runtime/kernel/pthread_sync.h"
#include "runtime/kernel/thread.h"

// Runs `entry(arg)` (System V ABI) with `stack` as its stack pointer and returns what it returns.
// The TEB's stack bounds (gs:0x08 / gs:0x10) are cleared for the call: the guest stack is not the
// one Windows gave the thread, and stack probes must not check against that one. The same as the
// bridge's _runOnAnotherStack (threads/stack_asm.cpp).
extern "C" void* SSPC_SYSV_ABI sspc_run_on_guest_stack(void* arg, void* entry, void* stack);
asm(R"(
.att_syntax prefix
.text
.global sspc_run_on_guest_stack
sspc_run_on_guest_stack:
  pushq %r12
  pushq %r13
  pushq %r14
  pushq %r15
  movq %gs:0x08, %r14
  movq %gs:0x10, %r15
  xorq %rcx, %rcx
  movq %rcx, %gs:0x08
  movq %rcx, %gs:0x10
  movq %rsp, %r12
  movq %rbp, %r13
  movq %rdx, %rsp
  movq %rdx, %rbp
  callq *%rsi
  movq %r13, %rbp
  movq %r12, %rsp
  movq %r14, %gs:0x08
  movq %r15, %gs:0x10
  popq %r15
  popq %r14
  popq %r13
  popq %r12
  ret
)");

namespace SecondSon::Kernel::Threads {

namespace {

using PthreadSync::Sce;

// POSIX error numbers (FreeBSD).
constexpr s32 kOk = 0;
constexpr s32 kEsrch = 3;
constexpr s32 kEdeadlk = 11;
constexpr s32 kEnomem = 12;
constexpr s32 kEinval = 22;
constexpr s32 kEagain = 35;
constexpr s32 kEnotsup = 45;

// Attributes (the oracle's PthreadAttrFlags, SchedPolicy, ThrPriorities, PthreadAttrDefault).
constexpr u32 kAttrDetached = 0x1;
constexpr u32 kAttrScopeSystem = 0x2;
constexpr s32 kPolicyFifo = 1;
constexpr s32 kPolicyOther = 2;
constexpr s32 kPolicyRoundRobin = 3;
constexpr s32 kInheritSched = 4;
constexpr s32 kExplicitSched = 0;
constexpr s32 kPrioFifoMin = 0x100;
constexpr s32 kPrioFifoMax = 0x2FF;
constexpr s32 kPrioFifoDefault = 0x2BC; // 700
constexpr s32 kPrioOtherDefault = 0x384;
constexpr u64 kStackDefault = 1ull << 20;
constexpr u64 kStackMin = 16 * 1024;
constexpr u64 kPage = 16 * 1024;
// The oracle adds this much to every stack it maps "for HLE": native functions the guest calls run
// on the guest stack.
constexpr u64 kExtraStack = 128 * 1024;
// No-access memory below every stack this manager maps (SecondSonPC; the oracle has none): an
// overflow faults instead of writing into the neighbouring stack.
constexpr u64 kStackGuard = 16 * 1024;
// Larger requests are refused (EAGAIN) before the size arithmetic could wrap.
constexpr u64 kMaxStackSize = 1ull << 30;

constexpr u32 kAttrMagic = 0x41545452;   // "RTTA"
constexpr u32 kThreadMagic = 0x44524854; // "THRD"
constexpr size_t kMaxThreads = 1024;
constexpr u32 kKeysMax = 256;
constexpr u32 kDestructorIterations = 4;
constexpr size_t kNameMax = 63;

// The floating point state libkernel gives every thread (the oracle's ORBIS_MXCSR/ORBIS_FPUCW).
constexpr u32 kMxcsr = 0x9fc0;
constexpr u16 kFpucw = 0x037f;

/// The object an ScePthreadAttr points to (allocated by scePthreadAttrInit; the guest keeps the
/// pointer). `magic` refuses a pointer this manager did not create.
struct Attr {
    u32 magic = kAttrMagic;
    s32 policy = kPolicyFifo;
    s32 inherit = kInheritSched;
    s32 prio = kPrioFifoDefault;
    u32 flags = kAttrScopeSystem;
    void* stackaddr = nullptr;
    u64 stacksize = kStackDefault;
    u64 guardsize = 0;
    bool has_cpuset = false;
    u64 cpuset = 0;
};
using AttrT = Attr*;

struct SchedParam {
    s32 sched_priority;
};

using Entry = void*(SSPC_SYSV_ABI*)(void*);
using KeyDestructor = void(SSPC_SYSV_ABI*)(void*);
using OnceRoutine = void(SSPC_SYSV_ABI*)();

enum class State : u8 { Free, Running, Exited };

struct alignas(64) Thread {
    std::mutex lock;
    std::condition_variable exited;
    u32 magic = 0; // kThreadMagic while the slot holds a thread
    State state = State::Free;
    bool detached = false;
    bool joining = false;
    Attr attr{};
    std::string name;
    Entry entry = nullptr;
    void* arg = nullptr;
    std::function<void()> host_body; // the main thread: SecondSonPC's loader code
    void* exit_value = nullptr;
    HANDLE host = nullptr;
    u32 tid = 0;
    void* stack = nullptr; // usable part (the guard is below)
    u64 stack_size = 0;
    u64 stack_guard = 0;
    bool own_stack = false;
    void* tcb = nullptr;
};

Thread g_threads[kMaxThreads];

std::mutex g_table_lock; // g_free*, g_pending, g_stacks, g_table_ready, the counters below
// Free slots, FIFO. Fixed storage (and g_pending/g_stacks reserved once): an exiting or detaching
// guest thread never allocates, so it cannot throw (the bridge's VEH ends the process on any C++
// exception) when the system commit limit is reached.
std::array<u32, kMaxThreads> g_free{};
u32 g_free_head = 0;
u32 g_free_count = 0;
std::vector<u32> g_pending; // detached threads that exited: reaped once their host thread is gone
// Stacks of gone threads, kept for the next thread of the same size: never unmapped (the bridge's
// memory manager asserts when a stack whose guard split its area is unmapped, D17; the bridge itself
// keeps every stack it maps, too).
struct CachedStack {
    void* base;
    u64 size;
    u64 guard;
};
std::vector<CachedStack> g_stacks;
bool g_table_ready = false;
u64 g_created = 0;
u64 g_reaped = 0;
u64 g_stacks_mapped = 0;

ThreadHost* g_host = nullptr;
std::atomic<bool> g_enabled{false};
std::atomic<u32> g_next_tid{1};

thread_local Thread* t_self = nullptr;

// TLS keys (the oracle's PthreadKey table): a value is valid only while its seqno is the key's.
struct Key {
    std::atomic<u32> allocated{0};
    std::atomic<u32> seqno{0};
    std::atomic<KeyDestructor> destructor{nullptr};
};
std::array<Key, kKeysMax> g_keys;
std::mutex g_keys_lock;

struct Specific {
    struct Elem {
        u32 seqno = 0;
        void* value = nullptr;
    };
    std::array<Elem, kKeysMax> elems{};
    u32 count = 0; // non-null values
};
thread_local std::unique_ptr<Specific> t_specific;

u32 IndexOf(const Thread* t) {
    return static_cast<u32>(t - g_threads);
}

// g_free as a ring (g_table_lock held). A slot is never pushed twice: at most kMaxThreads entries.
void PushFree(u32 index) {
    g_free[(g_free_head + g_free_count) % kMaxThreads] = index;
    ++g_free_count;
}

u32 PopFree() {
    const u32 index = g_free[g_free_head];
    g_free_head = (g_free_head + 1) % kMaxThreads;
    --g_free_count;
    return index;
}

/// The slot an ScePthread names, or nullptr (not a slot address). The slot may be free: callers
/// check it with Live() under its lock.
Thread* FromHandle(const void* handle) {
    const auto p = reinterpret_cast<uintptr_t>(handle);
    const auto base = reinterpret_cast<uintptr_t>(&g_threads[0]);
    if (p < base || p >= base + sizeof(g_threads) || (p - base) % sizeof(Thread) != 0) {
        return nullptr;
    }
    return &g_threads[(p - base) / sizeof(Thread)];
}

bool Live(const Thread& t) {
    return t.magic == kThreadMagic && t.state != State::Free;
}

u64 RoundUp(u64 value, u64 align) {
    return (value + align - 1) / align * align;
}

Attr* AttrOf(const AttrT* attr) {
    if (attr == nullptr || *attr == nullptr || (*attr)->magic != kAttrMagic) {
        return nullptr;
    }
    return *attr;
}

std::string GuestName(const char* name, u32 tid) {
    if (name != nullptr) {
        std::string out;
        for (size_t i = 0; i < kNameMax; ++i) {
            const char* c = name + i;
            if ((i == 0 || (reinterpret_cast<uintptr_t>(c) & 0xfff) == 0) && !g_host->IsReadable(c, 1)) {
                out.clear();
                break;
            }
            if (*c == '\0') {
                break;
            }
            out.push_back(*c);
        }
        if (!out.empty()) {
            return out;
        }
    }
    return "Thread" + std::to_string(tid);
}

void SetHostThreadName(const std::string& name) {
    const std::wstring wide(name.begin(), name.end());
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

/// Frees what a thread used and returns its slot. Its host thread has ended (or never ran).
void Release(Thread* t) {
    void* tcb;
    void* stack;
    u64 size, guard;
    bool own;
    HANDLE host;
    {
        std::scoped_lock lk{t->lock};
        tcb = t->tcb;
        stack = t->stack;
        size = t->stack_size;
        guard = t->stack_guard;
        own = t->own_stack;
        host = t->host;
        t->magic = 0;
        t->state = State::Free;
        t->tcb = nullptr;
        t->stack = nullptr;
        t->own_stack = false;
        t->host = nullptr;
        t->host_body = nullptr;
        t->name.clear();
    }
    if (tcb != nullptr) {
        g_host->DestroyTcb(tcb);
    }
    if (host != nullptr) {
        CloseHandle(host);
    }
    std::scoped_lock lk{g_table_lock};
    if (own) {
        // Reserved for kMaxThreads; grows (allocates) only past that many distinct stacks kept.
        g_stacks.push_back({stack, size, guard});
    }
    PushFree(IndexOf(t));
    ++g_reaped;
}

/// A kept stack of exactly this size and guard, or nullptr.
void* TakeCachedStack(u64 size, u64 guard) {
    std::scoped_lock lk{g_table_lock};
    const auto it = std::ranges::find_if(g_stacks, [&](const CachedStack& s) { return s.size == size && s.guard == guard; });
    if (it == g_stacks.end()) {
        return nullptr;
    }
    void* base = it->base;
    g_stacks.erase(it);
    return base;
}

void ReapPending() {
    std::array<u32, kMaxThreads> gone; // no allocation on the guest thread
    u32 count = 0;
    {
        std::scoped_lock lk{g_table_lock};
        for (size_t i = 0; i < g_pending.size();) {
            if (WaitForSingleObject(g_threads[g_pending[i]].host, 0) == WAIT_OBJECT_0) {
                gone[count++] = g_pending[i];
                g_pending[i] = g_pending.back();
                g_pending.pop_back();
            } else {
                ++i;
            }
        }
    }
    for (u32 i = 0; i < count; ++i) {
        Release(&g_threads[gone[i]]);
    }
}

Thread* AllocSlot() {
    std::scoped_lock lk{g_table_lock};
    if (!g_table_ready) {
        for (u32 i = 0; i < kMaxThreads; ++i) {
            PushFree(i);
        }
        // Every later push_back stays within this (see g_free): one entry per slot at most.
        g_pending.reserve(kMaxThreads);
        g_stacks.reserve(kMaxThreads);
        g_table_ready = true;
    }
    if (g_free_count == 0) {
        return nullptr;
    }
    return &g_threads[PopFree()];
}

void FreeSlot(Thread* t) {
    std::scoped_lock lk{g_table_lock};
    PushFree(IndexOf(t));
}

/// Destructors of the calling thread's TLS values (the oracle's _thread_cleanupspecific): up to
/// four passes, a destructor runs with its value already cleared and no lock held.
void RunKeyDestructors() {
    Specific* spec = t_specific.get();
    if (spec == nullptr) {
        return;
    }
    for (u32 pass = 0; pass < kDestructorIterations && spec->count > 0; ++pass) {
        for (u32 key = 0; key < kKeysMax && spec->count > 0; ++key) {
            auto& elem = spec->elems[key];
            if (elem.value == nullptr) {
                continue;
            }
            void* value = elem.value;
            KeyDestructor destructor = nullptr;
            if (g_keys[key].allocated.load() != 0 && elem.seqno == g_keys[key].seqno.load()) {
                destructor = g_keys[key].destructor.load();
            }
            elem.value = nullptr;
            --spec->count;
            if (destructor != nullptr) {
                destructor(value);
            }
        }
    }
    if (spec->count > 0) {
        SSPC_LOG(Kernel, Warning, "thread {} exits with {} thread-specific values left after {} destructor passes",
                 t_self != nullptr ? t_self->name : std::string("?"), spec->count, kDestructorIterations);
    }
    t_specific.reset();
}

void AbandonRunningOnces();

[[noreturn]] void ExitCurrent(void* value) {
    Thread* t = t_self;
    AbandonRunningOnces();
    RunKeyDestructors();
    g_host->ThreadExiting();
    bool reap_later;
    {
        std::scoped_lock lk{t->lock};
        t->exit_value = value;
        t->state = State::Exited;
        reap_later = t->detached;
        t->exited.notify_all();
    }
    if (reap_later) {
        std::scoped_lock lk{g_table_lock};
        g_pending.push_back(IndexOf(t));
    }
    t_self = nullptr;
    ExitThread(0); // the joiner (or the reaper) frees the stack once this thread is gone
}

SSPC_SYSV_ABI void* RunHostBody(void* param) {
    static_cast<Thread*>(param)->host_body();
    return nullptr;
}

DWORD WINAPI ThreadEntry(void* param) {
    auto* t = static_cast<Thread*>(param);
    t_self = t;
    g_host->SetTcbBase(t->tcb);
    _mm_setcsr(kMxcsr);
    asm volatile("fldcw %0" : : "m"(kFpucw));
    SetHostThreadName(t->name);
    g_host->ThreadStarted();
    void* const top = reinterpret_cast<void*>(
        (reinterpret_cast<uintptr_t>(t->stack) + t->stack_size) & ~uintptr_t{15});
    void* const ret = t->host_body ? sspc_run_on_guest_stack(t, reinterpret_cast<void*>(&RunHostBody), top)
                                   : sspc_run_on_guest_stack(t->arg, reinterpret_cast<void*>(t->entry), top);
    ExitCurrent(ret);
}

/// The oracle's posix_pthread_create_name_np. POSIX error numbers.
s32 Create(Thread** out, const AttrT* attrp, Entry entry, void* arg, const char* name,
           std::function<void()> host_body, bool initial) {
    if (out == nullptr || (entry == nullptr && !host_body) || g_host == nullptr) {
        return kEinval;
    }
    Attr attr{};
    if (attrp != nullptr && *attrp != nullptr) {
        if ((*attrp)->magic != kAttrMagic) {
            return kEinval; // not an attribute object of this manager
        }
        attr = **attrp;
    }
    if (Thread* self = t_self; self != nullptr && attr.inherit == kInheritSched) {
        std::scoped_lock lk{self->lock};
        attr.flags = (attr.flags & ~kAttrScopeSystem) | (self->attr.flags & kAttrScopeSystem);
        attr.prio = self->attr.prio;
        attr.policy = self->attr.policy;
    }

    if (attr.stackaddr == nullptr && attr.stacksize > kMaxStackSize) {
        // stacksize + kExtraStack would wrap; the oracle fails the same way (no memory for it).
        SSPC_LOG(Kernel, Error, "scePthreadCreate: stack of {:#x} bytes refused", attr.stacksize);
        return kEagain;
    }
    ReapPending();
    Thread* t = AllocSlot();
    if (t == nullptr) {
        SSPC_LOG(Kernel, Error, "scePthreadCreate: all {} thread slots are in use", kMaxThreads);
        return kEagain;
    }

    void* stack = attr.stackaddr;
    u64 stack_size = attr.stacksize;
    u64 guard = 0;
    const bool own = stack == nullptr;
    if (own) {
        stack_size = RoundUp(attr.stacksize + kExtraStack, kPage);
        guard = RoundUp(std::max(attr.guardsize, kStackGuard), kPage);
        stack = TakeCachedStack(stack_size, guard);
        if (stack != nullptr) {
            std::memset(stack, 0, stack_size); // as a fresh mapping (the oracle clears reused stacks too)
        } else {
            stack = g_host->MapStack(stack_size, guard);
            if (stack == nullptr) {
                FreeSlot(t);
                SSPC_LOG(Kernel, Error, "scePthreadCreate: no guest memory for a {:#x}-byte stack", stack_size);
                return kEagain;
            }
            std::scoped_lock lk{g_table_lock};
            ++g_stacks_mapped;
        }
        attr.stacksize = stack_size; // what scePthreadAttrGet reports (as the oracle)
    }
    const u32 tid = g_next_tid.fetch_add(1) + 1;
    {
        std::scoped_lock lk{t->lock};
        t->attr = attr;
        t->name = GuestName(name, tid);
        t->entry = entry;
        t->arg = arg;
        t->host_body = std::move(host_body);
        t->exit_value = nullptr;
        t->detached = (attr.flags & kAttrDetached) != 0;
        t->joining = false;
        t->tid = tid;
        t->stack = stack;
        t->stack_size = stack_size;
        t->stack_guard = guard;
        t->own_stack = own;
        t->tcb = nullptr;
        t->host = nullptr;
    }
    void* tcb = g_host->CreateTcb(t, initial);
    if (tcb == nullptr) {
        SSPC_LOG(Kernel, Error, "scePthreadCreate: no TCB for thread {}", tid);
        Release(t); // keeps the stack for reuse, returns the slot
        return kEagain;
    }
    {
        std::scoped_lock lk{t->lock};
        t->tcb = tcb;
        t->magic = kThreadMagic;
        t->state = State::Running;
    }
    *out = t; // before the thread runs: it may read what its creator stored (as the oracle)
    HANDLE host = CreateThread(nullptr, 0, ThreadEntry, t, CREATE_SUSPENDED, nullptr);
    if (host == nullptr) {
        const DWORD error = GetLastError();
        *out = nullptr;
        Release(t);
        SSPC_LOG(Kernel, Error, "scePthreadCreate: CreateThread failed ({})", error);
        return kEagain;
    }
    bool detached;
    {
        std::scoped_lock lk{t->lock};
        t->host = host;
        detached = t->detached; // another guest thread may detach it already (it has the handle)
    }
    {
        std::scoped_lock lk{g_table_lock};
        ++g_created;
    }
    // t->name is written only here and by Release (not before this thread is joined or reaped).
    SSPC_LOG(Kernel, Debug, "thread {} \"{}\" created: prio {} policy {} stack {:#x}+{:#x}{}", tid,
             t->name, attr.prio, attr.policy, reinterpret_cast<uintptr_t>(stack), stack_size,
             detached ? " detached" : "");
    ResumeThread(host);
    return kOk;
}

s32 Join(void* handle, void** value) {
    if (handle == nullptr) {
        return kEinval;
    }
    if (handle == t_self) {
        return kEdeadlk;
    }
    Thread* t = FromHandle(handle);
    if (t == nullptr) {
        return kEsrch;
    }
    void* result;
    HANDLE host;
    {
        std::unique_lock lk{t->lock};
        if (!Live(*t)) {
            return kEsrch;
        }
        if (t->detached) {
            return kEinval;
        }
        if (t->joining) {
            return kEnotsup; // one joiner (as the oracle)
        }
        t->joining = true;
        t->exited.wait(lk, [t] { return t->state == State::Exited; });
        result = t->exit_value;
        host = t->host;
    }
    // Its stack, TCB and handle are freed only once the host thread is really gone.
    WaitForSingleObject(host, INFINITE);
    Release(t);
    if (value != nullptr) {
        *value = result;
    }
    return kOk;
}

s32 Detach(void* handle) {
    if (handle == nullptr) {
        return kEinval;
    }
    Thread* t = FromHandle(handle);
    if (t == nullptr) {
        return kEsrch;
    }
    bool exited;
    {
        std::scoped_lock lk{t->lock};
        if (!Live(*t)) {
            return kEsrch;
        }
        if (t->detached || t->joining) {
            return kEinval;
        }
        t->detached = true;
        exited = t->state == State::Exited;
    }
    if (exited) {
        std::scoped_lock lk{g_table_lock};
        g_pending.push_back(IndexOf(t));
    }
    return kOk;
}

/// Runs `f` on a running thread under its lock; ESRCH otherwise (an exited thread counts as gone:
/// the oracle's FindThread(include_dead = false)).
template <typename F>
s32 WithThread(void* handle, F&& f) {
    Thread* t = FromHandle(handle);
    if (t == nullptr) {
        return kEsrch;
    }
    std::scoped_lock lk{t->lock};
    if (!Live(*t) || t->state != State::Running) {
        return kEsrch;
    }
    return f(*t);
}

// ------------------------------------------------------------------------------- attributes

s32 AttrInit(AttrT* attr) {
    if (attr == nullptr) {
        return kEinval;
    }
    auto* a = new (std::nothrow) Attr{};
    if (a == nullptr) {
        return kEnomem;
    }
    *attr = a;
    return kOk;
}

s32 AttrDestroy(AttrT* attr) {
    Attr* a = AttrOf(attr);
    if (a == nullptr) {
        return kEinval;
    }
    a->magic = 0;
    delete a;
    *attr = nullptr;
    return kOk;
}

s32 AttrSetdetachstate(AttrT* attr, s32 state) {
    Attr* a = AttrOf(attr);
    if (a == nullptr || (state != 0 && state != 1)) {
        return kEinval;
    }
    a->flags = state != 0 ? a->flags | kAttrDetached : a->flags & ~kAttrDetached;
    return kOk;
}

s32 AttrSetinheritsched(AttrT* attr, s32 inherit) {
    Attr* a = AttrOf(attr);
    if (a == nullptr) {
        return kEinval;
    }
    if (inherit != kInheritSched && inherit != kExplicitSched) {
        return kEnotsup;
    }
    a->inherit = inherit;
    return kOk;
}

s32 AttrSetschedparam(AttrT* attr, const SchedParam* param) {
    Attr* a = AttrOf(attr);
    if (a == nullptr) {
        return kEinval;
    }
    if (param == nullptr) {
        return kEnotsup;
    }
    if ((a->policy == kPolicyFifo || a->policy == kPolicyRoundRobin) &&
        (param->sched_priority < kPrioFifoMin || param->sched_priority > kPrioFifoMax)) {
        return kEnotsup;
    }
    a->prio = param->sched_priority;
    return kOk;
}

s32 AttrGetschedparam(const AttrT* attr, SchedParam* param) {
    Attr* a = AttrOf(attr);
    if (a == nullptr || param == nullptr) {
        return kEinval;
    }
    param->sched_priority = a->prio;
    return kOk;
}

s32 AttrSetschedpolicy(AttrT* attr, s32 policy) {
    Attr* a = AttrOf(attr);
    if (a == nullptr) {
        return kEinval;
    }
    if (policy < kPolicyFifo || policy > kPolicyRoundRobin) {
        return kEnotsup;
    }
    a->policy = policy;
    a->prio = policy == kPolicyOther ? kPrioOtherDefault : kPrioFifoDefault;
    return kOk;
}

s32 AttrSetstacksize(AttrT* attr, u64 size) {
    Attr* a = AttrOf(attr);
    if (a == nullptr || size < kStackMin) {
        return kEinval;
    }
    a->stacksize = size;
    return kOk;
}

s32 AttrSetaffinity(AttrT* attr, u64 mask) {
    Attr* a = AttrOf(attr);
    if (a == nullptr) {
        return kEinval;
    }
    a->has_cpuset = true;
    a->cpuset = mask;
    return kOk;
}

s32 AttrGetaffinity(const AttrT* attr, u64* mask) {
    Attr* a = AttrOf(attr);
    if (a == nullptr || mask == nullptr) {
        return kEinval;
    }
    *mask = a->has_cpuset ? a->cpuset : ~0ull; // none set: every CPU (as the oracle)
    return kOk;
}

/// The oracle's posix_pthread_attr_get_np: the thread's attributes into an initialised object.
s32 AttrGet(void* handle, AttrT* attr) {
    Attr* dst = AttrOf(attr);
    if (handle == nullptr || dst == nullptr) {
        return kEinval;
    }
    return WithThread(handle, [dst](Thread& t) {
        *dst = t.attr;
        if (t.detached) {
            dst->flags |= kAttrDetached;
        }
        return kOk;
    });
}

// ------------------------------------------------------------------------------- threads

s32 Getprio(void* handle, s32* prio) {
    if (prio == nullptr) {
        return kEinval;
    }
    return WithThread(handle, [prio](Thread& t) {
        *prio = t.attr.prio;
        return kOk;
    });
}

s32 Setprio(void* handle, s32 prio) {
    // Stored for scePthreadGetprio and the priority-ordered waits; Windows priorities are not
    // changed (the oracle maps none either).
    return WithThread(handle, [prio](Thread& t) {
        t.attr.prio = prio;
        return kOk;
    });
}

s32 Getaffinity(void* handle, u64* mask) {
    if (handle == nullptr || mask == nullptr) {
        return kEinval;
    }
    return WithThread(handle, [mask](Thread& t) {
        *mask = t.attr.has_cpuset ? t.attr.cpuset : ~0ull;
        return kOk;
    });
}

s32 Setaffinity(void* handle, u64 mask) {
    if (handle == nullptr) {
        return kEinval;
    }
    // Kept, not applied: the oracle does not pin guest threads either (its SetAffinity is off).
    return WithThread(handle, [mask](Thread& t) {
        t.attr.has_cpuset = true;
        t.attr.cpuset = mask;
        return kOk;
    });
}

// ------------------------------------------------------------------------------- once, keys

enum OnceState : u32 { kNeverDone = 0, kDone = 1, kInProgress = 2, kWait = 3 };
/// The guest's pthread_once_t starts with its state word (then libthr's mutex pointer, unused).
struct OnceControl {
    std::atomic<u32> state;
};
static_assert(sizeof(std::atomic<u32>) == 4);

// Once routines running on this thread, innermost last (AbandonRunningOnces). Deeper nesting than
// kOnceDepth is counted but not tracked.
constexpr u32 kOnceDepth = 8;
thread_local OnceControl* t_once[kOnceDepth];
thread_local u32 t_once_depth = 0;

s32 Once(OnceControl* once, OnceRoutine routine) {
    if (once == nullptr || routine == nullptr) {
        return kEinval;
    }
    auto& state = once->state;
    for (;;) {
        u32 s = state.load(std::memory_order_acquire);
        if (s == kDone) {
            return kOk;
        }
        if (s == kNeverDone) {
            if (state.compare_exchange_strong(s, kInProgress, std::memory_order_acquire)) {
                break;
            }
        } else if (s == kInProgress) {
            if (state.compare_exchange_strong(s, kWait, std::memory_order_acquire)) {
                state.wait(kWait);
            }
        } else if (s == kWait) {
            state.wait(kWait);
        } else {
            return kEinval;
        }
    }
    if (t_once_depth < kOnceDepth) {
        t_once[t_once_depth] = once;
    }
    ++t_once_depth;
    routine();
    --t_once_depth;
    u32 s = kInProgress;
    if (!state.compare_exchange_strong(s, kDone, std::memory_order_release)) {
        state.store(kDone, std::memory_order_release); // someone waits
        state.notify_all();
    }
    return kOk;
}

/// The thread exits inside once routines (scePthreadExit from one): as the oracle's cleanup
/// handler, each goes back to "never done" and its waiters wake (one of them runs the routine).
void AbandonRunningOnces() {
    for (u32 i = std::min(t_once_depth, kOnceDepth); i-- > 0;) {
        auto& state = t_once[i]->state;
        state.store(kNeverDone, std::memory_order_release);
        state.notify_all();
    }
    t_once_depth = 0;
}

s32 KeyCreate(u32* key, KeyDestructor destructor) {
    if (key == nullptr) {
        return kEinval;
    }
    std::scoped_lock lk{g_keys_lock};
    for (u32 i = 0; i < kKeysMax; ++i) {
        if (g_keys[i].allocated.load() == 0) {
            g_keys[i].destructor.store(destructor);
            g_keys[i].seqno.fetch_add(1);
            g_keys[i].allocated.store(1);
            *key = i;
            return kOk;
        }
    }
    return kEagain;
}

s32 KeyDelete(u32 key) {
    if (key >= kKeysMax) {
        return kEinval;
    }
    std::scoped_lock lk{g_keys_lock};
    if (g_keys[key].allocated.load() == 0) {
        return kEinval;
    }
    g_keys[key].allocated.store(0);
    return kOk;
}

void* Getspecific(u32 key) {
    const Specific* spec = t_specific.get();
    if (spec == nullptr || key >= kKeysMax) {
        return nullptr;
    }
    const auto& elem = spec->elems[key];
    if (g_keys[key].allocated.load() != 0 && elem.seqno == g_keys[key].seqno.load()) {
        return elem.value;
    }
    return nullptr;
}

s32 Setspecific(u32 key, const void* value) {
    if (key >= kKeysMax || g_keys[key].allocated.load() == 0) {
        return kEinval;
    }
    if (!t_specific) {
        t_specific.reset(new (std::nothrow) Specific{});
        if (!t_specific) {
            return kEnomem;
        }
    }
    auto& elem = t_specific->elems[key];
    if (elem.value == nullptr && value != nullptr) {
        ++t_specific->count;
    } else if (elem.value != nullptr && value == nullptr) {
        --t_specific->count;
    }
    elem.value = const_cast<void*>(value);
    elem.seqno = g_keys[key].seqno.load();
    return kOk;
}

// ------------------------------------------------------------------------------- exports

SSPC_SYSV_ABI void* PthreadSelf() {
    return t_self;
}
SSPC_SYSV_ABI s32 ScePthreadCreate(Thread** thread, const AttrT* attr, Entry entry, void* arg, const char* name) {
    return Sce(Create(thread, attr, entry, arg, name, nullptr, false));
}
SSPC_SYSV_ABI s32 PosixPthreadCreateNameNp(Thread** thread, const AttrT* attr, Entry entry, void* arg,
                                           const char* name) {
    return Create(thread, attr, entry, arg, name, nullptr, false);
}
SSPC_SYSV_ABI s32 ScePthreadJoin(void* thread, void** value) {
    return Sce(Join(thread, value));
}
SSPC_SYSV_ABI s32 PosixPthreadJoin(void* thread, void** value) {
    return Join(thread, value);
}
SSPC_SYSV_ABI s32 ScePthreadDetach(void* thread) {
    return Sce(Detach(thread));
}
SSPC_SYSV_ABI void ScePthreadExit(void* value) {
    if (t_self == nullptr) {
        // Not a native guest thread (with --native threads every guest thread is one).
        SSPC_LOG(Kernel, Critical, "scePthreadExit on host thread {}: ending it", GetCurrentThreadId());
        ExitThread(0);
    }
    ExitCurrent(value);
}
SSPC_SYSV_ABI s32 ScePthreadOnce(OnceControl* once, OnceRoutine routine) {
    return Sce(Once(once, routine));
}
SSPC_SYSV_ABI s32 ScePthreadGetprio(void* thread, s32* prio) {
    const s32 r = Getprio(thread, prio);
    // The oracle reports any failure of a valid call as ESRCH; a null pointer is EINVAL here.
    return r == kOk ? 0 : Sce(r == kEinval ? kEinval : kEsrch);
}
SSPC_SYSV_ABI s32 ScePthreadSetprio(void* thread, s32 prio) {
    return Sce(Setprio(thread, prio));
}
SSPC_SYSV_ABI s32 ScePthreadGetaffinity(void* thread, u64* mask) {
    return Sce(Getaffinity(thread, mask));
}
SSPC_SYSV_ABI s32 ScePthreadSetaffinity(void* thread, u64 mask) {
    return Sce(Setaffinity(thread, mask));
}
SSPC_SYSV_ABI s32 ScePthreadAttrInit(AttrT* attr) {
    return Sce(AttrInit(attr));
}
SSPC_SYSV_ABI s32 PosixPthreadAttrInit(AttrT* attr) {
    return AttrInit(attr);
}
SSPC_SYSV_ABI s32 ScePthreadAttrDestroy(AttrT* attr) {
    return Sce(AttrDestroy(attr));
}
SSPC_SYSV_ABI s32 ScePthreadAttrSetdetachstate(AttrT* attr, s32 state) {
    return Sce(AttrSetdetachstate(attr, state));
}
SSPC_SYSV_ABI s32 PosixPthreadAttrSetdetachstate(AttrT* attr, s32 state) {
    return AttrSetdetachstate(attr, state);
}
SSPC_SYSV_ABI s32 ScePthreadAttrSetinheritsched(AttrT* attr, s32 inherit) {
    return Sce(AttrSetinheritsched(attr, inherit));
}
SSPC_SYSV_ABI s32 ScePthreadAttrSetschedparam(AttrT* attr, const SchedParam* param) {
    return Sce(AttrSetschedparam(attr, param));
}
SSPC_SYSV_ABI s32 ScePthreadAttrGetschedparam(const AttrT* attr, SchedParam* param) {
    return Sce(AttrGetschedparam(attr, param));
}
SSPC_SYSV_ABI s32 ScePthreadAttrSetschedpolicy(AttrT* attr, s32 policy) {
    return Sce(AttrSetschedpolicy(attr, policy));
}
SSPC_SYSV_ABI s32 ScePthreadAttrSetstacksize(AttrT* attr, u64 size) {
    return Sce(AttrSetstacksize(attr, size));
}
SSPC_SYSV_ABI s32 ScePthreadAttrSetaffinity(AttrT* attr, u64 mask) {
    return Sce(AttrSetaffinity(attr, mask));
}
SSPC_SYSV_ABI s32 ScePthreadAttrGetaffinity(const AttrT* attr, u64* mask) {
    return Sce(AttrGetaffinity(attr, mask));
}
SSPC_SYSV_ABI s32 ScePthreadAttrGet(void* thread, AttrT* attr) {
    return Sce(AttrGet(thread, attr));
}
SSPC_SYSV_ABI s32 ScePthreadKeyCreate(u32* key, KeyDestructor destructor) {
    return Sce(KeyCreate(key, destructor));
}
SSPC_SYSV_ABI s32 ScePthreadKeyDelete(u32 key) {
    return Sce(KeyDelete(key));
}
SSPC_SYSV_ABI void* ScePthreadGetspecific(u32 key) {
    return Getspecific(key);
}
SSPC_SYSV_ABI s32 ScePthreadSetspecific(u32 key, const void* value) {
    return Sce(Setspecific(key, value));
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

#define SSPC_THREAD_EXPORT(nid, lib, name, fn)                                                     \
    Runtime::Export {                                                                              \
        nid, lib, "libkernel", name, Fn(&fn), nullptr, "threads", false, true                      \
    }

// Exactly the thread imports of the game (eboot, libc.prx, libSceFios2.prx; docs/generated/imports.csv).
// scePthreadEqual / scePthreadYield / sched_yield are NATIVE already ("kernel-util").
const Runtime::Export kExports[] = {
    SSPC_THREAD_EXPORT("aI+OeCz8xrQ", "libkernel", "scePthreadSelf", PthreadSelf),
    SSPC_THREAD_EXPORT("6UgtwV+0zb4", "libkernel", "scePthreadCreate", ScePthreadCreate),
    SSPC_THREAD_EXPORT("onNY9Byn-W8", "libkernel", "scePthreadJoin", ScePthreadJoin),
    SSPC_THREAD_EXPORT("4qGrR6eoP9Y", "libkernel", "scePthreadDetach", ScePthreadDetach),
    SSPC_THREAD_EXPORT("3kg7rT0NQIs", "libkernel", "scePthreadExit", ScePthreadExit),
    SSPC_THREAD_EXPORT("14bOACANTBo", "libkernel", "scePthreadOnce", ScePthreadOnce),
    SSPC_THREAD_EXPORT("1tKyG7RlMJo", "libkernel", "scePthreadGetprio", ScePthreadGetprio),
    SSPC_THREAD_EXPORT("W0Hpm2X0uPE", "libkernel", "scePthreadSetprio", ScePthreadSetprio),
    SSPC_THREAD_EXPORT("rcrVFJsQWRY", "libkernel", "scePthreadGetaffinity", ScePthreadGetaffinity),
    SSPC_THREAD_EXPORT("bt3CTBKmGyI", "libkernel", "scePthreadSetaffinity", ScePthreadSetaffinity),
    SSPC_THREAD_EXPORT("nsYoNRywwNg", "libkernel", "scePthreadAttrInit", ScePthreadAttrInit),
    SSPC_THREAD_EXPORT("62KCwEMmzcM", "libkernel", "scePthreadAttrDestroy", ScePthreadAttrDestroy),
    SSPC_THREAD_EXPORT("-Wreprtu0Qs", "libkernel", "scePthreadAttrSetdetachstate", ScePthreadAttrSetdetachstate),
    SSPC_THREAD_EXPORT("eXbUSpEaTsA", "libkernel", "scePthreadAttrSetinheritsched", ScePthreadAttrSetinheritsched),
    SSPC_THREAD_EXPORT("DzES9hQF4f4", "libkernel", "scePthreadAttrSetschedparam", ScePthreadAttrSetschedparam),
    SSPC_THREAD_EXPORT("FXPWHNk8Of0", "libkernel", "scePthreadAttrGetschedparam", ScePthreadAttrGetschedparam),
    SSPC_THREAD_EXPORT("4+h9EzwKF4I", "libkernel", "scePthreadAttrSetschedpolicy", ScePthreadAttrSetschedpolicy),
    SSPC_THREAD_EXPORT("UTXzJbWhhTE", "libkernel", "scePthreadAttrSetstacksize", ScePthreadAttrSetstacksize),
    SSPC_THREAD_EXPORT("3qxgM4ezETA", "libkernel", "scePthreadAttrSetaffinity", ScePthreadAttrSetaffinity),
    SSPC_THREAD_EXPORT("8+s5BzZjxSg", "libkernel", "scePthreadAttrGetaffinity", ScePthreadAttrGetaffinity),
    SSPC_THREAD_EXPORT("x1X76arYMxU", "libkernel", "scePthreadAttrGet", ScePthreadAttrGet),
    SSPC_THREAD_EXPORT("geDaqgH9lTg", "libkernel", "scePthreadKeyCreate", ScePthreadKeyCreate),
    SSPC_THREAD_EXPORT("PrdHuuDekhY", "libkernel", "scePthreadKeyDelete", ScePthreadKeyDelete),
    SSPC_THREAD_EXPORT("eoht7mQOCmo", "libkernel", "scePthreadGetspecific", ScePthreadGetspecific),
    SSPC_THREAD_EXPORT("+BzXYkqYeLE", "libkernel", "scePthreadSetspecific", ScePthreadSetspecific),
    SSPC_THREAD_EXPORT("EotR8a3ASf4", "libScePosix", "pthread_self", PthreadSelf),
    SSPC_THREAD_EXPORT("Jmi+9w9u0E4", "libScePosix", "pthread_create_name_np", PosixPthreadCreateNameNp),
    SSPC_THREAD_EXPORT("h9CcP3J0oVM", "libScePosix", "pthread_join", PosixPthreadJoin),
    SSPC_THREAD_EXPORT("wtkt-teR1so", "libScePosix", "pthread_attr_init", PosixPthreadAttrInit),
    SSPC_THREAD_EXPORT("E+tyo3lp5Lw", "libScePosix", "pthread_attr_setdetachstate", PosixPthreadAttrSetdetachstate),
};

#undef SSPC_THREAD_EXPORT

} // namespace

void SetThreadHost(ThreadHost* host) {
    g_host = host;
}

void Enable() {
    g_enabled.store(true);
}

bool Enabled() {
    return g_enabled.load(std::memory_order_relaxed);
}

bool StartMainThread(std::function<void()> body) {
    Thread* thread = nullptr;
    const s32 r = Create(&thread, nullptr, nullptr, nullptr, "Game:Main", std::move(body), true);
    if (r != kOk) {
        SSPC_LOG(Kernel, Critical, "cannot start the game's main thread ({})", r);
        return false;
    }
    SSPC_LOG(Kernel, Info, "native threads: the game's main thread is SecondSonPC's (handle {})",
             static_cast<void*>(thread));
    return true;
}

std::optional<s32> CurrentPriority() {
    Thread* t = t_self;
    if (t == nullptr) {
        return std::nullopt;
    }
    std::scoped_lock lk{t->lock};
    return t->attr.prio;
}

std::span<const Runtime::Export> ThreadExports() {
    return kExports;
}

namespace Test {

Counts GetCounts() {
    std::scoped_lock lk{g_table_lock};
    const u64 in_use = g_table_ready ? kMaxThreads - g_free_count : 0;
    return {in_use, g_created, g_reaped, g_pending.size(), g_stacks_mapped, g_stacks.size()};
}

void ReapDetached() {
    ReapPending();
}

bool WaitDetached(u32 ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;) {
        ReapPending();
        {
            std::scoped_lock lk{g_table_lock};
            if (g_pending.empty()) {
                return true;
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        Sleep(1);
    }
}

} // namespace Test

} // namespace SecondSon::Kernel::Threads
