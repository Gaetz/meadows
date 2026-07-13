#include "game/scenes/EditorScene.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>

#include <imgui.h>
#include <imgui_internal.h> // DockBuilder (8.7b default layout)

#include "data/forms/AnimForms.hpp"
#include "data/forms/CoreForms.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/plugins/Synthesis.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "engine/Engine.hpp"
#include "engine/FrameContext.hpp"
#include "engine/platform/Paths.hpp"
#include "engine/rhi/CommandBuffer.hpp"
#include "engine/reflect/Visit.hpp"
#include "game/AllForms.hpp"
#include "game/SceneStack.hpp" // host()->pop() when stacked over the game
#include "game/ui/ConditionBuilder.hpp"
#include "game/ui/EventPicker.hpp"
#include "game/ui/PropertyGrid.hpp"
#include "gameplay/ability/GameplayAbility.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
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

// 8.7b — the Browser's curated categories (dev-decided grouping): the
// ones with a dedicated editing surface first, then the frequent Forms,
// then "All types" (the ex-GameDB reflection browser) for everything
// else. A category is a Form type + which center editor serves it.
struct EditorCategory {
    const char* name;
    enum Kind {
        QuestGraph,
        DialogueGraph,
        AnimGraph,
        ClipTimeline,
        Timeline,
        FxPreview,
        Effect,
        Ability,
        Generic,
        AllTypes
    } kind;
    const reflect::TypeInfo* (*type)(); // null for AllTypes
};

constexpr EditorCategory kCategories[] = {
    { "Quests", EditorCategory::QuestGraph,
      [] { return &quest::QuestForm::staticTypeInfo(); } },
    { "Dialogues", EditorCategory::DialogueGraph,
      [] { return &quest::DialogueForm::staticTypeInfo(); } },
    { "Anim Graphs", EditorCategory::AnimGraph,
      [] { return &data::AnimGraphForm::staticTypeInfo(); } },
    { "Anim Clips", EditorCategory::ClipTimeline,
      [] { return &data::AnimClipForm::staticTypeInfo(); } },
    { "Schedules", EditorCategory::Timeline,
      [] { return &gameplay::ScheduleForm::staticTypeInfo(); } },
    { "Particles", EditorCategory::FxPreview,
      [] { return &data::ParticleForm::staticTypeInfo(); } },
    { "Cues", EditorCategory::Generic,
      [] { return &data::CueForm::staticTypeInfo(); } },
    { "Effects", EditorCategory::Effect,
      [] { return &gameplay::EffectForm::staticTypeInfo(); } },
    { "Abilities", EditorCategory::Ability,
      [] { return &gameplay::AbilityForm::staticTypeInfo(); } },
    { "Weapons", EditorCategory::Generic,
      [] { return &data::WeaponForm::staticTypeInfo(); } },
    { "Armors", EditorCategory::Generic,
      [] { return &data::ArmorForm::staticTypeInfo(); } },
    { "Consumables", EditorCategory::Generic,
      [] { return &data::ConsumableForm::staticTypeInfo(); } },
    { "Actors", EditorCategory::Generic,
      [] { return &data::ActorForm::staticTypeInfo(); } },
    { "All types", EditorCategory::AllTypes, nullptr },
};
constexpr int kCategoryCount =
    static_cast<int>(sizeof(kCategories) / sizeof(kCategories[0]));

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
    // Synchronous on purpose (audit U5-10, kept as-is): this is the dev
    // GameDB editor — a blocking reload is simpler (§10) and safer than
    // async here (no world in flight, nothing to keep interactive).
    stack = data::loadPluginStack(pluginDir, config, types);
    db = std::make_unique<data::FormDatabase>();
    report = data::resolve(data::pointersOf(stack), types, *db);
    session = std::make_unique<data::EditSession>(*db, types);
    console = std::make_unique<ConsolePanel>(*session, *db, types, *vm);
    layouts.load((pluginDir / "editor-layouts.toml").string());
    animGraph = std::make_unique<AnimGraphPanel>(*session, layouts, selected);
    questGraph =
        std::make_unique<QuestGraphPanel>(*session, layouts, selected);
    dialogueGraph =
        std::make_unique<DialogueGraphPanel>(*session, layouts, selected);
    clipTimeline = std::make_unique<ClipTimelinePanel>(*session, selected);
    // The asset VFS, layered like the game builds it (chantier 4 B1: the
    // editor edits exactly what the game runs) — the anim preview resolves
    // rigs and skinned meshes through it.
    assetDb = assets::AssetDatabase {};
    for (const data::Plugin& plugin : stack.plugins) {
        for (const data::AssetEntry& entry : plugin.assets) {
            assetDb.add(entry.id, plugin.baseDir, entry.path);
        }
    }
    animPreview = std::make_unique<AnimPreviewPanel>(engine->getDevice(),
                                                     *session, *db, assetDb);
    fxPanel = std::make_unique<FxPanel>(*session);
    effectPanel = std::make_unique<EffectPanel>(*session);
    abilityPanel = std::make_unique<AbilityPanel>(*session, *db);
    selected = {};
    itemSelected = {};
    synthPicks.assign(report.conflicts.size(), -1); // -1 = keep load order
    status = "loaded " + std::to_string(stack.plugins.size()) + " plugins, " +
             std::to_string(db->count()) + " forms, " +
             std::to_string(report.conflicts.size()) + " field conflicts";
}

