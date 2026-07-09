#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::Entity

namespace platform {
class Input;
}
namespace render {
class FlyCamera;
}
namespace data {
class FormDatabase;
struct WeaponForm;
}
namespace phys {
class PhysicsWorld;
class CharacterBody;
}
namespace gameplay {
class GameplayTagRegistry;
class DerivedStatRegistry;
struct StatsTuningForm;
struct EffectForm;
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
    const gameplay::EffectForm* sprintCostEffect; // §2.9: energy only moves here
    const data::WeaponForm* fallbackWeapon;       // pre-equipment default
    vector<uptr<Npc>>& npcs;          // melee targets + crime witnesses
    InteractionController& interaction; // fading gate + the crime toast
    bool overencumbered; // C3: no sprint, no jump (computed at the
                         //   equipMods site — it also feeds the tick)
    std::function<void()> syncWantedTag; // Crime.Wanted mirror stays with the
                                         //   scene's quest/crime wiring
};

// The first-person Play-mode controller extracted from LandscapeScene
// (audit U4-1): owns the kinematic capsule and the movement state
// (smoothed velocity, attack cooldown, sprint-cost accumulator), runs
// mouselook / camera-relative movement / jump / sprint cost (through the
// SprintCost GameplayEffect, §2.9) and the LMB melee swing with the D2
// crime-witnessing pass. MODE transitions stay in the scene (enter/exit/
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

private:
    void tryAttack(const PlayerContext& ctx);

    uptr<phys::CharacterBody> body_;
    Vec3 velocity { 0.0f };  // smoothed horizontal velocity (m/s)
    f32 jumpSpeed { 5.0f };  // fallback only — jumpPower stat drives it (C3)
    f32 attackCooldown { 0.0f };
    f32 sprintCostAccumulator { 0.0f };
};

} // namespace game
