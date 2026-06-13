#pragma once

#include "engine/core/Defines.hpp"

namespace ecs {
class World;
}

namespace world {

// Integrates Transform by Velocity (`position += velocity * dt`) for every
// entity that has both. The simplest movement system; collision/triggers land
// in a later brick.
void applyMovement(ecs::World& world, f32 dt);

} // namespace world
