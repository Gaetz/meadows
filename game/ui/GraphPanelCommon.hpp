#pragma once

#include <unordered_map>
#include <utility>

#include "data/editor/EditorLayouts.hpp"
#include "game/ui/NodeCanvas.hpp"

namespace game {

// The position plumbing shared by the three graph panels (anim, quest,
// dialogue). Each panel builds its own nodes/edges/roots — the domain
// part — and delegates the rest here. All three calls run inside the
// canvas frame (between begin() and end()).

// Position pass: stored side-store positions win, data::layoutGraph
// fills the rest; an explicit auto-layout overwrites the store and
// persists. Call only when the shown graph changed or on request.
void applyGraphLayout(
    NodeCanvas& canvas, data::EditorLayouts& layouts,
    const core::Guid& graphId, const vector<core::Guid>& nodes,
    const vector<std::pair<core::Guid, core::Guid>>& edges,
    const vector<core::Guid>& roots, bool autoLayoutRequested,
    const std::unordered_map<core::Guid, i32>* rankOrder = nullptr);

// One-frame deferred placement of a node created last frame (under the
// context menu / dragged-pin position). Resets `pendingPlace`.
void placePendingNode(NodeCanvas& canvas, data::EditorLayouts& layouts,
                      const core::Guid& graphId, core::Guid& pendingPlace,
                      const Vec2& position);

// Persist this frame's node drags into the layout side-store.
void persistMovedNodes(
    data::EditorLayouts& layouts, const core::Guid& graphId,
    const vector<std::pair<core::Guid, Vec2>>& movedNodes);

} // namespace game
