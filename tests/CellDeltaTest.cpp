#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/ecs/World.hpp"
#include "game/SaveGame.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/save/SaveState.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Spawner.hpp"
#include "world/streaming/CellLoader.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "world/worldspace/WorldModel.hpp"

// The pending in-memory layer — a looted/battered cell
// remembers its state across unload/reload, without any disk.

using core::Guid;

namespace {

const Guid kCell = *Guid::fromString("22220000-0000-4000-8000-000000000000");
const Guid kItemRef =
    *Guid::fromString("44440000-0000-4000-8000-000000000001");
const Guid kActorRef =
    *Guid::fromString("44440000-0000-4000-8000-000000000002");
const Guid kSword =
    *Guid::fromString("3d8b1f6a-92c4-4e07-b8d9-1a5c7e30f482");

constexpr const char* kBase = R"toml(
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
position = [3.0, 0.0, 3.0]
)toml";

ecs::Entity findByRef(ecs::World& world, const Guid& refGuid) {
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

TEST_CASE("cell delta: loot + damage survive unload/reload without disk") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    world::registerWorldFormTypes(types);
    gameplay::registerSaveFormTypes(types);
    const auto plugin = data::parsePluginToml(kBase, types, "base");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    const world::WorldModel model = world::WorldModel::build(db);
    world::CellLoader loader { world, db, model, spawner, categories };

    gameplay::GameplayTagRegistry tags;
    tags.registerTag("State.Dead");
    game::PendingSaveLayer pending;
    loader.beforeUnload = [&](data::FormHandle, ecs::Entity cellEntity) {
        pending.captureCell(world, db, cellEntity, tags);
    };
    loader.spawnFilter = [&](const Guid& referenceId) {
        return pending.isEnabled(referenceId);
    };

    // First visit: both references spawn.
    loader.loadCell(db.handleOf(kCell));
    REQUIRE(findByRef(world, kItemRef).is_alive());
    ecs::Entity actor = findByRef(world, kActorRef);
    REQUIRE(actor.is_alive());

    // The "actor" gets a stat sheet and takes a beating; the item is
    // picked up (destroyed + pending-disabled, the scene's pickup path).
    gameplay::AttributeSet vitals;
    vitals.health = 21.0f;
    actor.set<gameplay::AttributeSet>(vitals);
    actor.set<gameplay::AbilitySystem>({});
    gameplay::Inventory pockets;
    gameplay::addItem(pockets, kSword, 2);
    actor.set<gameplay::Inventory>(pockets);
    pending.disableReference(kItemRef, db, findByRef(world, kItemRef));
    findByRef(world, kItemRef).destruct();

    // Leave, come back.
    loader.unloadCell(db.handleOf(kCell));
    CHECK_FALSE(findByRef(world, kActorRef).is_alive());
    CHECK(pending.trackedCount() == 2);
    loader.loadCell(db.handleOf(kCell));

    // The item stayed looted; the actor's state re-applies.
    CHECK_FALSE(findByRef(world, kItemRef).is_alive());
    ecs::Entity reloaded = findByRef(world, kActorRef);
    REQUIRE(reloaded.is_alive());
    REQUIRE(pending.hasActorState(kActorRef));
    reloaded.set<gameplay::AttributeSet>({});
    reloaded.set<gameplay::AbilitySystem>({});
    reloaded.set<gameplay::Inventory>({});
    gameplay::applySavedState(reloaded, pending.actorState(kActorRef), tags);
    CHECK(reloaded.get<gameplay::AttributeSet>().health ==
          doctest::Approx(21.0f));
    CHECK(gameplay::itemCount(reloaded.get<gameplay::Inventory>(), kSword) ==
          2);

    // The flush is the disk save's input: deterministic, field-level.
    const auto records = pending.flush();
    CHECK(records.size() >= 2); // enabled=false patch + actor children
    bool sawDisable = false;
    const u32 enabledId =
        world::ReferenceForm::staticTypeInfo().findField("enabled")->id;
    for (const auto& record : records) {
        if (record.formId == kItemRef) {
            sawDisable = record.fields.contains(enabledId);
        }
    }
    CHECK(sawDisable);
}

