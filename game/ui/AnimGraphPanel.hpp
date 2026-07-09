#pragma once

#include "data/editor/EditorLayouts.hpp"
#include "data/plugins/EditSession.hpp"
#include "game/ui/NodeCanvas.hpp"

namespace game {

// The anim-graph editor (chantier 8.6 — the node canvas' first consumer):
// AnimStateForm = node, AnimTransitionForm = link (`from == 0` hangs off
// the "Any State" pseudo-node, keyed by the graph's own guid). Every edit
// is an ordinary EditSession op (createForm/setField/removeCreated) — the
// export is the same plugin as every other panel. Node x/y go to the
// EditorLayouts side-store, saved on drag release; graphs without stored
// positions get the deterministic data::layoutGraph.
//
// First panel following the class-per-panel rule (8.6+): EditorScene
// constructs it in reload() and calls draw() — the ConsolePanel pattern.
class AnimGraphPanel {
public:
    AnimGraphPanel(data::EditSession& session, data::EditorLayouts& layouts,
                   core::Guid& selected)
        : session { session }, layouts { layouts }, selected { selected } {}

    // Draws the "Anim Graph" window.
    void draw();

private:
    data::EditSession& session;
    data::EditorLayouts& layouts;
    core::Guid& selected; // editor-wide selection (the GameDB grid follows)

    NodeCanvas canvas;
    core::Guid graphSelected;
    core::Guid canvasShown;    // graph whose positions have been applied
    core::Guid pendingPlace;   // node created from the context menu...
    Vec2 pendingPlacePos {};   // ...placed on the NEXT frame (needs ed frame)
    Vec2 contextPos {};        // canvas pos of the background right-click
    core::Guid contextNode;    // node under the node context popup
    u32 stateCounter { 0 };    // editorId suffixes for + State / new links
};

} // namespace game
