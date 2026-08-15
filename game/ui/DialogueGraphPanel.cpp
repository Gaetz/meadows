#include "game/ui/DialogueGraphPanel.hpp"
#include "game/ui/Keywords.hpp"

#include <algorithm>
#include <deque>
#include <unordered_set>

#include <imgui.h>

#include "data/editor/GraphLayout.hpp"
#include "game/ui/GraphPanelCommon.hpp"
#include "game/ui/EventPicker.hpp"
#include "gameplay/condition/Condition.hpp"
#include "quest/Dialogue.hpp"
#include "quest/Quest.hpp"

namespace game {

namespace {

struct DialogueData {
    // Reachable from the root only (other dialogues share the type).
    vector<std::pair<core::Guid, const quest::DialogueNodeForm*>> nodes;
    std::unordered_map<core::Guid, core::Guid> parentOf;
    std::unordered_map<core::Guid, u32> conditionCount;
};

DialogueData collectDialogue(const data::EditSession& session,
                             const core::Guid& rootNode) {
    // All nodes + conditions once, then BFS the root's tree.
    vector<std::pair<core::Guid, const quest::DialogueNodeForm*>> all;
    std::unordered_map<core::Guid, u32> conditions;
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (type.id == quest::DialogueNodeForm::staticTypeInfo().id) {
            all.emplace_back(
                id, static_cast<const quest::DialogueNodeForm*>(&form));
        } else if (type.id == gameplay::ConditionForm::staticTypeInfo().id) {
            ++conditions[static_cast<const gameplay::ConditionForm*>(&form)
                             ->parent];
        }
    });
    std::sort(all.begin(), all.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    DialogueData data;
    std::unordered_set<core::Guid> reachable;
    std::deque<core::Guid> queue;
    if (rootNode.isValid()) {
        reachable.insert(rootNode);
        queue.push_back(rootNode);
    }
    while (!queue.empty()) {
        const core::Guid current = queue.front();
        queue.pop_front();
        for (const auto& [id, node] : all) {
            if (node->parent == current && reachable.insert(id).second) {
                queue.push_back(id);
            }
        }
    }
    for (const auto& [id, node] : all) {
        if (!reachable.contains(id)) {
            continue;
        }
        data.nodes.emplace_back(id, node);
        if (node->parent.isValid()) {
            data.parentOf.emplace(id, node->parent);
        }
        if (const auto it = conditions.find(id); it != conditions.end()) {
            data.conditionCount.emplace(id, it->second);
        }
    }
    return data;
}

} // namespace

