#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/ecs/World.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Spawner.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "world/worldspace/WorldModel.hpp"

using core::Guid;

namespace {

const Guid kCell = *Guid::fromString("22220000-0000-4000-8000-000000000000");

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    world::registerWorldFormTypes(types);
    return types;
}

// A worldspace with one cell holding three sword references — one disabled.
const char* kBase = R"toml(
[plugin]
id = "11111111-1111-4111-8111-111111111111"
name = "base"

[[records]]
form = "11110000-0000-4000-8000-000000000001"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "Overworld"

[[records]]
form = "22220000-0000-4000-8000-000000000000"
type = "CellForm"
new = true
[records.fields]
editorId = "Cell_0_0"
worldspace = "11110000-0000-4000-8000-000000000001"

[[records]]
form = "3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f482"
type = "WeaponForm"
new = true
[records.fields]
editorId = "IronSword"

[[records]]
form = "44440000-0000-4000-8000-000000000001"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f482"
cell = "22220000-0000-4000-8000-000000000000"
position = [-2.0, 1.0, 0.0]

[[records]]
form = "44440000-0000-4000-8000-000000000002"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f482"
cell = "22220000-0000-4000-8000-000000000000"

[[records]]
form = "44440000-0000-4000-8000-000000000003"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f482"
cell = "22220000-0000-4000-8000-000000000000"
enabled = false
)toml";

i32 spawnedCount(ecs::World& world) {
    i32 n = 0;
    world.handle().query<const world::Transform>().each(
        [&](flecs::entity, const world::Transform&) { ++n; });
    return n;
}

} // namespace

TEST_CASE("cell loader: loads enabled references, skips disabled ones") {
    const auto types = makeTypes();
    auto plugin = data::parsePluginToml(kBase, types, "base");
    REQUIRE(plugin.has_value());

    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);
    const auto model = world::WorldModel::build(db);

    ecs::World world;
    world::registerSceneComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);

    world::CellLoader loader { world, db, model, spawner, categories };
    loader.loadAll();

    // Two enabled references spawned, the disabled one skipped.
    CHECK(spawnedCount(world) == 2);

    const auto cell = db.handleOf(kCell);
    CHECK(loader.cellEntity(cell).is_alive());
}

TEST_CASE("cell loader: unloadCell removes the cell's entities") {
    const auto types = makeTypes();
    auto plugin = data::parsePluginToml(kBase, types, "base");
    REQUIRE(plugin.has_value());

    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);
    const auto model = world::WorldModel::build(db);

    ecs::World world;
    world::registerSceneComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);

    world::CellLoader loader { world, db, model, spawner, categories };
    loader.loadAll();
    REQUIRE(spawnedCount(world) == 2);

    loader.unloadCell(db.handleOf(kCell));
    CHECK(spawnedCount(world) == 0);
    CHECK_FALSE(loader.cellEntity(db.handleOf(kCell)).is_alive());
}
