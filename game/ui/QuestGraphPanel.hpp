#pragma once

#include "data/editor/EditorLayouts.hpp"
#include "data/plugins/EditSession.hpp"
#include "game/ui/NodeCanvas.hpp"

namespace game {

// The quest STRUCTURE view (chantier 8.7): QuestStateForm = node (kind
// badge, start marker), QuestBranchForm = link (label "n tasks") — the
// cross-state topology the 8.2 tree cannot show. The tree window stays
// as the detail view; both share the editor-wide selection. Same rules
// as every graph editor: EditSession-only writes, §5 deletes (created
// drafts only), positions in the EditorLayouts side-store keyed by the
// quest's guid.
class QuestGraphPanel {
public:
    QuestGraphPanel(data::EditSession& session, data::EditorLayouts& layouts,
                    core::Guid& selected)
        : session { session }, layouts { layouts }, selected { selected } {}

    // Draws the "Quest Graph" window.
    void draw();

private:
    data::EditSession& session;
    data::EditorLayouts& layouts;
    core::Guid& selected;

    NodeCanvas canvas;
    core::Guid questSelected;
    core::Guid canvasShown;
    core::Guid pendingPlace;
    Vec2 pendingPlacePos {};
    Vec2 contextPos {};
    core::Guid contextNode;
    u32 createCounter { 0 };
};

} // namespace game
