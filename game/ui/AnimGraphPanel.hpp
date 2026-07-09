#pragma once

#include "data/editor/EditorLayouts.hpp"
#include "data/plugins/EditSession.hpp"
#include "game/ui/NodeCanvas.hpp"

namespace game {

// The anim-graph editor (chantier 8.6, reshaped by 8.7b into the single
// "True Adventurer DB" window): AnimStateForm = node, AnimTransitionForm
// = link (`from == 0` hangs off the "Any State" pseudo-node, keyed by the
// graph's own guid). Every edit is an ordinary EditSession op — the
// export is the same plugin as every other panel. Node x/y go to the
// EditorLayouts side-store; graphs without stored positions get the
// deterministic data::layoutGraph.
//
// 8.7b contract (all graph panels): the shell owns the windows — the
// panel draws INTO them. drawCanvas() fills the central Editor window,
// drawHierarchy() the top of the Inspector, drawInspectorExtras() the
// type-specific widgets above the PropertyGrid (no-op when the selection
// is not ours). The Browser owns the item list.
class AnimGraphPanel {
public:
    AnimGraphPanel(data::EditSession& session, data::EditorLayouts& layouts,
                   core::Guid& selected)
        : session { session }, layouts { layouts }, selected { selected } {}

    void drawCanvas(const core::Guid& graph);
    void drawHierarchy(const core::Guid& graph);
    void drawInspectorExtras(const core::Guid& target);

private:
    data::EditSession& session;
    data::EditorLayouts& layouts;
    core::Guid& selected; // editor-wide selection (Inspector follows)

    NodeCanvas canvas;
    core::Guid canvasShown;    // graph whose positions have been applied
    core::Guid pendingPlace;   // node created from the context menu...
    Vec2 pendingPlacePos {};   // ...placed on the NEXT frame (needs ed frame)
    Vec2 contextPos {};        // canvas pos of the background right-click
    core::Guid contextNode;    // node under the node context popup
    core::Guid dragFrom;          // 8.7d: pin dragged into empty canvas...
    bool dragFromOutput { false }; // ...proposes a pre-linked new state
    Vec2 dragPos {};
    u32 stateCounter { 0 };    // editorId suffixes for + State / new links
};

} // namespace game
