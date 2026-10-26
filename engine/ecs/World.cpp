#include "engine/ecs/World.hpp"

namespace ecs {

const reflect::TypeInfo* reflectedComponentOf(flecs::id id) {
    if (id.raw_id() == 0 || !id.is_entity()) {
        return nullptr;
    }
    const flecs::entity component = id.entity();
    if (!component.is_valid()) {
        return nullptr;
    }
    const ReflectedComponent* link = component.try_get<ReflectedComponent>();
    return link ? link->type : nullptr;
}

const reflect::TypeInfo* World::reflectedComponent(flecs::id_t componentId) const {
    return reflectedComponentOf(flecs::id { world.c_ptr(), componentId });
}

} // namespace ecs
