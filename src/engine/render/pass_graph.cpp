// SecondSonPC — the frame's render passes as a dependency graph (see pass_graph.h).
#include "engine/render/pass_graph.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include <fmt/format.h>

namespace SecondSon::Engine::Render {

namespace {

template <typename T>
void AddUnique(std::vector<T>& list, const T& value) {
    if (std::find(list.begin(), list.end(), value) == list.end()) {
        list.push_back(value);
    }
}

void Merge(ScanResult& into, const ScanResult& from) {
    into.packets += from.packets;
    into.dwords += from.dwords;
    into.draws += from.draws;
    into.draws_indirect += from.draws_indirect;
    into.draws_instanced += from.draws_instanced;
    into.indices += from.indices;
    into.dispatches += from.dispatches;
    into.draws_depth_test += from.draws_depth_test;
    into.draws_depth_write += from.draws_depth_write;
    into.draws_no_color += from.draws_no_color;
    into.events += from.events;
    into.cache_actions += from.cache_actions;
    into.waits += from.waits;
    into.dma += from.dma;
    into.indirect_buffers += from.indirect_buffers;
    into.malformed += from.malformed;
    for (size_t i = 0; i < into.opcode_packets.size(); ++i) {
        into.opcode_packets[i] += from.opcode_packets[i];
    }
    for (size_t i = 0; i < into.context_writes.size(); ++i) {
        into.context_writes[i] += from.context_writes[i];
        into.sh_writes[i] += from.sh_writes[i];
        into.uconfig_writes[i] += from.uconfig_writes[i];
    }
    for (size_t i = 0; i < into.native_ops.size(); ++i) {
        into.native_ops[i] += from.native_ops[i];
    }
    for (const TargetDesc& t : from.targets) {
        AddUnique(into.targets, t);
    }
    for (const TargetDesc& t : from.depth_read) {
        AddUnique(into.depth_read, t);
    }
    for (const u64 s : from.shaders) {
        AddUnique(into.shaders, s);
    }
}

bool IsWrite(ResourceKind kind) {
    return kind == ResourceKind::RwTexture || kind == ResourceKind::RwBuffer;
}

u64 Key(const ResourceUse& u) {
    return u.base | (u64{static_cast<u8>(u.kind)} << 56) | (u64{u.format & 0xFF} << 48);
}

ResourceUse FromTarget(const TargetDesc& t) {
    return ResourceUse{t.base, t.width, t.height, t.format, ResourceKind::Texture};
}

} // namespace

ResourceUse DecodeTexture(const u32* t, ResourceKind kind) {
    ResourceUse u;
    u.base = (u64{t[0]} | (u64{t[1] & 0xFF} << 32)) << 8;
    u.width = (t[2] & 0x3FFF) + 1;
    u.height = ((t[2] >> 14) & 0x3FFF) + 1;
    u.format = ((t[1] >> 20) & 0x3F) | (((t[1] >> 26) & 0xF) << 8);
    u.kind = kind;
    return u;
}

ResourceUse DecodeBuffer(const u32* v, ResourceKind kind) {
    ResourceUse u;
    u.base = u64{v[0]} | (u64{v[1] & 0xFFF} << 32);
    u.width = (v[1] >> 16) & 0x3FFF;
    u.height = v[2];
    u.kind = kind;
    return u;
}

std::string NodeName(const Node& node) {
    switch (node.kind) {
    case NodeKind::Deform:
        return fmt::format("deform {}", node.id);
    case NodeKind::Pre:
        return "pre";
    case NodeKind::Post:
        return "post";
    case NodeKind::Pass:
        break;
    }
    return fmt::format("pass {}", node.id);
}

Graph BuildGraph(std::span<const ContextRecord> ordered) {
    Graph g;
    std::vector<std::unordered_set<u64>> read_keys, write_keys;
    for (const ContextRecord& c : ordered) {
        const bool same = !g.nodes.empty() && c.kind == NodeKind::Pass && g.nodes.back().kind == NodeKind::Pass &&
                          g.nodes.back().id == c.pass;
        if (!same) {
            Node n;
            n.kind = c.kind;
            n.id = c.kind == NodeKind::Pass || c.kind == NodeKind::Deform ? c.pass : 0;
            g.nodes.push_back(std::move(n));
            read_keys.emplace_back();
            write_keys.emplace_back();
        }
        Node& n = g.nodes.back();
        ++n.contexts;
        n.bytes += c.bytes;
        Merge(n.scan, c.scan);
        for (const ResourceUse& u : c.uses) {
            auto& keys = IsWrite(u.kind) ? write_keys.back() : read_keys.back();
            if (keys.insert(Key(u)).second) {
                (IsWrite(u.kind) ? n.writes : n.reads).push_back(u);
            }
        }
    }
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        Node& n = g.nodes[i];
        for (const TargetDesc& t : n.scan.targets) {
            if (write_keys[i].insert(Key(FromTarget(t))).second) {
                n.writes.push_back(FromTarget(t));
            }
        }
        for (const TargetDesc& t : n.scan.depth_read) {
            if (read_keys[i].insert(Key(FromTarget(t))).second) {
                n.reads.push_back(FromTarget(t));
            }
        }
    }

