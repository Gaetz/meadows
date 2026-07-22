#include <doctest/doctest.h>

#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/ecs/World.hpp"
#include "world/scene/Components.hpp"
#include "world/streaming/CellStreamer.hpp"

// Distance-driven cell residency — load ring, hysteresis
// eviction, entities spawning/despawning with their cells. Headless.

namespace {

// A 16 m-cell worldspace with three authored cells on the X axis (grid
// 0, 1 and 5), one marker reference in each.
constexpr const char* kToml = R"toml(
[plugin]
id = "77777777-7777-4777-8777-777777777777"
name = "stream-base"

[[records]]
form = "70000000-0000-4000-8000-000000000001"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "Space"
cellSize = 16.0

[[records]]
form = "70000000-0000-4000-8000-000000000010"
type = "CellForm"
new = true
[records.fields]
worldspace = "70000000-0000-4000-8000-000000000001"
gridX = 0
gridY = 0

[[records]]
form = "70000000-0000-4000-8000-000000000011"
type = "CellForm"
new = true
[records.fields]
worldspace = "70000000-0000-4000-8000-000000000001"
gridX = 1
gridY = 0

[[records]]
form = "70000000-0000-4000-8000-000000000015"
type = "CellForm"
new = true
[records.fields]
worldspace = "70000000-0000-4000-8000-000000000001"
gridX = 5
gridY = 0

[[records]]
form = "70000000-0000-4000-8000-000000000020"
type = "MarkerForm"
new = true
[records.fields]
editorId = "Spot"
kind = "spawn"

[[records]]
form = "70000000-0000-4000-8000-000000000030"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "70000000-0000-4000-8000-000000000020"
cell = "70000000-0000-4000-8000-000000000010"

[[records]]
form = "70000000-0000-4000-8000-000000000031"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "70000000-0000-4000-8000-000000000020"
cell = "70000000-0000-4000-8000-000000000011"

[[records]]
form = "70000000-0000-4000-8000-000000000032"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "70000000-0000-4000-8000-000000000020"
cell = "70000000-0000-4000-8000-000000000015"
)toml";

u32 markerCount(const ecs::World& world) {
    u32 count = 0;
    world.handle().query<const world::MarkerKind>().each(
        [&](flecs::entity, const world::MarkerKind&) { ++count; });
    return count;
}

} // namespace

TEST_CASE("cell streamer keeps a ring loaded and evicts with hysteresis") {
    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(kToml, types, "stream");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    const world::WorldModel model = world::WorldModel::build(db);
    world::CellLoader loader { world, db, model, spawner, categories };
    world::CellStreamer streamer { loader, model, db };

    const data::FormHandle space = db.handleOf(
        *core::Guid::fromString("70000000-0000-4000-8000-000000000001"));
    REQUIRE(space.isValid());

    // Focus in cell 0 (x = 8): grid 0 and 1 are within the load radius 2,
    // grid 5 is not.
    CHECK(streamer.update(space, 8.0f, 8.0f));
    CHECK(streamer.loadedCount() == 2);
    CHECK(markerCount(world) == 2);
    // Same focus: nothing changes.
    CHECK_FALSE(streamer.update(space, 8.0f, 8.0f));

    // Jump to cell 5 (x = 88): cell 5 loads; 0 and 1 are beyond the
    // unload radius 3 and evict — with their entities.
    CHECK(streamer.update(space, 88.0f, 8.0f));
    CHECK(streamer.loadedCount() == 1);
    CHECK(markerCount(world) == 1);

    // Step back to cell 3 (x = 56): cell 1 re-enters the load ring,
    // cell 5 sits at distance 2 (kept), cell 0 at 3 (not loaded, radius
    // 2 — but not evicted territory either since it isn't loaded).
    CHECK(streamer.update(space, 56.0f, 8.0f));
    CHECK(streamer.loadedCount() == 2);
    CHECK(markerCount(world) == 2);

    streamer.unloadAll();
    CHECK(streamer.loadedCount() == 0);
    CHECK(markerCount(world) == 0);
}

TEST_CASE("cell streamer: a load budget spreads the ring over calls") {
    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(kToml, types, "stream");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    const world::WorldModel model = world::WorldModel::build(db);
    world::CellLoader loader { world, db, model, spawner, categories };
    world::CellStreamer streamer { loader, model, db };

    const data::FormHandle space = db.handleOf(
        *core::Guid::fromString("70000000-0000-4000-8000-000000000001"));

    // Two cells sit in the initial ring; budget 1 = one per call, and the
    // incomplete ring keeps reporting work even with a static focus.
    CHECK(streamer.update(space, 8.0f, 8.0f, 2, 3, 1));
    CHECK(streamer.loadedCount() == 1);
    CHECK(streamer.update(space, 8.0f, 8.0f, 2, 3, 1));
    CHECK(streamer.loadedCount() == 2);
    // Ring complete: quiet again.
    CHECK_FALSE(streamer.update(space, 8.0f, 8.0f, 2, 3, 1));
    CHECK(streamer.loadedCount() == 2);
}

// A materialized (implicit) cell and the streamer.
TEST_CASE("cell streamer: adopted implicit cells are ordinary residents") {
    data::FormTypeRegistry types;
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(kToml, types, "stream");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    world::WorldModel model = world::WorldModel::build(db); // mutable: materialize
    world::CellLoader loader { world, db, model, spawner, categories };
    world::CellStreamer streamer { loader, model, db };

    const data::FormHandle space = db.handleOf(
        *core::Guid::fromString("70000000-0000-4000-8000-000000000001"));
    REQUIRE(space.isValid());

    // Ring around cell 0: authored 0 and 1 load.
    CHECK(streamer.update(space, 8.0f, 8.0f));
    CHECK(streamer.loadedCount() == 2);

    // Editor placement inside the ring: materialize (2, 0) + adopt. Loaded
    // NOW, once — adopting again is a no-op.
    const data::FormHandle placed = model.materializeCell(db, space, 2, 0);
    REQUIRE(placed.isValid());
    streamer.adopt(placed);
    CHECK(streamer.loadedCount() == 3);
    CHECK(loader.cellEntity(placed).is_alive());
    streamer.adopt(placed);
    CHECK(streamer.loadedCount() == 3);
    // The ring did not move: quiet, and no double-load of the adoptee.
    CHECK_FALSE(streamer.update(space, 8.0f, 8.0f));

    // A materialized cell NOT adopted streams in like an authored one the
    // moment the ring reaches it (the streamer needs no change).
    const data::FormHandle far = model.materializeCell(db, space, 7, 0);
    REQUIRE(far.isValid());
    CHECK_FALSE(loader.cellEntity(far).is_alive());

    // Focus jumps to cell 7 (x = 120): the far square streams in; 0, 1 and
    // the ADOPTED (2, 0) all sit beyond the unload radius and evict —
    // eviction manages adoptees exactly like authored residents.
    CHECK(streamer.update(space, 120.0f, 8.0f));
    CHECK(loader.cellEntity(far).is_alive());
    CHECK_FALSE(loader.cellEntity(placed).is_alive());
    CHECK(streamer.loadedCount() == 2); // (5, 0) at distance 2, and (7, 0)
}
