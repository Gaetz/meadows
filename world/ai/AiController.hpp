#pragma once

#include "engine/core/Defines.hpp"
#include "engine/reflect/Reflect.hpp"

namespace ecs {
class World;
}

namespace ai {

// A simple AI agent (a reflected component): perceives within `perceptionRadius`
// and moves at `speed`. The 2D-phase "AI package" is just a chase for now;
// richer packages and navmesh pathfinding land later.
struct AiAgent {
    f32 perceptionRadius { 5.0f };
    f32 speed { 2.5f };

    REFLECT_BEGIN(AiAgent, void)
        REFLECT_FIELD(perceptionRadius)
        REFLECT_FIELD(speed)
    REFLECT_END()
};

void registerAiComponents(ecs::World& world);

// Chase package: every AiAgent that perceives `target` (within its radius)
// seeks it (sets its Velocity); otherwise it stops. Requires Transform +
// Velocity on the agent.
void updateChaseAi(ecs::World& world, Vec3 target);

} // namespace ai
