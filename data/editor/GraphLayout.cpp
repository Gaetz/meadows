#include "data/editor/GraphLayout.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

#include <glm/glm.hpp> // Defines.hpp only forward-declares Vec2

namespace data {

namespace {
constexpr f32 kColumnSpacing = 280.0f;
constexpr f32 kRowSpacing = 110.0f;
} // namespace

GraphLayoutResult layoutGraph(
    const vector<core::Guid>& nodes,
    const vector<std::pair<core::Guid, core::Guid>>& edges,
    const vector<core::Guid>& roots,
    const std::unordered_map<core::Guid, i32>* rankOrder) {
    GraphLayoutResult result;
    if (nodes.empty()) {
        return result;
    }
    const std::unordered_set<core::Guid> known(nodes.begin(), nodes.end());

    // Outgoing adjacency, in edge-declaration order (determinism).
    std::unordered_map<core::Guid, vector<core::Guid>> outgoing;
    std::unordered_set<core::Guid> hasIncoming;
    for (const auto& [from, to] : edges) {
        if (!known.contains(from) || !known.contains(to)) {
            continue; // dangling edge — the panel's warning, not ours
        }
        outgoing[from].push_back(to);
        hasIncoming.insert(to);
    }

    // Roots: explicit list, else every node without an incoming edge.
    vector<core::Guid> seeds;
    for (const core::Guid& root : roots) {
        if (known.contains(root)) {
            seeds.push_back(root);
        }
    }
    if (seeds.empty()) {
        for (const core::Guid& node : nodes) {
            if (!hasIncoming.contains(node)) {
                seeds.push_back(node);
            }
        }
    }
    if (seeds.empty()) {
        seeds.push_back(nodes.front()); // pure cycle: arbitrary but stable
    }

    // BFS: depth = first visit (cuts cycles by construction).
    std::unordered_map<core::Guid, u32> depthOf;
    std::deque<core::Guid> queue;
    for (const core::Guid& seed : seeds) {
        if (!depthOf.contains(seed)) {
            depthOf.emplace(seed, 0u);
            queue.push_back(seed);
        }
    }
    u32 maxDepth = 0;
    while (!queue.empty()) {
        const core::Guid current = queue.front();
        queue.pop_front();
        const u32 depth = depthOf.at(current);
        maxDepth = std::max(maxDepth, depth);
        const auto it = outgoing.find(current);
        if (it == outgoing.end()) {
            continue;
        }
        for (const core::Guid& next : it->second) {
            if (depthOf.emplace(next, depth + 1).second) {
                queue.push_back(next);
            }
        }
    }

    // Orphans (unreached) land in one final layer.
    const u32 orphanDepth = maxDepth + 1;
    vector<vector<core::Guid>> layers(orphanDepth + 1);
    for (const core::Guid& node : nodes) {
        const auto it = depthOf.find(node);
        layers[it != depthOf.end() ? it->second : orphanDepth]
            .push_back(node);
    }

    // Rank within a layer: (order key, guid) — hash-order independent.
    const auto keyOf = [&](const core::Guid& node) -> i32 {
        if (!rankOrder) {
            return 0;
        }
        const auto it = rankOrder->find(node);
        return it != rankOrder->end() ? it->second : 0;
    };
    for (u32 depth = 0; depth < layers.size(); ++depth) {
        vector<core::Guid>& layer = layers[depth];
        std::sort(layer.begin(), layer.end(),
                  [&](const core::Guid& a, const core::Guid& b) {
                      const i32 ka = keyOf(a);
                      const i32 kb = keyOf(b);
                      return ka != kb ? ka < kb : a < b;
                  });
        for (u32 rank = 0; rank < layer.size(); ++rank) {
            result.positions.emplace(
                layer[rank], Vec2 { static_cast<f32>(depth) * kColumnSpacing,
                                    static_cast<f32>(rank) * kRowSpacing });
        }
    }
    return result;
}

bool isAncestorOf(
    const std::unordered_map<core::Guid, core::Guid>& parentOf,
    const core::Guid& ancestor, const core::Guid& node) {
    core::Guid current = node;
    for (u32 step = 0; step < 256 && current.isValid(); ++step) {
        if (current == ancestor) {
            return true;
        }
        const auto it = parentOf.find(current);
        current = it != parentOf.end() ? it->second : core::Guid {};
    }
    return false;
}

} // namespace data
