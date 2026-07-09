#include "game/ui/NodeCanvas.hpp"

#include <imgui.h>
#include <imgui_node_editor.h>

namespace ed = ax::NodeEditor;

namespace game {

NodeCanvas::NodeCanvas() {
    ed::Config config;
    // Positions live in the EditorLayouts side-store (chantier 8.6
    // decision) — ed's own json persistence stays off.
    config.SettingsFile = nullptr;
    context = ed::CreateEditor(&config);
}

NodeCanvas::~NodeCanvas() {
    ed::DestroyEditor(context);
}

u64 NodeCanvas::nodeIdFor(const core::Guid& node) {
    if (const auto it = nodeIds.find(node); it != nodeIds.end()) {
        return it->second;
    }
    const u64 id = nextId;
    nextId += 3; // node, input pin, output pin
    nodeIds.emplace(node, id);
    nodeOfId.emplace(id, node);
    pinOfId.emplace(id + 1, PinRef { node, /*output=*/false });
    pinOfId.emplace(id + 2, PinRef { node, /*output=*/true });
    return id;
}

u64 NodeCanvas::linkIdFor(const core::Guid& link) {
    if (const auto it = linkIds.find(link); it != linkIds.end()) {
        return it->second;
    }
    const u64 id = nextId++;
    linkIds.emplace(link, id);
    linkOfId.emplace(id, link);
    return id;
}

void NodeCanvas::begin(const char* id) {
    ed::SetCurrentEditor(context);
    ed::Begin(id, ImVec2(0.0f, 0.0f));
    drawnNodes.clear();
}

void NodeCanvas::beginNode(const core::Guid& node) {
    ed::BeginNode(nodeIdFor(node));
    drawnNodes.push_back(node);
}

void NodeCanvas::endNode() {
    ed::EndNode();
}

void NodeCanvas::inputPin(const core::Guid& node) {
    ed::BeginPin(nodeIdFor(node) + 1, ed::PinKind::Input);
    ImGui::TextUnformatted(">");
    ed::EndPin();
}

void NodeCanvas::outputPin(const core::Guid& node) {
    ed::BeginPin(nodeIdFor(node) + 2, ed::PinKind::Output);
    ImGui::TextUnformatted(">");
    ed::EndPin();
}

void NodeCanvas::link(const core::Guid& link, const core::Guid& fromNode,
                      const core::Guid& toNode, const Vec4& color,
                      f32 thickness) {
    ed::Link(linkIdFor(link), nodeIdFor(fromNode) + 2,
             nodeIdFor(toNode) + 1, ImVec4(color.x, color.y, color.z, color.w),
             thickness);
}

void NodeCanvas::setNodePosition(const core::Guid& node,
                                 const Vec2& position) {
    ed::SetNodePosition(nodeIdFor(node), ImVec2(position.x, position.y));
    lastPositions[node] = position; // a programmatic move is not a drag
}

Vec2 NodeCanvas::nodePosition(const core::Guid& node) const {
    const auto it = nodeIds.find(node);
    if (it == nodeIds.end()) {
        return Vec2 { 0.0f };
    }
    const ImVec2 position = ed::GetNodePosition(it->second);
    return Vec2 { position.x, position.y };
}

Vec2 NodeCanvas::nodeCenter(const core::Guid& node) const {
    const auto it = nodeIds.find(node);
    if (it == nodeIds.end()) {
        return Vec2 { 0.0f };
    }
    const ImVec2 position = ed::GetNodePosition(it->second);
    const ImVec2 size = ed::GetNodeSize(it->second);
    return Vec2 { position.x + size.x * 0.5f, position.y + size.y * 0.5f };
}

void NodeCanvas::navigateToContent() {
    ed::NavigateToContent(0.25f);
}

void NodeCanvas::end(Actions& out) {
    // New link: a pin->pin drag. Orient output -> input; the panel's
    // canLink adds graph-specific rules (e.g. nothing INTO "Any State").
    // NB (this develop commit): EndCreate/EndDelete assert unless their
    // Begin returned true — they go INSIDE the if, unlike upstream's
    // blueprint example.
    if (ed::BeginCreate()) {
        ed::PinId a, b;
        if (ed::QueryNewLink(&a, &b)) {
            const auto ia = pinOfId.find(static_cast<u64>(a.Get()));
            const auto ib = pinOfId.find(static_cast<u64>(b.Get()));
            bool valid = ia != pinOfId.end() && ib != pinOfId.end() &&
                         ia->second.output != ib->second.output &&
                         ia->second.node != ib->second.node;
            const PinRef* from = nullptr;
            const PinRef* to = nullptr;
            if (valid) {
                from = ia->second.output ? &ia->second : &ib->second;
                to = ia->second.output ? &ib->second : &ia->second;
                valid = !canLink || canLink(from->node, to->node);
            }
            if (!valid) {
                ed::RejectNewItem(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), 2.0f);
            } else if (ed::AcceptNewItem(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                         2.0f)) {
                out.linkCreated = true;
                out.linkFrom = from->node;
                out.linkTo = to->node;
            }
        }
        ed::EndCreate();
    }

