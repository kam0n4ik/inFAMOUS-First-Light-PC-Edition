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

constexpr u32 kRing = 64; ///< cases kept per run (the first ones: later mismatches only count)

struct CaseSlot {
    std::atomic<u32> state{0}; ///< 0 free, 1 being written, 2 ready, 3 written to the file
    u32 function = 0;
    u32 size = 0;
    u8 bytes[kCaseBytes];
};

std::array<CaseSlot, kRing> g_cases;
std::atomic<u32> g_next{0};
std::atomic<u64> g_dropped{0};

} // namespace

void RecordCase(u32 function, std::span<const u8> bytes) {
    const u32 i = g_next.fetch_add(1, std::memory_order_relaxed);
    if (i >= kRing || bytes.size() > kCaseBytes) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    CaseSlot& c = g_cases[i];
    c.state.store(1, std::memory_order_relaxed);
    c.function = function;
    c.size = static_cast<u32>(bytes.size());
    std::memcpy(c.bytes, bytes.data(), bytes.size());
    c.state.store(2, std::memory_order_release);
}

u64 CasesRecorded() {
    return std::min<u32>(g_next.load(std::memory_order_relaxed), kRing);
}

u64 CasesDropped() {
    return g_dropped.load(std::memory_order_relaxed);
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
    for (CaseSlot& c : g_cases) {
        if (c.state.load(std::memory_order_acquire) != 2) {
            continue;
        }
        const FunctionSpec& spec = Function(c.function);
        const std::filesystem::path file = std::filesystem::path(dir) / (std::string("cases_") + spec.key + ".txt");
        if (FILE* f = std::fopen(file.string().c_str(), "ab")) {
            const std::string hex = ToHex({c.bytes, c.size});
            std::fprintf(f, "%s %s\n", spec.key, hex.c_str());
            std::fclose(f);
        }
        c.state.store(3, std::memory_order_release);
    }
}

} // namespace SecondSon::NativeEngine
