#pragma once

#include "data/forms/CoreForms.hpp"
#include "game/scenes/WorldDemoScene.hpp"
#include "gameplay/ability/GameplayEffects.hpp"

namespace game {

// The character-stats bench (docs/STATS.md): a single actor with the nine
// attributes → primary stats, Resonance + Harmony, typed-damage
// mitigation, posture/stagger, survival→resonance on the game clock,
// buildup, injuries, afflictions and drugs — all driven from the
// CharacterStatsPanel so the cascade, shifting maxima and posture break
// stay visible. Headless siblings: the stats doctests; this scene is the
// eyes-on bench.
class StatsScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;
    void onEnter() override;
    void update(f32 dt) override;
    void drawUi() override;

private:
    gameplay::StatModifiers equipmentModifiers() const;

    // The bench actor is an ECS entity; all character-stats components
    // live on it. World-level resources (derived, tuning, clock, rng)
    // come from WorldDemoScene.
    ecs::Entity player {};

    // Sample gear + sample effects for the panel's test buttons (the
    // real game equips through Inventory/Equipment and data EffectForms).
    data::WeaponForm sampleWeapon;
    data::ArmorForm sampleArmor;
    bool armorEquipped { false };
    gameplay::EffectForm sampleDrug;
    gameplay::EffectForm sampleDisease;
    gameplay::EffectForm samplePsychosis;
};

} // namespace game
