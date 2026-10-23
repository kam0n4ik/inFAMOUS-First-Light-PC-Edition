// SecondSonPC — verification support for reconstructed functions (see native_engine_verify.h).
#include "native_engine/native_engine_verify.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>

#include "native_engine/native_function_registry.h"

namespace SecondSon::NativeEngine {

namespace {

// R8 (docs/NATIVE_ENGINE_R8_VERIFICATION.md §2.4): the cases are kept per function - before, one ring of 64 for the
// process, and a function with many mismatches (a test that injects them, or a real fault) took every slot, so the
// cases of the others were dropped (R7 RESULTS §4).
struct CaseSlot {
    std::atomic<u32> state{0}; ///< 0 free, 1 being written, 2 ready, 3 written to the file
    u32 size = 0;
    u8 bytes[kCaseBytes];
};

struct FunctionCases {
    std::atomic<u32> next{0};
    std::atomic<u64> dropped{0};
    CaseSlot slots[kCasesPerFunction];
};

std::array<FunctionCases, kMaxFunctions> g_cases;
std::atomic<u64> g_dropped_other{0}; ///< an index outside the registry, or a case larger than kCaseBytes

} // namespace

void RecordCase(u32 function, std::span<const u8> bytes) {
    if (function >= kMaxFunctions || bytes.size() > kCaseBytes) {
        if (function < kMaxFunctions) {
            g_cases[function].dropped.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_dropped_other.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    FunctionCases& f = g_cases[function];
    const u32 i = f.next.fetch_add(1, std::memory_order_relaxed);
    if (i >= kCasesPerFunction) {
        f.next.store(kCasesPerFunction, std::memory_order_relaxed); // no wrap of the index
        f.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CaseSlot& c = f.slots[i];
    c.state.store(1, std::memory_order_relaxed);
    c.size = static_cast<u32>(bytes.size());
    std::memcpy(c.bytes, bytes.data(), bytes.size());
    c.state.store(2, std::memory_order_release);
}

u64 CasesRecorded(u32 function) {
    return function < kMaxFunctions ? std::min<u32>(g_cases[function].next.load(std::memory_order_relaxed),
                                                    kCasesPerFunction)
                                    : 0;
}

u64 CasesDropped(u32 function) {
    return function < kMaxFunctions ? g_cases[function].dropped.load(std::memory_order_relaxed) : 0;
}

u64 CasesRecorded() {
    u64 n = 0;
    for (u32 i = 0; i < kMaxFunctions; ++i) {
        n += CasesRecorded(i);
    }
    return n;
}

u64 CasesDropped() {
    u64 n = g_dropped_other.load(std::memory_order_relaxed);
    for (u32 i = 0; i < kMaxFunctions; ++i) {
        n += CasesDropped(i);
    }
    return n;
}

std::string ToHex(std::span<const u8> bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (const u8 b : bytes) {
        s.push_back(kDigits[b >> 4]);
        s.push_back(kDigits[b & 15]);
    }
    return s;
}

bool FromHex(const std::string& hex, std::string& out) {
    if (hex.size() % 2 != 0) {
        return false;
    }
    out.clear();
    const auto nibble = [](char c) -> int {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<char>(hi * 16 + lo));
    }
    return true;
}

void FlushCases(const std::string& dir) {
    for (u32 function = 0; function < FunctionCount(); ++function) {
        for (CaseSlot& c : g_cases[function].slots) {
            if (c.state.load(std::memory_order_acquire) != 2) {
                continue;
            }
            const FunctionSpec& spec = Function(function);
            const std::filesystem::path file = std::filesystem::path(dir) / (std::string("cases_") + spec.key + ".txt");
            if (FILE* f = std::fopen(file.string().c_str(), "ab")) {
                const std::string hex = ToHex({c.bytes, c.size});
                std::fprintf(f, "%s %s\n", spec.key, hex.c_str());
                std::fclose(f);
            }
            c.state.store(3, std::memory_order_release);
        }
    }
}

} // namespace SecondSon::NativeEngine
