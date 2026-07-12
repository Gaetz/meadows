#include "world/scene/SpatialIndex.hpp"

#include <cmath>

#include <glm/glm.hpp>

#include "world/scene/Components.hpp" // Transform, ActorMarker

namespace world {

void SpatialIndex::rebuild(ecs::World& world) {
    for (auto& [cell, entries] : cells) {
        entries.clear(); // keep the buckets' capacity across frames
    }
    count = 0;
    world.handle()
        .query_builder<const Transform>()
        .with<ActorMarker>()
        .build()
        .each([&](flecs::entity entity, const Transform& transform) {
            cells[key(cellOf(transform.position.x),
                      cellOf(transform.position.z))]
                .push_back({ ecs::Entity { entity }, transform.position });
            ++count;
        });
}

void SpatialIndex::queryRadius(const Vec3& center, f32 radius,
                               vector<Entry>& out) const {
    if (radius < 0.0f) {
        return;
    }
    const f32 radiusSq = radius * radius;
    const i32 minX = cellOf(center.x - radius);
    const i32 maxX = cellOf(center.x + radius);
    const i32 minZ = cellOf(center.z - radius);
    const i32 maxZ = cellOf(center.z + radius);
    for (i32 x = minX; x <= maxX; ++x) {
        for (i32 z = minZ; z <= maxZ; ++z) {
            const auto it = cells.find(key(x, z));
            if (it == cells.end()) {
                continue;
            }
            for (const Entry& entry : it->second) {
                const Vec3 gap = entry.position - center;
                if (glm::dot(gap, gap) <= radiusSq) {
                    out.push_back(entry);
                }
            }
        }
    }
}

} // namespace world
