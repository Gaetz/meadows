#include "gameplay/actors/ActorKit.hpp"

namespace gameplay {

void registerActorKit(const flecs::world& world) {
    forEachActorKitType([&](auto tag) {
        using T = typename decltype(tag)::type;
        if constexpr (ReflectedKitComponent<T>) {
            ecs::registerReflectedComponent<T>(world);
        } else {
            world.component<T>(); // runtime-only: flecs storage, no reflection
        }
    });
}

void registerActorKit(ecs::World& world) {
    registerActorKit(world.handle());
}

void attachActorKit(ecs::World& world, ecs::Entity entity) {
    registerActorKit(world);
    forEachActorKitType([&](auto tag) {
        using T = typename decltype(tag)::type;
        if (!entity.has<T>()) {
            entity.set<T>(T {});
        }
    });
}

} // namespace gameplay
