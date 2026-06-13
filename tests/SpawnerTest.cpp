#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "engine/ecs/World.hpp"
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
