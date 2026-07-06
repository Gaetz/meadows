#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/ecs/World.hpp"
#include "game/SceneSubmit.hpp"
#include "gameplay/actors/CharacterForms.hpp"
#include "world/scene/AnimBridge.hpp"
#include "world/scene/Components.hpp"
#include "world/scene/Spawner.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldForms.hpp"

using core::Guid;

namespace {

const Guid kCell = *Guid::fromString("20000000-0000-4000-8000-00000000000a");
const Guid kSword = *Guid::fromString("30000000-0000-4000-8000-000000000001");
const Guid kSwordSprite =
    *Guid::fromString("5e5e5e5e-0000-4000-8000-000000000001");
const Guid kRef = *Guid::fromString("40000000-0000-4000-8000-000000000001");
const Guid kMissingBase =
    *Guid::fromString("40000000-0000-4000-8000-0000000000ff");

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    data::registerVisualFormTypes(types); // StaticForm/MaterialForm (B1)
    gameplay::registerCharacterFormTypes(types); // AppearanceForm (B6)
    world::registerWorldFormTypes(types);
    return types;
}

data::Plugin parse(const data::FormTypeRegistry& types, const char* toml,
                   const char* name) {
    auto plugin = data::parsePluginToml(toml, types, name);
    REQUIRE(plugin.has_value());
    return std::move(*plugin);
}

// A weapon with a sprite asset, and a reference placing it at (1, 2) in a cell.
const char* kBase = R"toml(
[plugin]
id = "11111111-1111-4111-8111-111111111111"
name = "base"

[[records]]
form = "20000000-0000-4000-8000-00000000000a"
type = "CellForm"
new = true
[records.fields]
editorId = "CellA"

[[records]]
form = "30000000-0000-4000-8000-000000000001"
type = "WeaponForm"
new = true
[records.fields]
editorId = "IronSword"
sprite = "5e5e5e5e-0000-4000-8000-000000000001"

[[records]]
form = "40000000-0000-4000-8000-000000000001"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "30000000-0000-4000-8000-000000000001"
cell = "20000000-0000-4000-8000-00000000000a"
position = [1.0, 2.0, 0.0]
)toml";

struct Fixture {
    data::FormTypeRegistry types = makeTypes();
    data::FormDatabase db;
    ecs::World world;
    world::FormCategoryRegistry categories;
    world::Spawner spawner;

    Fixture() {
        world::registerSceneComponents(world);
        world::registerCoreCategories(categories);
        world::registerCoreSpawners(spawner);
    }

    world::SpawnContext context() { return { world, db, categories }; }
};

} // namespace

TEST_CASE("spawner: a reference becomes an entity with its mandatory components") {
    Fixture fx;
    const auto base = parse(fx.types, kBase, "base");
    data::resolve({ &base }, fx.types, fx.db);

    const auto* reference = fx.db.find<world::ReferenceForm>(kRef);
    REQUIRE(reference != nullptr);

    ecs::Entity cell = fx.world.create();
    auto ctx = fx.context();
    ecs::Entity entity = fx.spawner.spawn(ctx, *reference, cell);
    REQUIRE(entity.is_alive());

    // Transform from the reference's instance fields.
    REQUIRE(entity.try_get<world::Transform>() != nullptr);
    CHECK(entity.get<world::Transform>().position.x == 1.0f);
    CHECK(entity.get<world::Transform>().position.y == 2.0f);

    // SpriteRender seeded from the base form's `sprite` field via reflection.
    REQUIRE(entity.try_get<world::SpriteRender>() != nullptr);
    CHECK(entity.get<world::SpriteRender>().sprite == kSwordSprite);

    // Identity back-link keyed on the persistent GUID.
    REQUIRE(entity.try_get<world::RefId>() != nullptr);
    CHECK(entity.get<world::RefId>().referenceId == kRef);
    CHECK(entity.get<world::RefId>().base == fx.db.handleOf(kSword));

    // Runtime cell membership + category dispatch (WeaponForm → Item).
    CHECK(entity.has<ecs::InCell>(cell));
    CHECK(entity.has<world::ItemMarker>());
    CHECK_FALSE(entity.has<world::ActorMarker>());
}

