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

constexpr size_t kCounterThunkSize = 16;
constexpr size_t kOrderThunkSize = 96;

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
size_t g_thunk_size = kCounterThunkSize;
bool g_sealed = false;
std::vector<Entry> g_entries;

size_t g_order_request = 0;  // SetCallOrderCapacity
size_t g_order_capacity = 0; // active capacity (0 = off)
std::atomic<u64>* g_order_cursor = nullptr;
u32* g_order = nullptr;

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
    case Owner::CommonStub:
        return "COMMON_STUB";
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

s32 Rel(const void* to, const u8* next) {
    return static_cast<s32>(reinterpret_cast<const u8*>(to) - next);
}

/// lock inc qword [rip+counter] ; jmp qword [rip+target]
void EmitCounter(u8* t, Slot* slot) {
    t[0] = 0xF0; // lock inc qword ptr [rip + disp32]
    t[1] = 0x48;
    t[2] = 0xFF;
    t[3] = 0x05;
    const s32 counter_disp = Rel(&slot->calls, t + 8);
    std::memcpy(t + 4, &counter_disp, 4);
    t[8] = 0xFF; // jmp qword ptr [rip + disp32]
    t[9] = 0x25;
    const s32 target_disp = Rel(&slot->target, t + 14);
    std::memcpy(t + 10, &target_disp, 4);
}

/// Call-order prefix (52 bytes); the counter thunk follows at +52.
void EmitOrder(u8* t, u32 index) {
    const u32 cap = static_cast<u32>(g_order_capacity);
    // +0  cmp qword ptr [rip + disp32], imm32
    t[0] = 0x48;
    t[1] = 0x81;
    t[2] = 0x3D;
    const s32 d0 = Rel(g_order_cursor, t + 11);
    std::memcpy(t + 3, &d0, 4);
    std::memcpy(t + 7, &cap, 4);
    // +11 jae skip (+52)
    t[11] = 0x73;
    t[12] = static_cast<u8>(52 - 13);
    // +13 mov r11d, 1
    t[13] = 0x41;
    t[14] = 0xBB;
    const u32 one = 1;
    std::memcpy(t + 15, &one, 4);
    // +19 lock xadd qword ptr [rip + disp32], r11
    t[19] = 0xF0;
    t[20] = 0x4C;
    t[21] = 0x0F;
    t[22] = 0xC1;
    t[23] = 0x1D;
    const s32 d1 = Rel(g_order_cursor, t + 28);
    std::memcpy(t + 24, &d1, 4);
    // +28 cmp r11, imm32
    t[28] = 0x49;
    t[29] = 0x81;
    t[30] = 0xFB;
    std::memcpy(t + 31, &cap, 4);
    // +35 jae skip (+52)
    t[35] = 0x73;
    t[36] = static_cast<u8>(52 - 37);
    // +37 lea r10, [rip + disp32]
    t[37] = 0x4C;
    t[38] = 0x8D;
    t[39] = 0x15;
    const s32 d2 = Rel(g_order, t + 44);
    std::memcpy(t + 40, &d2, 4);
    // +44 mov dword ptr [r10 + r11*4], imm32
    t[44] = 0x43;
    t[45] = 0xC7;
    t[46] = 0x04;
    t[47] = 0x9A;
    std::memcpy(t + 48, &index, 4);
}

} // namespace

void SetCallOrderCapacity(size_t call_order) {
    std::scoped_lock lock{g_mutex};
    g_order_request = call_order;
}

