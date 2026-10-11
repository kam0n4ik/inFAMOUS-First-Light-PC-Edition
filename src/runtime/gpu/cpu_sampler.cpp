// SecondSonPC — a sampling profiler of one thread (P4.4 diagnostics). See cpu_sampler.h.
#include "runtime/gpu/cpu_sampler.h"

#include <dbghelp.h>

#include <algorithm>
#include <chrono>
#include <map>

#include <fmt/format.h>

namespace SecondSon::Gpu {

CpuSampler::~CpuSampler() {
    Stop();
}

bool CpuSampler::Start(HANDLE thread, u32 period_us, u32 delay_s) {
    if (started_ || thread == nullptr) {
        return false;
    }
    table_.assign(size_t{1} << kTableBits, Entry{});
    thread_ = thread;
    period_us_ = std::max<u32>(period_us, 100);
    delay_s_ = delay_s;
    stop_.store(false);
    sampler_ = std::thread([this] { Run(); });
    started_ = true;
    return true;
}

void CpuSampler::Stop() {
    stop_.store(true);
    if (sampler_.joinable()) {
        sampler_.join();
    }
}

void CpuSampler::Count(u64 pc, bool leaf, bool own) {
    const u64 mask = (u64{1} << kTableBits) - 1;
    u64 i = (pc * 0x9E3779B97F4A7C15ull) >> (64 - kTableBits);
    for (u32 probe = 0; probe < 64; ++probe, i = (i + 1) & mask) {
        Entry& e = table_[i];
        if (e.pc == pc || e.pc == 0) {
            e.pc = pc;
            e.leaf += leaf ? 1 : 0;
            e.own += own ? 1 : 0;
            ++e.incl;
            return;
        }
    }
    ++dropped_;
}

void CpuSampler::Run() {
    SetThreadDescription(GetCurrentThread(), L"SecondSonPC:CpuSampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    const auto start = std::chrono::steady_clock::now() + std::chrono::seconds(delay_s_);
    while (!stop_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < start) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    // the executable's image: the nearest frame in it takes the time spent below it in the driver or the system
    const auto exe = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exe);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exe + dos->e_lfanew);
    const u64 exe_end = exe + nt->OptionalHeader.SizeOfImage;
    u64 pcs[kDepth];
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(period_us_));
        if (SuspendThread(thread_) == static_cast<DWORD>(-1)) {
            ++failed_;
            continue;
        }
        // nothing below allocates, logs or takes a lock the target may hold
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        u32 n = 0;
        if (GetThreadContext(thread_, &ctx)) {
            for (; n < kDepth && ctx.Rip != 0; ++n) {
                pcs[n] = ctx.Rip;
                DWORD64 image_base = 0;
                PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
                if (fe == nullptr) {
                    // a leaf function: its return address is at the stack pointer
                    if (ctx.Rsp == 0) {
                        ++n;
                        break;
                    }
                    ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                    ctx.Rsp += 8;
                } else {
                    PVOID handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fe, &ctx, &handler_data, &establisher,
                                     nullptr);
                }
            }
        }
        ResumeThread(thread_);
        if (n == 0) {
            ++failed_;
            continue;
        }
        ++samples_;
        u32 own = kDepth;
        for (u32 k = 0; k < n && own == kDepth; ++k) {
            own = pcs[k] >= exe && pcs[k] < exe_end ? k : kDepth;
        }
        for (u32 k = 0; k < n; ++k) {
            bool seen = false; // a recursion counts once in the inclusive figures
            for (u32 j = 0; j < k && !seen; ++j) {
                seen = pcs[j] == pcs[k];
            }
            if (!seen) {
                Count(k == 0 ? pcs[k] : pcs[k] - 1, k == 0, k == own); // a return address: the call is the byte before
            }
        }
    }
}

