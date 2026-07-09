#include "game/ui/QuestGraphPanel.hpp"

#include <algorithm>

#include <imgui.h>

#include "data/editor/GraphLayout.hpp"
#include "quest/Quest.hpp"

namespace game {

namespace {

constexpr ImVec4 kWarnColor { 1.0f, 0.6f, 0.2f, 1.0f };
constexpr ImVec4 kSuccessColor { 0.4f, 1.0f, 0.4f, 1.0f };
constexpr ImVec4 kFailureColor { 1.0f, 0.4f, 0.4f, 1.0f };

struct QuestData {
    vector<std::pair<core::Guid, const quest::QuestStateForm*>> states;
    vector<std::pair<core::Guid, const quest::QuestBranchForm*>> branches;
    vector<std::pair<core::Guid, const quest::QuestTaskForm*>> tasks;
};

QuestData collectQuest(const data::EditSession& session,
                       const core::Guid& questId) {
    QuestData data;
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& type) {
        if (type.id == quest::QuestStateForm::staticTypeInfo().id) {
            const auto* state =
                static_cast<const quest::QuestStateForm*>(&form);
            if (state->quest == questId) {
                data.states.emplace_back(id, state);
            }
        } else if (type.id == quest::QuestBranchForm::staticTypeInfo().id) {
            data.branches.emplace_back(
                id, static_cast<const quest::QuestBranchForm*>(&form));
        } else if (type.id == quest::QuestTaskForm::staticTypeInfo().id) {
            data.tasks.emplace_back(
                id, static_cast<const quest::QuestTaskForm*>(&form));
        }
    });
    const auto byGuid = [](const auto& a, const auto& b) {
        return a.first < b.first;
    };
    std::sort(data.states.begin(), data.states.end(), byGuid);
    // Branches of other quests: filtered against this quest's states.
    std::erase_if(data.branches, [&](const auto& entry) {
        return std::none_of(data.states.begin(), data.states.end(),
                            [&](const auto& s) {
                                return s.first == entry.second->state;
                            });
    });
    std::sort(data.branches.begin(), data.branches.end(), byGuid);
    std::sort(data.tasks.begin(), data.tasks.end(), byGuid);
    return data;
}

const quest::QuestForm* questOf(const data::EditSession& session,
                                const core::Guid& id) {
    const auto* type = id.isValid() ? session.viewType(id) : nullptr;
    if (!type || type->id != quest::QuestForm::staticTypeInfo().id) {
        return nullptr;
    }
    return static_cast<const quest::QuestForm*>(session.view(id));
}

} // namespace

