// SecondSonPC R8 - the verification sites of R8 (see r8_verify.h).
#include "runtime/gpu/r8/r8_verify.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#include <fmt/format.h>

#include "base/log.h"

namespace SecondSon::Gpu::R8::Verify {

namespace Detail {
std::atomic<u8> g_enabled[kMaxSites]{};
u32 g_period[kMaxSites]{};
} // namespace Detail

namespace {

inline constexpr u32 kNameChars = 32;
inline constexpr u32 kReasons = 8;

struct Record {
    std::atomic<u32> state{0}; ///< 0 free, 1 being written, 2 ready
    Where where;
    u64 nth = 0; ///< the site's mismatch it was (1: the first)
    u64 expected = 0, actual = 0;
    u32 differences = 0;
    char field[kFieldChars]{};
};

/// One site: its own counters and record slots (a cache line apart from the others' counters).
struct alignas(64) Site {
    std::atomic<u64> checks{0};
    std::atomic<u64> mismatches{0};
    std::atomic<u64> unverifiable[kReasons]{};
    std::atomic<u32> next_record{0};
    char name[kNameChars]{};
    Record records[kRecordsPerSite];
};

struct Requested {
    std::string name;
    u32 period = 1;
    bool registered = false;
};

struct Config {
    bool set = false; ///< SSPC_R8_VERIFY given (the report is written)
    bool all = false;
    u32 all_period = 1;
    std::string spec;
    std::vector<Requested> names;
};

std::array<Site, kMaxSites> g_sites; // [0] unused: SiteId 0 is "no site"
std::mutex g_mutex;                  // registration and the configuration
u32 g_count = 1;
Config g_config;
bool g_config_read = false;

u32 ParsePeriod(std::string_view s) {
    u32 v = 0;
    for (const char c : s) {
        if (c < '0' || c > '9') {
            return 1;
        }
        v = v * 10 + static_cast<u32>(c - '0');
        if (v > 1'000'000'000) {
            break;
        }
    }
    return v == 0 ? 1 : v;
}

Config ParseSpec(std::string_view spec) {
    Config c;
    c.set = !spec.empty();
    c.spec = std::string(spec);
    size_t pos = 0;
    while (pos <= spec.size() && !spec.empty()) {
        const size_t end = std::min(spec.find(',', pos), spec.size());
        std::string_view item = spec.substr(pos, end - pos);
        while (!item.empty() && item.front() == ' ') {
            item.remove_prefix(1);
        }
        while (!item.empty() && item.back() == ' ') {
            item.remove_suffix(1);
        }
        if (!item.empty()) {
            u32 period = 1;
            if (const size_t colon = item.find(':'); colon != std::string_view::npos) {
                period = ParsePeriod(item.substr(colon + 1));
                item = item.substr(0, colon);
            }
            if (item == "all" || item == "1") {
                c.all = true;
                c.all_period = period;
            } else if (item != "0" && item != "off") {
                c.names.push_back({std::string(item), period, false});
            }
        }
        pos = end + 1;
    }
    return c;
}

/// The configuration, read once (under g_mutex).
Config& ConfigLocked() {
    if (!g_config_read) {
        g_config_read = true;
        const char* v = std::getenv("SSPC_R8_VERIFY");
        g_config = ParseSpec(v != nullptr ? std::string_view(v) : std::string_view());
    }
    return g_config;
}

/// Decides a site's enable from the configuration (under g_mutex).
void Decide(SiteId id) {
    Config& c = ConfigLocked();
    u32 period = 0; // 0: off
    for (Requested& r : c.names) {
        if (r.name == g_sites[id].name) {
            r.registered = true;
            period = r.period;
        }
    }
    if (period == 0 && c.all) {
        period = c.all_period;
    }
    Detail::g_period[id] = period == 0 ? 1 : period;
    Detail::g_enabled[id].store(period != 0 ? 1 : 0, std::memory_order_relaxed);
}

void Count(std::atomic<u64>& c) {
    c.fetch_add(1, std::memory_order_relaxed);
}

void Keep(Site& s, SiteId id, const Where& where, u64 nth, std::string_view field, u64 expected, u64 actual,
          u32 differences) {
    const u32 i = s.next_record.fetch_add(1, std::memory_order_relaxed);
    if (i < kRecordsPerSite) {
        Record& r = s.records[i];
        r.state.store(1, std::memory_order_relaxed);
        r.where = where;
        r.nth = nth;
        r.expected = expected;
        r.actual = actual;
        r.differences = differences;
        const size_t n = std::min<size_t>(field.size(), kFieldChars - 1);
        std::memcpy(r.field, field.data(), n);
        r.field[n] = 0;
        r.state.store(2, std::memory_order_release);
    } else {
        s.next_record.store(kRecordsPerSite, std::memory_order_relaxed); // no wrap of the index
    }
    // the log: the first few, then one line at every power of two (bounded however long the session)
    if (nth <= kLogPerSite || std::has_single_bit(nth)) {
        SSPC_LOG(GPU, Warning,
                 "R8 verify: {} mismatch #{} at frame {} draw {} key {:#x}: {} expected {:#x} actual {:#x}{}", s.name,
                 nth, where.frame, where.draw, where.key, field, expected, actual,
                 differences > 1 ? fmt::format(" (+{} more fields)", differences - 1) : std::string());
    }
    (void)id;
}

thread_local u32 t_sample[kMaxSites]{};

} // namespace

SiteId Register(std::string_view name) {
    std::lock_guard lock(g_mutex);
    const std::string_view n = name.substr(0, kNameChars - 1);
    for (u32 i = 1; i < g_count; ++i) {
        if (n == g_sites[i].name) {
            return i;
        }
    }
    if (g_count >= kMaxSites) {
        SSPC_LOG(GPU, Warning, "R8 verify: more than {} sites; '{}' does not verify", kMaxSites - 1, n);
        return 0;
    }
    const SiteId id = g_count++;
    std::memcpy(g_sites[id].name, n.data(), n.size());
    g_sites[id].name[n.size()] = 0;
    Decide(id);
    if (Enabled(id)) {
        SSPC_LOG(GPU, Info, "R8 verify: site {} verifies (every {}. sample)", n, Detail::g_period[id]);
    }
    return id;
}

bool Sample(SiteId site) {
    if (site >= kMaxSites) {
        return false;
    }
    const u32 period = Detail::g_period[site];
    if (period <= 1) {
        return true;
    }
    if (++t_sample[site] >= period) {
        t_sample[site] = 0;
        return true;
    }
    return false;
}

// ---- Diff

void Diff::Note(std::string_view name, std::string_view suffix, u64 expected, u64 actual) {
    if (differences_++ != 0) {
        return;
    }
    expected_ = expected;
    actual_ = actual;
    size_t n = 0;
    const auto put = [&](std::string_view s) {
        const size_t k = std::min(s.size(), static_cast<size_t>(kFieldChars - 1) - n);
        std::memcpy(field_ + n, s.data(), k);
        n += k;
    };
    put({prefix_, prefix_len_});
    if (prefix_len_ != 0 && !name.empty() && name.front() != '[' && name.front() != '+') {
        put(".");
    }
    put(name);
    put(suffix);
    field_[n] = 0;
}

void Diff::Reset() {
    prefix_len_ = 0;
    prefix_[0] = 0;
    field_[0] = 0;
    expected_ = actual_ = 0;
    differences_ = 0;
}

bool Diff::Bytes(std::string_view name, const void* expected, const void* actual, size_t bytes) {
    const auto* e = static_cast<const u8*>(expected);
    const auto* a = static_cast<const u8*>(actual);
    if (bytes == 0 || std::memcmp(e, a, bytes) == 0) {
        return true;
    }
    size_t i = 0;
    while (e[i] == a[i]) {
        ++i;
    }
    const size_t w = i & ~size_t{7};
    u64 ew = 0, aw = 0;
    std::memcpy(&ew, e + w, std::min<size_t>(8, bytes - w));
    std::memcpy(&aw, a + w, std::min<size_t>(8, bytes - w));
    char suffix[24];
    const int k = std::snprintf(suffix, sizeof(suffix), "+0x%zx", i);
    Note(name, {suffix, static_cast<size_t>(k > 0 ? k : 0)}, ew, aw);
    return false;
}

bool Diff::Size(std::string_view name, size_t expected, size_t actual) {
    if (expected == actual) {
        return true;
    }
    Note(name, ".size", expected, actual);
    return false;
}

bool Diff::Sequence(std::string_view name, std::span<const u64> expected, std::span<const u64> actual) {
    const size_t n = std::min(expected.size(), actual.size());
    for (size_t i = 0; i < n; ++i) {
        if (expected[i] != actual[i]) {
            char suffix[24];
            const int k = std::snprintf(suffix, sizeof(suffix), "[%zu]", i);
            Note(name, {suffix, static_cast<size_t>(k > 0 ? k : 0)}, expected[i], actual[i]);
            return false;
        }
    }
    return Size(name, expected.size(), actual.size());
}

bool Diff::Fields(std::string_view name, std::span<const FieldDesc> fields, const void* expected, const void* actual) {
    const auto* e = static_cast<const u8*>(expected);
    const auto* a = static_cast<const u8*>(actual);
    bool same = true;
    for (const FieldDesc& f : fields) {
        if (std::memcmp(e + f.offset, a + f.offset, f.size) == 0) {
            continue;
        }
        Scope scope(*this, name);
        if (f.size <= 8) {
            u64 ev = 0, av = 0;
            std::memcpy(&ev, e + f.offset, f.size);
            std::memcpy(&av, a + f.offset, f.size);
            Note(f.name, {}, ev, av);
        } else {
            Bytes(f.name, e + f.offset, a + f.offset, f.size);
        }
        same = false;
    }
    return same;
}

Diff::Scope::Scope(Diff& d, std::string_view name, s64 index) : d_(d), len_(d.prefix_len_) {
    char buf[kFieldChars];
    int k = 0;
    if (index >= 0) {
        k = std::snprintf(buf, sizeof(buf), "%s%.*s[%lld]", d.prefix_len_ != 0 && !name.empty() ? "." : "",
                          static_cast<int>(name.size()), name.data(), static_cast<long long>(index));
    } else {
        k = std::snprintf(buf, sizeof(buf), "%s%.*s", d.prefix_len_ != 0 && !name.empty() ? "." : "",
                          static_cast<int>(name.size()), name.data());
    }
    const size_t n = std::min<size_t>(k > 0 ? static_cast<size_t>(k) : 0, kFieldChars - 1 - d.prefix_len_);
    std::memcpy(d.prefix_ + d.prefix_len_, buf, n);
    d.prefix_len_ += static_cast<u32>(n);
    d.prefix_[d.prefix_len_] = 0;
}

Diff::Scope::~Scope() {
    d_.prefix_len_ = len_;
    d_.prefix_[len_] = 0;
}

// ---- checks

bool Check(SiteId site, const Where& where, const Diff& d) {
    if (site == 0 || site >= kMaxSites) {
        return d.Same();
    }
    Site& s = g_sites[site];
    Count(s.checks);
    if (d.Same()) {
        return true;
    }
    const u64 nth = s.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    Keep(s, site, where, nth, d.FirstField(), d.Expected(), d.Actual(), d.Differences());
    return false;
}

bool Check(SiteId site, const Where& where, bool same, std::string_view field, u64 expected, u64 actual) {
    if (site == 0 || site >= kMaxSites) {
        return same;
    }
    Site& s = g_sites[site];
    Count(s.checks);
    if (same) {
        return true;
    }
    const u64 nth = s.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
    Keep(s, site, where, nth, field, expected, actual, 1);
    return false;
}

void Unverifiable(SiteId site, u32 reason) {
    if (site != 0 && site < kMaxSites) {
        Count(g_sites[site].unverifiable[std::min(reason, kReasons - 1)]);
    }
}

SiteStats Stats(SiteId site) {
    SiteStats out;
    if (site == 0 || site >= kMaxSites) {
        return out;
    }
    const Site& s = g_sites[site];
    out.checks = s.checks.load(std::memory_order_relaxed);
    out.mismatches = s.mismatches.load(std::memory_order_relaxed);
    for (const auto& u : s.unverifiable) {
        out.unverifiable += u.load(std::memory_order_relaxed);
    }
    out.records = std::min(s.next_record.load(std::memory_order_relaxed), kRecordsPerSite);
    return out;
}

std::string_view Name(SiteId site) {
    return site == 0 || site >= kMaxSites ? std::string_view() : std::string_view(g_sites[site].name);
}

std::string Records(SiteId site) {
    std::string out;
    if (site == 0 || site >= kMaxSites) {
        return out;
    }
    const Site& s = g_sites[site];
    for (const Record& r : s.records) {
        if (r.state.load(std::memory_order_acquire) != 2) {
            continue;
        }
        out += fmt::format("    #{} frame {} draw {} key {:#x}: {} expected {:#x} actual {:#x}{}\n", r.nth,
                           r.where.frame, r.where.draw, r.where.key, r.field, r.expected, r.actual,
                           r.differences > 1 ? fmt::format(" (+{} more fields)", r.differences - 1) : std::string());
    }
    return out;
}

std::string Report() {
    std::lock_guard lock(g_mutex);
    const Config& c = ConfigLocked();
    if (!c.set) {
        return {};
    }
    std::string out = fmt::format("R8 verify (SSPC_R8_VERIFY={}): {} sites registered\n", c.spec, g_count - 1);
    for (u32 i = 1; i < g_count; ++i) {
        const SiteStats st = Stats(i);
        const Site& s = g_sites[i];
        std::string reasons;
        for (u32 r = 0; r < kReasons; ++r) {
            if (const u64 n = s.unverifiable[r].load(std::memory_order_relaxed); n != 0) {
                reasons += fmt::format(" r{} {}", r, n);
            }
        }
        out += fmt::format("  {} [{}]: checks {}, mismatches {}, not verifiable {}{}, records kept {}\n", s.name,
                           Enabled(i) ? fmt::format("verify every {}", Detail::g_period[i]) : std::string("off"),
                           st.checks, st.mismatches, st.unverifiable,
                           reasons.empty() ? std::string() : " (by reason:" + reasons + ")", st.records);
        out += Records(i);
    }
    for (const Requested& r : c.names) {
        if (!r.registered) {
            out += fmt::format("  requested but no such site registered: {}\n", r.name);
        }
    }
    return out;
}

void ResetForTests(std::string_view spec) {
    std::lock_guard lock(g_mutex);
    g_config_read = true;
    g_config = ParseSpec(spec);
    for (u32 i = 1; i < g_count; ++i) {
        Site& s = g_sites[i];
        s.checks.store(0);
        s.mismatches.store(0);
        for (auto& u : s.unverifiable) {
            u.store(0);
        }
        s.next_record.store(0);
        for (Record& r : s.records) {
            r.state.store(0);
        }
        Decide(i);
    }
    std::fill(std::begin(t_sample), std::end(t_sample), 0u);
}

} // namespace SecondSon::Gpu::R8::Verify
