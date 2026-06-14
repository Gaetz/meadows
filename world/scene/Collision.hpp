#pragma once

#include <functional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp"

namespace world {

// Do two axis-aligned boxes overlap?
bool aabbOverlap(Vec2 posA, Vec2 halfA, Vec2 posB, Vec2 halfB);

// Minimum translation to push box A out of box B (along the least-penetration
// axis). Zero if they do not overlap.
Vec2 minimumTranslation(Vec2 posA, Vec2 halfA, Vec2 posB, Vec2 halfB);

// Pushes every dynamic entity (Velocity + solid Collider) out of every solid
// Collider it overlaps, by the minimum translation. O(n²) — fine for the small
// 2D-phase entity counts.
void resolveCollisions(ecs::World& world);

// Calls `onOverlap(dynamic, trigger)` for each dynamic entity (Velocity +
// Collider) overlapping a trigger Collider. Used for pickups, doors, zones.
void forEachTriggerOverlap(
    ecs::World& world,
    const std::function<void(ecs::Entity dynamic, ecs::Entity trigger)>& onOverlap);

} // namespace world