bool Init(size_t capacity) {
    std::scoped_lock lock{g_mutex};
    if (g_code != nullptr) {
        return true;
    }
    g_order_capacity = std::min<size_t>(g_order_request, 0x7FFFFFFF);
    g_thunk_size = g_order_capacity != 0 ? kOrderThunkSize : kCounterThunkSize;
    g_code_bytes = RoundUp(capacity * g_thunk_size, 0x10000);
    const size_t data_bytes = RoundUp(capacity * sizeof(Slot), 0x10000);
    const size_t order_bytes = g_order_capacity != 0 ? RoundUp(64 + g_order_capacity * 4, 0x10000) : 0;
    auto* base = static_cast<u8*>(VirtualAlloc(nullptr, g_code_bytes + data_bytes + order_bytes,
                                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (base == nullptr) {
        SSPC_LOG(Trace, Error, "import counter: VirtualAlloc({} bytes) failed: {}",
                 g_code_bytes + data_bytes + order_bytes, GetLastError());
        return false;
    }
    g_code = base;
    g_slots = reinterpret_cast<Slot*>(base + g_code_bytes);
    if (g_order_capacity != 0) {
        g_order_cursor = reinterpret_cast<std::atomic<u64>*>(base + g_code_bytes + data_bytes);
        g_order = reinterpret_cast<u32*>(base + g_code_bytes + data_bytes + 64);
        g_order_cursor->store(0);
    }
    g_capacity = capacity;
    std::memset(g_code, 0xCC, g_code_bytes);
    g_entries.reserve(capacity);
    SSPC_LOG(Trace, Info, "import counter: {} thunk slots at {} (code) / {} (counters){}", capacity,
             static_cast<void*>(g_code), static_cast<void*>(g_slots),
             g_order_capacity != 0 ? fmt::format(", call order: first {} calls", g_order_capacity) : "");
    return true;
}

void* Wrap(std::string_view nid, std::string_view library, std::string_view module,
           std::string_view name, Owner owner, void* target) {
    std::scoped_lock lock{g_mutex};
    if (g_code == nullptr || g_sealed || target == nullptr || g_entries.size() >= g_capacity) {
        return nullptr;
    }
    const size_t index = g_entries.size();
    u8* t = g_code + index * g_thunk_size;
    Slot* slot = &g_slots[index];
    slot->calls.store(0, std::memory_order_relaxed);
    slot->target = target;
    if (g_order_capacity != 0) {
        EmitOrder(t, static_cast<u32>(index));
        EmitCounter(t + 52, slot);
    } else {
        EmitCounter(t, slot);
    }
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

const void* ThunkTarget(u64 address) {
    std::scoped_lock lock{g_mutex};
    if (g_code == nullptr) {
        return nullptr;
    }
    const u64 begin = reinterpret_cast<u64>(g_code);
    if (address < begin || address >= begin + g_entries.size() * g_thunk_size ||
        (address - begin) % g_thunk_size != 0) {
        return nullptr;
    }
    return g_slots[(address - begin) / g_thunk_size].target;
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
    size_t native = 0, observed = 0, bridge = 0, stub = 0;
    {
        std::scoped_lock lock{g_mutex};
        for (size_t i = 0; i < g_entries.size(); ++i) {
            const auto& e = g_entries[i];
            native += e.owner == Owner::Native;
            observed += e.owner == Owner::Observed;
            bridge += e.owner == Owner::Bridge;
            stub += e.owner == Owner::CommonStub;
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
        "  \"wrapped\": {{\"native\": {}, \"observed\": {}, \"bridge\": {}, \"common_stub\": {}}},\n"
        "  \"entries\": [\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        native + observed + bridge + stub, native, observed, bridge, stub);
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

void DumpCallOrder(const std::filesystem::path& file) {
    std::string out;
    {
        std::scoped_lock lock{g_mutex};
        if (g_order_capacity == 0) {
            return;
        }
        const u64 total = g_order_cursor->load();
        const u64 n = std::min<u64>(total, g_order_capacity);
        out.reserve(n * 48);
        out += fmt::format("# first {} of {} import calls\n", n, total);
        for (u64 i = 0; i < n; ++i) {
            const u32 index = g_order[i];
            if (index < g_entries.size()) {
                const auto& e = g_entries[index];
                out += fmt::format("{}\t{}\t{}\t{}\n", i, e.name, e.library, OwnerName(e.owner));
            } else {
                out += fmt::format("{}\t?{}\n", i, index);
            }
        }
    }
    std::scoped_lock dump_lock{g_dump_mutex};
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (FILE* f = _wfopen(file.c_str(), L"wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
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
            DumpCallOrder(file.parent_path() / "call_order.txt"); // survives a crash (SSPC-001)
        }
    });
}

void FinalDump() {
    if (g_dump_file.empty()) {
        return;
    }
    DumpJson(g_dump_file);
    DumpCallOrder(g_dump_file.parent_path() / "call_order.txt");
    SSPC_LOG(Trace, Info, "import counter: final dump -> {}", g_dump_file.string());
}

} // namespace SecondSon::Trace
