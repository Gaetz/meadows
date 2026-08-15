#pragma once

#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity
#include "game/InputActions.hpp" // game::ActionMap
#include "game/Settings.hpp"     // game::Settings
#include "gameplay/actors/Swimming.hpp" // gameplay::MoveMode (D2b)
#include "gameplay/combat/PlayerAction.hpp" // gameplay::PlayerAction

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

namespace core {
class Rng;
}

namespace game {

struct Npc;
class InteractionController;
class ProjectileDirector;

// The scene systems the first-person player touches, bundled so the whole
// Play-mode controller (movement / jump / sprint cost / melee + crime) is
// decoupled from LandscapeScene. The scene rebuilds it each
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
    const data::TextTable& texts; // Player-facing strings by key
    const gameplay::EffectForm* sprintCostEffect; // §2.9: energy only moves here
    const data::WeaponForm* fallbackWeapon;       // pre-equipment default
    // The melee attack ability (energy cost + cooldown effects, §6)
    // gates every swing; the swing itself is the MeleeSwing component.
    const gameplay::AbilityForm* attackAbility;
    // Dodge (the 2D arena ability in 3D): cost/cooldown/i-frames are its
    // effects; the controller only drives the burst.
    const gameplay::AbilityForm* dodgeAbility;
    vector<uptr<Npc>>& npcs;          // melee targets + crime witnesses
    InteractionController& interaction; // fading gate + the crime toast
    bool overencumbered; // no sprint, no jump (computed at the
                         //   equipMods site — it also feeds the tick)
    std::function<void()> syncWantedTag; // Crime.Wanted mirror stays with the
                                         //   scene's quest/crime wiring
    // The player has no walk clip — footsteps fire as
    // "AnimEvent"/Footstep on the bus every strideLength meters walked.
    gameplay::EventBus* eventBus { nullptr };
    // Feedback cues (hit/block/parry) — the FxDirector's registry.
    gameplay::CueRegistry* cues { nullptr };
    // Injury rolls on landed hits (§8 seeded — the scene's combat RNG).
    core::Rng& combatRng;
    // The water surface (if any) over a world position — sea
    // level + placed WaterVolumes; the scene owns the geometry, the
    // controller only asks (the TriggerSystem callback pattern).
    std::function<std::optional<f32>(const Vec3&)> waterSurfaceAt;
    // Energy drain while swimming (§2.9: only effects move energy).
    const gameplay::EffectForm* swimCostEffect { nullptr };
    // Sneak: the drain while MOVING sneaked (SneakCost, ~3 energy/s).
    const gameplay::EffectForm* sneakCostEffect { nullptr };
    // Where fired arrows go (a ranged weapon = projectileSpeed > 0).
    ProjectileDirector* projectiles { nullptr };
    // The drain while the bow is DRAWN (3 energy/s, data).
    const gameplay::EffectForm* bowDrawCostEffect { nullptr };
    // The action layer + machine preferences — the controller reads
    // INTENTIONS (attack/block/dodge...), never raw keys; the settings
    // drive look sensitivity / invert / stick feel. Both owned by the
    // scene, always set (null only in never-built test contexts).
    const ActionMap* actions { nullptr };
    const Settings* settings { nullptr };
    // XZ current at a spot (rivers push, lakes/sea are still) — the
    // swim drift. Absent = no current (interiors, tests).
    std::function<Vec2(const Vec3&)> waterFlowAt;
};

// The first-person Play-mode controller extracted from LandscapeScene
//: owns the kinematic capsule and the movement state
// (smoothed velocity, swing weapon, sprint-cost accumulator), runs
// mouselook / camera-relative movement / jump / sprint cost (through the
// SprintCost GameplayEffect, §2.9) and the LMB melee swing —
// the ability-gated MeleeSwing whose blade must TOUCH (segment vs
// capsule) — with the crime-witnessing pass. MODE transitions stay in the scene (enter/exit/
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

    // The weapon the in-flight swing was activated with (null when
    // Idle) — the scene's viewmodel takes its timings and model from it.
    const data::WeaponForm* swingWeapon() const { return swingWeapon_; }

    // R toggles drawn/sheathed; sheathed = no viewmodel, no guard, and
    // the first LMB draws instead of swinging.
    bool weaponDrawn() const { return weaponDrawn_; }

    // Ctrl toggles sneak: softer, quieter steps and halved detection —
    // at the price of a slow energy drain while moving.
    bool sneaking() const { return sneaking_; }

    // The bow draw — 0..1 while LMB is held on a ranged weapon,
    // -1 when not drawing (the HUD gauge and the viewmodel arrow read
    // this).
    f32 bowCharge() const { return bowCharge_; }

