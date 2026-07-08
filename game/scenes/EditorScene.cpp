#include "game/scenes/EditorScene.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>

#include <imgui.h>

#include "data/forms/FormQuery.hpp"
#include "data/plugins/Synthesis.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/reflect/Visit.hpp"
#include "game/AllForms.hpp"
#include "game/ui/PropertyGrid.hpp"
#include "gameplay/ai/ScheduleSystem.hpp"
#include "gameplay/condition/Condition.hpp"
#include "quest/Dialogue.hpp"
#include "quest/Quest.hpp"

namespace game {

namespace {

// Short display form of a reflected value (8.5: the conflict view shows
// what each plugin wrote). Distinct from PropertyGrid::valueToString, which
// must stay round-trippable by valueFromString — here quotes and parentheses
// are for human reading only. Exhaustive per kind (engine/reflect/Visit.hpp).
str valueRepr(const reflect::Value& value) {
    const auto num = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%g", v);
        return str { buf };
    };
    return reflect::visit(value, reflect::overloaded {
        [&](bool b)       -> str { return b ? "true" : "false"; },
        [&](i32 x)        -> str { return std::to_string(x); },
        [&](u32 x)        -> str { return std::to_string(x); },
        [&](f32 x)        -> str { return num(x); },
        [&](f64 x)        -> str { return num(x); },
        [&](const str& s) -> str { return "\"" + s + "\""; },
        [&](const Vec2& v) -> str { return "(" + num(v.x) + ", " + num(v.y) + ")"; },
        [&](const Vec3& v) -> str {
            return "(" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + ")";
        },
        [&](const Vec4& v) -> str {
            return "(" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + ", " +
                   num(v.w) + ")";
        },
        [&](const Quat& q) -> str {
            return "(" + num(q.x) + ", " + num(q.y) + ", " + num(q.z) + ", " +
                   num(q.w) + ")";
        },
        [&](const core::Guid& g) -> str { return g.toString(); },
    });
}

} // namespace

void EditorScene::onEnter() {
    // The WHOLE game database: the shared registration site (chantier 4 B1).
    game::registerAllFormTypes(types);

    types.forEachType(
        [&](const reflect::TypeInfo& type) { typeNames.push_back(type.name); });
    std::sort(typeNames.begin(), typeNames.end());

    // Same stack as the game: data/plugins.toml over the data/ root
    // (chantier 4 B1 — the editor edits exactly what the game runs).
    pluginDir = platform::executableDir() / "data";
    if (const auto loaded =
            data::loadPluginConfigFile(pluginDir / "plugins.toml")) {
        config = *loaded;
    } else {
        config = data::defaultConfigFromDirectory(pluginDir / "base");
        for (auto& entry : config.entries) {
            entry.file = "base/" + entry.file;
        }
    }
    vm = std::make_unique<script::Vm>();
    reload();
}

void EditorScene::reload() {
    stack = data::loadPluginStack(pluginDir, config, types);
    db = std::make_unique<data::FormDatabase>();
    report = data::resolve(data::pointersOf(stack), types, *db);
    session = std::make_unique<data::EditSession>(*db, types);
    console = std::make_unique<ConsolePanel>(*session, *db, types, *vm);
    selected = {};
    synthPicks.assign(report.conflicts.size(), -1); // -1 = keep load order
    status = "loaded " + std::to_string(stack.plugins.size()) + " plugins, " +
             std::to_string(db->count()) + " forms, " +
             std::to_string(report.conflicts.size()) + " field conflicts";
}

void EditorScene::saveConfig() const {
    std::ofstream out { pluginDir / "plugins.toml", std::ios::binary };
    out << data::writePluginConfigToml(config);
}

void EditorScene::drawUi() {
    drawGameDb();
    drawPlugins();
    drawQuests();
    drawDialogues();
    drawSchedules();
    console->draw();
}

