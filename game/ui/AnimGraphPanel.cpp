#include "game/ui/AnimGraphPanel.hpp"

#include <algorithm>

#include <imgui.h>

#include "data/editor/GraphLayout.hpp"
#include "data/forms/AnimForms.hpp"
#include "game/ui/FormPicker.hpp"

namespace game {

namespace {

constexpr ImVec4 kWarnColor { 1.0f, 0.6f, 0.2f, 1.0f };

struct GraphData {
    vector<std::pair<core::Guid, const data::AnimStateForm*>> states;
    vector<std::pair<core::Guid, const data::AnimTransitionForm*>>
        transitions;
};

// The graph's children, sorted by guid — forEachVisible's draft order is
// a hash order and the canvas must not flicker between frames.
GraphData collectGraph(const data::EditSession& session,
                       const core::Guid& graph) {
    GraphData data;
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (type.id == data::AnimStateForm::staticTypeInfo().id) {
            const auto* state = static_cast<const data::AnimStateForm*>(&form);
            if (state->parent == graph) {
                data.states.emplace_back(id, state);
            }
        } else if (type.id == data::AnimTransitionForm::staticTypeInfo().id) {
            const auto* transition =
                static_cast<const data::AnimTransitionForm*>(&form);
            if (transition->parent == graph) {
                data.transitions.emplace_back(id, transition);
            }
        }
    });
    const auto byGuid = [](const auto& a, const auto& b) {
        return a.first < b.first;
    };
    std::sort(data.states.begin(), data.states.end(), byGuid);
    std::sort(data.transitions.begin(), data.transitions.end(), byGuid);
    return data;
}

const data::AnimGraphForm* graphOf(const data::EditSession& session,
                                   const core::Guid& id) {
    const auto* type = id.isValid() ? session.viewType(id) : nullptr;
    if (!type || type->id != data::AnimGraphForm::staticTypeInfo().id) {
        return nullptr;
    }
    return static_cast<const data::AnimGraphForm*>(session.view(id));
}

} // namespace

