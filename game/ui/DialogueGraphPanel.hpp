#pragma once

#include "data/editor/EditorLayouts.hpp"
#include "data/plugins/EditSession.hpp"
#include "game/ui/NodeCanvas.hpp"

namespace game {

// The dialogue tree laid flat (chantier 8.7, reshaped by 8.7b):
// DialogueNodeForm = node (speaker + text excerpt + condition badge),
// the `parent` link = edge. Depth becomes columns, the `order` field
// ranks the rows. Dragging a new link RE-PARENTS the target (setField
// parent, anti-cycle guarded); links themselves are never deletable (an
// orphan node is invisible). 8.7b contract: the shell owns the windows —
// drawCanvas() fills the central Editor window; the dialogue hierarchy
// tree (reorder, + reply, + condition) lives in the shell's Inspector.
class DialogueGraphPanel {
public:
    DialogueGraphPanel(data::EditSession& session,
                       data::EditorLayouts& layouts, core::Guid& selected)
        : session { session }, layouts { layouts }, selected { selected } {}

    void drawCanvas(const core::Guid& dialogue);

private:
    data::EditSession& session;
    data::EditorLayouts& layouts;
    core::Guid& selected;

    NodeCanvas canvas;
    core::Guid canvasShown;
    core::Guid contextNode;
    str status; // last refused re-parent, shown above the canvas
    u32 createCounter { 0 };
};

} // namespace game