// 8.3 — the dialogue editor: DialogueNodeForm records (parent-linked,
// alternating NPC lines and Player choices) as an indented tree with
// inline creation, sibling reorder and per-node conditions. Same
// session/grid/export as everything else.
void EditorScene::drawDialogues() {
    ImGui::Begin("Dialogues");

    ImGui::BeginChild("dlglist", ImVec2(220.0f, 0.0f),
                      ImGuiChildFlags_ResizeX);
    if (ImGui::Button("+ Dialogue")) {
        dialogueSelected = session->createForm(
            quest::DialogueForm::staticTypeInfo().id, "NewDialogue");
        selected = dialogueSelected;
    }
    session->forEachVisible([&](const core::Guid& id, const data::Form& form,
                                const reflect::TypeInfo& type) {
        if (type.id != quest::DialogueForm::staticTypeInfo().id) {
            return;
        }
        const str label =
            (form.editorId.empty() ? id.toString() : form.editorId) +
            (session->isDirty(id) ? " *" : "") + "##d" + id.toString();
        if (ImGui::Selectable(label.c_str(), dialogueSelected == id)) {
            dialogueSelected = id;
            selected = id;
        }
    });
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("dlgtree");
    const auto* dialogue = static_cast<const quest::DialogueForm*>(
        session->view(dialogueSelected));
    if (!dialogue) {
        ImGui::TextDisabled("(select a dialogue)");
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    // Visible nodes and conditions, once per frame.
    vector<std::pair<core::Guid, const quest::DialogueNodeForm*>> nodes;
    vector<std::pair<core::Guid, const gameplay::ConditionForm*>> conditions;
    session->forEachVisible([&](const core::Guid& id, const data::Form& form,
                                const reflect::TypeInfo& type) {
        if (type.id == quest::DialogueNodeForm::staticTypeInfo().id) {
            nodes.emplace_back(
                id, static_cast<const quest::DialogueNodeForm*>(&form));
        } else if (type.id ==
                   gameplay::ConditionForm::staticTypeInfo().id) {
            conditions.emplace_back(
                id, static_cast<const gameplay::ConditionForm*>(&form));
        }
    });
    const auto childrenOf = [&](const core::Guid& parent) {
        vector<std::pair<core::Guid, const quest::DialogueNodeForm*>> out;
        for (const auto& entry : nodes) {
            if (entry.second->parent == parent) {
                out.push_back(entry);
            }
        }
        // Stable: ties on `order` (hand-authored TOML) must not flicker
        // between frames.
        std::stable_sort(out.begin(), out.end(),
                         [](const auto& a, const auto& b) {
                             return a.second->order < b.second->order;
                         });
        return out;
    };

    ImGui::Text("%s", dialogue->displayName.empty()
                          ? dialogue->editorId.c_str()
                          : dialogue->displayName.c_str());
    if (!dialogue->rootNode.isValid() ||
        !session->view(dialogue->rootNode)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                           "(!) no root node");
        if (ImGui::Button("+ Root line")) {
            const core::Guid id = session->createForm(
                quest::DialogueNodeForm::staticTypeInfo().id,
                dialogue->editorId + "Root");
            session->setField(dialogueSelected, core::fnv1a("rootNode"),
                              reflect::Value { id });
            selected = id;
        }
    }
    ImGui::Separator();

    // Recursive node drawer. Depth-capped: `parent` links form a tree by
    // convention, but a bad edit could cycle — never hang the editor.
    const std::function<void(const core::Guid&, u32)> drawNode =
        [&](const core::Guid& nodeId, u32 depth) {
            const auto* node = static_cast<const quest::DialogueNodeForm*>(
                session->view(nodeId));
            if (!node || depth > 24) {
                return;
            }
            const bool isPlayer = node->speaker == "Player";
            str label = isPlayer ? "> " : "";
            label += node->speaker.empty() ? "(npc)" : node->speaker;
            label += ": ";
            label += node->text.size() > 48 ? node->text.substr(0, 48) + "..."
                                            : node->text;
            if (!node->event.empty()) {
                label += "  [" + node->event + "]";
            }
            u32 conditionCount = 0;
            for (const auto& [condId, cond] : conditions) {
                if (cond->parent == nodeId) {
                    ++conditionCount;
                }
            }
            if (conditionCount > 0) {
                label += "  (" + std::to_string(conditionCount) + " cond)";
            }
            const bool open = ImGui::TreeNodeEx(
                (label + "##n" + nodeId.toString()).c_str(),
                ImGuiTreeNodeFlags_OpenOnArrow |
                    ImGuiTreeNodeFlags_DefaultOpen |
                    (selected == nodeId ? ImGuiTreeNodeFlags_Selected
                                        : ImGuiTreeNodeFlags_None));
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                selected = nodeId;
            }
            if (!open) {
                return;
            }
            const auto siblings = childrenOf(nodeId);
            if (ImGui::SmallButton(
                    ("+ reply##" + nodeId.toString()).c_str())) {
                const core::Guid id = session->createForm(
                    quest::DialogueNodeForm::staticTypeInfo().id,
                    node->editorId + "Reply");
                session->setField(id, core::fnv1a("parent"),
                                  reflect::Value { nodeId });
                // Alternate speakers by default: an NPC line gets Player
                // choices, a Player choice gets the NPC's answer.
                session->setField(
                    id, core::fnv1a("speaker"),
                    reflect::Value { isPlayer ? str {} : str { "Player" } });
                const i32 nextOrder =
                    siblings.empty() ? 0 : siblings.back().second->order + 1;
                session->setField(id, core::fnv1a("order"),
                                  reflect::Value { nextOrder });
                selected = id;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(
                    ("+ condition##" + nodeId.toString()).c_str())) {
                const core::Guid id = session->createForm(
                    gameplay::ConditionForm::staticTypeInfo().id,
                    node->editorId + "Cond");
                session->setField(id, core::fnv1a("parent"),
                                  reflect::Value { nodeId });
                selected = id;
            }
            // Conditions as selectable leaves under their node.
            for (const auto& [condId, cond] : conditions) {
                if (cond->parent != nodeId) {
                    continue;
                }
                str condLabel = "if " + str { cond->negate ? "not " : "" } +
                                cond->kind;
                if (!cond->tag.empty()) {
                    condLabel += " " + cond->tag;
                }
                if (ImGui::Selectable(
                        (condLabel + "##c" + condId.toString()).c_str(),
                        selected == condId)) {
                    selected = condId;
                }
            }
            // Children, in order, with reorder arrows (swap the `order`
            // fields — a plain field edit, undo works for free).
            const auto children = childrenOf(nodeId);
            for (size_t i = 0; i < children.size(); ++i) {
                if (children.size() > 1) {
                    ImGui::PushID(children[i].first.toString().c_str());
                    ImGui::BeginDisabled(i == 0);
                    if (ImGui::SmallButton("^")) {
                        // Capture BOTH before writing: the first setField
                        // mutates the draft the second one would read.
                        const i32 mine = children[i].second->order;
                        const i32 theirs = children[i - 1].second->order;
                        session->setField(children[i].first,
                                          core::fnv1a("order"),
                                          reflect::Value { theirs });
                        session->setField(children[i - 1].first,
                                          core::fnv1a("order"),
                                          reflect::Value { mine });
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::PopID();
                }
                drawNode(children[i].first, depth + 1);
            }
            ImGui::TreePop();
        };
    if (dialogue->rootNode.isValid()) {
        drawNode(dialogue->rootNode, 0);
    }

    ImGui::Separator();
    if (selected.isValid()) {
        drawPropertyGrid(*session, selected);
    }
    ImGui::EndChild();
    ImGui::End();
}