void AnimGraphPanel::drawCanvas(const core::Guid& graphId) {
    const data::AnimGraphForm* graphForm = graphOf(session, graphId);
    if (!graphForm) {
        ImGui::TextDisabled("(select an anim graph in the Browser)");
        return;
    }

    const GraphData data = collectGraph(session, graphId);
    const auto isState = [&](const core::Guid& id) {
        return std::any_of(data.states.begin(), data.states.end(),
                           [&](const auto& s) { return s.first == id; });
    };
    // "Any State" is keyed by the graph's own guid (positions included).
    const core::Guid anyState = graphId;
    const auto nodeKeyOf = [&](const core::Guid& from) {
        return from.isValid() ? from : anyState;
    };

    // Warnings line — the graph validation set, inline and cheap.
    vector<str> warnings;
    if (!graphForm->initialState.isValid()) {
        warnings.push_back("no initial state");
    } else if (!isState(graphForm->initialState)) {
        warnings.push_back("initialState is not a state of this graph");
    }
    for (const auto& [id, state] : data.states) {
        if (!state->clip.isValid()) {
            warnings.push_back("state '" + state->editorId + "' has no clip");
        }
    }
    for (const auto& [id, transition] : data.transitions) {
        if (!transition->to.isValid() || !isState(transition->to)) {
            warnings.push_back("transition '" + transition->editorId +
                               "' has no destination");
        }
        if (transition->from.isValid() && !isState(transition->from)) {
            warnings.push_back("transition '" + transition->editorId +
                               "' comes from a foreign state");
        }
        if (!transition->param.empty() && transition->compare != "greater" &&
            transition->compare != "less") {
            warnings.push_back("transition '" + transition->editorId +
                               "': unknown compare '" + transition->compare +
                               "'");
        }
    }

    // Toolbar.
    const bool autoLayoutRequested = ImGui::Button("Auto-layout");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu states, %zu transitions", data.states.size(),
                        data.transitions.size());
    for (const str& warning : warnings) {
        ImGui::TextColored(kWarnColor, "(!) %s", warning.c_str());
    }

    // ---- The ed frame -------------------------------------------------
    canvas.canDeleteNode = [&](const core::Guid& id) {
        return session.isCreated(id); // §5: base records are immutable
    };
    canvas.canDeleteLink = canvas.canDeleteNode;
    canvas.canLink = [&](const core::Guid&, const core::Guid& to) {
        return to != anyState; // nothing transitions INTO "Any State"
    };

    canvas.begin("animgraph-canvas");

    // Position pass: stored side-store positions, auto-layout for the
    // rest — on first show of a graph, on request, and for fresh nodes.
    if (canvasShown != graphId || autoLayoutRequested) {
        vector<core::Guid> nodes { anyState };
        vector<std::pair<core::Guid, core::Guid>> edges;
        for (const auto& [id, state] : data.states) {
            nodes.push_back(id);
        }
        for (const auto& [id, transition] : data.transitions) {
            if (transition->to.isValid()) {
                edges.emplace_back(nodeKeyOf(transition->from),
                                   transition->to);
            }
        }
        vector<core::Guid> roots { anyState };
        if (graphForm->initialState.isValid()) {
            roots.push_back(graphForm->initialState);
        }
        const data::GraphLayoutResult layout =
            data::layoutGraph(nodes, edges, roots);
        for (const core::Guid& node : nodes) {
            if (autoLayoutRequested) {
                // Explicit re-layout overwrites the stored positions.
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
        canvasShown = graphId;
    }
    if (pendingPlace.isValid()) {
        canvas.setNodePosition(pendingPlace, pendingPlacePos);
        layouts.setPosition(graphId, pendingPlace, pendingPlacePos);
        layouts.save();
        pendingPlace = {};
    }

    // "Any State" pseudo-node: output only — transitions with from == 0.
    canvas.beginNode(anyState);
    ImGui::TextUnformatted("Any State");
    ImGui::SameLine();
    canvas.outputPin(anyState);
    canvas.endNode();

    for (const auto& [id, state] : data.states) {
        canvas.beginNode(id);
        canvas.inputPin(id);
        ImGui::SameLine();
        const str title =
            state->editorId.empty() ? id.toString() : state->editorId;
        ImGui::TextUnformatted(title.c_str());
        ImGui::SameLine();
        canvas.outputPin(id);
        if (graphForm->initialState == id) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "* initial");
        }
        ImGui::TextDisabled(
            "clip: %s", formDisplayName(session, state->clip).c_str());
        if (state->speed != 1.0f || state->referenceSpeed > 0.0f) {
            ImGui::TextDisabled("speed: %.2f  ref: %.2f", state->speed,
                                state->referenceSpeed);
        }
        canvas.endNode();
    }

    for (const auto& [id, transition] : data.transitions) {
        if (!transition->to.isValid() || !isState(transition->to)) {
            continue; // incomplete edge: listed in the warnings above
        }
        // waitForEnd reads as the "cooler" blue link; plain ones white.
        const Vec4 color = transition->waitForEnd
                               ? Vec4 { 0.5f, 0.7f, 1.0f, 1.0f }
                               : Vec4 { 0.9f, 0.9f, 0.9f, 0.8f };
        canvas.link(id, nodeKeyOf(transition->from), transition->to, color,
                    selected == id ? 3.5f : 2.0f);
    }

    // Link labels: ed has none — draw at the edge midpoint (canvas space,
    // the ed canvas transforms the draw list).
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (const auto& [id, transition] : data.transitions) {
        if (!transition->to.isValid() || !isState(transition->to)) {
            continue;
        }
        str label;
        if (!transition->param.empty()) {
            label = transition->param +
                    (transition->compare == "less" ? " < " : " > ") +
                    std::to_string(transition->threshold);
            label.erase(label.find_last_not_of('0') + 1);
            if (!label.empty() && label.back() == '.') {
                label.pop_back();
            }
        } else if (transition->waitForEnd) {
            label = "on end";
        }
        if (!transition->requiredTag.empty()) {
            label += (label.empty() ? "" : " ") + str { "[" } +
                     transition->requiredTag + "]";
        }
        if (label.empty()) {
            continue;
        }
        const Vec2 a = canvas.nodeCenter(nodeKeyOf(transition->from));
        const Vec2 b = canvas.nodeCenter(transition->to);
        const Vec2 mid = (a + b) * 0.5f;
        drawList->AddText(ImVec2(mid.x, mid.y - 8.0f),
                          IM_COL32(200, 200, 160, 220), label.c_str());
    }

    NodeCanvas::Actions actions;
    canvas.end(actions);
    // ---- Frame done: act on what the canvas reported -------------------

    if (actions.linkCreated) {
        data::EditSession::Gesture gesture { session }; // one undo step
        const core::Guid id = session.createForm(
            data::AnimTransitionForm::staticTypeInfo().id,
            graphForm->editorId + "T" + std::to_string(++stateCounter));
        session.setField(id, core::fnv1a("parent"),
                         reflect::Value { graphId });
        session.setField(id, core::fnv1a("from"),
                         reflect::Value { actions.linkFrom == anyState
                                              ? core::Guid {}
                                              : actions.linkFrom });
        session.setField(id, core::fnv1a("to"),
                         reflect::Value { actions.linkTo });
        selected = id;
    }
    for (const core::Guid& id : actions.deletedLinks) {
        session.removeCreated(id);
    }
    for (const core::Guid& id : actions.deletedNodes) {
        // A deleted state takes its session-created transitions along;
        // base transitions stay and surface as dangling warnings (§5).
        // One undo step for the whole cascade.
        data::EditSession::Gesture gesture { session };
        for (const auto& [transitionId, transition] : data.transitions) {
            if ((transition->from == id || transition->to == id) &&
                session.isCreated(transitionId)) {
                session.removeCreated(transitionId);
            }
        }
        session.removeCreated(id);
    }
    for (const auto& [node, position] : actions.movedNodes) {
        layouts.setPosition(graphId, node, position);
    }
    if (!actions.movedNodes.empty()) {
        layouts.save();
    }
    if (actions.clickedNode.isValid() && actions.clickedNode != anyState) {
        selected = actions.clickedNode;
    }
    if (actions.clickedLink.isValid()) {
        selected = actions.clickedLink;
    }
    if (actions.backgroundContext) {
        contextPos = actions.backgroundContextPos;
        ImGui::OpenPopup("ag-background");
    }
    if (actions.contextNode.isValid() && actions.contextNode != anyState) {
        contextNode = actions.contextNode;
        ImGui::OpenPopup("ag-node");
    }
    if (actions.newNodeRequested) {
        dragFrom = actions.newNodeFrom;
        dragFromOutput = actions.newNodeFromOutput;
        dragPos = actions.newNodePos;
        ImGui::OpenPopup("ag-newnode");
    }

    if (ImGui::BeginPopup("ag-background")) {
        if (ImGui::MenuItem("+ State")) {
            data::EditSession::Gesture gesture { session };
            const core::Guid id = session.createForm(
                data::AnimStateForm::staticTypeInfo().id,
                graphForm->editorId + "State" +
                    std::to_string(++stateCounter));
            session.setField(id, core::fnv1a("parent"),
                             reflect::Value { graphId });
            pendingPlace = id; // placed inside the NEXT ed frame
            pendingPlacePos = contextPos;
            selected = id;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("ag-node")) {
        if (ImGui::MenuItem("Set as initial")) {
            session.setField(graphId, core::fnv1a("initialState"),
                             reflect::Value { contextNode });
        }
        ImGui::EndPopup();
    }
    // A pin dragged into empty canvas — state + transition in one
    // gesture, oriented by which side was dragged ("Any State" only
    // emits, so an input-side drag from it can't happen).
    if (ImGui::BeginPopup("ag-newnode")) {
        const bool fromOutput = dragFromOutput;
        const str what = fromOutput ? "+ State (transition from here)"
                                    : "+ State (transition INTO here)";
        if (ImGui::MenuItem(what.c_str())) {
            data::EditSession::Gesture gesture { session };
            const core::Guid stateId = session.createForm(
                data::AnimStateForm::staticTypeInfo().id,
                graphForm->editorId + "State" +
                    std::to_string(++stateCounter));
            session.setField(stateId, core::fnv1a("parent"),
                             reflect::Value { graphId });
            const core::Guid transitionId = session.createForm(
                data::AnimTransitionForm::staticTypeInfo().id,
                graphForm->editorId + "T" + std::to_string(++stateCounter));
            session.setField(transitionId, core::fnv1a("parent"),
                             reflect::Value { graphId });
            const core::Guid fromState =
                dragFrom == anyState ? core::Guid {} : dragFrom;
            session.setField(transitionId, core::fnv1a("from"),
                             reflect::Value { fromOutput ? fromState
                                                         : stateId });
            session.setField(transitionId, core::fnv1a("to"),
                             reflect::Value { fromOutput ? stateId
                                                         : dragFrom });
            pendingPlace = stateId;
            pendingPlacePos = dragPos;
            selected = stateId;
        }
        ImGui::EndPopup();
    }
}

