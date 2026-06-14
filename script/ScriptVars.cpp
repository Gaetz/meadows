#include "script/ScriptVars.hpp"

#include "engine/ecs/World.hpp"

namespace script {

void registerScriptComponents(ecs::World& world) {
    world.handle().component<ScriptVars>(); // runtime-only (container)
}

} // namespace script
