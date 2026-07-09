#pragma once

#include "data/editor/EditorLayouts.hpp"
#include "data/plugins/EditSession.hpp"
#include "game/ui/NodeCanvas.hpp"

namespace game {

// The dialogue tree laid flat (chantier 8.7): DialogueNodeForm = node
// (speaker + text excerpt + condition badge), the `parent` link = edge.
// Depth becomes columns, the `order` field ranks the rows — branching
// conversations finally read at a glance. Dragging a new link RE-PARENTS
// the target (setField parent, anti-cycle guarded); links themselves are
// never deletable (an orphan node is invisible). Sibling reorder stays
// in the 8.3 tree (v1). Same selection as the tree window.
class DialogueGraphPanel {
public:
    DialogueGraphPanel(data::EditSession& session,
                       data::EditorLayouts& layouts, core::Guid& selected)
        : session { session }, layouts { layouts }, selected { selected } {}

    // Draws the "Dialogue Graph" window.
    void draw();

private:
    data::EditSession& session;
    data::EditorLayouts& layouts;
    core::Guid& selected;

    NodeCanvas canvas;
    core::Guid dialogueSelected;
    core::Guid canvasShown;
    core::Guid contextNode;
    str status; // last refused re-parent, shown above the canvas
    u32 createCounter { 0 };
};

} // namespace game