void DialogueGraphPanel::drawCanvas(const core::Guid& dialogueId) {
    const auto* type =
        dialogueId.isValid() ? session.viewType(dialogueId) : nullptr;
    if (!type || type->id != quest::DialogueForm::staticTypeInfo().id) {
        ImGui::TextDisabled("(select a dialogue in the Browser)");
        return;
    }
    const auto* dialogue =
        static_cast<const quest::DialogueForm*>(session.view(dialogueId));
    if (!dialogue->rootNode.isValid() || !session.view(dialogue->rootNode)) {
        ImGui::TextColored(kWarnColor,
                           "(!) no root node — create it in the Inspector");
        return;
    }

    const DialogueData data =
        collectDialogue(session, dialogue->rootNode);

    const bool autoLayoutRequested = ImGui::Button("Auto-layout");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu lines — drag a link to re-parent a reply",
                        data.nodes.size());
    if (!status.empty()) {
        ImGui::TextColored(kWarnColor, "%s",
                           status.c_str());
    }

    canvas.canDeleteNode = [&](const core::Guid& id) {
        return session.isCreated(id) && id != dialogue->rootNode;
    };
    // A parent link is never deletable: an orphan line is invisible.
    canvas.canDeleteLink = [](const core::Guid&) { return false; };
    // Anti-cycle: a node may not become a child of its own descendant,
    // of itself, and the root keeps no parent. end() rejects visually;
    // the accepted action re-checks below anyway (defence in depth).
    canvas.canLink = [&](const core::Guid& from, const core::Guid& to) {
        return to != dialogue->rootNode && from != to &&
               !data::isAncestorOf(data.parentOf, to, from);
    };

    canvas.begin("dialoguegraph-canvas");

    if (canvasShown != dialogueId || autoLayoutRequested) {
        vector<core::Guid> nodes;
        vector<std::pair<core::Guid, core::Guid>> edges;
        std::unordered_map<core::Guid, i32> rank;
        for (const auto& [id, node] : data.nodes) {
            nodes.push_back(id);
            rank.emplace(id, node->order);
            if (node->parent.isValid()) {
                edges.emplace_back(node->parent, id);
            }
        }
        applyGraphLayout(canvas, layouts, dialogueId, nodes, edges,
                         { dialogue->rootNode }, autoLayoutRequested,
                         &rank);
        canvasShown = dialogueId;
    }
    placePendingNode(canvas, layouts, dialogueId, pendingPlace,
                     pendingPlacePos);

    // Lint precompute: which event names anything reacts to (tasks,
    // quest startEvents, C++ listeners) — one scan, not one per node.
    std::unordered_set<str> listened;
    session.forEachVisible([&](const core::Guid&, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (type.id == quest::QuestTaskForm::staticTypeInfo().id) {
            listened.insert(
                static_cast<const quest::QuestTaskForm&>(form).event);
        } else if (type.id == quest::QuestForm::staticTypeInfo().id) {
            listened.insert(
                static_cast<const quest::QuestForm&>(form).startEvent);
        }
    });
    for (const char* builtin : { "OpenBarter", "OnPayFine" }) {
        listened.insert(builtin);
    }

    for (const auto& [id, node] : data.nodes) {
        canvas.beginNode(id);
        canvas.inputPin(id);
        ImGui::SameLine();
        const bool isPlayer = node->speaker == "Player";
        str title = isPlayer ? "> " : "";
        title += node->speaker.empty() ? "(npc)" : node->speaker;
        ImGui::TextUnformatted(title.c_str());
        ImGui::SameLine();
        canvas.outputPin(id);
        const str excerpt = node->text.size() > 48
                                ? node->text.substr(0, 48) + "..."
                                : node->text;
        ImGui::TextDisabled("%s", excerpt.empty() ? "(empty)"
                                                  : excerpt.c_str());
        if (!node->event.empty()) {
            ImGui::TextDisabled("[%s]", node->event.c_str());
            if (!listened.contains(node->event)) {
                ImGui::TextColored(kWarnColor,
                                   "(!) no listener");
            }
        }
        if (const auto it = data.conditionCount.find(id);
            it != data.conditionCount.end()) {
            ImGui::TextColored(ImVec4(0.7f, 0.8f, 1.0f, 1.0f), "%u cond",
                               it->second);
        }
        canvas.endNode();
    }

    for (const auto& [id, node] : data.nodes) {
        if (!node->parent.isValid()) {
            continue;
        }
        // The edge is the child's `parent` field: link id = child guid
        // (distinct id table from nodes — no clash in the canvas).
        canvas.link(id, node->parent, id, Vec4 { 0.9f, 0.9f, 0.9f, 0.8f },
                    2.0f);
    }

    NodeCanvas::Actions actions;
    canvas.end(actions);

    // Next sibling order under a parent line, shared by re-parenting
    // and both "+ reply" popups.
    const auto nextOrderUnder = [&](const core::Guid& parent) {
        i32 nextOrder = 0;
        for (const auto& [id, node] : data.nodes) {
            if (node->parent == parent) {
                nextOrder = std::max(nextOrder, node->order + 1);
            }
        }
        return nextOrder;
    };
    // Create a reply line under `parent` — alternate speakers, next
    // order — and select it. Returns the new node (null if the parent
    // vanished under a stale frame).
    const auto createReply = [&](const core::Guid& parent) -> core::Guid {
        const auto* parentNode =
            static_cast<const quest::DialogueNodeForm*>(
                session.view(parent));
        if (!parentNode) {
            return {};
        }
        data::EditSession::Gesture gesture { session };
        const bool parentIsPlayer = parentNode->speaker == "Player";
        const core::Guid id = session.createForm(
            quest::DialogueNodeForm::staticTypeInfo().id,
            parentNode->editorId + "Reply" +
                std::to_string(++createCounter));
        session.setField(id, core::fnv1a("parent"),
                         reflect::Value { parent });
        session.setField(
            id, core::fnv1a("speaker"),
            reflect::Value { parentIsPlayer ? str {}
                                            : str { "Player" } });
        session.setField(id, core::fnv1a("order"),
                         reflect::Value { nextOrderUnder(parent) });
        selected = id;
        return id;
    };

    if (actions.linkCreated) {
        // Re-parent, with the same guard the canvas already applied —
        // data may have changed under a stale frame.
        if (actions.linkTo == dialogue->rootNode ||
            data::isAncestorOf(data.parentOf, actions.linkTo,
                               actions.linkFrom)) {
            status = "(!) refused: would cycle the tree";
        } else {
            data::EditSession::Gesture gesture { session }; // parent+order
            session.setField(actions.linkTo, core::fnv1a("parent"),
                             reflect::Value { actions.linkFrom });
            session.setField(actions.linkTo, core::fnv1a("order"),
                             reflect::Value {
                                 nextOrderUnder(actions.linkFrom) });
            selected = actions.linkTo;
            status.clear();
        }
    }
    for (const core::Guid& id : actions.deletedNodes) {
        session.removeCreated(id);
    }
    persistMovedNodes(layouts, dialogueId, actions.movedNodes);
    if (actions.clickedNode.isValid()) {
        selected = actions.clickedNode;
    }
    if (actions.contextNode.isValid()) {
        contextNode = actions.contextNode;
        ImGui::OpenPopup("dgg-node");
    }
    // Dragging from a node's OUTPUT into empty canvas proposes a
    // new reply there (a tree: input-side drags re-parent, not create).
    if (actions.newNodeRequested && actions.newNodeFromOutput) {
        dragFrom = actions.newNodeFrom;
        dragPos = actions.newNodePos;
        ImGui::OpenPopup("dgg-newnode");
    }

    if (ImGui::BeginPopup("dgg-newnode")) {
        if (ImGui::MenuItem("+ reply here")) {
            if (const core::Guid id = createReply(dragFrom);
                id.isValid()) {
                pendingPlace = id;
                pendingPlacePos = dragPos;
            }
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("dgg-node")) {
        if (ImGui::MenuItem("+ reply")) {
            createReply(contextNode);
        }
        ImGui::EndPopup();
    }
}

} // namespace game
