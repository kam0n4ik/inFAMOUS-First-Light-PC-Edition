// SecondSonPC — a sampling profiler of one thread (P4.4 diagnostics). See cpu_sampler.h.
#include "runtime/gpu/cpu_sampler.h"

#include <dbghelp.h>
#include <tlhelp32.h>

#include <Zydis/Zydis.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

#include <fmt/format.h>

namespace SecondSon::Gpu {

namespace {
/// Reads this process's memory without faulting (clang-cl's __try does not catch a fault in an inline load without
/// -fasync-exceptions: l5 crashed at shutdown on a null pointer read): false if any byte is not readable.
bool SafeRead(u64 addr, void* dst, size_t bytes) {
    SIZE_T got = 0;
    return addr != 0 &&
           ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(addr), dst, bytes, &got) != FALSE &&
           got == bytes;
}
} // namespace

CpuSampler::~CpuSampler() {
    Stop();
    for (HANDLE h : threads_) {
        CloseHandle(h);
    }
}

bool CpuSampler::StartByName(const std::wstring& name_part, u32 period_us, u32 delay_s) {
    if (started_ || name_part.empty()) {
        return false;
    }
    table_.assign(size_t{1} << kTableBits, Entry{});
    name_part_ = name_part;
    unwind_ = false;
    ctx_by_pc_.reserve(256);
    period_us_ = std::max<u32>(period_us, 100);
    delay_s_ = delay_s;
    stop_.store(false);
    sampler_ = std::thread([this] { Run(); });
    started_ = true;
    return true;
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
    if (!name_part_.empty()) {
        // the threads by description (the runtime names the game's: Game:Main, JobWorker4, ...)
        const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te{.dwSize = sizeof(THREADENTRY32)};
        const DWORD self = GetCurrentProcessId(), me = GetCurrentThreadId();
        for (BOOL ok = snap != INVALID_HANDLE_VALUE && Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != self || te.th32ThreadID == me) {
                continue;
            }
            const HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION,
                                        FALSE, te.th32ThreadID);
            if (h == nullptr) {
                continue;
            }
            PWSTR desc = nullptr;
            // never another sampler: two suspending each other would deadlock (review LOW 9)
            const bool match = SUCCEEDED(GetThreadDescription(h, &desc)) && desc != nullptr &&
                               std::wstring_view(desc).find(name_part_) != std::wstring_view::npos &&
                               !std::wstring_view(desc).starts_with(L"SecondSonPC:CpuSampler");
            if (desc != nullptr) {
                LocalFree(desc);
            }
            if (match) {
                threads_.push_back(h);
            } else {
                CloseHandle(h);
            }
        }
        if (snap != INVALID_HANDLE_VALUE) {
            CloseHandle(snap);
        }
    }
    u64 pcs[kDepth];
    size_t next_thread = 0;
    while (!stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::microseconds(period_us_));
        HANDLE target = thread_;
        if (!threads_.empty()) {
            target = threads_[next_thread++ % threads_.size()];
        }
        if (target == nullptr || SuspendThread(target) == static_cast<DWORD>(-1)) {
            ++failed_;
            continue;
        }
        // nothing below allocates, logs or takes a lock the target may hold
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        u32 n = 0;
        if (!unwind_) {
            if (GetThreadContext(target, &ctx)) {
                // the latest registers at this address (no allocation: the capacity is reserved)
                PcContext* slot = nullptr;
                for (PcContext& pcx : ctx_by_pc_) {
                    if (pcx.pc == ctx.Rip) {
                        slot = &pcx;
                        break;
                    }
                }
                if (slot == nullptr && ctx_by_pc_.size() < ctx_by_pc_.capacity()) {
                    slot = &ctx_by_pc_.emplace_back();
                    slot->pc = ctx.Rip;
                }
                if (slot != nullptr) {
                    slot->ctx = ctx;
                }
                pcs[n++] = ctx.Rip;
                // the nearest return address into the executable on the stack (memory reads only)
                u64 sp[256];
                if (!(ctx.Rip >= exe && ctx.Rip < exe_end) && SafeRead(ctx.Rsp, sp, sizeof(sp))) {
                    for (u32 k = 0; k < 256 && n < 2; ++k) {
                        if (sp[k] >= exe && sp[k] < exe_end) {
                            pcs[n++] = sp[k];
                        }
                    }
                }
            }
        } else if (GetThreadContext(target, &ctx)) {
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
                    DWORD64 ret = 0;
                    if (!SafeRead(ctx.Rsp, &ret, sizeof(ret))) { // a fault here would kill the process (review MEDIUM 8)
                        ++n;
                        break;
                    }
                    ctx.Rip = ret;
                    ctx.Rsp += 8;
                } else {
                    PVOID handler_data = nullptr;
                    DWORD64 establisher = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fe, &ctx, &handler_data, &establisher,
                                     nullptr);
                }
            }
        }
        ResumeThread(target);
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
    // code outside every module (the game's): the hottest addresses themselves - a spin loop shows as a few of them
    std::vector<std::pair<u64, u32>> raw;
    for (const Entry& e : table_) {
        HMODULE mod = nullptr;
        if (e.pc != 0 && e.leaf != 0 &&
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                reinterpret_cast<LPCWSTR>(e.pc), &mod)) {
            raw.emplace_back(e.pc, e.leaf);
        }
    }
    std::ranges::sort(raw, [](const auto& a, const auto& b) { return a.second > b.second; });
    std::vector<std::pair<std::string, Fn>> v(fns.begin(), fns.end());
    std::string out = fmt::format("cpu sampler: {} samples every {} us (failed {}, addresses dropped {}){}{}\n", samples_,
                                  period_us_, failed_, dropped_, symbols ? "" : " - no symbols",
                                  threads_.empty() ? std::string()
                                                   : fmt::format(" - {} threads by name, no unwinding: the leaf and the "
                                                                 "nearest return address into the executable",
                                                                 threads_.size()));
    const auto pct = [&](u64 c) { return 100.0 * static_cast<double>(c) / static_cast<double>(samples_); };
    std::ranges::sort(v, [](const auto& a, const auto& b) { return a.second.leaf > b.second.leaf; });
    out += "exclusive (the function itself):\n";
    for (size_t i = 0; i < v.size() && i < top; ++i) {
        out += fmt::format("  {:5.1f}%  {:5.1f}% incl  {}\n", pct(v[i].second.leaf), pct(v[i].second.incl), v[i].first);
    }
    if (!raw.empty()) {
        out += "outside every module (exclusive), the hottest addresses:\n";
        for (size_t i = 0; i < raw.size() && i < 24; ++i) {
            out += fmt::format("  {:5.1f}%  {:#x}\n", pct(raw[i].second), raw[i].first);
        }
        // the code around the hottest one, from the lowest sampled address near it (an instruction start)
        const u64 top = raw[0].first;
        u64 lo = top;
        for (const auto& [a, c] : raw) {
            if (a < lo && top - a <= 64) {
                lo = a;
            }
        }
        u8 code[160] = {};
        const bool readable = SafeRead(lo, code, sizeof(code));
        if (readable) {
            ZydisDecoder dec;
            ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
            ZydisFormatter fmtr;
            ZydisFormatterInit(&fmtr, ZYDIS_FORMATTER_STYLE_INTEL);
            out += fmt::format("the code from {:#x}:\n", lo);
            size_t off = 0;
            for (u32 k = 0; k < 24 && off < sizeof(code); ++k) {
                ZydisDecodedInstruction insn;
                ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + off, sizeof(code) - off, &insn, ops))) {
                    break;
                }
                char text[128] = {};
                ZydisFormatterFormatInstruction(&fmtr, &insn, ops, insn.operand_count_visible, text, sizeof(text),
                                                lo + off, nullptr);
                u32 hits = 0;
                for (const auto& [a, c] : raw) {
                    hits += a == lo + off ? c : 0;
                }
                out += fmt::format("  {:#x}  {:5.1f}%  {}\n", lo + off, pct(hits), text);
                off += insn.length;
            }
        }
        const CONTEXT* hot = nullptr; // the registers of a sample at the hottest address
        for (const PcContext& pcx : ctx_by_pc_) {
            if (pcx.pc == top) {
                hot = &pcx.ctx;
            }
        }
        if (hot != nullptr) {
            // its stack (the caller's return address is among these) and the code before each code-like address
            u64 stack[16] = {};
            const bool ok_stack = SafeRead(hot->Rsp, stack, sizeof(stack));
            if (ok_stack) {
                out += "the last sample's stack:";
                for (u32 k = 0; k < 16; ++k) {
                    out += fmt::format(" [{:#x}] {:#x}", k * 8, stack[k]);
                }
                out += "\n";
                ZydisDecoder dec;
                ZydisDecoderInit(&dec, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
                ZydisFormatter fmtr;
                ZydisFormatterInit(&fmtr, ZYDIS_FORMATTER_STYLE_INTEL);
                for (u32 k = 0; k < 16; ++k) {
                    const u64 ra = stack[k];
                    if (ra < 0x400000 || ra >= 0x20000000 || ra == 0) {
                        continue; // not the game's code range
                    }
                    // decode forward from 48 bytes before; print what ends right at the return address
                    u8 code[96] = {};
                    if (!SafeRead(ra - 48, code, sizeof(code))) {
                        continue;
                    }
                    out += fmt::format("code around return address [{:#x}] {:#x}:\n", k * 8, ra);
                    for (u32 skip = 0; skip < 16; ++skip) {
                        // the first offset whose instruction stream lands exactly on the return address
                        size_t off = skip;
                        bool lands = false;
                        while (off < 48) {
                            ZydisDecodedInstruction insn;
                            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + off, sizeof(code) - off, &insn, ops))) {
                                break;
                            }
                            off += insn.length;
                        }
                        lands = off == 48;
                        if (!lands) {
                            continue;
                        }
                        off = skip;
                        for (u32 i = 0; i < 30 && off < sizeof(code); ++i) {
                            ZydisDecodedInstruction insn;
                            ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
                            if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&dec, code + off, sizeof(code) - off, &insn, ops))) {
                                break;
                            }
                            char text[128] = {};
                            ZydisFormatterFormatInstruction(&fmtr, &insn, ops, insn.operand_count_visible, text,
                                                            sizeof(text), ra - 48 + off, nullptr);
                            // a RIP-relative operand: its address, the u64 there and what that points to
                            std::string rel;
                            for (u32 o = 0; o < insn.operand_count_visible; ++o) {
                                if (ops[o].type != ZYDIS_OPERAND_TYPE_MEMORY || ops[o].mem.base != ZYDIS_REGISTER_RIP) {
                                    continue;
                                }
                                ZyanU64 abs = 0;
                                if (!ZYAN_SUCCESS(ZydisCalcAbsoluteAddress(&insn, &ops[o], ra - 48 + off, &abs))) {
                                    continue;
                                }
                                u64 v = 0, w[2] = {};
                                const bool okv = SafeRead(abs, &v, sizeof(v)) && SafeRead(v, w, sizeof(w));
                                rel += fmt::format("   ; [{:#x}] = {:#x}{}", static_cast<u64>(abs), v,
                                                   okv ? fmt::format(" -> {:#x} {:#x}", w[0], w[1]) : std::string());
                            }
                            out += fmt::format("  {:#x}{}  {}{}\n", ra - 48 + off, ra - 48 + off == ra ? " <-" : "   ",
                                               text, rel);
                            off += insn.length;
                        }
                        break;
                    }
                }
            }
            // SSPC_DIAG_PEEK=<hex>[,<hex>...]: a u64 at each address and, through it, 4 u64; SSPC_DIAG_PEEK_STACK=
            // <slot>:<hex off>[:<hex off>...]: the u32 at each offset from the stack slot's value (a saved this)
            char peek[256] = {};
            if (GetEnvironmentVariableA("SSPC_DIAG_PEEK", peek, sizeof(peek)) > 0) {
                for (char* t = std::strtok(peek, ","); t != nullptr; t = std::strtok(nullptr, ",")) {
                    const u64 a = std::strtoull(t, nullptr, 16);
                    u64 v[5] = {};
                    const bool ok = SafeRead(a, &v[0], sizeof(v[0])) && SafeRead(v[0], &v[1], 4 * sizeof(v[0]));
                    out += fmt::format("peek {:#x}: {:#x} -> {:#x} {:#x} {:#x} {:#x}{}\n", a, v[0], v[1], v[2], v[3],
                                       v[4], ok ? "" : " (unreadable)");
                }
            }
            char ps[128] = {};
            if (ok_stack && GetEnvironmentVariableA("SSPC_DIAG_PEEK_STACK", ps, sizeof(ps)) > 0) {
                char* t = std::strtok(ps, ":");
                const u32 slot = t != nullptr ? static_cast<u32>(std::strtoul(t, nullptr, 10)) : 0;
                const u64 base = slot < 16 ? stack[slot] : 0;
                out += fmt::format("peek stack slot {} = {:#x}:", slot, base);
                for (t = std::strtok(nullptr, ":"); t != nullptr; t = std::strtok(nullptr, ":")) {
                    const u64 off = std::strtoull(t, nullptr, 16);
                    u32 v = 0;
                    const bool ok = SafeRead(base + off, &v, sizeof(v));
                    out += fmt::format(" +{:#x} = {:#x}{}", off, v, ok ? "" : " (unreadable)");
                }
                out += "\n";
            }
            const CONTEXT& c = *hot;
            // each register's target (a u64 where it points) and SSPC_DIAG_PEEK_REG=<reg>:<hex off>[:...] (u64 at reg+off)
            const std::pair<const char*, u64> regs[] = {
                {"rax", c.Rax}, {"rbx", c.Rbx}, {"rcx", c.Rcx}, {"rdx", c.Rdx}, {"rsi", c.Rsi}, {"rdi", c.Rdi},
                {"rbp", c.Rbp}, {"r8", c.R8},   {"r9", c.R9},   {"r10", c.R10}, {"r11", c.R11}, {"r12", c.R12},
                {"r13", c.R13}, {"r14", c.R14}, {"r15", c.R15}};
            out += "where the registers point:";
            for (const auto& [name, val] : regs) {
                u64 v = 0;
                if (SafeRead(val, &v, sizeof(v))) {
                    out += fmt::format(" *{} = {:#x}", name, v);
                }
            }
            out += "\n";
            char pr[128] = {};
            if (GetEnvironmentVariableA("SSPC_DIAG_PEEK_REG", pr, sizeof(pr)) > 0) {
                char* t = std::strtok(pr, ":");
                u64 base = 0;
                for (const auto& [name, val] : regs) {
                    if (t != nullptr && std::string_view(t) == name) {
                        base = val;
                    }
                }
                out += fmt::format("peek {} = {:#x}:", t != nullptr ? t : "?", base);
                for (t = std::strtok(nullptr, ":"); t != nullptr; t = std::strtok(nullptr, ":")) {
                    const u64 off = std::strtoull(t, nullptr, 16);
                    u64 v = 0;
                    const bool ok = SafeRead(base + off, &v, sizeof(v));
                    out += fmt::format(" +{:#x} = {:#x}{}", off, v, ok ? "" : " (unreadable)");
                }
                out += "\n";
            }
            out += fmt::format("the last sample's registers: rip {:#x} rax {:#x} rbx {:#x} rcx {:#x} rdx {:#x} rsi {:#x} rdi "
                               "{:#x} rbp {:#x} rsp {:#x} r8 {:#x} r9 {:#x} r10 {:#x} r11 {:#x} r12 {:#x} r13 {:#x} r14 "
                               "{:#x} r15 {:#x}\n",
                               c.Rip, c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.Rbp, c.Rsp, c.R8, c.R9, c.R10, c.R11,
                               c.R12, c.R13, c.R14, c.R15);
        }
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
