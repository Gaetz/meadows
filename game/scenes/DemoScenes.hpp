#pragma once

#include <optional>

#include "data/forms/CoreForms.hpp"
#include "game/WorldEditor.hpp"
#include "game/scenes/WorldDemoScene.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/EquipmentStats.hpp"
#include "gameplay/stats/GameClock.hpp"
#include "gameplay/stats/Resonance.hpp"
#include "gameplay/stats/Survival.hpp"
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

// Demonstrates the character-stats slice (docs/STATS.md): a single actor with the
// nine attributes → primary stats, Resonance + Harmony, typed-damage mitigation,
// posture/stagger, and a hunger→resonance loop on the game clock. The ImGui panel
// drives it all so the cascade, shifting maxima and posture break are visible.
class StatsScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;
    void onEnter() override;
    void update(f32 dt) override;
    void drawUi() override;

private:
    gameplay::StatModifiers resonanceModifiers() const;
    void seedResources();

    gameplay::CoreAttributes core;
    gameplay::AttributeSet vitals;
    gameplay::Resonance resonance;   // persistent (wounds/drugs later)
    gameplay::Survival survival;
    gameplay::CombatState combat;
    gameplay::AbilitySystem system;
    gameplay::DerivedStatRegistry derived;
    gameplay::StatsTuningForm tuning; // resolved from data (§5), or defaults
    gameplay::GameClock clock;

    // F3 demo: sample gear (scene-local, so no plugin data needed).
    data::WeaponForm sampleWeapon;
    data::ArmorForm sampleArmor;
    bool armorEquipped { false };
};

} // namespace game
