// SecondSonPC - CITY-OPT Phase 4: the DrawIndex patch of a translated vertex stage (vk_instancing.h).
#include "runtime/gpu/vulkan/vk_instancing.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

namespace SecondSon::Gpu::Vk {
namespace {

// SPIR-V opcodes, storage classes, decorations, built-ins and capabilities used here (SPIR-V 1.6 specification)
enum : u32 {
    kOpEntryPoint = 15,
    kOpCapability = 17,
    kOpTypeInt = 21,
    kOpTypePointer = 32,
    kOpConstant = 43,
    kOpFunction = 54,
    kOpVariable = 59,
    kOpLoad = 61,
    kOpAccessChain = 65,
    kOpInBoundsAccessChain = 66,
    kOpDecorate = 71,
    kOpMemberDecorate = 72,
    kOpDecorationGroup = 73,
    kOpGroupDecorate = 74,
    kOpIAdd = 128,
    kOpIMul = 132,
    kOpBitFieldUExtract = 203,
    kOpLabel = 248,
    kOpDecorateId = 332,
    kOpDecorateString = 5632,
    kOpMemberDecorateString = 5633,
    kInput = 1,
    kPushConstant = 9,
    kBuiltIn = 11,
    kDrawIndex = 4426,
    kCapDrawParameters = 4427,
};

u32 Op(u32 w) {
    return w & 0xFFFF;
}
u32 Len(u32 w) {
    return w >> 16;
}
u32 Head(u32 op, u32 len) {
    return (len << 16) | op;
}
bool Annotation(u32 op) {
    return op == kOpDecorate || op == kOpMemberDecorate || op == kOpDecorationGroup || op == kOpGroupDecorate ||
           op == kOpDecorateId || op == kOpDecorateString || op == kOpMemberDecorateString;
}

} // namespace

bool PatchDrawIndexOffsets(std::span<const u32> w, std::span<const InstancedBuffer> buffers, std::vector<u32>& out,
                           std::string& why) {
    out.clear();
    if (w.size() < 5 || w[0] != 0x07230203 || buffers.empty()) {
        why = "not a SPIR-V module";
        return false;
    }
    std::vector<size_t> at; // instruction starts
    for (size_t i = 5; i < w.size();) {
        const u32 n = Len(w[i]);
        if (n == 0 || i + n > w.size()) {
            why = "malformed";
            return false;
        }
        at.push_back(i);
        i += n;
    }
    std::unordered_map<u32, u32> stride_of; // buf_offsets index -> stride
    for (const InstancedBuffer& b : buffers) {
        if (b.stride == 0 || b.stride % 4 != 0 || b.index >= 56) {
            why = fmt::format("buffer {}: stride {}", b.index, b.stride);
            return false;
        }
        stride_of[b.index] = b.stride;
    }
    u32 uint_id = 0, push_var = 0, input_ptr = 0, functions = 0;
    bool has_cap = false;
    size_t entry = ~size_t{0}, last_cap = 0, last_annotation = 0, first_function = ~size_t{0};
    std::unordered_map<u32, u32> constants;            // uint constant id -> value
    std::unordered_map<u32, u32> chain_word, load_word; // push constant member 1 element pointers / loads -> element
    struct Hit {
        size_t k = 0; // instruction number
        u32 stride = 0;
    };
    std::vector<Hit> hits;
    std::unordered_map<u32, u32> extract_uses; // a load of buf_offsets -> its uses as an extract's base
    for (size_t k = 0; k < at.size(); ++k) {
        const size_t i = at[k];
        const u32 op = Op(w[i]), n = Len(w[i]);
        if (op == kOpCapability) {
            last_cap = k;
            has_cap = has_cap || (n == 2 && w[i + 1] == kCapDrawParameters);
        } else if (op == kOpEntryPoint) {
            if (entry != ~size_t{0}) {
                why = "more than one entry point";
                return false;
            }
            entry = k;
        } else if (Annotation(op)) {
            last_annotation = k;
        } else if (op == kOpTypeInt && n == 4 && w[i + 2] == 32 && w[i + 3] == 0) {
            uint_id = uint_id == 0 ? w[i + 1] : uint_id;
        } else if (op == kOpTypePointer && n == 4 && w[i + 2] == kInput && w[i + 3] == uint_id && uint_id != 0) {
            input_ptr = w[i + 1];
        } else if (op == kOpConstant && n == 4 && w[i + 1] == uint_id && uint_id != 0) {
            constants[w[i + 2]] = w[i + 3];
        } else if (op == kOpVariable && n >= 4 && w[i + 3] == kPushConstant) {
            if (push_var != 0) {
                why = "two push constant blocks";
                return false;
            }
            push_var = w[i + 2];
        } else if (op == kOpFunction) {
            ++functions;
            first_function = std::min(first_function, k);
        } else if ((op == kOpAccessChain || op == kOpInBoundsAccessChain) && push_var != 0 && w[i + 3] == push_var) {
            const auto m = constants.find(n == 6 ? w[i + 4] : 0);
            const auto e = constants.find(n == 6 ? w[i + 5] : 0);
            if (m != constants.end() && m->second == 1 && e != constants.end()) {
                chain_word[w[i + 2]] = e->second; // (member 1: buf_offsets, as 14 dwords)
            }
        } else if (op == kOpLoad && n >= 4 && chain_word.contains(w[i + 3])) {
            load_word[w[i + 2]] = chain_word[w[i + 3]];
        } else if (op == kOpBitFieldUExtract && n == 6 && load_word.contains(w[i + 3])) {
            const auto off = constants.find(w[i + 4]);
            const auto cnt = constants.find(w[i + 5]);
            if (off == constants.end() || cnt == constants.end() || cnt->second != 8 || off->second % 8 != 0 ||
                off->second > 24) {
                why = "a buf_offsets extract of another shape";
                return false;
            }
            ++extract_uses[w[i + 3]];
            const u32 index = load_word[w[i + 3]] * 4 + off->second / 8;
            if (const auto s = stride_of.find(index); s != stride_of.end()) {
                hits.push_back({k, s->second});
            }
        }
    }
    if (entry == ~size_t{0} || functions != 1 || uint_id == 0 || push_var == 0 || hits.empty()) {
        why = functions != 1 ? "not one function" : hits.empty() ? "no read of those buffers" : "not the translator's form";
        return false;
    }
    // a load of buf_offsets used other than as an extract's base: its bytes reach the shader another way
    for (const auto& [load, uses] : extract_uses) {
        u32 refs = 0;
        for (size_t k = first_function; k < at.size(); ++k) { // (in the function: the globals' literals are no ids)
            const size_t i = at[k];
            for (size_t j = i + 1; j < i + Len(w[i]); ++j) {
                refs += w[j] == load ? 1 : 0;
            }
        }
        if (refs != uses + 1) { // (+ its own definition)
            why = "buf_offsets read other than by an extract";
            return false;
        }
    }
    for (const auto& [load, word] : load_word) {
        if (!extract_uses.contains(load)) {
            why = "buf_offsets loaded and not extracted";
            return false;
        }
    }
    if (last_annotation == 0) {
        why = "no annotations section";
        return false;
    }
    // the entry function's first block: after its OpLabel and leading OpVariables
    size_t body = first_function + 1;
    while (body < at.size() && Op(w[at[body]]) != kOpLabel) {
        ++body; // (OpFunctionParameter)
    }
    if (body >= at.size()) {
        why = "no first block";
        return false;
    }
    ++body;
    while (body < at.size() && Op(w[at[body]]) == kOpVariable) {
        ++body;
    }
    u32 bound = w[3];
    const auto fresh = [&] { return bound++; };
    const u32 ptr = input_ptr != 0 ? input_ptr : fresh();
    const u32 drawid = fresh();
    const u32 loaded = fresh();
    std::unordered_map<u32, std::pair<u32, u32>> scaled; // stride -> (constant id, DrawIndex * stride id)
    for (const Hit& h : hits) {
        if (!scaled.contains(h.stride)) {
            const u32 c = fresh();
            scaled[h.stride] = {c, fresh()};
        }
    }
    std::unordered_map<size_t, std::pair<u32, u32>> renamed; // hit instruction -> (the extract's new result, stride)
    for (const Hit& h : hits) {
        renamed[h.k] = {fresh(), h.stride};
    }

    out.reserve(w.size() + 32 + hits.size() * 5);
    out.insert(out.end(), w.begin(), w.begin() + 5);
    for (size_t k = 0; k < at.size(); ++k) {
        const size_t i = at[k];
        const u32 op = Op(w[i]), n = Len(w[i]);
        if (k == first_function) {
            // new globals before the first function: the pointer type (if none), the strides, the input
            if (input_ptr == 0) {
                out.insert(out.end(), {Head(kOpTypePointer, 4), ptr, kInput, uint_id});
            }
            for (const auto& [stride, ids] : scaled) {
                out.insert(out.end(), {Head(kOpConstant, 4), uint_id, ids.first, stride});
            }
            out.insert(out.end(), {Head(kOpVariable, 4), ptr, drawid, kInput});
        }
        if (k == body) {
            out.insert(out.end(), {Head(kOpLoad, 4), uint_id, loaded, drawid});
            for (const auto& [stride, ids] : scaled) {
                out.insert(out.end(), {Head(kOpIMul, 5), uint_id, ids.second, loaded, ids.first});
            }
        }
        if (k == entry) {
            out.push_back(Head(op, n + 1)); // the interface gets DrawIndex
            out.insert(out.end(), w.begin() + i + 1, w.begin() + i + n);
            out.push_back(drawid);
        } else if (const auto r = renamed.find(k); r != renamed.end()) {
            // %old = extract ...  ->  %new = extract ...; %old = %new + DrawIndex * stride
            const u32 old = w[i + 2];
            out.insert(out.end(), {w[i], w[i + 1], r->second.first, w[i + 3], w[i + 4], w[i + 5]});
            out.insert(out.end(), {Head(kOpIAdd, 5), uint_id, old, r->second.first, scaled[r->second.second].second});
        } else {
            out.insert(out.end(), w.begin() + i, w.begin() + i + n);
        }
        if (k == last_cap && !has_cap) {
            out.insert(out.end(), {Head(kOpCapability, 2), kCapDrawParameters});
        }
        if (k == last_annotation) {
            out.insert(out.end(), {Head(kOpDecorate, 4), drawid, kBuiltIn, kDrawIndex});
        }
    }
    out[3] = bound;
    return true;
}

} // namespace SecondSon::Gpu::Vk