    // Deletions (Del key / context): the predicates enforce §5 — only
    // session-created drafts may go; base records reject visibly.
    if (ed::BeginDelete()) {
        ed::LinkId deletedLink;
        while (ed::QueryDeletedLink(&deletedLink)) {
            const auto it = linkOfId.find(static_cast<u64>(deletedLink.Get()));
            const bool allowed = it != linkOfId.end() && canDeleteLink &&
                                 canDeleteLink(it->second);
            if (allowed && ed::AcceptDeletedItem()) {
                out.deletedLinks.push_back(it->second);
            } else if (!allowed) {
                ed::RejectDeletedItem();
            }
        }
        ed::NodeId deletedNode;
        while (ed::QueryDeletedNode(&deletedNode)) {
            const auto it = nodeOfId.find(static_cast<u64>(deletedNode.Get()));
            const bool allowed = it != nodeOfId.end() && canDeleteNode &&
                                 canDeleteNode(it->second);
            if (allowed && ed::AcceptDeletedItem()) {
                out.deletedNodes.push_back(it->second);
            } else if (!allowed) {
                ed::RejectDeletedItem();
            }
        }
        ed::EndDelete(); // inside the if — see the EndCreate note above
    }

    // Context menus: recorded here (canvas coords are only known inside
    // the frame), opened by the panel as ordinary popups after end().
    ed::Suspend();
    ed::NodeId contextNodeId;
    if (ed::ShowNodeContextMenu(&contextNodeId)) {
        const auto it = nodeOfId.find(static_cast<u64>(contextNodeId.Get()));
        if (it != nodeOfId.end()) {
            out.contextNode = it->second;
        }
    } else if (ed::ShowBackgroundContextMenu()) {
        out.backgroundContext = true;
        const ImVec2 canvasPos = ed::ScreenToCanvas(ImGui::GetMousePos());
        out.backgroundContextPos = Vec2 { canvasPos.x, canvasPos.y };
    }
    ed::Resume();

    // Node drags: report on release only (one layout write per drag, the
    // schedules-timeline cadence). setNodePosition updates lastPositions,
    // so programmatic placement never reports as a move.
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        for (const core::Guid& node : drawnNodes) {
            const Vec2 position = nodePosition(node);
            const auto it = lastPositions.find(node);
            if (it == lastPositions.end()) {
                lastPositions.emplace(node, position);
                continue;
            }
            if (it->second != position) {
                it->second = position;
                out.movedNodes.emplace_back(node, position);
            }
        }
    }

    // Selection: surface single-object selection changes as clicks.
    if (ed::GetSelectedObjectCount() == 1) {
        ed::NodeId selectedNode;
        ed::LinkId selectedLink;
        if (ed::GetSelectedNodes(&selectedNode, 1) == 1) {
            const auto it = nodeOfId.find(static_cast<u64>(selectedNode.Get()));
            if (it != nodeOfId.end() && it->second != lastSelectedNode) {
                out.clickedNode = it->second;
            }
            lastSelectedNode =
                it != nodeOfId.end() ? it->second : core::Guid {};
            lastSelectedLink = {};
        } else if (ed::GetSelectedLinks(&selectedLink, 1) == 1) {
            const auto it = linkOfId.find(static_cast<u64>(selectedLink.Get()));
            if (it != linkOfId.end() && it->second != lastSelectedLink) {
                out.clickedLink = it->second;
            }
            lastSelectedLink =
                it != linkOfId.end() ? it->second : core::Guid {};
            lastSelectedNode = {};
        }
    } else if (ed::GetSelectedObjectCount() == 0) {
        lastSelectedNode = {};
        lastSelectedLink = {};
    }

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

} // namespace game