// 8.2 — the quest editor: the decomposed records (Quest -> States ->
// Branches -> Tasks, linked by guid) shown as ONE tree, created with
// their parent pre-filled, edited through the same PropertyGrid and
// exported by the same session as everything else (§5: the editor is a
// plugin author, nothing more).
void EditorScene::drawQuests() {
    ImGui::Begin("Quests");

    // Left: every visible QuestForm (base + session-created).
    ImGui::BeginChild("questlist", ImVec2(220.0f, 0.0f),
                      ImGuiChildFlags_ResizeX);
    if (ImGui::Button("+ Quest")) {
        questSelected = session->createForm(
            quest::QuestForm::staticTypeInfo().id, "NewQuest");
        selected = questSelected;
    }
    session->forEachVisible([&](const core::Guid& id, const data::Form& form,
                                const reflect::TypeInfo& type) {
        if (type.id != quest::QuestForm::staticTypeInfo().id) {
            return;
        }
        const str label =
            (form.editorId.empty() ? id.toString() : form.editorId) +
            (session->isDirty(id) ? " *" : "") + "##q" + id.toString();
        if (ImGui::Selectable(label.c_str(), questSelected == id)) {
            questSelected = id;
            selected = id;
        }
    });
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("questtree");
    const auto* questForm = static_cast<const quest::QuestForm*>(
        session->view(questSelected));
    if (!questForm) {
        ImGui::TextDisabled("(select a quest)");
        ImGui::EndChild();
        ImGui::End();
        return;
    }

    // Collect the visible children at each level (guid-linked, §C.1).
    vector<std::pair<core::Guid, const quest::QuestStateForm*>> states;
    vector<std::pair<core::Guid, const quest::QuestBranchForm*>> branches;
    vector<std::pair<core::Guid, const quest::QuestTaskForm*>> tasks;
    session->forEachVisible([&](const core::Guid& id, const data::Form& form,
                                const reflect::TypeInfo& type) {
        if (type.id == quest::QuestStateForm::staticTypeInfo().id) {
            states.emplace_back(
                id, static_cast<const quest::QuestStateForm*>(&form));
        } else if (type.id == quest::QuestBranchForm::staticTypeInfo().id) {
            branches.emplace_back(
                id, static_cast<const quest::QuestBranchForm*>(&form));
        } else if (type.id == quest::QuestTaskForm::staticTypeInfo().id) {
            tasks.emplace_back(
                id, static_cast<const quest::QuestTaskForm*>(&form));
        }
    });
    const auto nameOf = [&](const core::Guid& id) -> str {
        const data::Form* form = session->view(id);
        if (!form) {
            return "(dangling)";
        }
        return form->editorId.empty() ? id.toString() : form->editorId;
    };

    // Header: the quest itself + its startState health.
    ImGui::Text("%s", questForm->displayName.empty()
                          ? questForm->editorId.c_str()
                          : questForm->displayName.c_str());
    if (!questForm->startState.isValid()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                           "(!) no startState");
    } else if (!session->view(questForm->startState)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                           "(!) startState dangles");
    }
    if (ImGui::Button("+ State")) {
        const core::Guid id = session->createForm(
            quest::QuestStateForm::staticTypeInfo().id,
            questForm->editorId + "State");
        session->setField(id, core::fnv1a("quest"),
                          reflect::Value { questSelected });
        selected = id;
    }
    ImGui::Separator();

    for (const auto& [stateId, state] : states) {
        if (state->quest != questSelected) {
            continue;
        }
        str label = nameOf(stateId) + "  [" + state->kind + "]";
        if (stateId == questForm->startState) {
            label += "  <- start";
        }
        const bool stateOpen = ImGui::TreeNodeEx(
            (label + "##s" + stateId.toString()).c_str(),
            ImGuiTreeNodeFlags_OpenOnArrow |
                (selected == stateId ? ImGuiTreeNodeFlags_Selected
                                     : ImGuiTreeNodeFlags_None) |
                ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selected = stateId;
        }
        if (!stateOpen) {
            continue;
        }
        if (ImGui::Button(("+ Branch##" + stateId.toString()).c_str())) {
            const core::Guid id = session->createForm(
                quest::QuestBranchForm::staticTypeInfo().id,
                nameOf(stateId) + "Branch");
            session->setField(id, core::fnv1a("state"),
                              reflect::Value { stateId });
            selected = id;
        }
        for (const auto& [branchId, branch] : branches) {
            if (branch->state != stateId) {
                continue;
            }
            str branchLabel = nameOf(branchId) + "  -> ";
            branchLabel += branch->destination.isValid()
                               ? nameOf(branch->destination)
                               : "(!) no destination";
            const bool branchOpen = ImGui::TreeNodeEx(
                (branchLabel + "##b" + branchId.toString()).c_str(),
                ImGuiTreeNodeFlags_OpenOnArrow |
                    (selected == branchId ? ImGuiTreeNodeFlags_Selected
                                          : ImGuiTreeNodeFlags_None) |
                    ImGuiTreeNodeFlags_DefaultOpen);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                selected = branchId;
            }
            if (!branchOpen) {
                continue;
            }
            if (ImGui::Button(
                    ("+ Task##" + branchId.toString()).c_str())) {
                const core::Guid id = session->createForm(
                    quest::QuestTaskForm::staticTypeInfo().id,
                    nameOf(branchId) + "Task");
                session->setField(id, core::fnv1a("branch"),
                                  reflect::Value { branchId });
                selected = id;
            }
            for (const auto& [taskId, task] : tasks) {
                if (task->branch != branchId) {
                    continue;
                }
                str taskLabel = nameOf(taskId);
                taskLabel += task->event.empty() ? "  (!) no event"
                                                 : "  on " + task->event;
                if (task->required > 1) {
                    taskLabel += " x" + std::to_string(task->required);
                }
                if (ImGui::Selectable(
                        (taskLabel + "##t" + taskId.toString()).c_str(),
                        selected == taskId)) {
                    selected = taskId;
                }
            }
            ImGui::TreePop();
        }
        ImGui::TreePop();
    }

    // The clicked node edits in place — same grid, same session.
    ImGui::Separator();
    if (selected.isValid()) {
        drawPropertyGrid(*session, selected);
    }
    ImGui::EndChild();
    ImGui::End();
}

