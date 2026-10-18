// SecondSonPC — the game's material draws from its packs (static). See static_draws.h.
#include "runtime/gpu/static/static_draws.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <span>
#include <unordered_set>

#include <fmt/format.h>
#include <xxhash.h>

#include "runtime/gpu/shader/gcn_program.h"

namespace SecondSon::Gpu::Static {

namespace GS = Gpu::Shader;

namespace {

constexpr u32 kTagKnli = 0x494C4E4B; // 'KNLI'
constexpr u32 kTagDic = 0x43494420;  // ' DIC'
constexpr u32 kTagEnd = 0x454E4420;  // ' DNE'
constexpr u32 kPassStride = 0xB8;
constexpr u32 kMaxPasses = 64, kMaxResources = 64, kMaxStreams = 16;

bool IsPack(const std::filesystem::path& p) {
    std::string ext = p.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return static_cast<char>(std::tolower(static_cast<u8>(c))); });
    return ext == ".xpps";
}

/// One pack's data and links. Offsets are relative to the data start (the header's size).
struct Pack {
    std::span<const u8> d;              ///< the data (after the header)
    std::unordered_map<u32, u32> ptr;   ///< kind 0/3: location -> target data offset
    std::unordered_map<u32, u32> addr;  ///< kind 4: location -> the target's data offset (the loader makes it >> 8)
    std::unordered_set<u32> vsharp;     ///< kind 2 locations
    std::unordered_map<u32, u64> dic;   ///< location -> class hash

    bool Has(u64 off, u64 n) const { return off + n <= d.size(); }
    u32 U32(u64 off) const {
        u32 v = 0;
        if (Has(off, 4)) {
            std::memcpy(&v, d.data() + off, 4);
        }
        return v;
    }
    u64 U64(u64 off) const {
        u64 v = 0;
        if (Has(off, 8)) {
            std::memcpy(&v, d.data() + off, 8);
        }
        return v;
    }
    u8 U8(u64 off) const { return Has(off, 1) ? d[off] : 0; }
    const u32* Ptr(u32 loc) const {
        const auto it = ptr.find(loc);
        return it != ptr.end() ? &it->second : nullptr;
    }
};

u32 RdU32(const std::vector<u8>& b, u64 o) {
    u32 v = 0;
    if (o + 4 <= b.size()) {
        std::memcpy(&v, b.data() + o, 4);
    }
    return v;
}

/// The KNLI stream decoder (eboot 0x5d2260): u16 codes -> 4-byte-granular data offsets.
template <typename F>
void LinkOffsets(std::span<const u8> s, F&& fn) {
    u32 base = 0xFFFFFFFFu, last = base;
    for (size_t o = 0; o + 2 <= s.size(); o += 2) {
        s16 v;
        std::memcpy(&v, s.data() + o, 2);
        const u16 u = static_cast<u16>(v);
        if (v >= 0) {
            last = static_cast<u32>(v) * 4 + base;
            fn(last);
        } else if ((u & 0x4000) == 0) {
            for (u32 i = 0; i < (u & 0x3FFFu); ++i) {
                last += 4;
                fn(last);
            }
        } else {
            base = static_cast<u32>(u & 0x3FFFu) << 17;
        }
    }
}

