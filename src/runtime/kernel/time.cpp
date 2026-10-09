// SecondSonPC — libkernel time functions and sceKernelIsNeoMode (NATIVE since Phase 2, P2.2).
// Host clocks, rounding and error behaviour are those of the Phase 0 oracle
// (third_party/shadPS4/src/core/libraries/kernel/time.cpp, process.cpp, common/rdtsc.cpp
// @ 0fe263a), so the game reads the same values; CLOCK_PROCTIME reads the bridge's process clock
// through Kernel::Host (kernel.h).
#include "runtime/kernel/kernel.h"

#include <windows.h>

#include <intrin.h>

#include <chrono>
#include <ctime>
#include <future>
#include <mutex>
#include <thread>

#include "base/log.h"
#include "runtime/exports.h"

namespace SecondSon::Kernel {

namespace {

constexpr s32 ORBIS_OK = 0;
constexpr s32 kErrnoBase = static_cast<s32>(0x80020000); // ORBIS_KERNEL_ERROR_UNKNOWN + errno
// Guest (FreeBSD) errno values.
constexpr s32 kEINTR = 4;
constexpr s32 kEFAULT = 14;
constexpr s32 kEINVAL = 22;

enum Clock : u32 {
    Realtime = 0,
    Virtual = 1,
    Prof = 2,
    Monotonic = 4,
    Uptime = 5,
    UptimePrecise = 7,
    UptimeFast = 8,
    RealtimePrecise = 9,
    RealtimeFast = 10,
    MonotonicPrecise = 11,
    MonotonicFast = 12,
    Second = 13,
    ThreadCputime = 14,
    Proctime = 15,
    ExtNetwork = 16,
    ExtDebugNetwork = 17,
    ExtAdNetwork = 18,
    ExtRawNetwork = 19,
};

struct Timespec {
    s64 tv_sec;
    s64 tv_nsec;
};
struct Timeval {
    s64 tv_sec;
    s64 tv_usec;
};
struct Timezone {
    s32 tz_minuteswest;
    s32 tz_dsttime;
};

Host* g_host = nullptr;
bool g_neo = false;
bool g_title_neo = false;
std::shared_future<u64> g_tsc_frequency;
std::chrono::system_clock::time_point g_network_boottime = std::chrono::system_clock::now();
std::chrono::steady_clock::time_point g_initial_uptime = std::chrono::steady_clock::now();

void SetErrno(s32 e) {
    if (g_host != nullptr) {
        *g_host->Errno() = e;
    }
}

u64 FileTime100Ns(const FILETIME& ft) {
    return (static_cast<u64>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

// ---- TSC (common/rdtsc.cpp of the oracle)

u64 FencedRdtsc() {
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 t = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return t;
}

u64 WallClockNs() {
    constexpr u64 kWindowsEpochToUnixEpoch = 0x19DB1DED53E8000LL;
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return 100 * (FileTime100Ns(ft) - kWindowsEpochToUnixEpoch);
}

u64 EstimateTscFrequency() {
    FencedRdtsc(); // the first measurement is discarded
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    FencedRdtsc();
    const u64 start_time = WallClockNs();
    const u64 tsc_start = FencedRdtsc();
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const u64 end_time = WallClockNs();
    const u64 tsc_end = FencedRdtsc();
    const auto freq = static_cast<u64>(static_cast<unsigned __int128>(tsc_end - tsc_start) *
                                       1'000'000'000ULL / (end_time - start_time));
    constexpr u64 kNearest = 100'000;
    const u64 mod = freq % kNearest;
    return mod >= kNearest / 2 ? freq - mod + kNearest : freq - mod;
}

// ---- clocks

s32 ClockGettime(u32 clock_id, Timespec* ts) {
    if (ts == nullptr) {
        SetErrno(kEFAULT);
        return -1;
    }
    constexpr u64 kDeltaEpochIn100ns = 116444736000000000ULL;
    const auto from_100ns = [ts](u64 v) {
        ts->tv_sec = static_cast<s64>(v / 10'000'000);
        ts->tv_nsec = static_cast<s64>((v % 10'000'000) * 100);
    };
    switch (clock_id) {
    case Proctime: {
        if (g_host == nullptr) {
            SetErrno(kEFAULT);
            return -1;
        }
        const u64 us = g_host->ProcessTimeUs();
        ts->tv_sec = static_cast<s64>(us / 1'000'000);
        ts->tv_nsec = static_cast<s64>((us % 1'000'000) * 1000);
        return 0;
    }
    case ExtNetwork:
    case ExtDebugNetwork:
    case ExtAdNetwork:
    case ExtRawNetwork: {
        const auto now = g_network_boottime + (std::chrono::steady_clock::now() - g_initial_uptime);
        const auto ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        ts->tv_sec = ns / 1'000'000'000;
        ts->tv_nsec = ns % 1'000'000'000;
        return 0;
    }
    case Realtime:
    case RealtimePrecise: {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        from_100ns(FileTime100Ns(ft) - kDeltaEpochIn100ns);
        return 0;
    }
    case Second:
    case RealtimeFast: {
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        from_100ns(FileTime100Ns(ft) - kDeltaEpochIn100ns);
        return 0;
    }
    case Uptime:
    case UptimePrecise:
    case Monotonic:
    case MonotonicPrecise:
    case UptimeFast:
    case MonotonicFast: {
        static const LARGE_INTEGER pf = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return f;
        }();
        LARGE_INTEGER pc{};
        if (!QueryPerformanceCounter(&pc)) {
            SetErrno(kEFAULT);
            return -1;
        }
        ts->tv_sec = pc.QuadPart / pf.QuadPart;
        ts->tv_nsec = ((pc.QuadPart % pf.QuadPart) * 1'000'000'000) / pf.QuadPart;
        return 0;
    }
    case ThreadCputime:
    case Virtual:
    case Prof: {
        FILETIME ct, et, kt, ut;
        const bool ok = clock_id == ThreadCputime
                            ? GetThreadTimes(GetCurrentThread(), &ct, &et, &kt, &ut)
                            : GetProcessTimes(GetCurrentProcess(), &ct, &et, &kt, &ut);
        if (!ok) {
            SetErrno(kEFAULT);
            return -1;
        }
        from_100ns(clock_id == ThreadCputime
                       ? FileTime100Ns(ut) + FileTime100Ns(kt)
                       : (clock_id == Virtual ? FileTime100Ns(ut) : FileTime100Ns(kt)));
        return 0;
    }
    default:
        // The oracle's answer to an unknown clock (FreeBSD would say EINVAL).
        SetErrno(kEFAULT);
        return -1;
    }
}

SSPC_SYSV_ABI s32 PosixClockGettime(u32 clock_id, Timespec* ts) {
    return ClockGettime(clock_id, ts);
}

SSPC_SYSV_ABI s32 SceClockGettime(u32 clock_id, Timespec* ts) {
    if (ClockGettime(clock_id, ts) < 0) {
        return kErrnoBase + (g_host != nullptr ? *g_host->Errno() : kEFAULT);
    }
    return ORBIS_OK;
}

SSPC_SYSV_ABI s32 PosixGettimeofday(Timeval* tp, Timezone* tz) {
    if (tp != nullptr) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        constexpr u64 kUnixTimeStartUs = 0x295E9648864000;
        const u64 us = FileTime100Ns(ft) / 10 - kUnixTimeStartUs;
        tp->tv_sec = static_cast<s64>(us / 1'000'000);
        tp->tv_usec = static_cast<s64>(us % 1'000'000);
    }
    if (tz != nullptr) {
        static std::once_flag tz_init;
        std::call_once(tz_init, [] { _tzset(); });
        long seconds_west = 0;
        int daylight = 0;
        _get_timezone(&seconds_west);
        _get_daylight(&daylight);
        tz->tz_minuteswest = static_cast<s32>(seconds_west / 60);
        tz->tz_dsttime = daylight;
    }
    return 0;
}

SSPC_SYSV_ABI s32 SceGettimeofday(Timeval* tp) {
    PosixGettimeofday(tp, nullptr);
    return ORBIS_OK;
}

// ---- sleeps (AccurateSleep of the oracle's common/thread.cpp: a waitable timer)

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible) {
    const auto begin = std::chrono::high_resolution_clock::now();
    LARGE_INTEGER interval{.QuadPart = -1 * (duration.count() / 100)};
    HANDLE timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    SetWaitableTimer(timer, &interval, 0, nullptr, nullptr, FALSE);
    const DWORD ret = WaitForSingleObjectEx(timer, INFINITE, interruptible ? TRUE : FALSE);
    CloseHandle(timer);
    if (remaining != nullptr) {
        const auto slept = std::chrono::high_resolution_clock::now() - begin;
        *remaining = duration > slept ? duration - slept : std::chrono::nanoseconds(0);
    }
    return ret == WAIT_OBJECT_0;
}

s32 Nanosleep(const Timespec* rqtp, Timespec* rmtp, bool interruptible) {
    if (rqtp == nullptr || rqtp->tv_sec < 0 || rqtp->tv_nsec < 0 || rqtp->tv_nsec >= 1'000'000'000) {
        SetErrno(kEINVAL);
        return -1;
    }
    const auto duration = std::chrono::nanoseconds(rqtp->tv_sec * 1'000'000'000 + rqtp->tv_nsec);
    std::chrono::nanoseconds remain{};
    const bool uninterrupted = AccurateSleep(duration, &remain, interruptible);
    if (rmtp != nullptr) {
        rmtp->tv_sec = remain.count() / 1'000'000'000;
        rmtp->tv_nsec = remain.count() % 1'000'000'000;
    }
    if (!uninterrupted) {
        SetErrno(kEINTR);
        return -1;
    }
    return 0;
}

SSPC_SYSV_ABI s32 PosixNanosleep(const Timespec* rqtp, Timespec* rmtp) {
    return Nanosleep(rqtp, rmtp, true);
}

SSPC_SYSV_ABI s32 SceUsleep(u32 microseconds) {
    const Timespec ts{static_cast<s64>(microseconds / 1'000'000),
                      static_cast<s64>((microseconds % 1'000'000) * 1'000)};
    if (Nanosleep(&ts, nullptr, false) < 0) {
        return kErrnoBase + (g_host != nullptr ? *g_host->Errno() : kEINTR);
    }
    return ORBIS_OK;
}

// ---- TSC, process

SSPC_SYSV_ABI u64 GetTscFrequency() {
    return g_tsc_frequency.valid() ? g_tsc_frequency.get() : EstimateTscFrequency();
}

SSPC_SYSV_ABI s32 IsNeoMode() {
    return g_neo && g_title_neo ? 1 : 0;
}

template <typename F>
void* Fn(F f) {
    return reinterpret_cast<void*>(f);
}

const Runtime::Export kExports[] = {
    {"lLMT9vJAck0", "libScePosix", "libkernel", "clock_gettime", Fn(&PosixClockGettime), nullptr, "kernel"},
    {"lLMT9vJAck0", "libkernel", "libkernel", "clock_gettime", Fn(&PosixClockGettime), nullptr, "kernel"},
    {"QBi7HCK03hw", "libkernel", "libkernel", "sceKernelClockGettime", Fn(&SceClockGettime), nullptr, "kernel"},
    {"n88vx3C5nW8", "libScePosix", "libkernel", "gettimeofday", Fn(&PosixGettimeofday), nullptr, "kernel"},
    {"n88vx3C5nW8", "libkernel", "libkernel", "gettimeofday", Fn(&PosixGettimeofday), nullptr, "kernel"},
    {"ejekcaNQNq0", "libkernel", "libkernel", "sceKernelGettimeofday", Fn(&SceGettimeofday), nullptr, "kernel"},
    {"yS8U2TGCe1A", "libkernel", "libkernel", "nanosleep", Fn(&PosixNanosleep), nullptr, "kernel"},
    {"1jfXLRVzisc", "libkernel", "libkernel", "sceKernelUsleep", Fn(&SceUsleep), nullptr, "kernel"},
    {"1j3S3n-tTW4", "libkernel", "libkernel", "sceKernelGetTscFrequency", Fn(&GetTscFrequency), nullptr, "kernel"},
    {"WslcK1FQcGI", "libkernel", "libkernel", "sceKernelIsNeoMode", Fn(&IsNeoMode), nullptr, "kernel"},
};

} // namespace

void SetHost(Host* host) {
    g_host = host;
}

Host* GetHost() {
    return g_host;
}

void Configure(bool neo, bool title_supports_neo) {
    g_neo = neo;
    g_title_neo = title_supports_neo;
}

bool NeoMode() {
    return IsNeoMode() != 0;
}

void Start() {
    g_network_boottime = std::chrono::system_clock::now();
    g_initial_uptime = std::chrono::steady_clock::now();
    g_tsc_frequency = std::async(std::launch::async, EstimateTscFrequency).share();
    SSPC_LOG(Kernel, Info, "libkernel time functions NATIVE (TSC frequency measured in the background)");
}

std::span<const Runtime::Export> KernelExports() {
    return kExports;
}

} // namespace SecondSon::Kernel
