#include "game/scenes/EditorScene.hpp"

#include <algorithm>
#include <fstream>

#include <imgui.h>

#include "data/forms/FormQuery.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/platform/Paths.hpp"
#include "game/AllForms.hpp"
#include "game/ui/PropertyGrid.hpp"
#include "gameplay/ai/ScheduleSystem.hpp"
#include "quest/Quest.hpp"

namespace game {

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
    drawSchedules();
    console->draw();
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

void EditorScene::drawSchedules() {
    ImGui::Begin("Schedules (debug)");
    ImGui::TextUnformatted(
        "The P0 schedule tool: what every schedule resolves to at a "
        "given hour, and why.");
    ImGui::SliderFloat("Hour", &debugHour, 0.0f, 24.0f, "%.1f");
    bool any = false;
    data::forEach<gameplay::ScheduleForm>(
        *db, [&](const gameplay::ScheduleForm& schedule) {
            any = true;
            const auto intent =
                gameplay::evaluateSchedule(*db, schedule.id, debugHour);
            if (intent) {
                const data::Form* location =
                    intent->location.isValid() ? db->find(intent->location)
                                               : nullptr;
                ImGui::BulletText("%s: %s%s%s", schedule.editorId.c_str(),
                                  intent->reason.c_str(),
                                  location ? " @ " : "",
                                  location ? location->editorId.c_str()
                                           : "");
            } else {
                ImGui::BulletText("%s: (no entry at this hour)",
                                  schedule.editorId.c_str());
            }
        });
    if (!any) {
        ImGui::TextDisabled(
            "No ScheduleForm in the database — create one in the Game DB "
            "(type ScheduleForm + ScheduleEntryForm children).");
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

    ImGui::SeparatorText("Field conflicts (last writer wins)");
    if (report.conflicts.empty()) {
        ImGui::TextDisabled("none");
    }
    for (const data::FieldConflict& conflict : report.conflicts) {
        str writers;
        for (const str& writer : conflict.writers) {
            writers += (writers.empty() ? "" : " -> ") + writer;
        }
        ImGui::Text("%s.%s: %s", conflict.typeName.c_str(),
                    conflict.fieldName.c_str(), writers.c_str());
    }
    ImGui::End();
}

} // namespace game
