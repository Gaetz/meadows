#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "world/worldspace/FormCategory.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "world/worldspace/WorldModel.hpp"

using core::Guid;

namespace {

const Guid kWorldspace = *Guid::fromString("10000000-0000-4000-8000-000000000001");
const Guid kCellA = *Guid::fromString("20000000-0000-4000-8000-00000000000a");
const Guid kCellB = *Guid::fromString("20000000-0000-4000-8000-00000000000b");
const Guid kSword = *Guid::fromString("30000000-0000-4000-8000-000000000001");
const Guid kRef1 = *Guid::fromString("40000000-0000-4000-8000-000000000001");
const Guid kRef3 = *Guid::fromString("40000000-0000-4000-8000-000000000003");

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

// One worldspace, two cells side by side, a base weapon, and three references:
// two in cell A (one with an instance position), one in cell B.
const char* kBase = R"toml(
[plugin]
id = "11111111-1111-4111-8111-111111111111"
name = "base-world"

[[records]]
form = "10000000-0000-4000-8000-000000000001"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "Tamriel"
cellSize = 16.0

[[records]]
form = "20000000-0000-4000-8000-00000000000a"
type = "CellForm"
new = true
[records.fields]
editorId = "CellA"
worldspace = "10000000-0000-4000-8000-000000000001"
gridX = 0
gridY = 0

[[records]]
form = "20000000-0000-4000-8000-00000000000b"
type = "CellForm"
new = true
[records.fields]
editorId = "CellB"
worldspace = "10000000-0000-4000-8000-000000000001"
gridX = 1
gridY = 0

[[records]]
form = "30000000-0000-4000-8000-000000000001"
type = "WeaponForm"
new = true
[records.fields]
editorId = "IronSword"
damage = 12.0

[[records]]
form = "40000000-0000-4000-8000-000000000001"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "30000000-0000-4000-8000-000000000001"
cell = "20000000-0000-4000-8000-00000000000a"
position = [1.0, 2.0, 0.0]

[[records]]
form = "40000000-0000-4000-8000-000000000002"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "30000000-0000-4000-8000-000000000001"
cell = "20000000-0000-4000-8000-00000000000a"

[[records]]
form = "40000000-0000-4000-8000-000000000003"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "30000000-0000-4000-8000-000000000001"
cell = "20000000-0000-4000-8000-00000000000b"
)toml";

} // namespace

TEST_CASE("world model: indexes worldspaces, cells, and references by cell") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");

    data::FormDatabase db;
    data::resolve({ &base }, types, db);
    const auto model = world::WorldModel::build(db);

    CHECK(model.worldspaces().size() == 1);
    CHECK(model.cells().size() == 2);

    const auto worldspace = db.handleOf(kWorldspace);
    const auto cellA = db.handleOf(kCellA);
    const auto cellB = db.handleOf(kCellB);

    CHECK(model.cellAt(worldspace, 0, 0) == cellA);
    CHECK(model.cellAt(worldspace, 1, 0) == cellB);
    CHECK_FALSE(model.cellAt(worldspace, 5, 5).isValid());

    CHECK(model.worldspaceOf(cellA) == worldspace);
    CHECK(model.worldspaceOf(cellB) == worldspace);

    CHECK(model.referencesIn(cellA).size() == 2);
    CHECK(model.referencesIn(cellB).size() == 1);
}

TEST_CASE("world model: a placed reference carries its instance fields") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");

    data::FormDatabase db;
    data::resolve({ &base }, types, db);

    const auto* ref1 = db.find<world::ReferenceForm>(kRef1);
    REQUIRE(ref1 != nullptr);
    CHECK(ref1->baseForm == kSword);
    CHECK(ref1->position.x == 1.0f);
    CHECK(ref1->position.y == 2.0f);
    CHECK(ref1->enabled == true);  // untouched default
    CHECK(ref1->count == 1);
}

TEST_CASE("world model: patching a reference's cell moves it (field-level, §5)") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");
    const auto moveMod = parse(types, R"toml(
[plugin]
id = "22222222-2222-4222-8222-222222222222"
name = "move-ref3"

[[records]]
form = "40000000-0000-4000-8000-000000000003"
type = "ReferenceForm"
[records.fields]
cell = "20000000-0000-4000-8000-00000000000a"
)toml",
                            "move");

    data::FormDatabase db;
    data::resolve({ &base, &moveMod }, types, db);
    const auto model = world::WorldModel::build(db);

    const auto cellA = db.handleOf(kCellA);
    const auto cellB = db.handleOf(kCellB);
    CHECK(model.referencesIn(cellA).size() == 3);  // ref3 moved in
    CHECK(model.referencesIn(cellB).size() == 0);

    const auto* ref3 = db.find<world::ReferenceForm>(kRef3);
    REQUIRE(ref3 != nullptr);
    CHECK(ref3->cell == kCellA);  // the resolved Form reflects the patch
}

TEST_CASE("form category: core base forms map to their category") {
    world::FormCategoryRegistry categories;
    world::registerCoreCategories(categories);

    const auto weapon =
        categories.categoryOf(data::WeaponForm::staticTypeInfo().id);
    REQUIRE(weapon.has_value());
    CHECK(*weapon == world::FormCategory::Item);

    const auto actor =
        categories.categoryOf(data::ActorForm::staticTypeInfo().id);
    REQUIRE(actor.has_value());
    CHECK(*actor == world::FormCategory::Actor);

    // Worldspace forms are not spawnable base forms: no category.
    CHECK_FALSE(
        categories.categoryOf(world::CellForm::staticTypeInfo().id).has_value());
}
