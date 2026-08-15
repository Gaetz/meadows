#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Spawner.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldModel.hpp"

// Prefab expansion — a placed reference whose base is a PrefabForm
// spawns its template children with DERIVED deterministic guids and
// composed transforms; templates never spawn on their own.

namespace {

constexpr const char* kToml = R"(
[plugin]
id = "ffff0000-0000-4000-8000-000000000001"
name = "prefabs"

[[records]]
form = "ffff0001-0000-4000-8000-000000000001"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "World"

[[records]]
form = "ffff0002-0000-4000-8000-000000000001"
type = "CellForm"
new = true
[records.fields]
editorId = "Cell"
worldspace = "ffff0001-0000-4000-8000-000000000001"

[[records]]
form = "ffff0003-0000-4000-8000-000000000001"
type = "WeaponForm"
new = true
[records.fields]
editorId = "CampSword"

[[records]]
form = "ffff0004-0000-4000-8000-000000000001"
type = "PrefabForm"
new = true
[records.fields]
editorId = "Camp"

# Two template children (relative to the prefab pivot).
[[records]]
form = "ffff0004-0000-4000-8000-000000000002"
type = "ReferenceForm"
new = true
[records.fields]
editorId = "CampSwordA"
prefab = "ffff0004-0000-4000-8000-000000000001"
baseForm = "ffff0003-0000-4000-8000-000000000001"
position = [1.0, 0.0, 0.0]

[[records]]
form = "ffff0004-0000-4000-8000-000000000003"
type = "ReferenceForm"
new = true
[records.fields]
editorId = "CampSwordB"
prefab = "ffff0004-0000-4000-8000-000000000001"
baseForm = "ffff0003-0000-4000-8000-000000000001"
position = [-1.0, 0.0, 0.0]

# The placed prefab instance.
[[records]]
form = "ffff0005-0000-4000-8000-000000000001"
type = "ReferenceForm"
new = true
[records.fields]
editorId = "CampAtLake"
baseForm = "ffff0004-0000-4000-8000-000000000001"
cell = "ffff0002-0000-4000-8000-000000000001"
position = [10.0, 0.0, 5.0]
scale = [2.0, 2.0, 2.0]
)";

} // namespace

TEST_CASE("guid combine is deterministic, order-sensitive and valid") {
    const core::Guid a = *core::Guid::fromString(
        "11111111-1111-4111-8111-111111111111");
    const core::Guid b = *core::Guid::fromString(
        "22222222-2222-4222-8222-222222222222");
    const core::Guid ab = core::Guid::combine(a, b);
    CHECK(ab == core::Guid::combine(a, b)); // stable
    CHECK(ab != core::Guid::combine(b, a)); // order matters
    CHECK(ab != a);
    CHECK(ab.isValid());
    // Version/variant bits stay canonical (string form round-trips).
    CHECK(core::Guid::fromString(ab.toString()) == ab);
}

TEST_CASE("prefab instances expand templates with derived guids") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    world::registerWorldFormTypes(types);
    const auto plugin = data::parsePluginToml(kToml, types, "prefabs");
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

    // Expected: 1 prefab root + 2 expanded children (templates themselves
    // never spawn — they belong to no cell AND carry `prefab`).
    const core::Guid instanceId = *core::Guid::fromString(
        "ffff0005-0000-4000-8000-000000000001");
    const core::Guid templateA = *core::Guid::fromString(
        "ffff0004-0000-4000-8000-000000000002");
    u32 roots = 0;
    u32 children = 0;
    bool foundDerivedA = false;
    world.handle()
        .query<const world::RefId, const world::Transform>()
        .each([&](flecs::entity entity, const world::RefId& ref,
                  const world::Transform& transform) {
            if (entity.has<world::PrefabRootMarker>()) {
                ++roots;
                CHECK(ref.referenceId == instanceId);
                return;
            }
            ++children;
            if (ref.referenceId ==
                core::Guid::combine(instanceId, templateA)) {
                foundDerivedA = true;
                // Instance transform composed: 10 + 1*2 (scaled template).
                CHECK(transform.position.x == doctest::Approx(12.0f));
                CHECK(transform.position.z == doctest::Approx(5.0f));
                CHECK(transform.scale.x == doctest::Approx(2.0f));
            }
        });
    CHECK(roots == 1);
    CHECK(children == 2);
    CHECK(foundDerivedA);
}
