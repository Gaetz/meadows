#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/VisualForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "game/LevelEditor.hpp"
#include "world/worldspace/WorldForms.hpp"
#include "world/worldspace/WorldModel.hpp"

// The level editor edits RECORDS through an EditSession
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
const core::Guid kRock =
    *core::Guid::fromString("90000000-0000-4000-8000-000000000001");

// A worldspace and ONE authored cell;
// every other square of the grid is implicit.
constexpr const char* kWorldExt = R"toml(
[plugin]
id = "88888888-8888-4888-8888-888888888888"
name = "world-ext"

[[records]]
form = "80000000-0000-4000-8000-000000000001"
type = "WorldspaceForm"
new = true
[records.fields]
editorId = "Overworld"
cellSize = 16.0

[[records]]
form = "80000000-0000-4000-8000-00000000000a"
type = "CellForm"
new = true
[records.fields]
editorId = "Authored"
worldspace = "80000000-0000-4000-8000-000000000001"
gridX = 0
gridY = 0
)toml";

const core::Guid kWorld =
    *core::Guid::fromString("80000000-0000-4000-8000-000000000001");
const core::Guid kAuthoredCell =
    *core::Guid::fromString("80000000-0000-4000-8000-00000000000a");
const core::Guid kModId =
    *core::Guid::fromString("aaaaaaaa-0000-4000-8000-0000000000ed");

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

// --- Implicit cells: the editor places anywhere ---------------------------

TEST_CASE("editor: placing in a virgin square ships the implicit cell") {
    const data::FormTypeRegistry types = makeTypes();
    const auto base = data::parsePluginToml(kBase, types, "base");
    const auto ext = data::parsePluginToml(kWorldExt, types, "ext");
    REQUIRE(base.has_value());
    REQUIRE(ext.has_value());
    data::FormDatabase db;
    data::resolve({ &*base, &*ext }, types, db);
    world::WorldModel model = world::WorldModel::build(db);
    const data::FormHandle ws = db.handleOf(kWorld);
    REQUIRE(ws.isValid());

    game::LevelEditor editor { db, types };
    const core::Guid cell = editor.ensureCell(model, db, ws, 5, -3);
    REQUIRE(cell.isValid());
    CHECK(cell == world::cellGuidFor(kWorld, 5, -3)); // derived identity

    // Live: index + database resolve it, and the session recorded it.
    CHECK(model.cellAt(ws, 5, -3).isValid());
    CHECK(db.find(cell) != nullptr);
    CHECK(editor.editSession().isCreated(cell));

    // Idempotent: same guid, still ONE session record.
    CHECK(editor.ensureCell(model, db, ws, 5, -3) == cell);
    const u32 dirtyAfterOne = editor.editSession().dirtyCount();

    // Place a reference into it, export, re-resolve: the mod SHIPS the
    // cell, and the reference lands in it on the next run.
    const core::Guid placed =
        editor.placeReference(kRock, cell, { 81.0f, 0.0f, -47.0f });
    REQUIRE(placed.isValid());
    CHECK(editor.editSession().dirtyCount() == dirtyAfterOne + 1);

    const data::Plugin mod =
        editor.editSession().exportPlugin(kModId, "edits");
    data::FormDatabase resolved;
    data::resolve({ &*base, &*ext, &mod }, types, resolved);
    world::WorldModel fresh = world::WorldModel::build(resolved);
    const data::FormHandle reloaded =
        fresh.cellAt(resolved.handleOf(kWorld), 5, -3);
    REQUIRE(reloaded.isValid());
    CHECK(resolved.get(reloaded)->id == cell); // same identity across runs
    const auto* cellForm =
        static_cast<const world::CellForm*>(resolved.get(reloaded));
    CHECK(cellForm->worldspace == kWorld);
    CHECK(cellForm->gridX == 5);
    CHECK(cellForm->gridY == -3);
    REQUIRE(fresh.referencesIn(reloaded).size() == 1);
    CHECK(resolved.get(fresh.referencesIn(reloaded)[0])->id == placed);

    // The core loop: a NEW session over the reloaded database sees the
    // square as AUTHORED — same guid back, zero records, no duplicate.
    game::LevelEditor second { resolved, types };
    CHECK(second.ensureCell(fresh, resolved, resolved.handleOf(kWorld), 5,
                            -3) == cell);
    CHECK(second.editSession().dirtyCount() == 0);
}