void EditorScene::saveConfig() const {
    std::ofstream out { pluginDir / "plugins.toml", std::ios::binary };
    out << data::writePluginConfigToml(config);
}

void EditorScene::exportSessionPlugin() {
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

// ---------------------------------------------------------------------
// 8.7b — the shell: ONE dockspace window, Unity-style. Browser | Editor
// | Inspector, menu bar on top; Plugins/Console toggle from the menu.

void EditorScene::buildDockLayout(unsigned int dockIdIn) {
    const ImGuiID dockId = dockIdIn;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::DockBuilderRemoveNode(dockId);
    ImGui::DockBuilderAddNode(dockId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockId, viewport->WorkSize);
    ImGuiID center = dockId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Left, 0.15f, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Right, 0.27f, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(
        center, ImGuiDir_Down, 0.25f, nullptr, &center);
    ImGui::DockBuilderDockWindow("Browser", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Plugins", right); // tabbed with Inspector
    ImGui::DockBuilderDockWindow("Editor", center);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Anim Preview", bottom); // tabbed w/ Console
    ImGui::DockBuilderFinish(dockId);
}

void EditorScene::drawShell() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking |
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;
    ImGui::Begin("True Adventurer DB", nullptr, flags);
    ImGui::PopStyleVar(3);

    const ImGuiID dockId = ImGui::GetID("tadb-dockspace");
    if (ImGui::DockBuilderGetNode(dockId) == nullptr) {
        buildDockLayout(dockId); // first run — imgui.ini persists the rest
    }
    ImGui::DockSpace(dockId);

    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            ImGui::SetNextItemWidth(180.0f);
            ImGui::InputText("plugin name", exportName, sizeof(exportName));
            ImGui::BeginDisabled(session->dirtyCount() == 0);
            if (ImGui::MenuItem("Export plugin")) {
                exportSessionPlugin();
            }
            ImGui::EndDisabled();
            if (ImGui::MenuItem("Reload data")) {
                reload(); // discards pending edits — status says so
            }
            // Overlay mode (interfaces-par-mode): pushed over the paused
            // game world from Edit mode — pop back to it, still warm.
            if (host() && host()->size() > 1 &&
                ImGui::MenuItem("Close (back to game)")) {
                host()->pop();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Undo", "Ctrl+Z", false,
                                session->canUndo())) {
                session->undo();
            }
            if (ImGui::MenuItem("Redo", "Ctrl+Y", false,
                                session->canRedo())) {
                session->redo();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Windows")) {
            ImGui::MenuItem("Plugins", nullptr, &showPlugins);
            ImGui::MenuItem("Console", nullptr, &showConsole);
            ImGui::MenuItem("Anim Preview", nullptr, &showAnimPreview);
            if (ImGui::MenuItem("Reset layout")) {
                buildDockLayout(dockId);
            }
            ImGui::EndMenu();
        }
        // Keyboard shortcuts for the menu items above.
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z,
                            ImGuiInputFlags_RouteGlobal) &&
            session->canUndo()) {
            session->undo();
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y,
                            ImGuiInputFlags_RouteGlobal) &&
            session->canRedo()) {
            session->redo();
        }
        ImGui::TextDisabled("| %u pending edits | %s", session->dirtyCount(),
                            status.c_str());
        ImGui::EndMenuBar();
    }
    ImGui::End();
}

