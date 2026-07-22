#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/plugins/CsvImport.hpp"
#include "data/plugins/Resolver.hpp"

// The CSV -> plugin bridge: rows become ordinary §5 records
// through reflection, with DETERMINISTIC identities — re-importing a sheet
// must never shift a guid.

using core::Guid;

namespace {

const Guid kPluginId =
    *Guid::fromString("c5000000-0000-4000-8000-000000000001");

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    return types;
}

} // namespace

TEST_CASE("csv import: rows become records through reflection") {
    const data::FormTypeRegistry types = makeTypes();
    const auto* type = types.findType("WeaponForm");
    REQUIRE(type != nullptr);

    // Quoted cell with a comma; empty cell keeps the field default.
    const char* csv = "editorId,displayName,damage,goldValue\n"
                      "IronSword,\"Sword, iron\",12,50\n"
                      "OakClub,Oak club,,15\n";
    const auto plugin = data::importCsv(csv, *type, kPluginId, "test.csv");
    REQUIRE(plugin.has_value());
    REQUIRE(plugin->records.size() == 2);
    CHECK(plugin->records[0].creates);
    CHECK(plugin->records[0].typeId == type->id);

    // Resolve like any plugin and read the fields back.
    data::FormDatabase db;
    const vector<const data::Plugin*> order { &*plugin };
    data::resolve(order, types, db);
    const auto* sword = db.find<data::WeaponForm>(plugin->records[0].formId);
    REQUIRE(sword != nullptr);
    CHECK(sword->editorId == "IronSword");
    CHECK(sword->displayName == "Sword, iron"); // quoted comma survived
    const auto* club = db.find<data::WeaponForm>(plugin->records[1].formId);
    REQUIRE(club != nullptr);
    CHECK(club->editorId == "OakClub");
}

TEST_CASE("csv import: identities are deterministic across imports") {
    const data::FormTypeRegistry types = makeTypes();
    const auto* type = types.findType("WeaponForm");
    const char* csv = "editorId,damage\nIronSword,12\n";

    const auto first = data::importCsv(csv, *type, kPluginId, "a.csv");
    const auto second = data::importCsv(csv, *type, kPluginId, "b.csv");
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    // Same plugin + same editorId = the SAME guid, always (re-imports keep
    // cross-references and §5 patches valid).
    CHECK(first->records[0].formId == second->records[0].formId);
    CHECK(first->records[0].formId ==
          data::csvRowGuid(kPluginId, "IronSword"));

    // A different plugin id namespaces the same editorId elsewhere.
    const Guid other = *Guid::fromString(
        "c5000000-0000-4000-8000-000000000002");
    CHECK(data::csvRowGuid(other, "IronSword") !=
          first->records[0].formId);
}

TEST_CASE("csv import: explicit form guid wins; bad rows are skipped") {
    const data::FormTypeRegistry types = makeTypes();
    const auto* type = types.findType("WeaponForm");
    const char* csv =
        "form,editorId,damage\n"
        "c5000000-0000-4000-8000-0000000000aa,Named,7\n"
        "not-a-guid,Broken,9\n"
        ",Derived,3\n";
    const auto plugin = data::importCsv(csv, *type, kPluginId, "test.csv");
    REQUIRE(plugin.has_value());
    REQUIRE(plugin->records.size() == 2); // the malformed guid row dropped
    CHECK(plugin->records[0].formId ==
          *Guid::fromString("c5000000-0000-4000-8000-0000000000aa"));
    CHECK(plugin->records[1].formId == data::csvRowGuid(kPluginId, "Derived"));
}

TEST_CASE("csv import: patch mode targets another plugin's rows (C9.5)") {
    const data::FormTypeRegistry types = makeTypes();
    const auto* type = types.findType("WeaponForm");

    // The base plugin creates the rows (the text-en model).
    const char* baseCsv = "editorId,displayName,damage\n"
                          "IronSword,Iron sword,12\n";
    const auto base = data::importCsv(baseCsv, *type, kPluginId, "base.csv");
    REQUIRE(base.has_value());

    // The pack patches them: same editorId column, but identities derive
    // from the TARGET plugin and the records are field patches.
    const char* packCsv = "editorId,displayName\n"
                          "IronSword,Epee de fer\n";
    const Guid packId =
        *Guid::fromString("c5000000-0000-4000-8000-000000000003");
    const auto pack = data::importCsv(packCsv, *type, packId, "pack.csv",
                                      kPluginId);
    REQUIRE(pack.has_value());
    REQUIRE(pack->records.size() == 1);
    CHECK_FALSE(pack->records[0].creates);
    // The pack declares its target (load-order validation material).
    REQUIRE(pack->dependencies.size() == 1);
    CHECK(pack->dependencies[0] == kPluginId);
    CHECK(pack->records[0].formId == data::csvRowGuid(kPluginId, "IronSword"));
    // The editorId column is identity-only on a patch: exactly ONE field
    // (displayName) rides the record.
    CHECK(pack->records[0].fields.size() == 1);

    // Resolved with the pack after the base: the patched field wins, the
    // untouched ones (editorId, damage) keep the base's values (§5).
    data::FormDatabase db;
    const vector<const data::Plugin*> order { &*base, &*pack };
    data::resolve(order, types, db);
    const auto* sword = db.find<data::WeaponForm>(pack->records[0].formId);
    REQUIRE(sword != nullptr);
    CHECK(sword->displayName == "Epee de fer");
    CHECK(sword->editorId == "IronSword");
}

TEST_CASE("csv import: a header without identity is a reasoned failure") {
    const data::FormTypeRegistry types = makeTypes();
    const auto* type = types.findType("WeaponForm");
    const auto plugin =
        data::importCsv("damage\n12\n", *type, kPluginId, "test.csv");
    REQUIRE_FALSE(plugin.has_value());
    CHECK(plugin.error().find("identity") != str::npos);
}
