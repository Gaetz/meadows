#pragma once

#include <filesystem>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/EditSession.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"
#include "game/Scene.hpp"
#include "game/ui/ConsolePanel.hpp"
#include "script/Vm.hpp"

namespace engine {
class Engine;
}

namespace game {

// The game-database editor (horizontal pass H2): GameDB browser (every
// Form type through the reflection property grid), plugin manager (load
// order + per-field conflict report), and the developer console. All
// edits flow through ONE EditSession and export as an ordinary plugin —
// the editor is a plugin author (§5), nothing more.
//
// HOW TO FILL (post-7/07): the level editor (MEADOWS-PLAN §A) builds on
// this scene's pieces — same session, PropertyGrid as the reference
// inspector, plus a world view with picking/gizmos. Live re-resolve after
// edits (instead of reload-on-restart) is its own vertical.
class EditorScene final : public Scene {
public:
    explicit EditorScene(engine::Engine& engineContext)
        : engine(&engineContext) {}

    void onEnter() override;
    void update(f32) override {}
    void drawUi() override;

private:
    void reload();
    void drawGameDb();
    void drawPlugins();
    void drawQuests();    // 8.2: Quest -> States -> Branches -> Tasks tree
    void drawDialogues(); // 8.3: DialogueNodeForm tree + conditions
    void drawSchedules(); // H7 debug: who does what at which hour, and why
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

    vector<str> typeNames; // sorted, for the filter combo
    int typeFilter { 0 };  // 0 = All
    char search[128] {};
    core::Guid selected {};
    core::Guid questSelected {};    // 8.2: the quest whose tree is shown
    core::Guid dialogueSelected {}; // 8.3: the dialogue whose tree is shown
    char exportName[128] { "my-edits" };
    str status;
};

} // namespace game
