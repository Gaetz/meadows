#include <doctest/doctest.h>

#include <algorithm>

#include <glm/glm.hpp>

#include "world/scene/Components.hpp"
#include "world/scene/SpatialIndex.hpp"

// Chantier P0 B1 — the shared actor grid: radius queries over a
// per-frame snapshot, cell boundaries included.

namespace {

ecs::Entity spawnActor(ecs::World& world, const Vec3& position) {
    ecs::Entity entity = world.create();
    entity.set<world::Transform>({ .position = position });
    entity.add<world::ActorMarker>();
    return entity;
}

bool contains(const vector<world::SpatialIndex::Entry>& entries,
              ecs::Entity entity) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const world::SpatialIndex::Entry& entry) {
                           return entry.entity == entity;
                       });
}

} // namespace

TEST_CASE("the spatial index answers radius queries across cell borders") {
    ecs::World world;
    world::registerSceneComponents(world);
    const ecs::Entity near = spawnActor(world, { 1.0f, 0.0f, 1.0f });
    // 3.9 m away but in the NEIGHBOR cell (4 m grid): the query must not
    // stop at the bucket edge.
    const ecs::Entity acrossCell = spawnActor(world, { 4.5f, 0.0f, 1.0f });
    const ecs::Entity far = spawnActor(world, { 40.0f, 0.0f, 40.0f });
    // Vertical offset counts: 3D distance, planar bucketing.
    const ecs::Entity above = spawnActor(world, { 1.0f, 10.0f, 1.0f });

    world::SpatialIndex index;
    index.rebuild(world);
    CHECK(index.size() == 4);

    vector<world::SpatialIndex::Entry> found;
    index.queryRadius({ 1.0f, 0.0f, 1.0f }, 5.0f, found);
    CHECK(contains(found, near));
    CHECK(contains(found, acrossCell));
    CHECK(!contains(found, far));
    CHECK(!contains(found, above)); // 10 m up > 5 m radius

    // A non-actor entity is invisible to the index.
    ecs::Entity prop = world.create();
    prop.set<world::Transform>({ .position = Vec3 { 1.0f, 0.0f, 1.0f } });
    index.rebuild(world);
    CHECK(index.size() == 4);

    // Despawned actors vanish on the next rebuild.
    const_cast<ecs::Entity&>(far).destruct();
    index.rebuild(world);
    CHECK(index.size() == 3);
}
