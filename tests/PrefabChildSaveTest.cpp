#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "game/SaveGame.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Spawner.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldModel.hpp"

// Prefab-derived children persist. Their references exist
// in NO plugin (derived guids), so the save materializes them as full
// `creates` records — and the expansion steps aside when such a record
// exists (or the pending layer vetoes them in-session).

namespace {

// Same fixture as PrefabTest: one prefab with two sword children, placed
// once in one cell.
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

[[records]]
form = "ffff0004-0000-4000-8000-000000000002"
type = "ReferenceForm"
new = true
[records.fields]
prefab = "ffff0004-0000-4000-8000-000000000001"
baseForm = "ffff0003-0000-4000-8000-000000000001"
position = [1.0, 0.0, 0.0]

[[records]]
form = "ffff0004-0000-4000-8000-000000000003"
type = "ReferenceForm"
new = true
[records.fields]
prefab = "ffff0004-0000-4000-8000-000000000001"
baseForm = "ffff0003-0000-4000-8000-000000000001"
position = [-1.0, 0.0, 0.0]

[[records]]
form = "ffff0005-0000-4000-8000-000000000001"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "ffff0004-0000-4000-8000-000000000001"
cell = "ffff0002-0000-4000-8000-000000000001"
position = [10.0, 0.0, 5.0]
)";

const core::Guid kInstance =
    *core::Guid::fromString("ffff0005-0000-4000-8000-000000000001");
const core::Guid kChildA =
    *core::Guid::fromString("ffff0004-0000-4000-8000-000000000002");

struct Fixture {
    data::FormTypeRegistry types;
    data::Plugin base;
    ecs::World world;
    world::FormCategoryRegistry categories;
    world::Spawner spawner;

    Fixture() {
        data::registerCoreFormTypes(types);
        world::registerWorldFormTypes(types);
        base = *data::parsePluginToml(kToml, types, "prefabs");
        world::registerSceneComponents(world);
        world::registerCoreCategories(categories);
        world::registerCoreSpawners(spawner);
    }
};

u32 childCount(ecs::World& world) {
    u32 count = 0;
    world.handle().query<const world::RefId>().each(
        [&](flecs::entity e, const world::RefId& ref) {
            const ecs::Entity entity { e };
            if (!entity.has<world::PrefabRootMarker>() &&
                ref.referenceId.isValid()) {
                ++count;
            }
        });
    return count;
}

ecs::Entity findByRef(ecs::World& world, const core::Guid& refGuid) {
    ecs::Entity found {};
    world.handle().query<const world::RefId>().each(
        [&](flecs::entity e, const world::RefId& ref) {
            if (ref.referenceId == refGuid) {
                found = ecs::Entity { e };
            }
        });
    return found;
}

} // namespace

TEST_CASE("prefab child save: a picked-up child stays gone across a save") {
    const core::Guid derivedA = core::Guid::combine(kInstance, kChildA);

    // Session 1: spawn, pick up child A (pending veto), verify the save
    // record materializes as a full create.
    Fixture one;
    data::FormDatabase db;
    data::resolve({ &one.base }, one.types, db);
    const auto model = world::WorldModel::build(db);
    game::PendingSaveLayer pending;
    world::CellLoader loader { one.world, db, model, one.spawner,
                               one.categories };
    loader.spawnFilter = [&](const core::Guid& id) {
        return pending.isEnabled(id);
    };
    loader.loadAll();
    CHECK(childCount(one.world) == 2); // instance root excluded

    ecs::Entity childEntity = findByRef(one.world, derivedA);
    REQUIRE(childEntity.is_alive());
    pending.disableReference(derivedA, db, childEntity);
    childEntity.destruct();

    // In-session reload: the expansion consults the pending veto.
    loader.unloadAll();
    loader.loadAll();
    CHECK(childCount(one.world) == 1);
    CHECK_FALSE(findByRef(one.world, derivedA).is_alive());

    // The flush carries the disable; as a PATCH it would be an orphan —
    // resolve it as a save layer and verify the child record EXISTS
    // disabled (materialized) or the expansion skips it.
    data::Plugin save;
    save.name = "slot";
    save.records = pending.flush();
    const str toml = data::writePluginToml(save, one.types);
    const auto reparsed = data::parsePluginToml(toml, one.types, "slot");
    REQUIRE(reparsed.has_value());

    // Session 2: fresh resolve with the save layer last.
    Fixture two;
    data::FormDatabase db2;
    data::resolve({ &two.base, &*reparsed }, two.types, db2);
    const auto model2 = world::WorldModel::build(db2);
    world::CellLoader loader2 { two.world, db2, model2, two.spawner,
                                two.categories };
    loader2.loadAll();
    CHECK_FALSE(findByRef(two.world, derivedA).is_alive());
    CHECK(childCount(two.world) == 1); // the untouched sibling only
}

TEST_CASE("prefab child save: a captured child materializes as a create") {
    const core::Guid derivedA = core::Guid::combine(kInstance, kChildA);

    Fixture one;
    data::FormDatabase db;
    data::resolve({ &one.base }, one.types, db);
    const auto model = world::WorldModel::build(db);
    world::CellLoader loader { one.world, db, model, one.spawner,
                               one.categories };
    loader.loadAll();
    ecs::Entity child = findByRef(one.world, derivedA);
    REQUIRE(child.is_alive());

    const auto record = game::captureReference(child, db);
    REQUIRE(record.has_value());
    CHECK(record->creates); // full record, not an orphan patch
    CHECK(record->formId == derivedA);
    const u32 baseFormId =
        world::ReferenceForm::staticTypeInfo().findField("baseForm")->id;
    CHECK(record->fields.contains(baseFormId));
}
