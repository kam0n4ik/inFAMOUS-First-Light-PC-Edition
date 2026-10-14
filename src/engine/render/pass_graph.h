// SecondSonPC — the frame's render passes as a dependency graph (P5.3A, diagnostics;
// docs/native-reconstruction/RENDER_ENGINE_PASSES.md §3). Input: the frame's contexts in the order SubmitFrame
// (eboot 0x1f1e30) hands them to the GPU, each with what its command buffer did (dcb_scan.h) and the resources the
// engine bound on it (the constant update engine's setters). Output: one node per pass (and per pre/post/deformation
// context), edges "node A wrote what node B reads", and the lifetime of every resource written in the frame. Pure
// data: no game, testable.
#pragma once

#include <span>
#include <string>
#include <vector>

#include "engine/render/dcb_scan.h"

namespace SecondSon::Engine::Render {

enum class ResourceKind : u8 { Texture, RwTexture, Buffer, RwBuffer };

/// A resource bound by a CUE setter (eboot 0x16d10 / 0x16de0 / 0x16b70 / 0x16c40), decoded from its descriptor.
struct ResourceUse {
    u64 base = 0;
    u32 width = 0;  ///< texture: width; buffer: stride
    u32 height = 0; ///< texture: height; buffer: records
    u32 format = 0; ///< texture: data format | number format << 8
    ResourceKind kind = ResourceKind::Texture;
    bool operator==(const ResourceUse&) const = default;
};

/// T# (8 dwords) / V# (4 dwords) of the GCN ISA.
ResourceUse DecodeTexture(const u32* t, ResourceKind kind);
ResourceUse DecodeBuffer(const u32* v, ResourceKind kind);

enum class NodeKind : u8 { Deform, Pre, Pass, Post };

/// One context of the frame, in submission order.
struct ContextRecord {
    NodeKind kind = NodeKind::Pass;
    s32 pass = 0; ///< Pass: 0..40; Deform: 0..4
    u32 sub = 0;
    u64 context = 0; ///< R
    u32 chunks = 0;
    u64 bytes = 0;
    u64 ccb_bytes = 0; ///< constant command buffer G+0x80 (cmdptr - begin)
    ScanResult scan;
    std::vector<ResourceUse> uses;
};

struct Node {
    NodeKind kind = NodeKind::Pass;
    s32 id = 0;
    u32 contexts = 0;
    u64 bytes = 0;
    ScanResult scan; ///< counts summed, lists merged
    std::vector<ResourceUse> reads;  ///< Texture/Buffer uses, depth tested without writing (as Texture)
    std::vector<ResourceUse> writes; ///< colour/depth targets (as Texture), RwTexture/RwBuffer uses
};

struct Edge {
    u32 from = 0, to = 0; ///< node indices
    u32 resources = 0;    ///< distinct resources carried
    u64 sample = 0;       ///< one of them
    bool prev_frame = false; ///< the reader runs before the writer: it reads what the previous frame wrote
};

struct Lifetime {
    u64 base = 0;
    u32 first_write = 0, last_write = 0;
    s32 first_read = -1, last_read = -1; ///< node indices; -1: not read in the frame
    u32 readers = 0;
    bool target = false; ///< written as a colour/depth target (else by a compute/RW binding)
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::vector<Lifetime> lifetimes;
};

/// Groups consecutive contexts of the same pass (SubmitFrame's order keeps them together) into nodes and links them.
Graph BuildGraph(std::span<const ContextRecord> ordered);

std::string NodeName(const Node& node);

} // namespace SecondSon::Engine::Render