bool ParsePack(const std::vector<u8>& b, Pack& p) {
    if (b.size() < 0xC8 || std::memcmp(b.data(), "KCAP", 4) != 0) {
        return false;
    }
    const u32 hs = RdU32(b, 0x28);
    const u32 segs = RdU32(b, 0xA0);
    if (hs > b.size() || segs > 4096 || 0xC8 + u64{segs} * 0x28 > b.size()) {
        return false;
    }
    p.d = std::span<const u8>(b.data() + hs, b.size() - hs);
    std::vector<std::pair<u32, u32>> links; // (kind, location)
    for (u32 i = 0; i < segs; ++i) {
        const u64 so = 0xC8 + u64{i} * 0x28;
        u16 pool;
        std::memcpy(&pool, b.data() + so + 2, 2);
        const u32 size = RdU32(b, so + 4);
        u64 off;
        std::memcpy(&off, b.data() + so + 8, 8);
        if (pool != 2) {
            continue;
        }
        const u64 st = hs + off, end = std::min<u64>(st + size, b.size());
        for (u64 o = st; o + 8 <= end;) {
            const u32 tag = RdU32(b, o), csize = RdU32(b, o + 4);
            const u64 pl = o + 8;
            if (tag == kTagEnd || pl + csize > b.size()) {
                break;
            }
            if (tag == kTagKnli && csize >= 4) {
                const u32 n = RdU32(b, pl);
                u64 stream = pl + 4 + u64{n} * 8;
                for (u32 k = 0; k < n && pl + 12 + u64{k} * 8 <= b.size(); ++k) {
                    const u32 kind = RdU32(b, pl + 4 + u64{k} * 8);
                    const s32 ln = static_cast<s32>(RdU32(b, pl + 8 + u64{k} * 8));
                    if (ln < 0 || stream + u64(ln) * 2 > b.size()) {
                        break;
                    }
                    LinkOffsets(std::span<const u8>(b.data() + stream, size_t(ln) * 2),
                                [&](u32 loc) { links.emplace_back(kind, loc); });
                    stream += u64(ln) * 2;
                }
            } else if (tag == kTagDic && csize >= 8) {
                const u32 n = RdU32(b, pl);
                for (u32 k = 0; k < n && pl + 8 + u64{k} * 16 + 16 <= b.size(); ++k) {
                    const u64 e = pl + 8 + u64{k} * 16;
                    u64 h;
                    std::memcpy(&h, b.data() + e + 8, 8);
                    p.dic[RdU32(b, e)] = h;
                }
            }
            o = pl + csize;
        }
    }
    for (const auto& [kind, loc] : links) {
        if (!p.Has(loc, 4)) {
            continue;
        }
        const u32 v = p.U32(loc);
        if (kind == 0 || kind == 3) {
            p.ptr[loc] = v;
        } else if (kind == 4) {
            p.addr[loc] = v;
        } else if (kind == 2) {
            p.vsharp.insert(loc);
        }
    }
    return true;
}

/// The program whose code starts at `code` (a data offset): its footer (by the first instruction's token).
bool ProgramAt(const Pack& p, u32 code, GS::ProgramFooter& f, u64& end) {
    if (p.U32(code) != GS::kTokenMovVcchi) {
        return false;
    }
    const u64 fo = u64{code} + (u64{p.U32(code + 4)} + 1) * 8;
    if (!p.Has(fo, GS::kFooterBytes) || !GS::ParseFooter(p.d.data() + fo, f)) {
        return false;
    }
    if (f.length == 0 || f.length % 4 != 0 || f.length > fo - code) {
        return false;
    }
    end = fo + GS::kFooterBytes;
    return true;
}

/// S_SETPC_B64 (SOP1, op 0x20): the fetch shader's last instruction.
bool IsSetpc(u32 w) {
    return (w >> 23) == 0x17D && ((w >> 8) & 0xFF) == 0x20;
}

} // namespace

bool StaticDraws::Scan(const std::filesystem::path& game_dir, StaticScanStats& stats, std::string& error,
                       void (*progress)(void*, u64, u64), void* ctx) {
    const auto t0 = std::chrono::steady_clock::now();
    stats = {};
    *this = StaticDraws{};
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             game_dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && IsPack(it->path())) {
            files.push_back(it->path());
        }
    }
    if (files.empty()) {
        error = fmt::format("no .xpps pack under {}", game_dir.string());
        return false;
    }
    std::ranges::sort(files);
    std::vector<u8> buf;
    for (size_t fi = 0; fi < files.size(); ++fi) {
        std::ifstream in(files[fi], std::ios::binary | std::ios::ate);
        if (in) {
            const auto size = static_cast<size_t>(in.tellg());
            buf.resize(size);
            in.seekg(0);
            in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
        }
        if (!in) {
            ++stats.unreadable;
            continue;
        }
        ++stats.packs;
        stats.bytes += buf.size();
        const u32 pack = static_cast<u32>(packs_.size());
        packs_.push_back(std::filesystem::relative(files[fi], game_dir, ec).generic_string());
        ScanPack(buf, pack, stats);
        if (progress != nullptr) {
            progress(ctx, fi + 1, files.size());
        }
    }
    // texture ids an instance names from another pack
    for (const PendingTexture& t : pending_) {
        const auto it = texture_by_id_.find(t.id);
        if (it != texture_by_id_.end()) {
            instances_[t.instance].textures[t.binding] = it->second + 1;
        } else {
            ++stats.instance_textures_missing;
        }
    }
    pending_.clear();
    stats.programs = programs_.size();
    stats.passes = passes_.size();
    stats.instances = instances_.size();
    stats.geometries = geometries_.size();
    stats.textures = textures_.size();
    stats.shaders = shaders_.size();
    stats.fetches = fetches_.size();
    stats.computes = computes_.size();
    stats.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