void AnimGraphPanel::drawHierarchy(const core::Guid& graphId) {
    const data::AnimGraphForm* graphForm = graphOf(session, graphId);
    if (!graphForm) {
        return;
    }
    const GraphData data = collectGraph(session, graphId);
    if (ImGui::SmallButton("+ State")) {
        data::EditSession::Gesture gesture { session };
        const core::Guid id = session.createForm(
            data::AnimStateForm::staticTypeInfo().id,
            graphForm->editorId + "State" + std::to_string(++stateCounter));
        session.setField(id, core::fnv1a("parent"),
                         reflect::Value { graphId });
        selected = id;
    }
    for (const auto& [id, state] : data.states) {
        str label = state->editorId.empty() ? id.toString() : state->editorId;
        if (graphForm->initialState == id) {
            label += "  <- initial";
        }
        if (ImGui::Selectable((label + "##hs" + id.toString()).c_str(),
                              selected == id)) {
            selected = id;
        }
    }
    for (const auto& [id, transition] : data.transitions) {
        const str label =
            (transition->from.isValid()
                 ? formDisplayName(session, transition->from)
                 : str { "Any State" }) +
            " -> " + formDisplayName(session, transition->to) + "##ht" +
            id.toString();
        if (ImGui::Selectable(label.c_str(), selected == id)) {
            selected = id;
        }
    }
}

} // namespace game
