#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "game/LevelEditor.hpp"
#include "world/worldspace/WorldForms.hpp"

// Chantier 2 B3/B4: the level editor edits RECORDS through an EditSession
// (§5 — the editor is just another plugin author). Headless: move a
// reference, export, re-resolve, the move persists; group two references
// into a prefab and check the template/instance structure.

namespace {

constexpr const char* kBase = R"toml(
[plugin]
id = "99999999-9999-4999-8999-999999999999"
name = "editor-base"

[[records]]
form = "90000000-0000-4000-8000-000000000001"
type = "StaticForm"
new = true
[records.fields]
editorId = "Rock"
model = "90000000-0000-4000-8000-0000000000aa"

[[records]]
form = "90000000-0000-4000-8000-000000000002"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "90000000-0000-4000-8000-000000000001"
position = [1.0, 0.0, 1.0]

[[records]]
form = "90000000-0000-4000-8000-000000000003"
type = "ReferenceForm"
new = true
[records.fields]
baseForm = "90000000-0000-4000-8000-000000000001"
position = [3.0, 0.0, 1.0]
)toml";

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    data::registerVisualFormTypes(types);
    world::registerWorldFormTypes(types);
    return types;
}

const core::Guid kRefA =
    *core::Guid::fromString("90000000-0000-4000-8000-000000000002");
const core::Guid kRefB =
    *core::Guid::fromString("90000000-0000-4000-8000-000000000003");

} // namespace

TEST_CASE("editor: a committed transform survives export + re-resolve") {
    const data::FormTypeRegistry types = makeTypes();
    const auto base = data::parsePluginToml(kBase, types, "base");
    REQUIRE(base.has_value());
    data::FormDatabase db;
    data::resolve({ &*base }, types, db);

    game::LevelEditor editor { db, types };
    REQUIRE(editor.commitTransform(kRefA, { 5.0f, 0.5f, 7.0f },
                                   { 1.0f, 0.0f, 0.0f, 0.0f },
                                   { 2.0f, 2.0f, 2.0f }));

    const data::Plugin mod = editor.editSession().exportPlugin(
        *core::Guid::fromString("aaaaaaaa-0000-4000-8000-0000000000ed"),
        "edits");
    data::FormDatabase resolved;
    data::resolve({ &*base, &mod }, types, resolved);
    const auto* moved = resolved.find<world::ReferenceForm>(kRefA);
    REQUIRE(moved != nullptr);
    CHECK(moved->position.x == doctest::Approx(5.0f));
    CHECK(moved->scale.x == doctest::Approx(2.0f));
    // The untouched reference is NOT in the diff.
    const auto* untouched = resolved.find<world::ReferenceForm>(kRefB);
    CHECK(untouched->position.x == doctest::Approx(3.0f));
}

TEST_CASE("editor: create prefab from selection replaces the originals") {
    const data::FormTypeRegistry types = makeTypes();
    const auto base = data::parsePluginToml(kBase, types, "base");
    REQUIRE(base.has_value());
    data::FormDatabase db;
    data::resolve({ &*base }, types, db);

    game::LevelEditor editor { db, types };
    const core::Guid instance = editor.createPrefabFromSelection(
        { kRefA, kRefB }, "TwoRocks");
    REQUIRE(instance.isValid());

    const data::Plugin mod = editor.editSession().exportPlugin(
        *core::Guid::fromString("aaaaaaaa-0000-4000-8000-0000000000ed"),
        "edits");
    data::FormDatabase resolved;
    data::resolve({ &*base, &mod }, types, resolved);

    // Originals disabled; instance placed at the centroid (2, 0, 1).
    CHECK_FALSE(resolved.find<world::ReferenceForm>(kRefA)->enabled);
    CHECK_FALSE(resolved.find<world::ReferenceForm>(kRefB)->enabled);
    const auto* placed = resolved.find<world::ReferenceForm>(instance);
    REQUIRE(placed != nullptr);
    CHECK(placed->position.x == doctest::Approx(2.0f));

    // Two template children point at the prefab, offsets ±1 on x.
    u32 templates = 0;
    data::forEach<world::ReferenceForm>(
        resolved, [&](const world::ReferenceForm& ref) {
            if (ref.prefab.isValid() && ref.prefab == placed->baseForm) {
                ++templates;
                CHECK(std::abs(std::abs(ref.position.x) - 1.0f) < 1e-4f);
            }
        });
    CHECK(templates == 2);
}