TEST_CASE("editor: ensureCell honours an authored cell's hand-minted guid") {
    const data::FormTypeRegistry types = makeTypes();
    const auto base = data::parsePluginToml(kBase, types, "base");
    const auto ext = data::parsePluginToml(kWorldExt, types, "ext");
    REQUIRE(base.has_value());
    REQUIRE(ext.has_value());
    data::FormDatabase db;
    data::resolve({ &*base, &*ext }, types, db);
    world::WorldModel model = world::WorldModel::build(db);
    const data::FormHandle ws = db.handleOf(kWorld);

    game::LevelEditor editor { db, types };
    const core::Guid cell = editor.ensureCell(model, db, ws, 0, 0);
    CHECK(cell == kAuthoredCell); // NOT the derived guid
    CHECK(cell != world::cellGuidFor(kWorld, 0, 0));
    CHECK(editor.editSession().dirtyCount() == 0); // nothing to ship

    // A non-worldspace handle refuses.
    CHECK_FALSE(
        editor.ensureCell(model, db, db.handleOf(kRock), 1, 1).isValid());
}

TEST_CASE("editor: an undone cell record re-records on the next placement") {
    const data::FormTypeRegistry types = makeTypes();
    const auto base = data::parsePluginToml(kBase, types, "base");
    const auto ext = data::parsePluginToml(kWorldExt, types, "ext");
    REQUIRE(base.has_value());
    REQUIRE(ext.has_value());
    data::FormDatabase db;
    data::resolve({ &*base, &*ext }, types, db);
    world::WorldModel model = world::WorldModel::build(db);
    const data::FormHandle ws = db.handleOf(kWorld);

    game::LevelEditor editor { db, types };
    core::Guid cell {};
    {
        data::EditSession::Gesture gesture { editor.editSession() };
        cell = editor.ensureCell(model, db, ws, 5, -3);
    }
    REQUIRE(editor.editSession().isCreated(cell));

    // Undo drops the record; the LIVE form cannot be unmade (harmless).
    editor.editSession().undo();
    CHECK_FALSE(editor.editSession().isDirty(cell));
    CHECK(model.cellAt(ws, 5, -3).isValid());

    // Re-placement: the database now "has" the cell, but the session must
    // re-record it or the export would ship a dangling reference.
    CHECK(editor.ensureCell(model, db, ws, 5, -3) == cell);
    CHECK(editor.editSession().isCreated(cell));
}

TEST_CASE("edit session: createForm under an imposed guid") {
    const data::FormTypeRegistry types = makeTypes();
    const auto base = data::parsePluginToml(kBase, types, "base");
    REQUIRE(base.has_value());
    data::FormDatabase db;
    data::resolve({ &*base }, types, db);
    data::EditSession session { db, types };

    const core::Guid imposed =
        *core::Guid::fromString("77777777-7777-4777-8777-777777777777");
    const u32 typeId = world::CellForm::staticTypeInfo().id;
    CHECK(session.createForm(typeId, "cell", imposed) == imposed);
    // A second create under the same identity refuses.
    CHECK_FALSE(session.createForm(typeId, "again", imposed).isValid());
    // Undo drops the draft; redo restores the SAME identity.
    session.undo();
    CHECK(session.view(imposed) == nullptr);
    session.redo();
    REQUIRE(session.view(imposed) != nullptr);
    CHECK(session.view(imposed)->id == imposed);
}

TEST_CASE("level editor: duplicate clones the reference, offset, one undo") {
    // duplicateReference = the
    // duplicateForm clone + a position nudge, ONE undo gesture.
    const data::FormTypeRegistry types = makeTypes();
    const auto plugin = data::parsePluginToml(kBase, types, "base");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*plugin }, types, db);

    game::LevelEditor editor { db, types };
    const core::Guid copy = editor.duplicateReference(
        kRefA, Vec3 { 1.0f, 0.0f, 1.0f });
    REQUIRE(copy.isValid());
    CHECK(copy != kRefA);
    CHECK(editor.selected() == copy); // the copy becomes the selection

    const auto* form = static_cast<const world::ReferenceForm*>(
        editor.editSession().view(copy));
    REQUIRE(form != nullptr);
    CHECK(form->baseForm == kRock); // every reflected field cloned
    CHECK(form->position.x == doctest::Approx(2.0f)); // 1 + offset
    CHECK(form->position.z == doctest::Approx(2.0f));

    // One Ctrl+Z removes the WHOLE copy (create + the position edit).
    editor.editSession().undo();
    CHECK(editor.editSession().view(copy) == nullptr);

    // A non-reference target is refused.
    CHECK_FALSE(
        editor.duplicateReference(kRock, Vec3 { 0.0f }).isValid());
}
