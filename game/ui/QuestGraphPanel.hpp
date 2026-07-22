#pragma once

#include "data/editor/EditorLayouts.hpp"
#include "data/plugins/EditSession.hpp"
#include "game/ui/NodeCanvas.hpp"

namespace game {

// The quest STRUCTURE editor: the graph
// IS the quest editor — states = nodes (kind badge, start marker),
// branches = links ("n tasks"), everything created here or from the
// Inspector hierarchy. Contract: the shell owns the windows —
// drawCanvas() fills the central Editor window, drawInspectorExtras()
// adds the branch task list above the PropertyGrid (no-op otherwise);
// the quest hierarchy tree lives in the shell (EditorScene).
class QuestGraphPanel {
public:
    QuestGraphPanel(data::EditSession& session, data::EditorLayouts& layouts,
                    core::Guid& selected)
        : session { session }, layouts { layouts }, selected { selected } {}

    void drawCanvas(const core::Guid& quest);
    void drawInspectorExtras(const core::Guid& target);

private:
    data::EditSession& session;
    data::EditorLayouts& layouts;
    core::Guid& selected;

    NodeCanvas canvas;
    core::Guid canvasShown;
    core::Guid pendingPlace;
    Vec2 pendingPlacePos {};
    Vec2 contextPos {};
    core::Guid contextNode;
    core::Guid dragFrom;          // pin dragged into empty canvas...
    bool dragFromOutput { false }; // ...proposes a pre-linked new state
    Vec2 dragPos {};
    u32 createCounter { 0 };
};

} // namespace game
