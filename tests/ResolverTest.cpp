#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/Resolver.hpp"

using core::Guid;

namespace {

const Guid kSwordId = *Guid::fromString("22222222-2222-4222-8222-222222222222");

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    return types;
}

data::Plugin parse(const data::FormTypeRegistry& types, const char* toml,
                   const char* name) {
    auto plugin = data::parsePluginToml(toml, types, name);
    REQUIRE(plugin.has_value());
    return std::move(*plugin);
}

const char* kBase = R"toml(
[plugin]
id = "11111111-1111-4111-8111-111111111111"
name = "base-game"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
new = true
[records.fields]
editorId = "IronSword"
displayName = "Iron Sword"
damage = 12.0
)toml";

} // namespace

TEST_CASE("resolver: create = defaults plus the record's own fields") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");

    data::FormDatabase db;
    const auto report = data::resolve({ &base }, types, db);

    CHECK(report.formsCreated == 1);
    CHECK(report.recordsApplied == 1);
    CHECK(!report.hasConflicts());

    const auto* sword = db.find<data::WeaponForm>(kSwordId);
    REQUIRE(sword != nullptr);
    CHECK(sword->id == kSwordId);
    CHECK(sword->editorId == "IronSword");   // inherited field, via patch
    CHECK(sword->damage == 12.0f);           // record value
    CHECK(sword->weight == 1.0f);            // untouched C++ default
    CHECK(sword->goldValue == 0);
}

TEST_CASE("resolver: two mods on different fields both apply, no conflict") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");
    const auto modA = parse(types, R"toml(
[plugin]
id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
name = "sharper-swords"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
goldValue = 50
)toml",
                            "modA");
    const auto modB = parse(types, R"toml(
[plugin]
id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
name = "heavier-swords"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
weight = 4.5
)toml",
                            "modB");

    data::FormDatabase db;
    const auto report = data::resolve({ &base, &modA, &modB }, types, db);

    // This is the whole point of field-level patches (§5).
    CHECK(!report.hasConflicts());
    const auto* sword = db.find<data::WeaponForm>(kSwordId);
    REQUIRE(sword != nullptr);
    CHECK(sword->goldValue == 50);
    CHECK(sword->weight == 4.5f);
    CHECK(sword->damage == 12.0f); // base value untouched by either mod
}

TEST_CASE("resolver: same field, last writer wins, conflict reported") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");
    const auto modA = parse(types, R"toml(
[plugin]
id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
name = "modA"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
damage = 20.0
)toml",
                            "modA");
    const auto modB = parse(types, R"toml(
[plugin]
id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
name = "modB"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
damage = 7.5
)toml",
                            "modB");

    data::FormDatabase db;
    const auto report = data::resolve({ &base, &modA, &modB }, types, db);

    const auto* sword = db.find<data::WeaponForm>(kSwordId);
    REQUIRE(sword != nullptr);
    CHECK(sword->damage == 7.5f); // modB loads last, modB wins

    REQUIRE(report.conflicts.size() == 1);
    const auto& conflict = report.conflicts[0];
    CHECK(conflict.formId == kSwordId);
    CHECK(conflict.fieldName == "damage");
    REQUIRE(conflict.writers.size() == 3);
    CHECK(conflict.writers[0].plugin == "base-game");
    CHECK(conflict.writers[1].plugin == "modA");
    CHECK(conflict.writers[2].plugin == "modB"); // last entry = winner
    // 8.5: each writer's VALUE rides along (what the synthesis tool shows).
    CHECK(std::get<f32>(conflict.writers[1].value) == 20.0f);
    CHECK(std::get<f32>(conflict.writers[2].value) == 7.5f);
    CHECK(conflict.fieldId == core::fnv1a("damage"));
}

TEST_CASE("resolver: load order is the only tie-breaker") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");
    const auto modA = parse(types, R"toml(
[plugin]
id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
name = "modA"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
damage = 20.0
)toml",
                            "modA");
    const auto modB = parse(types, R"toml(
[plugin]
id = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"
name = "modB"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
damage = 7.5
)toml",
                            "modB");

    data::FormDatabase dbAB;
    data::resolve({ &base, &modA, &modB }, types, dbAB);
    data::FormDatabase dbBA;
    data::resolve({ &base, &modB, &modA }, types, dbBA);

    CHECK(dbAB.find<data::WeaponForm>(kSwordId)->damage == 7.5f);
    CHECK(dbBA.find<data::WeaponForm>(kSwordId)->damage == 20.0f);
}