std::string CpuSampler::Report(size_t top) const {
    if (!started_ || samples_ == 0) {
        return "cpu sampler: no samples\n";
    }
    using SymSetOptionsFn = decltype(&SymSetOptions);
    using SymInitializeFn = decltype(&SymInitialize);
    using SymFromAddrFn = decltype(&SymFromAddr);
    using SymCleanupFn = decltype(&SymCleanup);
    const HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    const auto set_options = dbghelp ? reinterpret_cast<SymSetOptionsFn>(GetProcAddress(dbghelp, "SymSetOptions")) : nullptr;
    const auto initialize = dbghelp ? reinterpret_cast<SymInitializeFn>(GetProcAddress(dbghelp, "SymInitialize")) : nullptr;
    const auto from_addr = dbghelp ? reinterpret_cast<SymFromAddrFn>(GetProcAddress(dbghelp, "SymFromAddr")) : nullptr;
    const auto cleanup = dbghelp ? reinterpret_cast<SymCleanupFn>(GetProcAddress(dbghelp, "SymCleanup")) : nullptr;
    const HANDLE process = GetCurrentProcess();
    bool symbols = false;
    if (set_options && initialize && from_addr) {
        set_options(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        symbols = initialize(process, nullptr, TRUE) != FALSE;
    }
    struct Fn {
        u64 leaf = 0, incl = 0, own = 0;
    };
    std::map<std::string, Fn> fns;
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 512];
    for (const Entry& e : table_) {
        if (e.pc == 0) {
            continue;
        }
        std::string name;
        if (symbols) {
            auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 511;
            DWORD64 disp = 0;
            if (from_addr(process, e.pc, &disp, si)) {
                name.assign(si->Name, si->NameLen);
            }
        }
        if (name.empty()) {
            HMODULE mod = nullptr;
            wchar_t path[MAX_PATH] = {};
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(e.pc), &mod) &&
                GetModuleFileNameW(mod, path, MAX_PATH) != 0) {
                std::wstring w(path);
                const size_t slash = w.find_last_of(L"\\/");
                std::string base;
                for (const wchar_t c : w.substr(slash == std::wstring::npos ? 0 : slash + 1)) {
                    base.push_back(c < 128 ? static_cast<char>(c) : '?');
                }
                name = fmt::format("[{}]", base);
            } else {
                name = "[unknown]";
            }
        }
        Fn& f = fns[name];
        f.leaf += e.leaf;
        f.incl += e.incl;
        f.own += e.own;
    }
    if (symbols && cleanup) {
        cleanup(process);
    }
    std::vector<std::pair<std::string, Fn>> v(fns.begin(), fns.end());
    std::string out = fmt::format("cpu sampler: {} samples every {} us (failed {}, addresses dropped {}){}\n", samples_,
                                  period_us_, failed_, dropped_, symbols ? "" : " - no symbols");
    const auto pct = [&](u64 c) { return 100.0 * static_cast<double>(c) / static_cast<double>(samples_); };
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second.leaf > b.second.leaf; });
    out += "exclusive (the function itself):\n";
    for (size_t i = 0; i < v.size() && i < top; ++i) {
        out += fmt::format("  {:5.1f}%  {:5.1f}% incl  {}\n", pct(v[i].second.leaf), pct(v[i].second.incl), v[i].first);
    }
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second.own > b.second.own; });
    out += "by the nearest function of the executable (itself + the driver/system code below it):\n";
    for (size_t i = 0; i < v.size() && i < top; ++i) {
        out += fmt::format("  {:5.1f}%  {}\n", pct(v[i].second.own), v[i].first);
    }
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second.incl > b.second.incl; });
    out += "inclusive (with what it calls):\n";
    for (size_t i = 0; i < v.size() && i < top; ++i) {
        out += fmt::format("  {:5.1f}%  {:5.1f}% excl  {}\n", pct(v[i].second.incl), pct(v[i].second.leaf), v[i].first);
    }
    return out;
}

} // namespace SecondSon::Gpu
