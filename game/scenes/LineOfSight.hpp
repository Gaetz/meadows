#pragma once

#include <glm/glm.hpp>

#include "engine/core/Defines.hpp"
#include "engine/physics/Physics.hpp"

namespace game {

// The B5 LOS idiom, NAMED (chantier propreté P0 R2 — it was hand-rolled
// inline in perception and the crime-witness pass): a world-geometry
// raycast from `eye` to `target`. Actors live OUTSIDE the Jolt broadphase,
// so only walls/terrain block. `slack` forgives hits within that many
// meters of the target — railings or props at the target's feet don't
// blind anyone. [cpp-tuning]
inline bool hasLineOfSight(const phys::PhysicsWorld& physics,
                           const Vec3& eye, const Vec3& target,
                           f32 slack = 0.6f) {
    const Vec3 to = target - eye;
    const f32 distance = glm::length(to);
    if (distance < 1e-3f) {
        return true; // on top of each other: nothing to occlude
    }
    const phys::RayHit hit = physics.rayCast(eye, to / distance, distance);
    return !(hit.hit && hit.distance < distance - slack);
}

} // namespace game
