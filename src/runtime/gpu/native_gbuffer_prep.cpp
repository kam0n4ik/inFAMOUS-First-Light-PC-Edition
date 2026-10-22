// SecondSonPC - the Native Engine G-buffer draw preparation (0x1a1770). See native_gbuffer_prep.h.
#include "runtime/gpu/native_gbuffer_prep.h"

#include <cstdlib>

#include <windows.h>

#include "base/log.h"

namespace SecondSon::Gpu {

namespace {

std::string EnvA(const char* name) {
    char v[32] = {};
    const DWORD n = GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) ? std::string(v, n) : std::string();
}

u64 Mix(u64 h, u64 x) {
    h ^= x + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

} // namespace

NativeGBuffer::NativeGBuffer() {
    const std::string v = EnvA("SSPC_NATIVE_GBUFFER");
    if (v.empty() || v == "0" || v == "off") {
        return;
    }
    if (v == "verify") {
        mode_ = Mode::Verify;
    } else if (v == "guarded") {
        mode_ = Mode::Guarded;
    } else if (v == "native" || v == "1") {
        mode_ = Mode::Native;
    } else {
        SSPC_LOG(GPU, Warning, "SSPC_NATIVE_GBUFFER={} not understood (verify|guarded|native|off) - off", v);
        return;
    }
    if (mode_ == Mode::Guarded) {
        sample_ = 32;
        if (const std::string s = EnvA("SSPC_NATIVE_GBUFFER_SAMPLE"); !s.empty()) {
            const u32 n = static_cast<u32>(std::strtoul(s.c_str(), nullptr, 10));
            sample_ = std::max<u32>(n, 1);
        }
    } else if (mode_ == Mode::Verify) {
        sample_ = 1; // (every hit)
    }
    SSPC_LOG(GPU, Warning,
             "EXPERIMENT: SSPC_NATIVE_GBUFFER={} - the Native Engine prepares 0x1a1770's G-buffer draws (keyed by engine "
             "identity, re-derived through RedecodeStage){}",
             v, mode_ == Mode::Guarded ? fmt::format(", every {}th hit verified", sample_) : "");
}

u64 NativeGBuffer::Hash(const Key& k) {
    u64 h = 0xcbf29ce484222325ull;
    h = Mix(h, k.vs_addr);
    h = Mix(h, k.ps_addr);
    h = Mix(h, (u64{k.vs_rsrc1} << 32) | k.vs_rsrc2);
    h = Mix(h, (u64{k.ps_rsrc1} << 32) | k.ps_rsrc2);
    h = Mix(h, k.pipeline_key);
    h = Mix(h, k.color_hash);
    return h;
}

NativeGBuffer::Entry* NativeGBuffer::Find(u64 h, const Key& k) {
    lookups_.fetch_add(1, std::memory_order_relaxed);
    const auto it = entries_.find(h);
    if (it == entries_.end()) {
        return nullptr;
    }
    if (!(it->second.key == k)) {
        collisions_.fetch_add(1, std::memory_order_relaxed);
        return nullptr; // another identity at the same hash: not this one's anchor
    }
    it->second.last_frame = frame_.load(std::memory_order_relaxed);
    if (!it->second.usable) {
        return nullptr; // known not reusable: the frontend prepares it
    }
    return &it->second;
}

void NativeGBuffer::Store(u64 h, const Entry& e) {
    const auto it = entries_.find(h);
    if (it != entries_.end()) {
        if (!(it->second.key == e.key)) {
            return; // a collision: the first identity keeps the slot (the other stays on the frontend path)
        }
        it->second = e;
        it->second.last_frame = frame_.load(std::memory_order_relaxed);
        return;
    }
    if (entries_.size() >= max_entries_) {
        full_.fetch_add(1, std::memory_order_relaxed);
        return; // (the city's distinct G-buffer identities are far fewer than the cap; no eviction needed)
    }
    Entry& slot = entries_[h];
    slot = e;
    slot.last_frame = frame_.load(std::memory_order_relaxed);
    (slot.usable ? learned_ : unusable_).fetch_add(1, std::memory_order_relaxed);
}

void NativeGBuffer::NoteVerified(bool mismatch) {
    verified_.fetch_add(1, std::memory_order_relaxed);
    if (mismatch) {
        const u64 n = mismatches_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (mode_ == Mode::Guarded && n >= disable_after_ && trusted_.exchange(false)) {
            SSPC_LOG(GPU, Error,
                     "SSPC_NATIVE_GBUFFER=guarded: {} mismatches - the native G-buffer path is disabled, the frontend "
                     "prepares every draw from here",
                     n);
        }
    }
}

std::string NativeGBuffer::Report() const {
    if (mode_ == Mode::Off) {
        return {};
    }
    const auto ld = [](const std::atomic<u64>& a) { return a.load(std::memory_order_relaxed); };
    const char* name = mode_ == Mode::Verify ? "verify" : mode_ == Mode::Guarded ? "guarded" : "native";
    std::string out = fmt::format(
        "native G-buffer 0x1a1770 (SSPC_NATIVE_GBUFFER={}): lookups {}, hits {} (reused the anchor, no frontend), "
        "learned {}, not reusable {}, hash collisions {}, at the cap {}; refused by RedecodeStage (2 wrote/read "
        "failed, 3 layout, 4 key, 5 bindings, 6 walk): {}:{} {}:{} {}:{} {}:{} {}:{}",
        name, ld(lookups_), ld(hits_), ld(learned_), ld(unusable_), ld(collisions_), ld(full_), 2, ld(refused_[2]), 3,
        ld(refused_[3]), 4, ld(refused_[4]), 5, ld(refused_[5]), 6, ld(refused_[6]));
    if (mode_ != Mode::Native) {
        out += fmt::format("; verified {}, mismatches {}", ld(verified_), ld(mismatches_));
    }
    if (!trusted_.load(std::memory_order_relaxed)) {
        out += " [DISABLED by mismatches]";
    }
    out += "\n";
    return out;
}

} // namespace SecondSon::Gpu
