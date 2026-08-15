#include "world/scene/Components.hpp"

#include "engine/ecs/World.hpp"
#include "world/ai/Perception.hpp"

namespace world {

void registerSceneComponents(ecs::World& world) {
    world.registerComponent<Transform>();
    world.registerComponent<SpriteRender>();
    world.registerComponent<RefId>();
    world.registerComponent<Velocity>();
    world.registerComponent<Collider>();
    // 3D-demo components.
    world.registerComponent<MeshRender>();
    world.registerComponent<LightSource>();
    world.registerComponent<TriggerVolume>();
    world.registerComponent<Floater>();
    world.registerComponent<MarkerKind>();
    world.registerComponent<FurnitureRef>();
    world.registerComponent<DoorTarget>();
    world.registerComponent<Perception>(); // reflected: alerts persist
    world.registerComponent<WaterVolume>();
    // Marker tags (StaticMarker/ItemMarker/ActorMarker) carry no fields and are
    // never serialized, so flecs auto-registers them on first use — they do not
    // go through the reflected-component bridge.
}

} // namespace world
