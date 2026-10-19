// SecondSonPC — timing stubs on engine functions (see native_measure.h).
#include "native_engine/native_measure.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <windows.h>
#include <xbyak/xbyak.h>

#include "engine/guest_hooks.h"
#include "native_engine/guest_code_clone.h"

namespace SecondSon::NativeEngine::Measure {

namespace {

constexpr u32 kMaxSites = 32;
constexpr u32 kMaxThreads = 512;
constexpr u32 kDepth = 512;

struct Site {
    const char* name = nullptr;
    u64 address = 0;
    void* original = nullptr;
    std::unique_ptr<Xbyak::CodeGenerator> stub;
};
std::array<Site, kMaxSites> g_sites;
std::atomic<u32> g_site_count{0};
std::mutex g_install;

struct ThreadTotals {
    std::atomic<u64> calls[kMaxSites]{};
    std::atomic<u64> inclusive[kMaxSites]{};
    std::atomic<u64> exclusive[kMaxSites]{};
};
std::array<std::atomic<ThreadTotals*>, kMaxThreads> g_threads{};
std::atomic<u32> g_thread_count{0};
ThreadTotals g_overflow;

struct Frame {
    u64 ret;
    u64 start;
    u64 child;
    u32 site;
};
struct ThreadState {
    u32 depth = 0;
    Frame frames[kDepth];
    ThreadTotals* totals = nullptr;
    bool shared = false;
};
thread_local ThreadState* t_state = nullptr;

ThreadState* State() {
    if (t_state == nullptr) {
        auto* s = new ThreadState; // first measured call on this thread; never freed (read by the reporter)
        const u32 i = g_thread_count.fetch_add(1, std::memory_order_relaxed);
        if (i < kMaxThreads) {
            s->totals = new ThreadTotals;
            g_threads[i].store(s->totals, std::memory_order_release);
        } else {
            s->totals = &g_overflow;
            s->shared = true;
        }
        t_state = s;
    }
    return t_state;
}

inline u64 Ticks() {
    u32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (static_cast<u64>(hi) << 32) | lo;
}

inline void Add(std::atomic<u64>& c, u64 v, bool shared) {
    if (shared) {
        c.fetch_add(v, std::memory_order_relaxed);
    } else {
        c.store(c.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
    }
}

SSPC_SYSV_ABI void Enter(u32 site, u64 ret) {
    ThreadState* s = State();
    if (s->depth >= kDepth) {
        std::abort(); // the caller's return address could not be kept: nothing safe to do
    }
    Frame& f = s->frames[s->depth++];
    f.ret = ret;
    f.site = site;
    f.child = 0;
    f.start = Ticks();
}

SSPC_SYSV_ABI u64 Leave(u32 site) {
    const u64 now = Ticks();
    ThreadState* s = State();
    Frame& f = s->frames[--s->depth];
    const u64 total = now - f.start;
    Add(s->totals->calls[site], 1, s->shared);
    Add(s->totals->inclusive[site], total, s->shared);
    Add(s->totals->exclusive[site], total > f.child ? total - f.child : 0, s->shared);
    if (s->depth > 0) {
        s->frames[s->depth - 1].child += total;
    }
    (void)site;
    return f.ret;
}

/// The stub (System V entry, signature-independent): see native_measure.h.
struct Stub : Xbyak::CodeGenerator {
    Stub(u32 site, const void* original) : Xbyak::CodeGenerator(512) {
        using namespace Xbyak::util;
        // entry: rsp = 8 mod 16. Save the argument registers and AL (varargs), 0xb8 bytes keeps the call aligned.
        sub(rsp, 0xb8);
        mov(ptr[rsp + 0x00], rdi);
        mov(ptr[rsp + 0x08], rsi);
        mov(ptr[rsp + 0x10], rdx);
        mov(ptr[rsp + 0x18], rcx);
        mov(ptr[rsp + 0x20], r8);
        mov(ptr[rsp + 0x28], r9);
        mov(ptr[rsp + 0x30], rax);
        for (int k = 0; k < 8; ++k) {
            movdqu(ptr[rsp + 0x38 + 16 * k], Xbyak::Xmm(k));
        }
        mov(edi, site);
        mov(rsi, ptr[rsp + 0xb8]); // the caller's return address
        mov(rax, reinterpret_cast<size_t>(&Enter));
        call(rax);
        mov(rdi, ptr[rsp + 0x00]);
        mov(rsi, ptr[rsp + 0x08]);
        mov(rdx, ptr[rsp + 0x10]);
        mov(rcx, ptr[rsp + 0x18]);
        mov(r8, ptr[rsp + 0x20]);
        mov(r9, ptr[rsp + 0x28]);
        mov(rax, ptr[rsp + 0x30]);
        for (int k = 0; k < 8; ++k) {
            movdqu(Xbyak::Xmm(k), ptr[rsp + 0x38 + 16 * k]);
        }
        add(rsp, 0xb8 + 8); // and drop the caller's return address (kept on the shadow stack)
        mov(r11, reinterpret_cast<size_t>(original));
        call(r11); // pushes into the same slot: the stack arguments are where the function expects them
        sub(rsp, 0x30);
        mov(ptr[rsp + 0x00], rax);
        mov(ptr[rsp + 0x08], rdx);
        movdqu(ptr[rsp + 0x10], xmm0);
        movdqu(ptr[rsp + 0x20], xmm1);
        mov(edi, site);
        mov(rax, reinterpret_cast<size_t>(&Leave));
        call(rax);
        mov(r11, rax);
        mov(rax, ptr[rsp + 0x00]);
        mov(rdx, ptr[rsp + 0x08]);
        movdqu(xmm0, ptr[rsp + 0x10]);
        movdqu(xmm1, ptr[rsp + 0x20]);
        add(rsp, 0x30);
        push(r11);
        ret();
        ready();
    }
};

double g_overhead = -1.0;

int Register(u64 address, const char* name, void* original, std::string& error) {
    const u32 i = g_site_count.load(std::memory_order_relaxed);
    if (i >= kMaxSites) {
        error = "too many sites";
        return -1;
    }
    Site& s = g_sites[i];
    s.name = name;
    s.address = address;
    s.original = original;
    try {
        s.stub = std::make_unique<Stub>(i, original);
    } catch (const std::exception& e) {
        error = fmt::format("stub: {}", e.what());
        return -1;
    }
    g_site_count.store(i + 1, std::memory_order_release);
    return static_cast<int>(i);
}

SSPC_SYSV_ABI u64 Empty(u64 a) {
    return a;
}

void Calibrate() {
    if (g_overhead >= 0) {
        return;
    }
    std::string error;
    const int i = Register(0, "(calibration)", reinterpret_cast<void*>(&Empty), error);
    if (i < 0) {
        g_overhead = 0;
        return;
    }
    using Fn = SSPC_SYSV_ABI u64 (*)(u64);
    const auto stub = reinterpret_cast<Fn>(const_cast<u8*>(g_sites[i].stub->getCode()));
    const u64 t0 = Ticks();
    u64 sink = 0;
    for (u64 k = 0; k < 100000; ++k) {
        sink += stub(k);
    }
    const u64 t1 = Ticks();
    const SiteTotals t = Totals(static_cast<u32>(i));
    // inclusive time the stub records for an empty function = the part of its cost inside the timed region; the
    // whole call costs (t1 - t0) / n
    g_overhead = t.calls != 0 ? static_cast<double>(t1 - t0) / static_cast<double>(t.calls) : 0;
    (void)sink;
}

} // namespace

int InstallSite(u64 address, u32 size, const char* name, std::string& error) {
    std::lock_guard guard(g_install);
    Calibrate();
    const CloneResult clone = CloneGuestFunction(address, reinterpret_cast<const u8*>(address), size, error);
    if (clone.code == nullptr) {
        return -1;
    }
    const int i = Register(address, name, clone.code, error);
    if (i < 0) {
        return -1;
    }
    if (!Engine::InstallJump(address, reinterpret_cast<const void*>(g_sites[i].stub->getCode()), error)) {
        return -1;
    }
    return i;
}

u32 SiteCount() {
    return g_site_count.load(std::memory_order_acquire);
}

SiteTotals Totals(u32 site) {
    SiteTotals t{g_sites[site].name, g_sites[site].address, 0, 0, 0};
    const auto add = [&](const ThreadTotals& tt) {
        t.calls += tt.calls[site].load(std::memory_order_relaxed);
        t.inclusive_ticks += tt.inclusive[site].load(std::memory_order_relaxed);
        t.exclusive_ticks += tt.exclusive[site].load(std::memory_order_relaxed);
    };
    add(g_overflow);
    const u32 n = std::min(g_thread_count.load(std::memory_order_acquire), kMaxThreads);
    for (u32 i = 0; i < n; ++i) {
        if (const ThreadTotals* tt = g_threads[i].load(std::memory_order_acquire)) {
            add(*tt);
        }
    }
    return t;
}

double StubOverheadTicks() {
    return g_overhead;
}

} // namespace SecondSon::NativeEngine::Measure
