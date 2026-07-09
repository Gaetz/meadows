#pragma once

#include <unordered_map>

#include <glm/glm.hpp> // Defines.hpp only forward-declares Vec2

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

// Deterministic layered auto-layout for the node-graph editors (chantier
// 8.6). PURE data: no ImGui, no forms — nodes are guids, edges are guid
// pairs, output is a position per node. Used for graphs that have no
// stored position yet (the editor side-store, EditorLayouts) and for the
// explicit "Auto-layout" button.
//
// Algorithm: BFS from the roots — column = depth (x = depth * spacing),
// row = rank within the layer (y = rank * spacing). Rank sorts by
// (order key, guid) so the result never depends on hash iteration; nodes
// unreachable from any root (orphans, cycle remnants) land in one final
// layer. Cycles are cut by first-visit depth (BFS never revisits).

namespace data {

struct GraphLayoutResult {
    std::unordered_map<core::Guid, Vec2> positions;
};

// `roots` seed the BFS in the given order; when empty, every node without
// an incoming edge is a root (in `nodes` order). `rankOrder` is an
// optional per-node sort key within a layer (e.g. a dialogue reply's
// `order` field); missing entries sort as 0.
GraphLayoutResult layoutGraph(
    const vector<core::Guid>& nodes,
    const vector<std::pair<core::Guid, core::Guid>>& edges,
    const vector<core::Guid>& roots,
    const std::unordered_map<core::Guid, i32>* rankOrder = nullptr);

} // namespace data
