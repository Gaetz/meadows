#pragma once

#include "game/WorldEditor.hpp"
#include "game/scenes/WorldDemoScene.hpp"

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

private:
    ecs::Entity player {};
};

} // namespace game
