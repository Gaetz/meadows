#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/core/Defines.hpp"

// The physics seam (horizontal pass H3): a narrow facade over Jolt. NO
// Jolt type crosses this header (§3.1 pimpl rule) — swapping or upgrading
// the physics engine stays a .cpp affair. Headless by nature: the physics
// world belongs to the SIM side and is doctested without any renderer.
//
// HOW TO FILL (post-7/07, "socle 3D gameplay" vertical):
//  - cook cell collision (static meshes from placed StaticForms) into
//    addStaticMesh calls at cell load, remove on unload;
//  - drive the player/NPCs through CharacterBody::move from the intent
//    system (in-place animation decision: the controller owns motion);
//  - hook trigger volumes (TriggerForm) via sensor bodies + the callback;
//  - interaction/combat raycasts go through rayCast/shapeCast here, never
//    straight into Jolt.

namespace phys {

using BodyId = u64; // opaque; 0 = invalid

struct RayHit {
    bool hit { false };
    Vec3 position {};
    Vec3 normal {};
    f32 distance { 0.0f };
    BodyId body { 0 };
};

class PhysicsWorld {
public:
    PhysicsWorld();
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Fixed-step simulation (accumulates internally, 60 Hz substeps —
    // deterministic step count for a given dt sequence, §8).
    void tick(f32 dt);

    // Static colliders (world geometry). Boxes cover kit modules and
    // prototyping; triangle meshes arrive with the cell-cooking vertical.
    BodyId addStaticBox(const Vec3& halfExtents, const Vec3& position,
                        const Quat& rotation = { 1.0f, 0.0f, 0.0f, 0.0f });
    void removeBody(BodyId body);

    // First hit along a ray (interaction, camera, combat traces).
    RayHit rayCast(const Vec3& from, const Vec3& direction,
                   f32 maxDistance) const;

    struct Impl;
    Impl& impl() { return *pimpl; }

private:
    uptr<Impl> pimpl;
};

// A kinematic capsule character (Jolt CharacterVirtual): the controller
// OWNS motion (no root motion — decision 2026-07-05). Gravity and ground
// snapping are handled inside move().
class CharacterBody {
public:
    // `height` = total capsule height including caps; feet at `position`.
    CharacterBody(PhysicsWorld& world, f32 radius, f32 height,
                  const Vec3& position);
    ~CharacterBody();
    CharacterBody(const CharacterBody&) = delete;
    CharacterBody& operator=(const CharacterBody&) = delete;

    // Desired HORIZONTAL velocity (m/s); jumps add vertical impulse.
    void move(const Vec3& desiredVelocity, f32 dt);
    void jump(f32 speed);

    Vec3 position() const; // feet position
    bool onGround() const;

private:
    struct Impl;
    uptr<Impl> pimpl;
};

} // namespace phys
