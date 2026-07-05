#pragma once

#include <string>
#include <vector>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"
#include "engine/ecs/World.hpp"
#include "engine/fx/Particles.hpp"
#include "game/scenes/WorldDemoScene.hpp"
#include "gameplay/cue/GameplayCues.hpp"

namespace gameplay {
struct AbilityForm;
}

namespace data {
struct WeaponForm;
}

namespace game {

// Phase 8 — the playable "village + arena" scene (docs/PHASE-8.md).
//
// Step 1 (foundation): a player + training dummies, all driven by the full
// `tickCharacter` pipeline every frame — the move from the ImGui-driven
// single-actor StatsScene to a live multi-combatant simulation. Player control,
// attacks, enemy AI, and the rest/merchant NPCs land in the following steps.
class CombatArenaScene : public WorldDemoScene {
public:
    using WorldDemoScene::WorldDemoScene;

    void onEnter() override;
    void update(f32 dt) override;
    void draw(render::SpriteRenderer& renderer) override;
    void drawUi() override;

private:
    // A spawned combatant plus a display name for the debug readout.
    struct Combatant {
        ecs::Entity entity;
        std::string name;
    };

    // Creates a combatant entity with the full character-stats sheet (same
    // component set the Spawner attaches to data-driven actors) at full vitals.
    // `sheet` is an 8-direction sprite-sheet asset; `facing` picks the frame.
    ecs::Entity spawnCombatant(std::string name, Vec3 position,
                               const core::Guid& sheet, Vec2 facing);

    // Step 2 player controller: WASD movement, facing toward the mouse cursor,
    // and the dodge ability. Runs before the per-combatant tick each frame.
    void updatePlayer(f32 dt);

    // Step 3 melee attack: the windup→active→recovery state machine driven by the
    // left mouse button, dealing typed weapon damage through a transient hitbox.
    void updatePlayerAttack(f32 dt);

    // Equipment mitigation for a combatant: folds its equipped armor (if any) into
    // StatModifiers, fed to its tickCharacter each frame so mitigation reflects it.
    gameplay::StatModifiers equipmentModsFor(ecs::Entity e) const;

    ecs::Entity player {};
    std::vector<Combatant> combatants; // player + dummies (ticked + inspected)

    // Dodge state (Step 2). The GAS ability owns cost/cooldown/i-frames; the
    // scene only drives the movement burst and the sprite facing.
    const gameplay::AbilityForm* dodgeAbility { nullptr }; // resolved in onEnter
    f32 dodgeTimer { 0.0f };            // > 0 → dodge velocity burst in progress
    Vec2 dodgeDir { 0.0f, 0.0f };       // burst direction
    Vec2 aimDir { 0.0f, -1.0f };        // current facing (default south)

    // Attack state (Step 3). The GAS ability gates cost/cooldown; the scene runs
    // the swing timing and the melee hitbox. Damage is weapon-driven (applyDamage
    // + weaponDamageEvent), not a generic effect.
    enum class AttackPhase { None, Windup, Active, Recovery };
    const gameplay::AbilityForm* attackAbility { nullptr }; // resolved in onEnter
    // Swappable test weapons (keys 1-5), resolved in onEnter. Each has its own
    // typed damage, posture damage, and status effect.
    static constexpr int kWeaponCount = 5;
    const data::WeaponForm* weapons[kWeaponCount] {}; // [0]=sword … [4]=flame scimitar
    int weaponIndex { 0 };
    AttackPhase attackPhase { AttackPhase::None };
    f32 attackTimer { 0.0f };            // time left in the current phase
    Vec2 attackDir { 0.0f, -1.0f };      // aim locked at the swing's start
    std::vector<ecs::Entity> hitThisSwing; // enemies already struck this swing

    // H7 proof: the sim emits "Cue.Hit.*" on melee impact; the scene's
    // handler bursts sparks (fx::ParticleSim), drawn as sprites after the
    // world. Headless would simply have no handler — no coupling.
    gameplay::CueRegistry cues;
    fx::ParticleSim particles;
};

} // namespace game