TEST_CASE("spawner: a mod patch on the reference flows into the spawned entity (§5)") {
    Fixture fx;
    const auto base = parse(fx.types, kBase, "base");
    const auto mod = parse(fx.types, R"toml(
[plugin]
id = "22222222-2222-4222-8222-222222222222"
name = "move-and-keep"

[[records]]
form = "40000000-0000-4000-8000-000000000001"
type = "ReferenceForm"
[records.fields]
position = [5.0, 6.0, 0.0]
)toml",
                          "mod");
    data::resolve({ &base, &mod }, fx.types, fx.db);

    const auto* reference = fx.db.find<world::ReferenceForm>(kRef);
    REQUIRE(reference != nullptr);

    auto ctx = fx.context();
    ecs::Entity entity = fx.spawner.spawn(ctx, *reference, fx.world.create());
    REQUIRE(entity.is_alive());
    CHECK(entity.get<world::Transform>().position.x == 5.0f); // patched value
    CHECK(entity.get<world::Transform>().position.y == 6.0f);
}

TEST_CASE("extract (B1): a spawned StaticForm reference lands in the "
          "snapshot's mesh section") {
    Fixture fx;
    const auto base = parse(fx.types, R"toml(
[plugin]
id = "44444444-4444-4444-8444-444444444444"
name = "mesh-base"

[[records]]
form = "50000000-0000-4000-8000-000000000001"
type = "MaterialForm"
new = true
[records.fields]
editorId = "RockMaterial"
tint = [0.5, 0.6, 0.4, 1.0]

[[records]]
form = "50000000-0000-4000-8000-000000000002"
type = "StaticForm"
new = true
[records.fields]
editorId = "Rock"
model = "50000000-0000-4000-8000-0000000000aa"
material = "50000000-0000-4000-8000-000000000001"

[[records]]
form = "50000000-0000-4000-8000-000000000003"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "50000000-0000-4000-8000-000000000002"
position = [3.0, 0.0, 5.0]
scale = [2.0, 2.0, 2.0]
)toml",
                            "mesh-base");
    data::resolve({ &base }, fx.types, fx.db);

    const Guid refId = *Guid::fromString("50000000-0000-4000-8000-000000000003");
    const auto* reference = fx.db.find<world::ReferenceForm>(refId);
    REQUIRE(reference != nullptr);

    auto ctx = fx.context();
    ecs::Entity entity = fx.spawner.spawn(ctx, *reference, fx.world.create());
    REQUIRE(entity.is_alive());
    // MeshRender wired by reflection from the base form's model/material.
    REQUIRE(entity.try_get<world::MeshRender>() != nullptr);

    game::RenderSnapshot snapshot;
    game::extractMeshes(fx.world, snapshot);
    REQUIRE(snapshot.meshes.size() == 1);
    CHECK(snapshot.meshes[0].model ==
          *Guid::fromString("50000000-0000-4000-8000-0000000000aa"));
    CHECK(snapshot.meshes[0].material ==
          *Guid::fromString("50000000-0000-4000-8000-000000000001"));
    // Fully composed world transform: translation column + scale diagonal.
    CHECK(snapshot.meshes[0].transform[3].x == doctest::Approx(3.0f));
    CHECK(snapshot.meshes[0].transform[3].z == doctest::Approx(5.0f));
    CHECK(snapshot.meshes[0].transform[0].x == doctest::Approx(2.0f));
}