void StaticDraws::ScanPack(const std::vector<u8>& data, u32 pack, StaticScanStats& stats) {
    Pack p;
    if (!ParsePack(data, p)) {
        ++stats.unreadable;
        return;
    }
    // textures: a kind-4 link at the T#'s dword 0 with an image type in dword 3; the texture object's id is 0x20
    // after the T# (object +0x48, T# at +0x28)
    for (const auto& [loc, v] : p.addr) {
        if (!p.Has(loc, 0x28)) {
            continue;
        }
        std::array<u32, 8> t;
        std::memcpy(t.data(), p.d.data() + loc, 32);
        if ((t[3] >> 28) < 8) {
            continue;
        }
        const u64 id = p.U64(loc + 0x20);
        if (id != 0 && texture_by_id_.try_emplace(id, static_cast<u32>(textures_.size())).second) {
            textures_.push_back(t);
        }
    }

    // shaders: the Gnmx struct at `s` (its code link at +8); one per footer hash
    const auto shader = [&](u32 s, bool want_ps) -> u32 {
        const auto a = p.addr.find(s + 8);
        if (a == p.addr.end()) {
            return 0;
        }
        GS::ProgramFooter f;
        u64 end = 0;
        if (!ProgramAt(p, a->second, f, end) || f.type != (want_ps ? 0 : 1)) {
            return 0;
        }
        if (const auto it = shader_by_hash_.find(f.shader_hash); it != shader_by_hash_.end()) {
            return it->second + 1;
        }
        StaticShader sh;
        sh.footer_hash = f.shader_hash;
        sh.ps = want_ps;
        sh.image.assign(p.d.begin() + a->second, p.d.begin() + static_cast<ptrdiff_t>(end));
        const u32 nslots = p.U32(s) >> 24;
        sh.rsrc1 = p.U32(s + 0x10);
        sh.rsrc2 = p.U32(s + 0x14);
        if (want_ps) {
            for (u32 i = 0; i < 8; ++i) {
                sh.regs[i] = p.U32(s + 0x18 + i * 4);
            }
            const u32 nin = std::min<u32>(p.U32(s + 0x38), 32);
            for (u32 i = 0; i < nslots; ++i) {
                sh.slots.push_back(p.U32(s + 0x3C + i * 4));
            }
            const u32 io = s + 0x3C + nslots * 4;
            for (u32 i = 0; i < nin; ++i) {
                sh.ps_inputs.push_back(static_cast<u16>(p.U32(io + i * 2) & 0xFFFF));
            }
        } else {
            for (u32 i = 0; i < 3; ++i) {
                sh.regs[i] = p.U32(s + 0x18 + i * 4);
            }
            const u32 nin = p.U8(s + 0x24), nexp = std::min<u32>(p.U8(s + 0x25), 32);
            for (u32 i = 0; i < nslots; ++i) {
                sh.slots.push_back(p.U32(s + 0x28 + i * 4));
            }
            const u32 eo = s + 0x28 + nslots * 4 + nin * 4;
            for (u32 i = 0; i < nexp; ++i) {
                sh.vs_exports.push_back(static_cast<u16>(p.U32(eo + i * 2) & 0xFFFF));
            }
        }
        shader_by_hash_.emplace(f.shader_hash, static_cast<u32>(shaders_.size()));
        shaders_.push_back(std::move(sh));
        return static_cast<u32>(shaders_.size());
    };
    const auto fetch = [&](u32 code) -> u32 {
        std::vector<u32> w;
        for (u32 i = 0; i < 256 && p.Has(code + u64{i} * 4, 4); ++i) {
            w.push_back(p.U32(code + i * 4));
            if (IsSetpc(w.back())) {
                break;
            }
        }
        if (w.empty() || !IsSetpc(w.back())) {
            return 0;
        }
        const u64 h = XXH3_64bits(w.data(), w.size() * 4);
        if (const auto it = fetch_by_hash_.find(h); it != fetch_by_hash_.end()) {
            return it->second + 1;
        }
        fetch_by_hash_.emplace(h, static_cast<u32>(fetches_.size()));
        fetches_.push_back(std::move(w));
        return static_cast<u32>(fetches_.size());
    };

    // compute shaders: a Gnmx CsShader struct wherever a kind-4 link is a CS program's address (the struct's code link
    // at +8): RSRC1/2 at +0x10, COMPUTE_NUM_THREAD_X/Y/Z at +0x18, its input usage slots at +0x28 (count: the common
    // header's top byte); one per footer hash (E:\research\rea\q\pack_shader_index.py)
    for (const auto& [loc, code] : p.addr) {
        GS::ProgramFooter f;
        u64 end = 0;
        if (loc < 8 || !ProgramAt(p, code, f, end) || f.type != 4) {
            continue;
        }
        const u32 s = loc - 8;
        const u32 nslots = p.U32(s) >> 24;
        if (shader_by_hash_.contains(f.shader_hash) || !p.Has(s + 0x28, u64{nslots} * 4)) {
            continue;
        }
        StaticShader sh;
        sh.footer_hash = f.shader_hash;
        sh.cs = true;
        sh.image.assign(p.d.begin() + code, p.d.begin() + static_cast<ptrdiff_t>(end));
        sh.rsrc1 = p.U32(s + 0x10);
        sh.rsrc2 = p.U32(s + 0x14);
        for (u32 i = 0; i < 3; ++i) {
            sh.regs[i] = p.U32(s + 0x18 + i * 4);
        }
        for (u32 i = 0; i < nslots; ++i) {
            sh.slots.push_back(p.U32(s + 0x28 + i * 4));
        }
        shader_by_hash_.emplace(f.shader_hash, static_cast<u32>(shaders_.size()));
        computes_.push_back(static_cast<u32>(shaders_.size()));
        shaders_.push_back(std::move(sh));
    }

    // pass elements: a DIC fixup at E+0x20 (the pass class) and a PS struct at E+0x40
    std::unordered_set<u32> pass_elems;
    for (const auto& [loc, cls] : p.dic) {
        if (loc < 0x20) {
            continue;
        }
        const u32* ps = p.Ptr(loc - 0x20 + 0x40);
        if (ps != nullptr && p.addr.contains(*ps + 8)) {
            pass_elems.insert(loc - 0x20);
        }
    }
    // program objects: X+0x80 -> the first of X+0x88 pass elements
    std::unordered_map<u32, u32> program_at; // X -> programs_ index
    std::unordered_map<u32, u32> ntex_of;
    for (const auto& [loc, first] : p.ptr) {
        if (!pass_elems.contains(first) || loc < 0x80) {
            continue;
        }
        const u32 X = loc - 0x80;
        const u32 n = p.U32(loc + 8);
        if (n == 0 || n > kMaxPasses || program_at.contains(X)) {
            continue;
        }
        bool all = true;
        for (u32 i = 0; i < n && all; ++i) {
            all = pass_elems.contains(first + i * kPassStride);
        }
        const u32 ntex = p.U32(X + 0x2C), nbuf = p.U32(X + 0x30);
        if (!all || ntex > kMaxResources || nbuf > kMaxResources) {
            continue;
        }
        const u32* tr = p.Ptr(X + 0xD0);
        const u32* br = p.Ptr(X + 0xC0);
        const u32* tid = p.Ptr(X + 0xE0);
        const u32* bid = p.Ptr(X + 0xF0);
        const auto material_index = [&](const u32* ids, u32 count, u8 id) -> s16 {
            for (u32 i = 0; ids != nullptr && i < count; ++i) {
                if (p.U8(*ids + i) == id) {
                    return static_cast<s16>(i);
                }
            }
            return -1;
        };
        StaticProgram prog{pack, static_cast<u32>(passes_.size()), n};
        for (u32 i = 0; i < n; ++i) {
            const u32 E = first + i * kPassStride;
            StaticPass sp;
            sp.cls = p.dic.at(E + 0x20);
            if (const u32* s = p.Ptr(E + 0x38)) {
                sp.vs = shader(*s, false);
            }
            if (const u32* s = p.Ptr(E + 0x40)) {
                sp.ps = shader(*s, true);
            }
            if (const u32* fc = p.Ptr(E + 0x30)) {
                sp.fetch = fetch(*fc);
            }
            const auto pairs = [&](u32 lo, u32 co, const u32* rec, u32 rec_size, u32 api_off, const u32* ids,
                                   u32 count, std::vector<StaticSlot>& out) {
                const u32* a = p.Ptr(E + lo);
                const u32 np = p.U32(E + co);
                if (a == nullptr || np > 256) {
                    return;
                }
                for (u32 k = 0; k < np; ++k) {
                    StaticSlot sl;
                    sl.stage = p.U8(*a + k * 2);
                    const u8 id = p.U8(*a + k * 2 + 1);
                    if (rec != nullptr) {
                        sl.api = p.U8(*rec + u32{id} * rec_size + api_off);
                        sl.name = p.U64(*rec + u32{id} * rec_size);
                    }
                    sl.material = material_index(ids, count, id);
                    out.push_back(sl);
                }
            };
            pairs(0x98, 0xA0, tr, 0x10, 0xB, tid, ntex, sp.textures);
            pairs(0x88, 0x90, br, 0x20, 0x8, bid, nbuf, sp.buffers);
            passes_.push_back(std::move(sp));
        }
        program_at.emplace(X, static_cast<u32>(programs_.size()));
        ntex_of.emplace(X, ntex);
        programs_.push_back(prog);
    }
    // material instances: D+0x20 -> X, D+0x30 -> bindings (0x30 each, u64 texture id), D+0x38 = X's ntex
    std::unordered_map<u32, u32> instance_at; // D -> instances_ index
    for (const auto& [loc, X] : p.ptr) {
        const auto pr = program_at.find(X);
        if (pr == program_at.end() || loc < 0x20) {
            continue;
        }
        const u32 D = loc - 0x20;
        const u32* B = p.Ptr(D + 0x30);
        const u32 n = p.U32(D + 0x38);
        if (B == nullptr || n == 0 || n != ntex_of[X] || instance_at.contains(D)) {
            continue;
        }
        StaticInstance si;
        si.program = pr->second;
        si.textures.resize(n);
        const u32 index = static_cast<u32>(instances_.size());
        for (u32 i = 0; i < n; ++i) {
            const u64 id = p.U64(*B + i * 0x30);
            if (const auto it = texture_by_id_.find(id); it != texture_by_id_.end()) {
                si.textures[i] = it->second + 1;
            } else {
                pending_.push_back({index, i, id});
            }
        }
        instance_at.emplace(D, index);
        instances_.push_back(std::move(si));
    }
    // geometry: G+0x10 -> a material instance, G+0x60 -> the V# array (a kind-2 link), G+0x6C stream count;
    // one per (instance, stream formats)
    std::unordered_set<u64> seen;
    for (const auto& [loc, D] : p.ptr) {
        const auto in = instance_at.find(D);
        if (in == instance_at.end() || loc < 0x10) {
            continue;
        }
        const u32 G = loc - 0x10;
        const u32* A = p.Ptr(G + 0x60);
        const u32 n = p.U8(G + 0x6C);
        if (A == nullptr || n == 0 || n > kMaxStreams || !p.vsharp.contains(*A) || !p.Has(*A, u64{n} * 16)) {
            continue;
        }
        StaticGeometry g;
        g.instance = in->second;
        u64 h = in->second;
        for (u32 i = 0; i < n; ++i) {
            std::array<u32, 4> v;
            std::memcpy(v.data(), p.d.data() + *A + i * 16, 16);
            g.streams.push_back(v);
            h = (h ^ (v[1] >> 16) ^ (u64{v[3] & 0x7FFFFu} << 20)) * 0x100000001B3ull;
        }
        if (seen.insert(h).second) {
            geometries_.push_back(std::move(g));
        }
    }
}

} // namespace SecondSon::Gpu::Static
