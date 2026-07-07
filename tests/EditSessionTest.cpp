#include <doctest/doctest.h>

#include <memory>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/EditSession.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/TomlWriter.hpp"
#include "quest/Quest.hpp"

// H2: the editor-writes-plugins loop, end to end and headless — edit a
// resolved form, export the diff, re-parse it through the NORMAL plugin
// pipeline, and get the edited world back.

namespace {

constexpr const char* kBase = R"(
[plugin]
id = "cccc0000-0000-4000-8000-000000000001"
name = "base"

[[records]]
form = "cccc0001-0000-4000-8000-000000000001"
type = "WeaponForm"
new = true
[records.fields]
editorId = "IronSword"
damage = 10.0
weight = 3.0
)";

const core::Guid kSwordId =
    *core::Guid::fromString("cccc0001-0000-4000-8000-000000000001");

} // namespace

TEST_CASE("edit -> export -> resolve round-trip") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    const auto basePlugin = data::parsePluginToml(kBase, types, "base");
    REQUIRE(basePlugin.has_value());
    data::FormDatabase db;
    data::resolve({ &*basePlugin }, types, db);

    data::EditSession session { db, types };

    // Edit one field; the resolved database is untouched (§2.2).
    const u32 damageId = core::fnv1a("damage");
    REQUIRE(session.setField(kSwordId, damageId, reflect::Value { 42.0f }));
    CHECK(db.find<data::WeaponForm>(kSwordId)->damage ==
          doctest::Approx(10.0f));
    CHECK(static_cast<const data::WeaponForm*>(session.view(kSwordId))
              ->damage == doctest::Approx(42.0f));

    // Create a brand-new weapon.
    const core::Guid newId = session.createForm(
        data::WeaponForm::staticTypeInfo().id, "SteelDagger");
    REQUIRE(newId.isValid());
    session.setField(newId, damageId, reflect::Value { 7.0f });
    CHECK(session.dirtyCount() == 2);

    // Export = an ordinary plugin. The patch record carries ONLY damage.
    const data::Plugin exported = session.exportPlugin(
        core::Guid::generate(), "my-edits");
    REQUIRE(exported.records.size() == 2);
    for (const data::Record& record : exported.records) {
        if (record.formId == kSwordId) {
            CHECK_FALSE(record.creates);
            CHECK(record.fields.size() == 1);
            CHECK(record.fields.contains(damageId));
        } else {
            CHECK(record.creates);
        }
    }

    // Serialize with the EXISTING writer, re-parse, re-resolve: the loop.
    const str toml = data::writePluginToml(exported, types);
    const auto reparsed = data::parsePluginToml(toml, types, "my-edits");
    REQUIRE(reparsed.has_value());
    data::FormDatabase db2;
    data::resolve({ &*basePlugin, &*reparsed }, types, db2);
    CHECK(db2.find<data::WeaponForm>(kSwordId)->damage ==
          doctest::Approx(42.0f));
    CHECK(db2.find<data::WeaponForm>(kSwordId)->weight ==
          doctest::Approx(3.0f)); // untouched field survives
    const auto* dagger =
        data::findByEditorId<data::WeaponForm>(db2, "SteelDagger");
    REQUIRE(dagger != nullptr);
    CHECK(dagger->damage == doctest::Approx(7.0f));
}

TEST_CASE("undo/redo replay field edits and creations") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    const auto basePlugin = data::parsePluginToml(kBase, types, "base");
    data::FormDatabase db;
    data::resolve({ &*basePlugin }, types, db);
    data::EditSession session { db, types };

    const u32 damageId = core::fnv1a("damage");
    session.setField(kSwordId, damageId, reflect::Value { 42.0f });
    session.setField(kSwordId, damageId, reflect::Value { 50.0f });

    const auto damageOf = [&] {
        return static_cast<const data::WeaponForm*>(session.view(kSwordId))
            ->damage;
    };
    CHECK(damageOf() == doctest::Approx(50.0f));
    session.undo();
    CHECK(damageOf() == doctest::Approx(42.0f));
    session.undo();
    CHECK(damageOf() == doctest::Approx(10.0f));
    session.redo();
    CHECK(damageOf() == doctest::Approx(42.0f));

    // Creation undo drops the draft.
    const core::Guid created = session.createForm(
        data::WeaponForm::staticTypeInfo().id, "Temp");
    CHECK(session.view(created) != nullptr);
    session.undo();
    CHECK(session.view(created) == nullptr);
    session.redo();
    CHECK(session.view(created) != nullptr);
}

