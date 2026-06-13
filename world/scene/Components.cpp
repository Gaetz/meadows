#include "world/scene/Components.hpp"

#include "engine/ecs/World.hpp"

namespace world {

void registerSceneComponents(ecs::World& world) {
    world.registerComponent<Transform>();
    world.registerComponent<SpriteRender>();
    world.registerComponent<RefId>();
    // Marker tags (StaticMarker/ItemMarker/ActorMarker) carry no fields and are
    // never serialized, so flecs auto-registers them on first use — they do not
    // go through the reflected-component bridge.
}

} // namespace world
