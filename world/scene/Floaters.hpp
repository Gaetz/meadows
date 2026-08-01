#pragma once

#include <functional>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/ecs/World.hpp" // ecs::World

// Kinematic drift for Floater entities (Components.hpp): stick to the
// water surface, ride the current, bob a little. Headless — the scene
// hands in the same surface/flow callbacks the swim controller uses, so
// a crate and a swimmer always agree on the water.

namespace world {

using WaterSurfaceFn = std::function<std::optional<f32>(const Vec3&)>;
using WaterFlowFn = std::function<Vec2(const Vec3&)>;

// Advances every [Transform, Floater] entity by dt. Entities over dry
// land are left untouched (they rest wherever they are). The bob phase
// derives from the entity id — deterministic, no RNG.
void updateFloaters(ecs::World& world, f32 dt, f32 timeSeconds,
                    const WaterSurfaceFn& surface,
                    const WaterFlowFn& flow);

} // namespace world