// 8.4 — the schedule timeline: one 24 h strip per ScheduleForm, one lane
// per ScheduleEntryForm (no overlap ambiguity — the last-in-load-order
// rule is what the scrubbed-hour eval line shows). Bars drag by their
// edges (0.5 h snap, committed as ONE field edit on release so undo
// stays sane); midnight-wrapping entries render as two segments. The H7
// debug eval ("who does what at this hour, and why") stays at the top
// of each strip.
void EditorScene::drawSchedules() {
    ImGui::Begin("Schedules");
    ImGui::SliderFloat("Hour", &debugHour, 0.0f, 24.0f, "%.1f");
    ImGui::SameLine();
    if (ImGui::Button("+ Schedule")) {
        selected = session->createForm(
            gameplay::ScheduleForm::staticTypeInfo().id, "NewSchedule");
    }

    vector<std::pair<core::Guid, const gameplay::ScheduleForm*>> schedules;
    vector<std::pair<core::Guid, const gameplay::ScheduleEntryForm*>> entries;
    session->forEachVisible([&](const core::Guid& id, const data::Form& form,
                                const reflect::TypeInfo& type) {
        if (type.id == gameplay::ScheduleForm::staticTypeInfo().id) {
            schedules.emplace_back(
                id, static_cast<const gameplay::ScheduleForm*>(&form));
        } else if (type.id ==
                   gameplay::ScheduleEntryForm::staticTypeInfo().id) {
            entries.emplace_back(
                id, static_cast<const gameplay::ScheduleEntryForm*>(&form));
        }
    });
    if (schedules.empty()) {
        ImGui::TextDisabled("No ScheduleForm yet — + Schedule above.");
    }
    const auto nameOf = [&](const core::Guid& id) -> str {
        const data::Form* form = session->view(id);
        if (!form) {
            return "(dangling)";
        }
        return form->editorId.empty() ? id.toString() : form->editorId;
    };

    for (const auto& [scheduleId, schedule] : schedules) {
        const str header = nameOf(scheduleId) +
                           (session->isDirty(scheduleId) ? " *" : "") +
                           "##sch" + scheduleId.toString();
        if (!ImGui::CollapsingHeader(header.c_str(),
                                     ImGuiTreeNodeFlags_DefaultOpen)) {
            continue;
        }
        // The H7 eval line — resolved base only (a schedule created this
        // session previews after the next reload).
        if (const auto intent =
                gameplay::evaluateSchedule(*db, scheduleId, debugHour)) {
            const data::Form* location =
                intent->location.isValid() ? db->find(intent->location)
                                           : nullptr;
            ImGui::TextDisabled("at %.1f h: %s%s%s", debugHour,
                                intent->reason.c_str(),
                                location ? " @ " : "",
                                location ? location->editorId.c_str() : "");
        } else {
            ImGui::TextDisabled("at %.1f h: (no entry)", debugHour);
        }
        if (ImGui::SmallButton(
                ("+ entry##" + scheduleId.toString()).c_str())) {
            const core::Guid id = session->createForm(
                gameplay::ScheduleEntryForm::staticTypeInfo().id,
                nameOf(scheduleId) + "Entry");
            session->setField(id, core::fnv1a("parent"),
                              reflect::Value { scheduleId });
            selected = id;
        }

        // The strip: hour grid + one lane per entry.
        constexpr f32 kLane = 20.0f;
        constexpr f32 kGridTop = 16.0f;
        vector<std::pair<core::Guid, const gameplay::ScheduleEntryForm*>>
            mine;
        for (const auto& entry : entries) {
            if (entry.second->parent == scheduleId) {
                mine.push_back(entry);
            }
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const f32 width = glm::max(ImGui::GetContentRegionAvail().x, 240.0f);
        const f32 stripHeight =
            kGridTop +
            kLane * static_cast<f32>(std::max<size_t>(mine.size(), 1));
        const auto xAt = [&](f32 hour) {
            return origin.x + hour / 24.0f * width;
        };
        for (int h = 0; h <= 24; h += 6) {
            const f32 x = xAt(static_cast<f32>(h));
            draw->AddLine(ImVec2(x, origin.y + kGridTop - 4.0f),
                          ImVec2(x, origin.y + stripHeight),
                          IM_COL32(120, 120, 120, 120));
            draw->AddText(ImVec2(x + 2.0f, origin.y),
                          IM_COL32(160, 160, 160, 255),
                          std::to_string(h).c_str());
        }
        for (size_t lane = 0; lane < mine.size(); ++lane) {
            const auto& [entryId, entry] = mine[lane];
            const f32 top = origin.y + kGridTop + kLane * static_cast<f32>(lane);
            // Live preview of an edge drag; the field commits on release.
            f32 start = entry->startHour;
            f32 end = entry->endHour;
            if (schedDragEntry == entryId) {
                (schedDragEdge == 0 ? start : end) = schedDragHour;
            }
            const u32 hash = core::fnv1a(entryId.toString());
            const ImColor color = ImColor::HSV(
                static_cast<f32>(hash % 360u) / 360.0f, 0.55f,
                selected == entryId ? 0.95f : 0.65f);
            const auto segment = [&](f32 a, f32 b) {
                draw->AddRectFilled(ImVec2(xAt(a), top + 2.0f),
                                    ImVec2(xAt(b), top + kLane - 2.0f),
                                    color, 3.0f);
            };
            if (start <= end) {
                segment(start, end);
            } else { // wraps midnight
                segment(start, 24.0f);
                segment(0.0f, end);
            }
            // The clickable body follows the FIRST segment (start->24 when
            // the entry wraps midnight — never the empty middle).
            const f32 bodyStart = start;
            const f32 bodyEnd = start <= end ? end : 24.0f;
            draw->AddText(ImVec2(xAt(bodyStart) + 4.0f, top + 2.0f),
                          IM_COL32(255, 255, 255, 220),
                          nameOf(entry->package).c_str());

            // Bar body: click selects. Edge handles: drag retunes, snapped
            // to 0.5 h, ONE undoable edit on release.
            const auto handle = [&](f32 hour, int edge, u32 fieldId) {
                ImGui::SetCursorScreenPos(
                    ImVec2(xAt(hour) - 4.0f, top + 2.0f));
                ImGui::InvisibleButton(
                    ("##e" + entryId.toString() + std::to_string(edge))
                        .c_str(),
                    ImVec2(8.0f, kLane - 4.0f));
                if (ImGui::IsItemActivated()) {
                    schedDragEntry = entryId;
                    schedDragEdge = edge;
                    schedDragHour = hour;
                }
                if (ImGui::IsItemActive() && schedDragEntry == entryId) {
                    const f32 raw = (ImGui::GetMousePos().x - origin.x) /
                                    width * 24.0f;
                    schedDragHour =
                        glm::clamp(std::round(raw * 2.0f) / 2.0f, 0.0f,
                                   24.0f);
                }
                if (ImGui::IsItemDeactivated() &&
                    schedDragEntry == entryId) {
                    session->setField(entryId, fieldId,
                                      reflect::Value { schedDragHour });
                    selected = entryId;
                    schedDragEntry = {};
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                }
            };
            ImGui::SetCursorScreenPos(
                ImVec2(xAt(bodyStart) + 5.0f, top + 2.0f));
            const f32 bodyWidth =
                glm::max(xAt(bodyEnd) - xAt(bodyStart) - 10.0f, 4.0f);
            ImGui::InvisibleButton(
                ("##b" + entryId.toString()).c_str(),
                ImVec2(bodyWidth, kLane - 4.0f));
            if (ImGui::IsItemClicked()) {
                selected = entryId;
            }
            handle(start, 0, core::fnv1a("startHour"));
            handle(end, 1, core::fnv1a("endHour"));
        }
        // Scrubbed hour marker across the strip.
        draw->AddLine(ImVec2(xAt(debugHour), origin.y + kGridTop - 4.0f),
                      ImVec2(xAt(debugHour), origin.y + stripHeight),
                      IM_COL32(255, 220, 80, 200), 2.0f);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + stripHeight));
        ImGui::Dummy(ImVec2(width, 4.0f));
    }

    // Edit the clicked entry/schedule in place.
    ImGui::Separator();
    if (selected.isValid()) {
        drawPropertyGrid(*session, selected);
    }
    ImGui::End();
}

