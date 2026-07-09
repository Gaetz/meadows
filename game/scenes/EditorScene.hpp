#pragma once

#include <filesystem>

#include "data/editor/EditorLayouts.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/EditSession.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "game/Scene.hpp"
#include "game/ui/AnimGraphPanel.hpp"
#include "game/ui/ConsolePanel.hpp"
#include "game/ui/DialogueGraphPanel.hpp"
#include "game/ui/QuestGraphPanel.hpp"
#include "script/Vm.hpp"

namespace engine {
class Engine;
}

namespace game {

// The game-database editor — since 8.7b, ONE dockspace window ("True
// Adventurer DB", Unity-style): Browser (left: curated categories + item
// list), Editor (center: the graph canvas / timeline / summary of the
// selected item), Inspector (right: the item's HIERARCHY on top — the
// ex-8.2/8.3 trees — and the PropertyGrid of the selected sub-object
// below). All edits flow through ONE EditSession and export as an
// ordinary plugin (§5) via the File menu — the editor is a plugin
// author, nothing more. Plugins/Console are dockable windows toggled
// from the Windows menu.
class EditorScene final : public Scene {
public:
    explicit EditorScene(engine::Engine& engineContext)
        : engine(&engineContext) {}

    void onEnter() override;
    void update(f32) override {}
    void drawUi() override;

private:
    void reload();
    void drawShell();   // fullscreen dockspace host + menu bar
    void buildDockLayout(unsigned int dockId); // default Browser/Editor/Inspector
    void drawBrowser();
    void drawEditor();
    void drawInspector();
    void drawQuestHierarchy();    // ex-8.2 tree, now the Inspector top
    void drawDialogueHierarchy(); // ex-8.3 tree, now the Inspector top
    void drawSchedulesContent();  // 8.4 timeline, now the Editor center
    void drawGenericSummary();    // non-graph types: record + used-by
    void drawPlugins();
    void exportSessionPlugin();
    void saveConfig() const;
    f32 debugHour { 12.0f };

    engine::Engine* engine { nullptr };

    data::FormTypeRegistry types;
    std::filesystem::path pluginDir;
    data::PluginConfig config;
    data::PluginStack stack;
    data::ResolveReport report;
    uptr<data::FormDatabase> db;
    uptr<data::EditSession> session;
    uptr<script::Vm> vm;
    uptr<ConsolePanel> console;
    data::EditorLayouts layouts; // node x/y side-store (8.6, NOT a plugin)
    uptr<AnimGraphPanel> animGraph;         // 8.6
    uptr<QuestGraphPanel> questGraph;       // 8.7
    uptr<DialogueGraphPanel> dialogueGraph; // 8.7

    vector<str> typeNames; // sorted, for the "All types" filter combo
    int typeFilter { 0 };  // 0 = All (within the "All types" category)
    int categorySelected { 0 };
    char search[128] {};
    core::Guid itemSelected {}; // the Browser item (quest, graph, form...)
    core::Guid selected {};     // the Inspector target (sub-object)
    bool showPlugins { false };
    bool showConsole { false };
    core::Guid schedDragEntry {}; // 8.4: entry whose edge is dragging
    int schedDragEdge { 0 };      //      0 = startHour, 1 = endHour
    f32 schedDragHour { 0.0f };   //      live preview value (0.5 h snap)
    vector<int> synthPicks;       // 8.5: per-conflict pick (-1 = load order)
    char exportName[128] { "my-edits" };
    str status;
};

} // namespace game