private:
    // What the stance half hands the locomotion half — the frame's
    // ONE action (gameplay::decidePlayerAction owns every exclusion)
    // plus the facts both halves read.
    struct StanceFrame {
        gameplay::PlayerAction action { gameplay::PlayerAction::Idle };
        bool staggered { false };    // State.Staggered, read once (§4)
        bool rangedWeapon { false }; // equipped weapon fires projectiles
    };
    // The stance half of the frame — R draw/sheathe (the ONLY
    // weaponDrawn_ writer), Ctrl sneak, the action decision, the guard
    // clock + State.Blocking sync, and the bow-vs-melee input dispatch.
    StanceFrame updateStance(f32 dt, const PlayerContext& ctx);
    // The locomotion half — mouselook, wish/speeds, the swim
    // early-out, dodge tap + burst, jump, strides, drains, and the
    // camera/entity-transform sync tail.
    void updateLocomotion(f32 dt, const PlayerContext& ctx,
                          const StanceFrame& frame);
    void tryAttack(const PlayerContext& ctx);
    // The equipped weapon (inventory), or the context fallback.
    const data::WeaponForm* equippedWeapon(const PlayerContext& ctx) const;
    // The charged shot — LMB held draws (gauge + drain), release
    // fires at a force proportional to the draw; exhaustion or a
    // stagger lets go early. `inhibited` = guarding or reeling.
    void updateBowDraw(f32 dt, const PlayerContext& ctx,
                       const data::WeaponForm& weapon, bool inhibited);
    // D2b: decides the mode, swims when swimming (3D wish toward the
    // look, surface clamp, energy drain, drowning once exhausted).
    // True = the frame's movement was consumed (no jump/dodge/sprint).
    bool updateSwimming(f32 dt, const PlayerContext& ctx, f32 jog,
                        f32 accelRate);
    // Advances the MeleeSwing machine and, through the Active window,
    // sweeps the blade segment against the NPC capsules (one code path
    // with the NPCs: gameplay/combat/MeleeSwing).
    void updateSwing(f32 dt, const PlayerContext& ctx);
    // The weapon hit: weapon damage through resolveMeleeStrike.
    // Fires once per target per swing.
    void applyHit(const PlayerContext& ctx, Npc& target,
                  const data::WeaponForm& weapon);
    // Crime v1: hitting a peaceful NPC in front of a witness raises
    // the bounty.
    void witnessCrime(const PlayerContext& ctx, const Npc& target,
                      const Vec3& playerEye);

    uptr<phys::CharacterBody> body_;
    Vec3 velocity { 0.0f };  // smoothed horizontal velocity (m/s)
    f32 jumpSpeed { 5.0f };  // fallback only — the jumpPower stat drives it
    const data::WeaponForm* swingWeapon_ { nullptr };
    f32 sprintCostAccumulator { 0.0f };
    f32 strideAccumulator { 0.0f }; // C4a: grounded meters since last step
    // Dodge: tap-vs-hold on the sprint key, then the burst window.
    f32 shiftHeldSeconds { 0.0f };
    f32 dodgeTimer { 0.0f };
    Vec3 dodgeDir { 0.0f };
    bool weaponDrawn_ { true }; // R toggles; starts drawn (adventurer)
    // Swimming (decideMoveMode owns the transitions).
    gameplay::MoveMode moveMode_ { gameplay::MoveMode::Ground };
    f32 swimCostAccumulator { 0.0f };
    f32 drownAccumulator { 0.0f };
    // Fall damage: highest airborne Y since leaving the ground; the
    // air->ground edge pays fallDamage(peak - landing) through applyDamage
    // (water landings never hurt — the swim path resets the tracker).
    bool wasGrounded_ { true };
    f32 fallPeakY_ { 0.0f };
    // Sneak toggle (Ctrl) + its moving-only drain accumulator.
    bool sneaking_ { false };
    f32 sneakCostAccumulator { 0.0f };
    // The bow draw (charge -1 = idle) + its drain accumulator.
    f32 bowCharge_ { -1.0f };
    f32 bowDrawAccumulator { 0.0f };
};

} // namespace game