void EditorScene::drawGameDb() {
    ImGui::Begin("Game DB");
    ImGui::TextDisabled("%s", status.c_str());

    // Type filter + search.
    const str filterName = typeFilter > 0 ? typeNames[typeFilter - 1] : "";
    if (ImGui::BeginCombo("Type",
                          typeFilter == 0 ? "All" : filterName.c_str())) {
        if (ImGui::Selectable("All", typeFilter == 0)) {
            typeFilter = 0;
        }
        for (int i = 0; i < static_cast<int>(typeNames.size()); ++i) {
            if (ImGui::Selectable(typeNames[i].c_str(),
                                  typeFilter == i + 1)) {
                typeFilter = i + 1;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::InputTextWithHint("##search", "search editorId...", search,
                             sizeof(search));

    // Toolbar: create (needs a concrete type), undo/redo, export.
    if (typeFilter > 0) {
        if (ImGui::Button("New")) {
            const reflect::TypeInfo* type = types.findType(filterName);
            if (type) {
                selected =
                    session->createForm(type->id, "New" + filterName);
            }
        }
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!session->canUndo());
    if (ImGui::Button("Undo")) { session->undo(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!session->canRedo());
    if (ImGui::Button("Redo")) { session->redo(); }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::Text("pending edits: %u", session->dirtyCount());

    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##exportname", exportName, sizeof(exportName));
    ImGui::SameLine();
    ImGui::BeginDisabled(session->dirtyCount() == 0);
    if (ImGui::Button("Export plugin")) {
        const str name = exportName;
        const data::Plugin plugin =
            session->exportPlugin(core::Guid::generate(), name);
        const str toml = data::writePluginToml(plugin, types);
        const str file = name + ".toml";
        std::ofstream out { pluginDir / file, std::ios::binary };
        out << toml;
        const bool known = std::any_of(
            config.entries.begin(), config.entries.end(),
            [&](const data::PluginConfigEntry& e) { return e.file == file; });
        if (!known) {
            config.entries.push_back({ file, true });
            saveConfig();
        }
        status = "exported " + std::to_string(plugin.records.size()) +
                 " records to " + file + " (applies on reload)";
    }
    ImGui::EndDisabled();
    ImGui::Separator();

    // Left: form list. Right: the reflection property grid.
    ImGui::BeginChild("list", ImVec2(260.0f, 0.0f),
                      ImGuiChildFlags_ResizeX);
    for (u32 i = 1; i <= db->count(); ++i) {
        const data::FormHandle handle { i };
        const data::Form* form = db->get(handle);
        const reflect::TypeInfo* type = db->typeOf(handle);
        if (!form || !type) {
            continue;
        }
        if (typeFilter > 0 && type->name != filterName) {
            continue;
        }
        if (search[0] != '\0' && form->editorId.find(search) == str::npos) {
            continue;
        }
        const str label =
            (form->editorId.empty() ? form->id.toString() : form->editorId) +
            (session->isDirty(form->id) ? " *" : "") +
            "##" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), selected == form->id)) {
            selected = form->id;
        }
    }
    // Session-created forms are not in the database yet: list drafts too.
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("grid");
    if (selected.isValid()) {
        // 8.1: clone the selected form into the session (children are NOT
        // copied — recursive clones are the quest editor's job).
        if (const data::Form* form = session->view(selected)) {
            if (ImGui::Button("Duplicate")) {
                const str baseName =
                    form->editorId.empty() ? str { "Form" } : form->editorId;
                const core::Guid copy =
                    session->duplicateForm(selected, baseName + "Copy");
                if (copy.isValid()) {
                    selected = copy;
                }
            }
        }
        drawPropertyGrid(*session, selected);
        // 8.1: reverse lookup — who points at this form. Resolved base
        // only (session drafts are not scanned, v1).
        if (ImGui::CollapsingHeader("Used by")) {
            const auto hits = data::referencesTo(*db, selected);
            if (hits.empty()) {
                ImGui::TextDisabled("(no referencers)");
            }
            for (size_t i = 0; i < hits.size(); ++i) {
                const data::FormReferenceHit& hit = hits[i];
                const data::Form* from = db->find(hit.from);
                const str label =
                    (from && !from->editorId.empty() ? from->editorId
                                                     : hit.from.toString()) +
                    "  (" + hit.typeName + "." + hit.fieldName + ")##ub" +
                    std::to_string(i);
                if (ImGui::Selectable(label.c_str())) {
                    selected = hit.from;
                }
            }
        }
    } else {
        ImGui::TextDisabled("(select a form)");
    }
    ImGui::EndChild();
    ImGui::End();
}

void EditorScene::drawPlugins() {
    ImGui::Begin("Plugins");
    ImGui::TextUnformatted("Load order (top loads first, last writer wins):");
    for (int i = 0; i < static_cast<int>(config.entries.size()); ++i) {
        data::PluginConfigEntry& entry = config.entries[i];
        ImGui::PushID(i);
        ImGui::Checkbox("##on", &entry.enabled);
        ImGui::SameLine();
        if (ImGui::ArrowButton("up", ImGuiDir_Up) && i > 0) {
            std::swap(config.entries[i], config.entries[i - 1]);
        }
        ImGui::SameLine();
        if (ImGui::ArrowButton("down", ImGuiDir_Down) &&
            i + 1 < static_cast<int>(config.entries.size())) {
            std::swap(config.entries[i], config.entries[i + 1]);
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(entry.file.c_str());
        ImGui::PopID();
    }
    if (ImGui::Button("Save order")) {
        saveConfig();
        status = "plugins.toml saved";
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload data")) {
        reload(); // discards pending edits — the status line says so
    }
    for (const str& error : stack.errors) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s",
                           error.c_str());
    }

    // Chantier 4 B7: declared dependencies (Plugin::dependencies guids) —
    // ok / loads-after / missing, per loaded plugin.
    ImGui::SeparatorText("Dependencies");
    bool anyDependency = false;
    for (size_t i = 0; i < stack.plugins.size(); ++i) {
        const data::Plugin& plugin = stack.plugins[i];
        for (const core::Guid& dependency : plugin.dependencies) {
            anyDependency = true;
            size_t found = stack.plugins.size();
            for (size_t j = 0; j < stack.plugins.size(); ++j) {
                if (stack.plugins[j].id == dependency) {
                    found = j;
                    break;
                }
            }
            if (found == stack.plugins.size()) {
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f),
                                   "%s requires %s — MISSING",
                                   plugin.name.c_str(),
                                   dependency.toString().c_str());
            } else if (found > i) {
                ImGui::TextColored(
                    ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                    "%s requires %s — loads AFTER it (reorder)",
                    plugin.name.c_str(),
                    stack.plugins[found].name.c_str());
            } else {
                ImGui::Text("%s requires %s — ok", plugin.name.c_str(),
                            stack.plugins[found].name.c_str());
            }
        }
    }
    if (!anyDependency) {
        ImGui::TextDisabled("none declared");
    }

    // 8.5 — the synthesis view (§5.1): each conflicted field shows every
    // writer's VALUE; picking one and generating emits an ORDINARY plugin
    // loaded last. No new mechanism — one more layer.
    ImGui::SeparatorText("Field conflicts (last writer wins)");
    if (report.conflicts.empty()) {
        ImGui::TextDisabled("none");
    }
    for (size_t ci = 0;
         ci < report.conflicts.size() && ci < synthPicks.size(); ++ci) {
        const data::FieldConflict& conflict = report.conflicts[ci];
        const data::Form* form = db->find(conflict.formId);
        ImGui::Text("%s  %s.%s",
                    form && !form->editorId.empty()
                        ? form->editorId.c_str()
                        : conflict.formId.toString().c_str(),
                    conflict.typeName.c_str(), conflict.fieldName.c_str());
        ImGui::Indent();
        int& pick = synthPicks[ci];
        ImGui::RadioButton(
            ("keep load order##k" + std::to_string(ci)).c_str(), &pick, -1);
        for (int w = 0; w < static_cast<int>(conflict.writers.size()); ++w) {
            const data::FieldWrite& write = conflict.writers[w];
            const str label = write.plugin + " = " + valueRepr(write.value) +
                              "##w" + std::to_string(ci) + "_" +
                              std::to_string(w);
            ImGui::RadioButton(label.c_str(), &pick, w);
        }
        ImGui::Unindent();
    }
    bool anyPick = false;
    for (const int pick : synthPicks) {
        anyPick = anyPick || pick >= 0;
    }
    ImGui::BeginDisabled(!anyPick);
    if (ImGui::Button("Generate synthesis patch")) {
        vector<data::SynthesisChoice> choices;
        vector<str> arbitratedNames;
        for (size_t ci = 0;
             ci < report.conflicts.size() && ci < synthPicks.size(); ++ci) {
            const int pick = synthPicks[ci];
            if (pick < 0) {
                continue;
            }
            const data::FieldConflict& conflict = report.conflicts[ci];
            const data::FieldWrite& write =
                conflict.writers[static_cast<size_t>(pick)];
            choices.push_back({ conflict.formId, conflict.typeId,
                                conflict.fieldId, conflict.fieldName,
                                write.value, write.plugin });
            if (std::find(arbitratedNames.begin(), arbitratedNames.end(),
                          write.plugin) == arbitratedNames.end()) {
                arbitratedNames.push_back(write.plugin);
            }
        }
        vector<core::Guid> dependencies;
        for (const data::Plugin& plugin : stack.plugins) {
            if (std::find(arbitratedNames.begin(), arbitratedNames.end(),
                          plugin.name) != arbitratedNames.end()) {
                dependencies.push_back(plugin.id);
            }
        }
        const data::Plugin patch = data::makeSynthesisPatch(
            core::Guid::generate(), "synthesis", choices, dependencies);
        const str toml =
            data::writeSynthesisToml(patch, types, choices, db.get());
        std::ofstream out { pluginDir / "synthesis.toml",
                            std::ios::binary };
        out << toml;
        const bool known = std::any_of(
            config.entries.begin(), config.entries.end(),
            [](const data::PluginConfigEntry& e) {
                return e.file == "synthesis.toml";
            });
        if (!known) {
            config.entries.push_back({ "synthesis.toml", true });
            saveConfig();
        }
        status = "synthesis.toml: " + std::to_string(choices.size()) +
                 " arbitrated field(s), loads last (applies on reload)";
    }
    ImGui::EndDisabled();
    ImGui::End();
}

} // namespace game
