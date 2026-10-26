#include "gameplay/actors/ScriptVars.hpp"

#include "engine/ecs/World.hpp"

namespace gameplay {

void registerScriptComponents(ecs::World& world) {
    world.handle().component<ScriptVars>();
}

} // namespace gameplay
