#pragma once

#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity
#include "gameplay/actors/Swimming.hpp" // gameplay::MoveMode (D2b)

namespace platform {
class Input;
}
namespace render {
class FlyCamera;
}
namespace data {
class FormDatabase;
class TextTable;
struct WeaponForm;
}
namespace phys {
class PhysicsWorld;
class CharacterBody;
}
namespace gameplay {
class GameplayTagRegistry;
class DerivedStatRegistry;
class EventBus;
class CueRegistry;
struct StatsTuningForm;
struct EffectForm;
struct AbilityForm;
}

namespace game {

struct Npc;
class InteractionController;

// The scene systems the first-person player touches, bundled so the whole
// Play-mode controller (movement / jump / sprint cost / melee + crime) is
// decoupled from LandscapeScene (audit U4-1). The scene rebuilds it each
// call from its own members — references, a few scalars, one closure.
// Mirrors the other *Context contracts.
struct PlayerContext {
    data::FormDatabase& forms;
    render::FlyCamera& flyCamera; // mouselook writes yaw/pitch; eye follows
    platform::Input& input;
    phys::PhysicsWorld* physics; // crime witness line-of-sight
    ecs::Entity playerEntity;
    const gameplay::GameplayTagRegistry& gameTags;
    const gameplay::DerivedStatRegistry& derivedStats;
    const gameplay::StatsTuningForm& statsTuning;
    const data::TextTable& texts; // U4-11: player-facing strings by key
    const gameplay::EffectForm* sprintCostEffect; // §2.9: energy only moves here
    const data::WeaponForm* fallbackWeapon;       // pre-equipment default
    // P0 A3: the melee attack ability (energy cost + cooldown effects, §6)
    // gates every swing; the swing itself is the MeleeSwing component.
    const gameplay::AbilityForm* attackAbility;
    // Dodge (the 2D arena ability in 3D): cost/cooldown/i-frames are its
    // effects; the controller only drives the burst.
    const gameplay::AbilityForm* dodgeAbility;
    vector<uptr<Npc>>& npcs;          // melee targets + crime witnesses
    InteractionController& interaction; // fading gate + the crime toast
    bool overencumbered; // C3: no sprint, no jump (computed at the
                         //   equipMods site — it also feeds the tick)
    std::function<void()> syncWantedTag; // Crime.Wanted mirror stays with the
                                         //   scene's quest/crime wiring
    // Chantier P0 C4a: the player has no walk clip — footsteps fire as
    // "AnimEvent"/Footstep on the bus every strideLength meters walked.
    gameplay::EventBus* eventBus { nullptr };
    // P0 C2: feedback cues (hit/block/parry) — the FxDirector's registry.
    gameplay::CueRegistry* cues { nullptr };
    // P0 D2b: the water surface (if any) over a world position — sea
    // level + placed WaterVolumes; the scene owns the geometry, the
    // controller only asks (the TriggerSystem callback pattern).
    std::function<std::optional<f32>(const Vec3&)> waterSurfaceAt;
    // Energy drain while swimming (§2.9: only effects move energy).
    const gameplay::EffectForm* swimCostEffect { nullptr };
    // Sneak: the drain while MOVING sneaked (SneakCost, ~3 energy/s).
    const gameplay::EffectForm* sneakCostEffect { nullptr };
};

// The first-person Play-mode controller extracted from LandscapeScene
// (audit U4-1): owns the kinematic capsule and the movement state
// (smoothed velocity, swing weapon, sprint-cost accumulator), runs
// mouselook / camera-relative movement / jump / sprint cost (through the
// SprintCost GameplayEffect, §2.9) and the LMB melee swing — P0 A3/A4:
// the ability-gated MeleeSwing whose blade must TOUCH (segment vs
// capsule) — with the D2 crime-witnessing pass. MODE transitions stay in the scene (enter/exit/
// restoreMode are SceneMode plumbing); they and travel/tp drive the body
// through spawnBody/destroyBody.
class PlayerController {
public:
    // (Re)spawns the capsule with the standard adventurer dimensions at
    // `position` (boot spot, travel marker, tp target) and zeroes the
    // smoothed velocity. Replaces any previous body.
    void spawnBody(phys::PhysicsWorld& physics, const Vec3& position);
    void destroyBody(); // exitPlayMode / onExit

    // The capsule, or null outside Play — the scene's focus/context sites
    // read it exactly like the former `player` member.
    phys::CharacterBody* body() { return body_.get(); }
    const phys::CharacterBody* body() const { return body_.get(); }

    // Dev-panel readout (m/s, smoothed horizontal).
    const Vec3& smoothedVelocity() const { return velocity; }

    // Per frame in Play (the scene gates on mode + body): melee on LMB,
    // mouselook, movement, jump, sprint cost, camera + entity-transform
    // sync. Frozen while the interaction fade runs.
    void update(f32 dt, const PlayerContext& ctx);

    // P0 A3: the weapon the in-flight swing was activated with (null when
    // Idle) — the scene's viewmodel takes its timings and model from it.
    const data::WeaponForm* swingWeapon() const { return swingWeapon_; }

    // R toggles drawn/sheathed; sheathed = no viewmodel, no guard, and
    // the first LMB draws instead of swinging.
    bool weaponDrawn() const { return weaponDrawn_; }

    // Ctrl toggles sneak: softer, quieter steps and halved detection —
    // at the price of a slow energy drain while moving.
    bool sneaking() const { return sneaking_; }

private:
    void tryAttack(const PlayerContext& ctx);
    // D2b: decides the mode, swims when swimming (3D wish toward the
    // look, surface clamp, energy drain, drowning once exhausted).
    // True = the frame's movement was consumed (no jump/dodge/sprint).
    bool updateSwimming(f32 dt, const PlayerContext& ctx, const Vec3& wish,
                        bool moving, f32 jog, f32 accelRate);
    // A4: advances the MeleeSwing machine and, through the Active window,
    // sweeps the blade segment against the NPC capsules (one code path
    // with the NPCs: gameplay/combat/MeleeSwing).
    void updateSwing(f32 dt, const PlayerContext& ctx);
    // The moved B6 hit: weapon damage through the GAS pipeline + the D2
    // crime-witnessing pass. Fires once per target per swing.
    void applyHit(const PlayerContext& ctx, Npc& target,
                  const data::WeaponForm& weapon);

    uptr<phys::CharacterBody> body_;
    Vec3 velocity { 0.0f };  // smoothed horizontal velocity (m/s)
    f32 jumpSpeed { 5.0f };  // fallback only — jumpPower stat drives it (C3)
    const data::WeaponForm* swingWeapon_ { nullptr };
    f32 sprintCostAccumulator { 0.0f };
    f32 strideAccumulator { 0.0f }; // C4a: grounded meters since last step
    // Dodge: tap-vs-hold on the sprint key, then the burst window.
    f32 shiftHeldSeconds { 0.0f };
    f32 dodgeTimer { 0.0f };
    Vec3 dodgeDir { 0.0f };
    bool weaponDrawn_ { true }; // R toggles; starts drawn (adventurer)
    // P0 D2b: swimming (decideMoveMode owns the transitions).
    gameplay::MoveMode moveMode_ { gameplay::MoveMode::Ground };
    f32 swimCostAccumulator { 0.0f };
    f32 drownAccumulator { 0.0f };
    // Sneak toggle (Ctrl) + its moving-only drain accumulator.
    bool sneaking_ { false };
    f32 sneakCostAccumulator { 0.0f };
};

} // namespace game
