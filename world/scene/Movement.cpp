#include "world/scene/Movement.hpp"

#include "engine/ecs/World.hpp"
#include "world/scene/Components.hpp"

namespace world {

void applyMovement(ecs::World& world, f32 dt) {
    world.handle().query<Transform, const Velocity>().each(
        [dt](flecs::entity, Transform& transform, const Velocity& velocity) {
            transform.position += velocity.value * dt;
        });
}

} // namespace world
