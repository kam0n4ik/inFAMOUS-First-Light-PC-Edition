// SecondSonPC — import call counter (hand-encoded x86-64 thunks, no JIT library needed).
#include "trace/import_counter.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

#include <fmt/format.h>

#include "base/log.h"

namespace SecondSon::Trace {

namespace {

constexpr size_t kThunkSize = 16;

struct Slot {
    std::atomic<u64> calls; // target of `lock inc`
    void* target;           // target of `jmp [rip+...]`
};
static_assert(sizeof(Slot) == 16);

struct Entry {
    std::string nid;
    std::string library;
    std::string module;
    std::string name;
    Owner owner;
};

std::mutex g_mutex;
u8* g_code = nullptr;
Slot* g_slots = nullptr;
size_t g_capacity = 0;
size_t g_code_bytes = 0;
bool g_sealed = false;
std::vector<Entry> g_entries;

std::mutex g_dump_mutex;
std::filesystem::path g_dump_file;
std::jthread g_dumper;

size_t RoundUp(size_t value, size_t align) {
    return (value + align - 1) / align * align;
}

const char* OwnerName(Owner owner) {
    switch (owner) {
    case Owner::Native:
        return "NATIVE";
    case Owner::Observed:
        return "OBSERVED";
    case Owner::Bridge:
        return "BRIDGE";
    }
    return "?";
}

void AppendJsonString(std::string& out, std::string_view s) {
    out += '"';
    for (const char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            out += fmt::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += c;
        }
    }
    out += '"';
}

} // namespace

bool Init(size_t capacity) {
    std::scoped_lock lock{g_mutex};
    if (g_code != nullptr) {
        return true;
    }
    g_code_bytes = RoundUp(capacity * kThunkSize, 0x10000);
    const size_t data_bytes = RoundUp(capacity * sizeof(Slot), 0x10000);
    auto* base = static_cast<u8*>(
        VirtualAlloc(nullptr, g_code_bytes + data_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (base == nullptr) {
        SSPC_LOG(Trace, Error, "import counter: VirtualAlloc({} bytes) failed: {}",
                 g_code_bytes + data_bytes, GetLastError());
        return false;
    }
    g_code = base;
    g_slots = reinterpret_cast<Slot*>(base + g_code_bytes);
    g_capacity = capacity;
    std::memset(g_code, 0xCC, g_code_bytes);
    g_entries.reserve(capacity);
    SSPC_LOG(Trace, Info, "import counter: {} thunk slots at {} (code) / {} (counters)", capacity,
             static_cast<void*>(g_code), static_cast<void*>(g_slots));
    return true;
}

void* Wrap(std::string_view nid, std::string_view library, std::string_view module,
           std::string_view name, Owner owner, void* target) {
    std::scoped_lock lock{g_mutex};
    if (g_code == nullptr || g_sealed || target == nullptr || g_entries.size() >= g_capacity) {
        return nullptr;
    }
    const size_t index = g_entries.size();
    u8* t = g_code + index * kThunkSize;
    Slot* slot = &g_slots[index];
    slot->calls.store(0, std::memory_order_relaxed);
    slot->target = target;

    const auto rel = [](const void* to, const u8* next) {
        return static_cast<s32>(reinterpret_cast<const u8*>(to) - next);
    };
    // F0 48 FF 05 disp32 : lock inc qword ptr [rip + disp32]
    t[0] = 0xF0;
    t[1] = 0x48;
    t[2] = 0xFF;
    t[3] = 0x05;
    const s32 counter_disp = rel(&slot->calls, t + 8);
    std::memcpy(t + 4, &counter_disp, 4);
    // FF 25 disp32 : jmp qword ptr [rip + disp32]
    t[8] = 0xFF;
    t[9] = 0x25;
    const s32 target_disp = rel(&slot->target, t + 14);
    std::memcpy(t + 10, &target_disp, 4);
    // t[14], t[15] stay 0xCC (int3)

    g_entries.push_back(Entry{std::string(nid), std::string(library), std::string(module),
                              std::string(name), owner});
    return t;
}

void Seal() {
    std::scoped_lock lock{g_mutex};
    if (g_code == nullptr || g_sealed) {
        return;
    }
    DWORD old = 0;
    if (!VirtualProtect(g_code, g_code_bytes, PAGE_EXECUTE_READ, &old)) {
        SSPC_LOG(Trace, Critical, "import counter: VirtualProtect(RX) failed: {}", GetLastError());
        return;
    }
    FlushInstructionCache(GetCurrentProcess(), g_code, g_code_bytes);
    g_sealed = true;
    SSPC_LOG(Trace, Info, "import counter: sealed {} thunks", g_entries.size());
}

size_t WrappedCount() {
    std::scoped_lock lock{g_mutex};
    return g_entries.size();
}

size_t WrappedCount(Owner owner) {
    std::scoped_lock lock{g_mutex};
    return static_cast<size_t>(std::count_if(g_entries.begin(), g_entries.end(),
                                             [owner](const Entry& e) { return e.owner == owner; }));
}

void DumpJson(const std::filesystem::path& file) {
    struct Row {
        const Entry* entry;
        u64 calls;
    };
    std::vector<Row> rows;
    size_t native = 0, observed = 0, bridge = 0;
    {
        std::scoped_lock lock{g_mutex};
        for (size_t i = 0; i < g_entries.size(); ++i) {
            const auto& e = g_entries[i];
            native += e.owner == Owner::Native;
            observed += e.owner == Owner::Observed;
            bridge += e.owner == Owner::Bridge;
            const u64 calls = g_slots[i].calls.load(std::memory_order_relaxed);
            // SecondSonPC's own exports are always listed (ownership), BRIDGE ones only when called.
            if (calls != 0 || e.owner != Owner::Bridge) {
                rows.push_back({&e, calls});
            }
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.calls > b.calls; });

    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string out = fmt::format(
        "{{\n  \"generated\": \"{:04}-{:02}-{:02} {:02}:{:02}:{:02}\",\n  \"wrapped_total\": {},\n"
        "  \"wrapped\": {{\"native\": {}, \"observed\": {}, \"bridge\": {}}},\n  \"entries\": [\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, native + observed + bridge,
        native, observed, bridge);
    for (size_t i = 0; i < rows.size(); ++i) {
        const auto& e = *rows[i].entry;
        out += "    {\"nid\": ";
        AppendJsonString(out, e.nid);
        out += ", \"name\": ";
        AppendJsonString(out, e.name);
        out += ", \"library\": ";
        AppendJsonString(out, e.library);
        out += ", \"module\": ";
        AppendJsonString(out, e.module);
        out += fmt::format(", \"owner\": \"{}\", \"calls\": {}}}{}\n", OwnerName(e.owner),
                           rows[i].calls, i + 1 < rows.size() ? "," : "");
    }
    out += "  ]\n}\n";

    std::scoped_lock dump_lock{g_dump_mutex};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = std::filesystem::path(file).concat(L".tmp");
    if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

void StartPeriodicDump(const std::filesystem::path& file, u32 seconds) {
    g_dump_file = file;
    g_dumper = std::jthread([file, seconds](std::stop_token stop) {
        while (!stop.stop_requested()) {
            for (u32 i = 0; i < seconds * 10 && !stop.stop_requested(); ++i) {
                Sleep(100);
            }
            DumpJson(file);
        }
    });
}

void FinalDump() {
    if (g_dump_file.empty()) {
        return;
    }
    DumpJson(g_dump_file);
    SSPC_LOG(Trace, Info, "import counter: final dump -> {}", g_dump_file.string());
}

} // namespace SecondSon::Trace