void QuestGraphPanel::drawCanvas(const core::Guid& questId) {
    const quest::QuestForm* questForm = questOf(session, questId);
    if (!questForm) {
        ImGui::TextDisabled("(select a quest in the Browser)");
        return;
    }

    const QuestData data = collectQuest(session, questId);
    const auto isState = [&](const core::Guid& id) {
        return std::any_of(data.states.begin(), data.states.end(),
                           [&](const auto& s) { return s.first == id; });
    };
    const auto tasksOf = [&](const core::Guid& branchId) {
        u32 count = 0;
        for (const auto& [id, task] : data.tasks) {
            if (task->branch == branchId) {
                ++count;
            }
        }
        return count;
    };

    vector<str> warnings;
    if (!questForm->startState.isValid()) {
        warnings.push_back("no start state");
    } else if (!isState(questForm->startState)) {
        warnings.push_back("startState is not a state of this quest");
    }
    for (const auto& [id, branch] : data.branches) {
        if (!branch->destination.isValid() || !isState(branch->destination)) {
            warnings.push_back("branch '" + branch->editorId +
                               "' has no destination");
        }
        if (tasksOf(id) == 0) {
            warnings.push_back("branch '" + branch->editorId +
                               "' has no task (never completes)");
        }
    }

    const bool autoLayoutRequested = ImGui::Button("Auto-layout");
    ImGui::SameLine();
    ImGui::TextDisabled("%zu states, %zu branches", data.states.size(),
                        data.branches.size());
    for (const str& warning : warnings) {
        ImGui::TextColored(kWarnColor, "(!) %s", warning.c_str());
    }

    canvas.canDeleteNode = [&](const core::Guid& id) {
        return session.isCreated(id);
    };
    canvas.canDeleteLink = canvas.canDeleteNode;
    canvas.canLink = nullptr; // any state -> any state is legal

    canvas.begin("questgraph-canvas");

    if (canvasShown != questId || autoLayoutRequested) {
        vector<core::Guid> nodes;
        vector<std::pair<core::Guid, core::Guid>> edges;
        for (const auto& [id, state] : data.states) {
            nodes.push_back(id);
        }
        for (const auto& [id, branch] : data.branches) {
            if (branch->destination.isValid()) {
                edges.emplace_back(branch->state, branch->destination);
            }
        }
        vector<core::Guid> roots;
        if (questForm->startState.isValid()) {
            roots.push_back(questForm->startState);
        }
        const data::GraphLayoutResult layout =
            data::layoutGraph(nodes, edges, roots);
        for (const core::Guid& node : nodes) {
            if (autoLayoutRequested) {
                const auto it = layout.positions.find(node);
                if (it != layout.positions.end()) {
                    canvas.setNodePosition(node, it->second);
                    layouts.setPosition(questId, node, it->second);
                }
                continue;
            }
            if (const auto stored = layouts.positionOf(questId, node)) {
                canvas.setNodePosition(node, *stored);
            } else if (const auto it = layout.positions.find(node);
                       it != layout.positions.end()) {
                canvas.setNodePosition(node, it->second);
            }
        }
        if (autoLayoutRequested) {
            layouts.save();
        }
        canvasShown = questId;
    }
    if (pendingPlace.isValid()) {
        canvas.setNodePosition(pendingPlace, pendingPlacePos);
        layouts.setPosition(questId, pendingPlace, pendingPlacePos);
        layouts.save();
        pendingPlace = {};
    }

    for (const auto& [id, state] : data.states) {
        canvas.beginNode(id);
        canvas.inputPin(id);
        ImGui::SameLine();
        const str title =
            state->editorId.empty() ? id.toString() : state->editorId;
        ImGui::TextUnformatted(title.c_str());
        ImGui::SameLine();
        canvas.outputPin(id);
        if (questForm->startState == id) {
            ImGui::TextColored(kSuccessColor, "<- start");
        }
        if (state->kind == "Success") {
            ImGui::TextColored(kSuccessColor, "Success");
        } else if (state->kind == "Failure") {
            ImGui::TextColored(kFailureColor, "Failure");
        }
        // Incomplete branches (no destination = no link to draw) live on
        // their source node, selectable so the grid can fix them.
        for (const auto& [branchId, branch] : data.branches) {
            if (branch->state != id ||
                (branch->destination.isValid() &&
                 isState(branch->destination))) {
                continue;
            }
            const str label = "(!) branch: " +
                              std::to_string(tasksOf(branchId)) +
                              " tasks, no dest##b" + branchId.toString();
            if (ImGui::Selectable(label.c_str(), selected == branchId,
                                  ImGuiSelectableFlags_None,
                                  ImVec2(180.0f, 0.0f))) {
                selected = branchId;
            }
        }
        canvas.endNode();
    }

    for (const auto& [id, branch] : data.branches) {
        if (!branch->destination.isValid() || !isState(branch->destination)) {
            continue;
        }
        canvas.link(id, branch->state, branch->destination,
                    Vec4 { 0.9f, 0.9f, 0.9f, 0.8f },
                    selected == id ? 3.5f : 2.0f);
    }

    // Link labels: "n tasks" at the midpoint.
    ImDrawList* drawList = ImGui::GetWindowDrawList();
    for (const auto& [id, branch] : data.branches) {
        if (!branch->destination.isValid() || !isState(branch->destination)) {
            continue;
        }
        const str label = std::to_string(tasksOf(id)) + " tasks";
        const Vec2 a = canvas.nodeCenter(branch->state);
        const Vec2 b = canvas.nodeCenter(branch->destination);
        const Vec2 mid = (a + b) * 0.5f;
        drawList->AddText(ImVec2(mid.x, mid.y - 8.0f),
                          IM_COL32(200, 200, 160, 220), label.c_str());
    }

    NodeCanvas::Actions actions;
    canvas.end(actions);

    if (actions.linkCreated) {
        const core::Guid id = session.createForm(
            quest::QuestBranchForm::staticTypeInfo().id,
            questForm->editorId + "Branch" + std::to_string(++createCounter));
        session.setField(id, core::fnv1a("state"),
                         reflect::Value { actions.linkFrom });
        session.setField(id, core::fnv1a("destination"),
                         reflect::Value { actions.linkTo });
        selected = id;
    }
    for (const core::Guid& id : actions.deletedLinks) {
        session.removeCreated(id);
    }
    for (const core::Guid& id : actions.deletedNodes) {
        // A deleted state takes its session-created branches along; base
        // branches stay and surface as dangling warnings (§5).
        for (const auto& [branchId, branch] : data.branches) {
            if ((branch->state == id || branch->destination == id) &&
                session.isCreated(branchId)) {
                session.removeCreated(branchId);
            }
        }
        session.removeCreated(id);
    }
    for (const auto& [node, position] : actions.movedNodes) {
        layouts.setPosition(questId, node, position);
    }
    if (!actions.movedNodes.empty()) {
        layouts.save();
    }
    if (actions.clickedNode.isValid()) {
        selected = actions.clickedNode;
    }
    if (actions.clickedLink.isValid()) {
        selected = actions.clickedLink;
    }
    if (actions.backgroundContext) {
        contextPos = actions.backgroundContextPos;
        ImGui::OpenPopup("qg-background");
    }
    if (actions.contextNode.isValid()) {
        contextNode = actions.contextNode;
        ImGui::OpenPopup("qg-node");
    }

    if (ImGui::BeginPopup("qg-background")) {
        if (ImGui::MenuItem("+ State")) {
            const core::Guid id = session.createForm(
                quest::QuestStateForm::staticTypeInfo().id,
                questForm->editorId + "State" +
                    std::to_string(++createCounter));
            session.setField(id, core::fnv1a("quest"),
                             reflect::Value { questId });
            pendingPlace = id;
            pendingPlacePos = contextPos;
            selected = id;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopup("qg-node")) {
        if (ImGui::MenuItem("Set as start state")) {
            session.setField(questId, core::fnv1a("startState"),
                             reflect::Value { contextNode });
        }
        ImGui::EndPopup();
    }
}

void QuestGraphPanel::drawInspectorExtras(const core::Guid& target) {
    const auto* type = target.isValid() ? session.viewType(target) : nullptr;
    if (!type || type->id != quest::QuestBranchForm::staticTypeInfo().id) {
        return;
    }
    const auto* branch =
        static_cast<const quest::QuestBranchForm*>(session.view(target));
    ImGui::TextUnformatted("Branch tasks:");
    session.forEachVisible([&](const core::Guid& id, const data::Form& form,
                               const reflect::TypeInfo& formType) {
        if (formType.id != quest::QuestTaskForm::staticTypeInfo().id) {
            return;
        }
        const auto* task = static_cast<const quest::QuestTaskForm*>(&form);
        if (task->branch != target) {
            return;
        }
        const str label =
            (task->displayName.empty() ? task->editorId : task->displayName) +
            "  [" + task->event + "]##t" + id.toString();
        if (ImGui::Selectable(label.c_str(), false)) {
            selected = id;
        }
    });
    if (ImGui::SmallButton("+ Task")) {
        const core::Guid id = session.createForm(
            quest::QuestTaskForm::staticTypeInfo().id,
            branch->editorId + "Task" + std::to_string(++createCounter));
        session.setField(id, core::fnv1a("branch"),
                         reflect::Value { target });
        selected = id;
    }
}

} // namespace game
