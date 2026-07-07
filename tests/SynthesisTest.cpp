#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"
#include "data/plugins/Synthesis.hpp"

// Chantier 8.5 (§5.1) — the synthesis patch is an ORDINARY plugin loaded
// last. Two mods fight over the same field; the user picks the LOSER's
// value; the generated plugin re-resolves to that value with zero new
// mechanism.

namespace {

constexpr const char* kBase = R"(
[plugin]
id = "dddd0000-0000-4000-8000-000000000001"
name = "base-game"

[[records]]
form = "dddd0001-0000-4000-8000-000000000001"
type = "WeaponForm"
new = true
[records.fields]
editorId = "IronSword"
damage = 12.0
)";

constexpr const char* kSharper = R"(
[plugin]
id = "dddd0000-0000-4000-8000-000000000002"
name = "sharper-swords"

[[records]]
form = "dddd0001-0000-4000-8000-000000000001"
type = "WeaponForm"
[records.fields]
damage = 20.0
)";

constexpr const char* kLighter = R"(
[plugin]
id = "dddd0000-0000-4000-8000-000000000003"
name = "lighter-swords"

[[records]]
form = "dddd0001-0000-4000-8000-000000000001"
type = "WeaponForm"
[records.fields]
damage = 6.0
weight = 1.5
)";

const core::Guid kSwordId =
    *core::Guid::fromString("dddd0001-0000-4000-8000-000000000001");

} // namespace

TEST_CASE("synthesis patch arbitrates a conflict as one more layer") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    const auto base = data::parsePluginToml(kBase, types, "base");
    const auto sharper = data::parsePluginToml(kSharper, types, "sharper");
    const auto lighter = data::parsePluginToml(kLighter, types, "lighter");
    REQUIRE(base.has_value());
    REQUIRE(sharper.has_value());
    REQUIRE(lighter.has_value());

    data::FormDatabase db;
    const auto report =
        data::resolve({ &*base, &*sharper, &*lighter }, types, db);
    CHECK(db.find<data::WeaponForm>(kSwordId)->damage == 6.0f); // last wins

    // The damage conflict carries all three writers' values.
    const data::FieldConflict* damageConflict = nullptr;
    for (const data::FieldConflict& conflict : report.conflicts) {
        if (conflict.fieldName == "damage") {
            damageConflict = &conflict;
        }
    }
    REQUIRE(damageConflict != nullptr);
    REQUIRE(damageConflict->writers.size() == 3);
    CHECK(std::get<f32>(damageConflict->writers[1].value) == 20.0f);

    // The user wants sharper-swords' damage (the LOSER by load order) —
    // unexpressible by load order alone, THE §5.1 case.
    const data::SynthesisChoice choice {
        .formId = damageConflict->formId,
        .typeId = damageConflict->typeId,
        .fieldId = damageConflict->fieldId,
        .fieldName = damageConflict->fieldName,
        .value = damageConflict->writers[1].value,
        .provenance = damageConflict->writers[1].plugin,
    };
    const data::Plugin patch = data::makeSynthesisPatch(
        core::Guid::generate(), "synthesis",
        { choice }, { sharper->id, lighter->id });
    REQUIRE(patch.records.size() == 1);
    CHECK_FALSE(patch.records[0].creates); // a PATCH record, never a create
    CHECK(patch.dependencies.size() == 2);

    // The TOML round-trips through the NORMAL pipeline with a provenance
    // header the parser ignores.
    const str toml =
        data::writeSynthesisToml(patch, types, { choice }, &db);
    CHECK(toml.find("# IronSword.damage from: sharper-swords") != str::npos);
    const auto reparsed = data::parsePluginToml(toml, types, "synthesis");
    REQUIRE(reparsed.has_value());

    // Loaded LAST, it arbitrates: sharper's damage, lighter's weight.
    data::FormDatabase db2;
    const auto report2 = data::resolve(
        { &*base, &*sharper, &*lighter, &*reparsed }, types, db2);
    const auto* sword = db2.find<data::WeaponForm>(kSwordId);
    REQUIRE(sword != nullptr);
    CHECK(sword->damage == 20.0f); // the arbitrated value
    CHECK(sword->weight == 1.5f);  // lighter's un-conflicted field survives

    // The synthesis plugin is now the LAST writer of the arbitrated field
    // (the conflict view can mark it arbitrated).
    for (const data::FieldConflict& conflict : report2.conflicts) {
        if (conflict.fieldName == "damage") {
            CHECK(conflict.writers.back().plugin == "synthesis");
        }
    }
}