    // Per address: the nodes that write it (targets, RW bindings) and the nodes that read it (textures, buffers,
    // depth tests, and RW bindings: a compute pass may read what an earlier one left).
    struct Users {
        std::vector<u32> writers;
        std::vector<u32> readers;
        bool target = false;
    };
    std::unordered_map<u64, Users> users;
    for (u32 i = 0; i < g.nodes.size(); ++i) {
        for (const ResourceUse& w : g.nodes[i].writes) {
            Users& u = users[w.base];
            if (u.writers.empty() || u.writers.back() != i) {
                u.writers.push_back(i);
            }
            u.target |= w.kind == ResourceKind::Texture;
        }
    }
    for (u32 i = 0; i < g.nodes.size(); ++i) {
        const auto add_reader = [&](u64 base) {
            const auto it = users.find(base);
            if (it != users.end() && (it->second.readers.empty() || it->second.readers.back() != i)) {
                it->second.readers.push_back(i);
            }
        };
        for (const ResourceUse& r : g.nodes[i].reads) {
            add_reader(r.base);
        }
        for (const ResourceUse& w : g.nodes[i].writes) {
            if (IsWrite(w.kind)) {
                add_reader(w.base);
            }
        }
    }

    std::map<std::tuple<u32, u32, bool>, Edge> edges;
    std::vector<std::pair<u64, const Users*>> ordered_users;
    ordered_users.reserve(users.size());
    for (const auto& [base, u] : users) {
        ordered_users.emplace_back(base, &u);
    }
    std::sort(ordered_users.begin(), ordered_users.end());
    for (const auto& [base, u] : ordered_users) {
        for (const u32 reader : u->readers) {
            s32 from = -1;
            bool prev = false;
            for (const u32 w : u->writers) {
                if (w < reader) {
                    from = static_cast<s32>(w);
                }
            }
            if (from < 0) {
                const u32 last = u->writers.back();
                if (last > reader) {
                    from = static_cast<s32>(last);
                    prev = true;
                }
            }
            if (from < 0) {
                continue; // the reader is the only writer
            }
            Edge& e = edges[{static_cast<u32>(from), reader, prev}];
            e.from = static_cast<u32>(from);
            e.to = reader;
            e.prev_frame = prev;
            if (e.resources++ == 0) {
                e.sample = base;
            }
        }
        Lifetime l;
        l.base = base;
        l.first_write = u->writers.front();
        l.last_write = u->writers.back();
        l.target = u->target;
        for (const u32 r : u->readers) {
            if (u->writers.size() == 1 && r == l.first_write) {
                continue; // a node reading its own output (RW binding) is not a later use
            }
            l.first_read = l.first_read < 0 ? static_cast<s32>(r) : std::min(l.first_read, static_cast<s32>(r));
            l.last_read = std::max(l.last_read, static_cast<s32>(r));
            ++l.readers;
        }
        g.lifetimes.push_back(l);
    }
    for (auto& [key, e] : edges) {
        g.edges.push_back(e);
    }
    return g;
}

} // namespace SecondSon::Engine::Render