// Chantier 8.1 — the GameDB "duplicate" tool.
TEST_CASE("duplicate clones every field under a new guid, undo/redo safe") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    const auto basePlugin = data::parsePluginToml(kBase, types, "base");
    data::FormDatabase db;
    data::resolve({ &*basePlugin }, types, db);
    data::EditSession session { db, types };

    const core::Guid copyId =
        session.duplicateForm(kSwordId, "IronSwordCopy");
    REQUIRE(copyId.isValid());
    CHECK(copyId != kSwordId);
    const auto asWeapon = [&](const core::Guid& id) {
        return static_cast<const data::WeaponForm*>(session.view(id));
    };
    REQUIRE(asWeapon(copyId) != nullptr);
    CHECK(asWeapon(copyId)->damage == doctest::Approx(10.0f));
    CHECK(asWeapon(copyId)->editorId == "IronSwordCopy");

    // Unknown source: refused.
    CHECK_FALSE(session.duplicateForm(core::Guid::generate(), "x").isValid());

    // Undo drops the copy; redo re-clones the FIELDS, not just the shell.
    session.undo(); // pops the failed... no-op ops are not pushed
    CHECK(session.view(copyId) == nullptr);
    session.redo();
    REQUIRE(asWeapon(copyId) != nullptr);
    CHECK(asWeapon(copyId)->damage == doctest::Approx(10.0f));

    // Duplicating an EDITED form copies the draft state, not the stale
    // base — and the export's create record carries the non-default field
    // (damage 10 IS the C++ default, so the first copy's record rightly
    // omits it: a create record only emits what differs, §5).
    session.setField(kSwordId, core::fnv1a("damage"),
                     reflect::Value { 42.0f });
    const core::Guid hotId = session.duplicateForm(kSwordId, "IronSword42");
    REQUIRE(hotId.isValid());
    CHECK(asWeapon(hotId)->damage == doctest::Approx(42.0f));

    const data::Plugin exported =
        session.exportPlugin(core::Guid::generate(), "dup");
    bool foundHot = false;
    for (const data::Record& record : exported.records) {
        if (record.formId == hotId) {
            foundHot = true;
            CHECK(record.creates);
            CHECK(record.fields.contains(core::fnv1a("damage")));
        }
    }
    CHECK(foundHot);
}

// Chantier 8.1 — the GameDB "used by" tool.
TEST_CASE("referencesTo finds every Guid field pointing at a form") {
    const core::Guid questId =
        *core::Guid::fromString("cccc0002-0000-4000-8000-000000000001");
    const core::Guid stateId =
        *core::Guid::fromString("cccc0002-0000-4000-8000-000000000002");
    data::FormDatabase db;
    auto questForm = std::make_unique<quest::QuestForm>();
    questForm->id = questId;
    questForm->startState = stateId;
    db.add(std::move(questForm), quest::QuestForm::staticTypeInfo());
    auto state = std::make_unique<quest::QuestStateForm>();
    state->id = stateId;
    state->quest = questId;
    db.add(std::move(state), quest::QuestStateForm::staticTypeInfo());

    // The state points at the quest through its `quest` field...
    const auto questHits = data::referencesTo(db, questId);
    REQUIRE(questHits.size() == 1);
    CHECK(questHits[0].from == stateId);
    CHECK(questHits[0].fieldName == "quest");

    // ...and the quest points back through `startState`.
    const auto stateHits = data::referencesTo(db, stateId);
    REQUIRE(stateHits.size() == 1);
    CHECK(stateHits[0].from == questId);
    CHECK(stateHits[0].fieldName == "startState");

    // An unreferenced guid has no referencers.
    CHECK(data::referencesTo(db, core::Guid::generate()).empty());
}
