#include "engine/ecs/World.hpp"

namespace ecs {

const reflect::TypeInfo* World::reflectedComponent(flecs::id_t componentId) const {
    const auto it = reflectedComponents.find(componentId);
    return it != reflectedComponents.end() ? it->second : nullptr;
}

} // namespace ecs