namespace {

const Guid kActorCell =
    *Guid::fromString("22220000-0000-4000-8000-0000000000a0");
const Guid kActorForm =
    *Guid::fromString("55550000-0000-4000-8000-0000000000a1");
const Guid kMovedActorRef =
    *Guid::fromString("55550000-0000-4000-8000-0000000000a2");

constexpr const char* kActorPlugin = R"toml(
[plugin]
id = "11111111-1111-4111-8111-1111111111a0"
name = "base"

[[records]]
form = "11110000-0000-4000-8000-0000000000a0"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "Overworld"

[[records]]
form = "22220000-0000-4000-8000-0000000000a0"
type = "CellForm"
new = true
[records.fields]
worldspace = "11110000-0000-4000-8000-0000000000a0"

[[records]]
form = "55550000-0000-4000-8000-0000000000a1"
type = "ActorForm"
new = true
[records.fields]
editorId = "Guard"
maxHealth = 45.0

[[records]]
form = "55550000-0000-4000-8000-0000000000a2"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "55550000-0000-4000-8000-0000000000a1"
cell = "22220000-0000-4000-8000-0000000000a0"
position = [3.0, 0.0, 3.0]
)toml";

} // namespace

TEST_CASE("cell delta: a moved actor reloads where it moved, not at its spawn") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    world::registerWorldFormTypes(types);
    gameplay::registerSaveFormTypes(types);
    const auto plugin = data::parsePluginToml(kActorPlugin, types, "base");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    const world::WorldModel model = world::WorldModel::build(db);
    world::CellLoader loader { world, db, model, spawner, categories };

    gameplay::GameplayTagRegistry tags;
    tags.registerTag("State.Dead");
    game::PendingSaveLayer pending;
    loader.beforeUnload = [&](data::FormHandle, ecs::Entity cellEntity) {
        pending.captureCell(world, db, cellEntity, tags);
    };
    loader.spawnFilter = [&](const Guid& referenceId) {
        return pending.isEnabled(referenceId);
    };

    loader.loadCell(db.handleOf(kActorCell));
    ecs::Entity actor = findByRef(world, kMovedActorRef);
    REQUIRE(actor.is_alive());
    REQUIRE(actor.has<world::ActorMarker>()); // spawnActor marks actors

    // It walks off its authored spot (combat chase / a wander) and is left
    // there (dead or alive — position capture is actors-only either way).
    const Vec3 movedTo { 41.0f, 0.5f, 58.0f };
    actor.get_mut<world::Transform>().position = movedTo;

    loader.unloadCell(db.handleOf(kActorCell)); // captures the moved position
    loader.loadCell(db.handleOf(kActorCell));

    ecs::Entity reloaded = findByRef(world, kMovedActorRef);
    REQUIRE(reloaded.is_alive());
    // The cell loader respawns it at the AUTHORED spot...
    CHECK(reloaded.get<world::Transform>().position.x == doctest::Approx(3.0f));

    // ...until the pending reference override re-homes it (finalizeActorSpawn).
    pending.applyReferenceOverrides(reloaded, kMovedActorRef);
    CHECK(reloaded.get<world::Transform>().position.x ==
          doctest::Approx(movedTo.x));
    CHECK(reloaded.get<world::Transform>().position.z ==
          doctest::Approx(movedTo.z));
}

TEST_CASE("cell delta: a scale change survives the unload/reload (audit U5-5)") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    world::registerWorldFormTypes(types);
    gameplay::registerSaveFormTypes(types);
    const auto plugin = data::parsePluginToml(kActorPlugin, types, "base");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    ecs::World world;
    world::registerSceneComponents(world);
    gameplay::registerGameplayComponents(world);
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);
    world::Spawner spawner;
    world::registerCoreSpawners(spawner);
    const world::WorldModel model = world::WorldModel::build(db);
    world::CellLoader loader { world, db, model, spawner, categories };

    gameplay::GameplayTagRegistry tags;
    game::PendingSaveLayer pending;
    loader.beforeUnload = [&](data::FormHandle, ecs::Entity cellEntity) {
        pending.captureCell(world, db, cellEntity, tags);
    };

    loader.loadCell(db.handleOf(kActorCell));
    ecs::Entity actor = findByRef(world, kMovedActorRef);
    REQUIRE(actor.is_alive());

    // A script/console shrinks it (setscale). The patch path used to drop
    // scale entirely — only the prefab materialize path carried it.
    actor.get_mut<world::Transform>().scale = Vec3 { 0.5f, 0.5f, 0.5f };

    loader.unloadCell(db.handleOf(kActorCell));
    loader.loadCell(db.handleOf(kActorCell));

    ecs::Entity reloaded = findByRef(world, kMovedActorRef);
    REQUIRE(reloaded.is_alive());
    CHECK(reloaded.get<world::Transform>().scale.x == doctest::Approx(1.0f));
    pending.applyReferenceOverrides(reloaded, kMovedActorRef);
    CHECK(reloaded.get<world::Transform>().scale.x == doctest::Approx(0.5f));
    CHECK(reloaded.get<world::Transform>().scale.y == doctest::Approx(0.5f));
}
