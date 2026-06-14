#pragma once

#include <optional>

#include "game/WorldEditor.hpp"
#include "game/scenes/WorldDemoScene.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "quest/Dialogue.hpp"
#include "quest/Quest.hpp"

namespace game {

// Demonstrates the §5 data/modding model: toggle the sample mod and watch the
// resolve report + the world re-arrange (sword recolor, references moved/
// disabled) live.
class PluginScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;
    void drawUi() override;
};

// Demonstrates the in-game world editor: add / select / move / delete objects.
class WorldEditScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;
    void onEnter() override;
    void drawUi() override;

private:
    uptr<WorldEditor> editor;
};

// Demonstrates the simplified GAS: each actor's AbilitySystem, with a Strike
// ability (effect + cooldown) and derived death.
class CombatScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;
    void drawUi() override;
};

// Demonstrates the player controller + movement: spawns a player entity moved
// with WASD / arrows (input → Velocity → applyMovement).
class GameplayScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;
    void onEnter() override;
    void update(f32 dt) override;
    void drawUi() override;

private:
    ecs::Entity player {};
};

// Demonstrates the narrative layer (Phase 4): a guard offers a quest through a
// dialogue (one option is condition-gated), accepting it begins the quest via an
// event, and a debug button advances/completes it.
class NarrativeScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;
    void onEnter() override;
    void drawUi() override;

private:
    gameplay::EventBus bus;
    quest::QuestLog questLog;
    std::optional<quest::DialogueRunner> dialogue;
    gameplay::AbilitySystem playerAbilities; // condition context for the player
    gameplay::Inventory playerInventory;     // receives the quest reward
    bool brave { false };
    bool rewarded { false };
};

} // namespace game
