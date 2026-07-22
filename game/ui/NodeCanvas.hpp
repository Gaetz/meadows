#pragma once

#include <functional>
#include <optional>
#include <unordered_map>

#include <glm/glm.hpp> // Defines.hpp only forward-declares Vec2/Vec4

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"

namespace ax::NodeEditor {
struct EditorContext;
}

namespace game {

// The thin, reusable wrapper over ax::NodeEditor — shared
// by every graph editor (anim graph, quests, dialogues). It
// owns what all of them need and nothing more:
//  - the ed context (one per canvas, internal json persistence DISABLED —
//    node positions belong to the data::EditorLayouts side-store);
//  - the guid<->integer id tables (ed ids are pointer-sized, our guids
//    are 128-bit; a monotonic counter per canvas, stable across frames);
//  - the BeginCreate/BeginDelete plumbing, surfaced as ACTIONS the panel
//    consumes AFTER the frame (create this transition, delete that node,
//    selection moved, nodes dragged) — the §5 rules (delete only
//    session-created drafts) stay in the panel via the predicates below.
// The CONTENT of a node is drawn by each editor between beginNode/endNode
// — no mega-abstraction; its three consumers calibrate this class.
class NodeCanvas {
public:
    NodeCanvas();
    ~NodeCanvas();
    NodeCanvas(const NodeCanvas&) = delete;
    NodeCanvas& operator=(const NodeCanvas&) = delete;

    struct Actions {
        bool linkCreated { false }; // a pin->pin drag completed...
        core::Guid linkFrom;        // ...from this node's output
        core::Guid linkTo;          // ...to this node's input
        // A pin->EMPTY-CANVAS drag released: the panel proposes
        // what can be created there, pre-linked to the source.
        bool newNodeRequested { false };
        core::Guid newNodeFrom;          // the dragged pin's node
        bool newNodeFromOutput { false }; // which side was dragged
        Vec2 newNodePos {};               // canvas coords for the new node
        vector<core::Guid> deletedNodes; // accepted by canDeleteNode
        vector<core::Guid> deletedLinks; // accepted by canDeleteLink
        vector<std::pair<core::Guid, Vec2>> movedNodes; // on drag release
        core::Guid clickedNode; // selection change this frame (or null)
        core::Guid clickedLink;
        bool backgroundContext { false }; // right-click on empty canvas
        Vec2 backgroundContextPos {};     // canvas coords ("+ node here")
        core::Guid contextNode;           // right-click on a node
    };

    // Frame protocol: begin() -> setNodePosition/beginNode/link... ->
    // end(actions). Everything between the two runs inside the ed frame.
    void begin(const char* id);
    void end(Actions& out);

    void beginNode(const core::Guid& node);
    void endNode();
    // Pins are drawn where the caller places them inside the node.
    void inputPin(const core::Guid& node);
    void outputPin(const core::Guid& node);

    void link(const core::Guid& link, const core::Guid& fromNode,
              const core::Guid& toNode, const Vec4& color,
              f32 thickness = 2.0f);

    // Canvas-space position (top-left) and center — inside the frame only.
    void setNodePosition(const core::Guid& node, const Vec2& position);
    Vec2 nodePosition(const core::Guid& node) const;
    Vec2 nodeCenter(const core::Guid& node) const;

    void navigateToContent(); // zoom/pan to fit — inside the frame only

    // §5 gates, checked before ed accepts a deletion (default: refuse).
    std::function<bool(const core::Guid&)> canDeleteNode;
    std::function<bool(const core::Guid&)> canDeleteLink;
    // Optional extra validation for new links (from, to node guids).
    std::function<bool(const core::Guid&, const core::Guid&)> canLink;

private:
    struct PinRef {
        core::Guid node;
        bool output { false };
    };
    u64 nodeIdFor(const core::Guid& node); // base id; in = +1, out = +2
    u64 linkIdFor(const core::Guid& link);

    ax::NodeEditor::EditorContext* context { nullptr };
    std::unordered_map<core::Guid, u64> nodeIds;
    std::unordered_map<u64, core::Guid> nodeOfId;
    std::unordered_map<u64, PinRef> pinOfId;
    std::unordered_map<core::Guid, u64> linkIds;
    std::unordered_map<u64, core::Guid> linkOfId;
    std::unordered_map<core::Guid, Vec2> lastPositions; // drag detection
    vector<core::Guid> drawnNodes; // this frame, for move collection
    core::Guid lastSelectedNode;
    core::Guid lastSelectedLink;
    u64 nextId { 1 };
};

} // namespace game