TEST_CASE("resolver: duplicate create degrades to patch") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");
    const auto rogue = parse(types, R"toml(
[plugin]
id = "cccccccc-cccc-4ccc-8ccc-cccccccccccc"
name = "rogue-mod"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
new = true
[records.fields]
damage = 99.0
)toml",
                            "rogue");

    data::FormDatabase db;
    const auto report = data::resolve({ &base, &rogue }, types, db);

    CHECK(report.formsCreated == 1); // not two
    const auto* sword = db.find<data::WeaponForm>(kSwordId);
    REQUIRE(sword != nullptr);
    CHECK(sword->damage == 99.0f);          // its fields still applied
    CHECK(sword->editorId == "IronSword");  // base fields kept
}

TEST_CASE("resolver: orphan patches are counted and dropped") {
    const auto types = makeTypes();
    const auto orphan = parse(types, R"toml(
[plugin]
id = "dddddddd-dddd-4ddd-8ddd-dddddddddddd"
name = "orphan-mod"

[[records]]
form = "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee"
type = "WeaponForm"
[records.fields]
damage = 1.0
)toml",
                             "orphan");

    data::FormDatabase db;
    const auto report = data::resolve({ &orphan }, types, db);

    CHECK(report.formsCreated == 0);
    CHECK(report.orphanPatches == 1);
    CHECK(db.count() == 0);
}

TEST_CASE("resolver: patch before its creator still applies by load order") {
    const auto types = makeTypes();
    // earlyMod loads BEFORE base but patches base's sword: legal without
    // enforced masters; strict load order means base's later create fields
    // override the early patch where they overlap.
    const auto earlyMod = parse(types, R"toml(
[plugin]
id = "ffffffff-ffff-4fff-8fff-ffffffffffff"
name = "early-mod"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
damage = 99.0
weight = 9.0
)toml",
                               "early");
    const auto base = parse(types, kBase, "base");

    data::FormDatabase db;
    const auto report = data::resolve({ &earlyMod, &base }, types, db);

    CHECK(report.orphanPatches == 0);
    const auto* sword = db.find<data::WeaponForm>(kSwordId);
    REQUIRE(sword != nullptr);
    CHECK(sword->damage == 12.0f); // base wrote damage later: base wins
    CHECK(sword->weight == 9.0f);  // base never writes weight: patch holds
}

TEST_CASE("resolver: wrong-type patch is skipped") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");
    const auto wrong = parse(types, R"toml(
[plugin]
id = "abababab-abab-4bab-8bab-abababababab"
name = "wrong-type-mod"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "ActorForm"
[records.fields]
maxHealth = 1.0
)toml",
                             "wrong");

    data::FormDatabase db;
    const auto report = data::resolve({ &base, &wrong }, types, db);

    CHECK(report.recordsSkipped == 1);
    const auto* sword = db.find<data::WeaponForm>(kSwordId);
    REQUIRE(sword != nullptr);
    CHECK(sword->damage == 12.0f); // untouched
}

TEST_CASE("resolver: deterministic across runs") {
    const auto types = makeTypes();
    const auto base = parse(types, kBase, "base");
    const auto modA = parse(types, R"toml(
[plugin]
id = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"
name = "modA"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
damage = 20.0
displayName = "Sharp Iron Sword"
)toml",
                            "modA");

    data::FormDatabase db1;
    const auto report1 = data::resolve({ &base, &modA }, types, db1);
    data::FormDatabase db2;
    const auto report2 = data::resolve({ &base, &modA }, types, db2);

    REQUIRE(report1.conflicts.size() == report2.conflicts.size());
    for (size_t i = 0; i < report1.conflicts.size(); ++i) {
        CHECK(report1.conflicts[i].fieldName ==
              report2.conflicts[i].fieldName);
        CHECK(report1.conflicts[i].writers == report2.conflicts[i].writers);
    }
    CHECK(db1.find<data::WeaponForm>(kSwordId)->damage ==
          db2.find<data::WeaponForm>(kSwordId)->damage);
}
