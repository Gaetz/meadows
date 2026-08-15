#include "game/ui/GraphPanelCommon.hpp"

#include "data/editor/GraphLayout.hpp"

namespace game {

void applyGraphLayout(
    NodeCanvas& canvas, data::EditorLayouts& layouts,
    const core::Guid& graphId, const vector<core::Guid>& nodes,
    const vector<std::pair<core::Guid, core::Guid>>& edges,
    const vector<core::Guid>& roots, bool autoLayoutRequested,
    const std::unordered_map<core::Guid, i32>* rankOrder) {
    const data::GraphLayoutResult layout =
        data::layoutGraph(nodes, edges, roots, rankOrder);
    for (const core::Guid& node : nodes) {
        if (autoLayoutRequested) {
            const auto it = layout.positions.find(node);
            if (it != layout.positions.end()) {
                canvas.setNodePosition(node, it->second);
                layouts.setPosition(graphId, node, it->second);
            }
            continue;
        }
        if (const auto stored = layouts.positionOf(graphId, node)) {
            canvas.setNodePosition(node, *stored);
        } else if (const auto it = layout.positions.find(node);
                   it != layout.positions.end()) {
            canvas.setNodePosition(node, it->second);
        }
    }
    if (autoLayoutRequested) {
        layouts.save();
    }
}

void placePendingNode(NodeCanvas& canvas, data::EditorLayouts& layouts,
                      const core::Guid& graphId, core::Guid& pendingPlace,
                      const Vec2& position) {
    if (!pendingPlace.isValid()) {
        return;
    }
    canvas.setNodePosition(pendingPlace, position);
    layouts.setPosition(graphId, pendingPlace, position);
    layouts.save();
    pendingPlace = {};
}

void persistMovedNodes(
    data::EditorLayouts& layouts, const core::Guid& graphId,
    const vector<std::pair<core::Guid, Vec2>>& movedNodes) {
    for (const auto& [node, position] : movedNodes) {
        layouts.setPosition(graphId, node, position);
    }
    if (!movedNodes.empty()) {
        layouts.save();
    }
}

} // namespace game
