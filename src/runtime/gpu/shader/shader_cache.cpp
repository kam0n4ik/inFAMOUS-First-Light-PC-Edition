// SecondSonPC — the shader frontend's caches (P4.3). See shader_cache.h.
#include "runtime/gpu/shader/shader_cache.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

namespace SecondSon::Gpu::Shader {

Layout* Program::FindLayout(const RuntimeParams& runtime) {
    for (Layout& l : layouts) {
        if (std::memcmp(&l.runtime, &runtime, sizeof(RuntimeParams)) == 0) {
            return &l;
        }
    }
    return nullptr;
}

Module* Program::FindModule(const SpecializationKey& key, u64& collisions) {
    for (auto& m : modules) {
        if (!m->keyed || m->key.hash != key.hash) {
            continue;
        }
        if (m->key.words == key.words) {
            return m.get();
        }
        ++collisions; // the same 64-bit hash, another specialization: never taken for it
    }
    return nullptr;
}

ProgramCache::Lookup ProgramCache::Get(GuestReader& memory, u64 addr, HwStage stage) {
    ++stats_.lookups;
    Lookup out;
    const u64 key = (addr << 3) | static_cast<u64>(stage);
    auto it = addresses_.find(key);
    if (it != addresses_.end() && !it->second.dirty && validation_ == Validation::PerFrame &&
        it->second.epoch == epoch_) {
        ++stats_.trusted;
        out.program = it->second.program;
        out.footer_hash = it->second.footer_hash;
        return out;
    }
    // read and compare (the first use of the address, or its revalidation)
    ++stats_.reads;
    out.read = true;
    const ProgramError e = ReadProgram(memory, addr, stage, scratch_, scan_);
    if (e != ProgramError::None) {
        ++stats_.errors[static_cast<size_t>(e)];
        out.error = e;
        if (it != addresses_.end()) {
            it->second.dirty = true; // not trusted until it reads again
        }
        return out;
    }
    stats_.bytes_read += scratch_.code.size() * 4 + kFooterBytes;
    if (it != addresses_.end()) {
        Program* p = it->second.program;
        if (p->code.size() == scratch_.code.size() &&
            std::memcmp(p->code.data(), scratch_.code.data(), p->code.size() * 4) == 0) {
            ++stats_.same;
            it->second.epoch = epoch_;
            it->second.dirty = false;
            it->second.footer_hash = scratch_.footer.shader_hash;
            out.program = p;
            out.footer_hash = scratch_.footer.shader_hash;
            return out;
        }
        ++stats_.changed;
        out.changed = true;
    }
    const ContentHash hash = test_hash_ != nullptr ? test_hash_(scratch_.code) : scratch_.hash;
    const ProgramIdentity id{hash, static_cast<u32>(scratch_.code.size() * 4), stage};
    auto pit = programs_.find(id);
    Program* program = nullptr;
    if (pit != programs_.end() && pit->second->code == scratch_.code) {
        program = pit->second.get();
    } else if (pit != programs_.end()) {
        // a 128-bit hash collision (never seen): refuse rather than merge two programs
        ++stats_.errors[static_cast<size_t>(ProgramError::Collision)];
        out.error = ProgramError::Collision;
        return out;
    } else {
        auto p = std::make_unique<Program>();
        p->index = next_index_++;
        p->id = id;
        p->footer_hash = scratch_.footer.shader_hash;
        p->first_address = addr;
        p->code = scratch_.code;
        code_bytes_ += p->code.size() * 4;
        program = p.get();
        programs_.emplace(id, std::move(p));
        ++stats_.new_programs;
    }
    ++program->addresses;
    addresses_[key] = AddressEntry{program, epoch_, scratch_.footer.shader_hash, id.length, false};
    out.program = program;
    out.footer_hash = scratch_.footer.shader_hash;
    return out;
}

void ProgramCache::Invalidate(u64 addr, u64 size) {
    const u64 end = addr + size;
    for (auto& [key, entry] : addresses_) {
        const u64 start = key >> 3;
        // the program's code and its footer
        if (start < end && addr < start + entry.length + kFooterBytes + kFooterSearchBytes) {
            if (!entry.dirty) {
                ++stats_.invalidations;
            }
            entry.dirty = true;
        }
    }
}

std::vector<Program*> ProgramCache::All() const {
    std::vector<Program*> out;
    out.reserve(programs_.size());
    for (const auto& [id, p] : programs_) {
        out.push_back(p.get());
    }
    std::sort(out.begin(), out.end(), [](const Program* a, const Program* b) { return a->index < b->index; });
    return out;
}

const FetchCache::Entry& FetchCache::Get(GuestReader& memory, u64 addr, u32 sgpr_base, u64 epoch,
                                         Validation validation) {
    const u64 key = (addr << 8) ^ sgpr_base;
    Entry& e = entries_[key];
    if (e.epoch == epoch && validation == Validation::PerFrame && e.epoch != 0) {
        return e;
    }
    ++reads;
    // read up to the fetch shader's s_setpc_b64 (64 dwords at a time, at most kMaxFetchDwords)
    scratch_.clear();
    std::string error;
    FetchShader parsed;
    bool ok = false;
    for (u32 have = 0; have < kMaxFetchDwords;) {
        const u32 chunk = std::min<u32>(64, kMaxFetchDwords - have);
        scratch_.resize(have + chunk);
        if (addr == 0 || addr + (have + chunk) * 4ull > (1ull << 40) ||
            !memory.Read(addr + have * 4ull, scratch_.data() + have, chunk * 4ull)) {
            error = fmt::format("the fetch shader at {:#x} is unreadable", addr);
            break;
        }
        have += chunk;
        if (ParseFetchShaderCode(scratch_, sgpr_base, parsed, error)) {
            ok = true;
            break;
        }
        if (error.rfind("no s_setpc_b64", 0) != 0 && error.rfind("a buffer load cut", 0) != 0) {
            break; // malformed, not merely cut by the read
        }
    }
    if (ok) {
        scratch_.resize(parsed.size / 4);
        if (e.ok && e.code != scratch_) {
            ++changed;
        }
        e.code = scratch_;
        e.fetch = std::move(parsed);
        e.error.clear();
    } else {
        e.fetch = {};
        e.code.clear();
        e.error = std::move(error);
    }
    e.ok = ok;
    e.epoch = epoch;
    return e;
}

} // namespace SecondSon::Gpu::Shader
