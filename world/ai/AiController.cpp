#include "world/ai/AiController.hpp"

#include "engine/ecs/World.hpp"
#include "world/ai/Steering.hpp"
#include "world/scene/Components.hpp"

namespace ai {

void registerAiComponents(ecs::World& world) {
    world.registerComponent<AiAgent>();
}

void updateChaseAi(ecs::World& world, Vec3 target) {
    world.handle()
        .query<const world::Transform, const AiAgent, world::Velocity>()
        .each([&](flecs::entity, const world::Transform& transform,
                  const AiAgent& agent, world::Velocity& velocity) {
            if (withinRange(transform.position, target, agent.perceptionRadius)) {
                velocity.value = seek(transform.position, target, agent.speed);
            } else {
                velocity.value = { 0.0f, 0.0f, 0.0f };
            }
        });
}

} // namespace ai
