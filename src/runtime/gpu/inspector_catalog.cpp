// SecondSonPC — the Performance Inspector's effect categories and catalog (P4.8). See inspector.h.
#include "runtime/gpu/inspector.h"

#include <charconv>
#include <fstream>
#include <sstream>

namespace SecondSon::Gpu::Inspector {

namespace {

constexpr std::array<std::string_view, kCategories> kCategoryNames = {
    "Unknown",  "Smoke",       "Fire",     "Particles",      "ParticleSimulation", "ParticleSorting", "Foliage",
    "Shadows",  "Water",       "Reflections", "Lighting",    "PostProcessing",     "Terrain",         "Geometry",
    "ComputeOther",
};
constexpr std::array<std::string_view, 5> kBasisNames = {"none", "rule", "probable", "shared", "confirmed"};

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

/// The next whitespace-separated word of `s` (consumed).
std::string_view Word(std::string_view& s) {
    s = Trim(s);
    size_t n = 0;
    while (n < s.size() && s[n] != ' ' && s[n] != '\t') {
        ++n;
    }
    const std::string_view w = s.substr(0, n);
    s.remove_prefix(n);
    return w;
}

} // namespace

std::string_view CategoryName(Category c) {
    const auto i = static_cast<size_t>(c);
    return i < kCategories ? kCategoryNames[i] : "?";
}

bool CategoryFromName(std::string_view name, Category& out) {
    for (size_t i = 0; i < kCategories; ++i) {
        if (kCategoryNames[i] == name) {
            out = static_cast<Category>(i);
            return true;
        }
    }
    return false;
}

std::string_view BasisName(Basis b) {
    const auto i = static_cast<size_t>(b);
    return i < kBasisNames.size() ? kBasisNames[i] : "?";
}

bool BasisFromName(std::string_view name, Basis& out) {
    for (size_t i = 0; i < kBasisNames.size(); ++i) {
        if (kBasisNames[i] == name) {
            out = static_cast<Basis>(i);
            return true;
        }
    }
    return false;
}

Class RuleForDraw(const DrawShape& s) {
    if (s.colors == 0 && s.depth) {
        if (s.width == s.height && s.width >= 256) {
            return {Category::Shadows, Basis::Rule};
        }
        return {Category::Geometry, Basis::Rule};
    }
    if (s.colors >= 3) {
        return {Category::Geometry, Basis::Rule};
    }
    if (s.colors >= 1 && !s.depth && !s.indexed && !s.indirect && s.count != 0 && s.count <= 6 && s.instances <= 1) {
        return {Category::PostProcessing, Basis::Rule};
    }
    return {};
}

u32 Catalog::Parse(std::string_view text, std::string* first_error) {
    u32 bad = 0;
    while (!text.empty()) {
        const size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        if (const size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        line = Trim(line);
        if (line.empty()) {
            continue;
        }
        std::string_view rest = line;
        const std::string_view stage = Word(rest), hex = Word(rest), cat = Word(rest), basis = Word(rest);
        u64 content = 0;
        Class c;
        const auto [p, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), content, 16);
        const bool ok = (stage == "vs" || stage == "ps" || stage == "cs") && ec == std::errc{} &&
                        p == hex.data() + hex.size() && !hex.empty() && CategoryFromName(cat, c.category) &&
                        BasisFromName(basis, c.basis) && c.basis != Basis::None && c.basis != Basis::Rule;
        if (!ok) {
            if (bad++ == 0 && first_error != nullptr) {
                *first_error = std::string(line);
            }
            continue;
        }
        (stage == "vs" ? vs_ : stage == "ps" ? ps_ : cs_)[content] = c;
    }
    return bad;
}

bool Catalog::Load(const std::filesystem::path& file, std::string& error) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        error = "cannot read " + file.string();
        return false;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string first;
    if (const u32 bad = Parse(ss.str(), &first); bad != 0) {
        error = std::to_string(bad) + " malformed line(s), the first: " + first;
    }
    return true;
}

const Class* Catalog::Find(char stage, u64 content) const {
    const auto& m = stage == 'v' ? vs_ : stage == 'p' ? ps_ : cs_;
    const auto it = m.find(content);
    return it != m.end() ? &it->second : nullptr;
}

const Class* Catalog::DrawEntry(u64 vs_content, u64 ps_content, bool has_ps) const {
    if (has_ps) {
        if (const Class* c = Find('p', ps_content); c != nullptr) {
            return c;
        }
    }
    return Find('v', vs_content);
}

Class Catalog::Draw(u64 vs_content, u64 ps_content, bool has_ps, const DrawShape& shape) const {
    const Class* c = DrawEntry(vs_content, ps_content, has_ps);
    return c != nullptr ? *c : RuleForDraw(shape);
}

Class Catalog::Dispatch(u64 cs_content) const {
    const Class* c = Find('c', cs_content);
    return c != nullptr ? *c : Class{Category::ComputeOther, Basis::Rule};
}

} // namespace SecondSon::Gpu::Inspector