TEST_CASE("resolveActorVisual (B6): ActorForm + AppearanceForm resolve to "
          "a drawable visual") {
    Fixture fx;
    const auto base = parse(fx.types, R"toml(
[plugin]
id = "55555555-5555-4555-8555-555555555555"
name = "npc-base"

[[records]]
form = "60000000-0000-4000-8000-000000000001"
type = "AppearanceForm"
new = true
[records.fields]
editorId = "Look"
skeleton = "60000000-0000-4000-8000-0000000000aa"
torsoMesh = "60000000-0000-4000-8000-0000000000ab"
skinTint = [0.5, 0.4, 0.3, 1.0]

[[records]]
form = "60000000-0000-4000-8000-000000000002"
type = "ActorForm"
new = true
[records.fields]
editorId = "Npc"
appearance = "60000000-0000-4000-8000-000000000001"
animGraph = "60000000-0000-4000-8000-0000000000ac"

[[records]]
form = "60000000-0000-4000-8000-000000000003"
type = "ActorForm"
new = true
[records.fields]
editorId = "LegacyNpc"
)toml",
                            "npc-base");
    data::resolve({ &base }, fx.types, fx.db);

    const auto* actor = fx.db.find<data::ActorForm>(
        *Guid::fromString("60000000-0000-4000-8000-000000000002"));
    REQUIRE(actor != nullptr);
    const auto visual = world::resolveActorVisual(fx.db, *actor);
    REQUIRE(visual.has_value());
    CHECK(visual->skeleton ==
          *Guid::fromString("60000000-0000-4000-8000-0000000000aa"));
    CHECK(visual->mesh ==
          *Guid::fromString("60000000-0000-4000-8000-0000000000ab"));
    CHECK(visual->animGraph ==
          *Guid::fromString("60000000-0000-4000-8000-0000000000ac"));
    CHECK(visual->tint.x == doctest::Approx(0.5f));

    // An actor without an appearance is a 2D/legacy actor: no visual.
    const auto* legacy = fx.db.find<data::ActorForm>(
        *Guid::fromString("60000000-0000-4000-8000-000000000003"));
    REQUIRE(legacy != nullptr);
    CHECK_FALSE(world::resolveActorVisual(fx.db, *legacy).has_value());
}

TEST_CASE("collectLights picks the nearest N, deterministically (ch.2 B5)") {
    Fixture fx;
    const auto light = [&](f32 x, f32 intensity) {
        ecs::Entity e = fx.world.create();
        e.set<world::Transform>({ .position = { x, 0.0f, 0.0f } });
        e.set<world::LightSource>({ .color = { 1.0f, 1.0f, 1.0f },
                                    .intensity = intensity,
                                    .radius = 8.0f });
        return e;
    };
    light(30.0f, 3.0f);
    light(5.0f, 1.0f);
    light(12.0f, 2.0f);

    const auto lights =
        game::collectLights(fx.world, { 0.0f, 0.0f, 0.0f }, 2);
    REQUIRE(lights.size() == 2);
    CHECK(lights[0].intensity == doctest::Approx(1.0f)); // x = 5, nearest
    CHECK(lights[1].intensity == doctest::Approx(2.0f)); // x = 12
    // Same call, same order (stable).
    const auto again =
        game::collectLights(fx.world, { 0.0f, 0.0f, 0.0f }, 2);
    CHECK(again[0].position.x == lights[0].position.x);
}

TEST_CASE("spawner: an unresolvable base form yields no entity") {
    Fixture fx;
    const auto base = parse(fx.types, kBase, "base");
    const auto orphan = parse(fx.types, R"toml(
[plugin]
id = "33333333-3333-4333-8333-333333333333"
name = "orphan-ref"

[[records]]
form = "40000000-0000-4000-8000-0000000000ff"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "deadbeef-0000-4000-8000-000000000000"
cell = "20000000-0000-4000-8000-00000000000a"
)toml",
                             "orphan");
    data::resolve({ &base, &orphan }, fx.types, fx.db);

    const auto* reference = fx.db.find<world::ReferenceForm>(kMissingBase);
    REQUIRE(reference != nullptr);

    auto ctx = fx.context();
    ecs::Entity entity = fx.spawner.spawn(ctx, *reference, fx.world.create());
    CHECK_FALSE(entity.is_valid());
}