void EditorScene::drawUi() {
    drawShell();
    drawBrowser();
    drawEditor();
    drawInspector();
    if (showPlugins) {
        drawPlugins();
    }
    if (showConsole) {
        console->draw();
    }
    if (showAnimPreview) {
        if (ImGui::Begin("Anim Preview", &showAnimPreview)) {
            animPreview->drawEditor(itemSelected);
        }
        ImGui::End();
    }
}

void EditorScene::render(engine::FrameContext& frame) {
    // The preview's offscreen pass must record BEFORE the stack's sprite
    // pass (passes don't nest), hence the frame ownership: draw the
    // preview target, then leave the backbuffer cleared as the contract
    // demands (the ImGui shell draws over it afterwards).
    if (animPreview) {
        animPreview->render(frame);
    }
    frame.cmd.beginRenderPass({ .loadOp = rhi::LoadOp::Clear,
                                .clearColor = frame.clearColor });
    frame.cmd.endRenderPass();
}

// ---------------------------------------------------------------------
// Browser: categories on top, the item list of the active one below.

void EditorScene::drawBrowser() {
    ImGui::Begin("Browser");
    for (int i = 0; i < kCategoryCount; ++i) {
        if (ImGui::Selectable(kCategories[i].name, categorySelected == i)) {
            if (categorySelected != i) {
                categorySelected = i;
                itemSelected = {};
                selected = {};
            }
        }
    }
    ImGui::Separator();

    const EditorCategory& category = kCategories[categorySelected];
    u32 typeId = 0;
    str typeName;
    if (category.kind == EditorCategory::AllTypes) {
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
        if (typeFilter > 0) {
            if (const reflect::TypeInfo* type = types.findType(filterName)) {
                typeId = type->id;
                typeName = type->name;
            }
        }
    } else {
        const reflect::TypeInfo& type = *category.type();
        typeId = type.id;
        typeName = type.name;
    }
    ImGui::InputTextWithHint("##search", "search editorId...", search,
                             sizeof(search));

    if (typeId != 0) {
        if (ImGui::Button("+ New")) {
            itemSelected = session->createForm(typeId, "New" + typeName);
            selected = itemSelected;
        }
        ImGui::SameLine();
    }
    if (itemSelected.isValid()) {
        // 8.1: clone into the session (children NOT copied).
        if (const data::Form* form = session->view(itemSelected)) {
            if (ImGui::Button("Duplicate")) {
                const str baseName =
                    form->editorId.empty() ? str { "Form" } : form->editorId;
                const core::Guid copy =
                    session->duplicateForm(itemSelected, baseName + "Copy");
                if (copy.isValid()) {
                    itemSelected = copy;
                    selected = copy;
                }
            }
        }
    }
    ImGui::Separator();

    ImGui::BeginChild("items");
    session->forEachVisible([&](const core::Guid& id, const data::Form& form,
                                const reflect::TypeInfo& type) {
        if (typeId != 0 && type.id != typeId) {
            return;
        }
        if (typeId == 0 && category.kind != EditorCategory::AllTypes) {
            return;
        }
        if (search[0] != '\0' && form.editorId.find(search) == str::npos) {
            return;
        }
        const str label =
            (form.editorId.empty() ? id.toString() : form.editorId) +
            (session->isDirty(id) ? " *" : "") + "##i" + id.toString();
        if (ImGui::Selectable(label.c_str(), itemSelected == id)) {
            itemSelected = id;
            selected = id;
        }
    });
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------
// Editor (center): the editing surface of the active category.

void EditorScene::drawEditor() {
    ImGui::Begin("Editor");
    switch (kCategories[categorySelected].kind) {
    case EditorCategory::QuestGraph:
        questGraph->drawCanvas(itemSelected);
        break;
    case EditorCategory::DialogueGraph:
        dialogueGraph->drawCanvas(itemSelected);
        break;
    case EditorCategory::AnimGraph:
        animGraph->drawCanvas(itemSelected);
        break;
    case EditorCategory::ClipTimeline:
        clipTimeline->drawEditor(itemSelected);
        break;
    case EditorCategory::Timeline:
        drawSchedulesContent();
        break;
    case EditorCategory::FxPreview:
        fxPanel->drawEditor(itemSelected);
        break;
    case EditorCategory::Effect:
        effectPanel->drawEditor(itemSelected);
        break;
    case EditorCategory::Ability:
        abilityPanel->drawEditor(itemSelected);
        break;
    default:
        drawGenericSummary();
        break;
    }
    ImGui::End();
}

// Non-graph types: what the record is + who points at it. The editing
// itself happens in the Inspector grid (8.11 will grow dedicated
// Effect/Ability surfaces here).
void EditorScene::drawGenericSummary() {
    if (!itemSelected.isValid()) {
        ImGui::TextDisabled("(select an item in the Browser)");
        return;
    }
    const data::Form* form = session->view(itemSelected);
    const reflect::TypeInfo* type = session->viewType(itemSelected);
    if (!form || !type) {
        ImGui::TextDisabled("(unknown record)");
        return;
    }
    ImGui::Text("%s  (%s)", form->editorId.c_str(), type->name.c_str());
    ImGui::TextDisabled("%s", form->id.toString().c_str());
    ImGui::Separator();
    ImGui::TextUnformatted("Used by:");
    const auto hits = data::referencesTo(*db, itemSelected);
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

// ---------------------------------------------------------------------
// Inspector (right): the item's HIERARCHY on top (the ex-8.2/8.3 trees),
// the PropertyGrid of the selected sub-object below — the Unity model.

void EditorScene::drawInspector() {
    ImGui::Begin("Inspector");
    // The Inspector's selection reads GOLD — the same tint as the node
    // editor's selected-node border, so graph and inspector visibly
    // point at the same thing (dev feedback 8.7d).
    constexpr ImVec4 kSelGold { 1.0f, 0.69f, 0.20f, 0.45f };
    constexpr ImVec4 kSelGoldHovered { 1.0f, 0.69f, 0.20f, 0.60f };
    constexpr ImVec4 kSelGoldActive { 1.0f, 0.69f, 0.20f, 0.80f };
    ImGui::PushStyleColor(ImGuiCol_Header, kSelGold);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kSelGoldHovered);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, kSelGoldActive);
    const reflect::TypeInfo* itemType =
        itemSelected.isValid() ? session->viewType(itemSelected) : nullptr;
    const bool hasHierarchy =
        itemType && (itemType->id == quest::QuestForm::staticTypeInfo().id ||
                     itemType->id ==
                         quest::DialogueForm::staticTypeInfo().id ||
                     itemType->id == data::AnimGraphForm::staticTypeInfo().id);
    if (hasHierarchy) {
        ImGui::BeginChild("hierarchy",
                          ImVec2(0.0f,
                                 ImGui::GetContentRegionAvail().y * 0.45f),
                          ImGuiChildFlags_ResizeY);
        if (itemType->id == quest::QuestForm::staticTypeInfo().id) {
            drawQuestHierarchy();
        } else if (itemType->id == quest::DialogueForm::staticTypeInfo().id) {
            drawDialogueHierarchy();
        } else {
            animGraph->drawHierarchy(itemSelected);
        }
        ImGui::EndChild();
        ImGui::Separator();
    }
    ImGui::BeginChild("inspector-grid");
    if (selected.isValid()) {
        questGraph->drawInspectorExtras(selected);
        drawPropertyGrid(*session, selected);
        // 8.7c: the quest<->dialogue articulation made visible — who
        // fires / listens to / starts on this record's event — and the
        // explicit wiring buttons (dev feedback: creating the link must
        // not require typing the same name twice).
        drawEventCrossRef(*session, selected, selected);
        drawEventWiring(*session, selected);
        // 8.9: the shared condition builder — dialogue options and
        // abilities carry ANDed ConditionForm clauses. Selecting a clause
        // keeps the list anchored on its parent (the builder must not
        // vanish under its own selection).
        core::Guid conditionParent;
        if (const auto* selType = session->viewType(selected)) {
            if (selType->id ==
                    quest::DialogueNodeForm::staticTypeInfo().id ||
                selType->id == gameplay::AbilityForm::staticTypeInfo().id) {
                conditionParent = selected;
            } else if (selType->id ==
                       gameplay::ConditionForm::staticTypeInfo().id) {
                conditionParent =
                    static_cast<const gameplay::ConditionForm*>(
                        session->view(selected))
                        ->parent;
            }
        }
        if (conditionParent.isValid()) {
            drawConditionList(*session, conditionParent, selected);
        }
        // 8.7d: the one-gesture link — a dialogue option becomes the
        // start of a brand-new quest (event generated when missing),
        // and the editor navigates to it.
        if (const auto* selType = session->viewType(selected);
            selType &&
            selType->id == quest::DialogueNodeForm::staticTypeInfo().id) {
            if (ImGui::Button("Start a NEW quest on this option")) {
                data::EditSession::Gesture gesture { *session };
                const auto* node =
                    static_cast<const quest::DialogueNodeForm*>(
                        session->view(selected));
                const str base =
                    node->editorId.empty() ? str { "Quest" } : node->editorId;
                str eventName = node->event;
                if (eventName.empty()) {
                    eventName = "OnAccept" + base;
                    session->setField(selected, core::fnv1a("event"),
                                      reflect::Value { eventName });
                }
                const core::Guid questId = session->createForm(
                    quest::QuestForm::staticTypeInfo().id, base + "Quest");
                session->setField(questId, core::fnv1a("startEvent"),
                                  reflect::Value { eventName });
                const core::Guid stateId = session->createForm(
                    quest::QuestStateForm::staticTypeInfo().id,
                    base + "QuestStart");
                session->setField(stateId, core::fnv1a("quest"),
                                  reflect::Value { questId });
                session->setField(questId, core::fnv1a("startState"),
                                  reflect::Value { stateId });
                categorySelected = 0; // Quests (first category)
                itemSelected = questId;
                selected = questId;
            }
        }
    } else {
        ImGui::TextDisabled("(select something)");
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(3); // the gold selection tint
    ImGui::End();
}

// Ex-8.2 quest tree (states -> branches -> tasks), now the Inspector's
// hierarchy pane. Same creation flows, same session.
void EditorScene::drawQuestHierarchy() {
    const auto* questForm =
        static_cast<const quest::QuestForm*>(session->view(itemSelected));
    if (!questForm) {
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
        data::EditSession::Gesture gesture { *session };
        const core::Guid id = session->createForm(
            quest::QuestStateForm::staticTypeInfo().id,
            questForm->editorId + "State");
        session->setField(id, core::fnv1a("quest"),
                          reflect::Value { itemSelected });
        selected = id;
    }
    ImGui::Separator();

    for (const auto& [stateId, state] : states) {
        if (state->quest != itemSelected) {
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
            data::EditSession::Gesture gesture { *session };
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
                data::EditSession::Gesture gesture { *session };
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
}

// Ex-8.3 dialogue tree (parent-linked, alternating NPC lines and Player
// choices), now the Inspector's hierarchy pane: inline creation, sibling
// reorder and per-node conditions.
void EditorScene::drawDialogueHierarchy() {
    const auto* dialogue = static_cast<const quest::DialogueForm*>(
        session->view(itemSelected));
    if (!dialogue) {
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
            data::EditSession::Gesture gesture { *session };
            const core::Guid id = session->createForm(
                quest::DialogueNodeForm::staticTypeInfo().id,
                dialogue->editorId + "Root");
            session->setField(itemSelected, core::fnv1a("rootNode"),
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
                data::EditSession::Gesture gesture { *session };
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
                data::EditSession::Gesture gesture { *session };
                const core::Guid id = session->createForm(
                    gameplay::ConditionForm::staticTypeInfo().id,
                    node->editorId + "Cond");
                session->setField(id, core::fnv1a("parent"),
                                  reflect::Value { nodeId });
                selected = id;
            }
            // Conditions as selectable leaves under their node (8.9:
            // the shared summary — the builder in the grid below edits).
            for (const auto& [condId, cond] : conditions) {
                if (cond->parent != nodeId) {
                    continue;
                }
                if (ImGui::Selectable(
                        (gameplay::conditionSummary(*cond) + "##c" +
                         condId.toString())
                            .c_str(),
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
}

// 8.4 — the schedule timeline: one 24 h strip per ScheduleForm, one lane
// per ScheduleEntryForm. Bars drag by their edges (0.5 h snap, committed
// as ONE field edit on release); midnight-wrapping entries render as two
// segments. The H7 debug eval stays at the top of each strip. Since
// 8.7b: the Editor-center content of the Schedules category.
void EditorScene::drawSchedulesContent() {
    ImGui::SliderFloat("Hour", &debugHour, 0.0f, 24.0f, "%.1f");
    ImGui::SameLine();
    if (ImGui::Button("+ Schedule")) {
        selected = session->createForm(
            gameplay::ScheduleForm::staticTypeInfo().id, "NewSchedule");
        itemSelected = selected;
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
            data::EditSession::Gesture gesture { *session };
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
}

void EditorScene::drawPlugins() {
    ImGui::Begin("Plugins", &showPlugins);
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
