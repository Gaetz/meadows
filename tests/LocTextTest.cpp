#include <doctest/doctest.h>

#include "data/forms/FormDatabase.hpp"
#include "data/forms/LocForms.hpp"
#include "data/plugins/CsvImport.hpp"
#include "data/plugins/Resolver.hpp"
#include "game/Settings.hpp" // languagePackCode

// The localisation seam: LocStringForm
// records (key = editorId) -> TextTable index -> loc lookups, fed by the
// CSV import pipeline end to end.

using core::Guid;

namespace {
const Guid kPluginId =
    *Guid::fromString("c5000000-0000-4000-8000-0000000000f1");
}

TEST_CASE("loc: the CSV pipeline feeds the text table end to end") {
    data::FormTypeRegistry types;
    data::registerLocFormTypes(types);
    const auto* type = types.findType("LocStringForm");
    REQUIRE(type != nullptr);

    const char* csv = "editorId,text\n"
                      "crime.observed,\"Crime observe ! Prime : {} pieces "
                      "d'or.\"\n"
                      "talk.greeting,\"Belle journee, voyageur.\"\n";
    const auto plugin = data::importCsv(csv, *type, kPluginId, "fr.csv");
    REQUIRE(plugin.has_value());

    data::FormDatabase db;
    const vector<const data::Plugin*> order { &*plugin };
    data::resolve(order, types, db);

    data::TextTable texts;
    texts.build(db);
    CHECK(texts.size() == 2);
    CHECK(texts.get("talk.greeting") == "Belle journee, voyageur.");
    CHECK(texts.format("crime.observed", "40") ==
          "Crime observe ! Prime : 40 pieces d'or.");
}

TEST_CASE("loc: a missing key falls back to the key itself") {
    data::TextTable texts; // empty
    CHECK(texts.get("no.such.key") == "no.such.key");
    // format on a template-less text leaves it untouched.
    CHECK(texts.format("no.such.key", "x") == "no.such.key");
}

TEST_CASE("loc: format fills N slots left to right (C9.5)") {
    data::FormTypeRegistry types;
    data::registerLocFormTypes(types);
    const auto* type = types.findType("LocStringForm");

    const char* csv = "editorId,text\n"
                      "quest.done,\"Quest complete: {}{}.\"\n"
                      "quest.reward,\" (+{} {})\"\n"
                      "plain,No slots here.\n";
    const auto plugin = data::importCsv(csv, *type, kPluginId, "t.csv");
    REQUIRE(plugin.has_value());
    data::FormDatabase db;
    const vector<const data::Plugin*> order { &*plugin };
    data::resolve(order, types, db);
    data::TextTable texts;
    texts.build(db);

    CHECK(texts.format("quest.reward", { "3", "Gold" }) == " (+3 Gold)");
    CHECK(texts.format("quest.done", { "The Long Road", " (+3 Gold)" }) ==
          "Quest complete: The Long Road (+3 Gold).");
    // Fewer args than slots: the extra "{}" stays literal (visible hole).
    CHECK(texts.format("quest.done", { "The Long Road" }) ==
          "Quest complete: The Long Road{}.");
    // No args / extra args on a slotless text: untouched, extras dropped.
    CHECK(texts.format("plain", {}) == "No slots here.");
    CHECK(texts.format("plain", { "x", "y" }) == "No slots here.");
    // An arg that CONTAINS "{}" is data, never a new slot.
    CHECK(texts.format("quest.reward", { "{}", "Gold" }) == " (+{} Gold)");
    // The 1-arg overload keeps its historical behavior.
    CHECK(texts.format("quest.reward", str { "9" }) == " (+9 {})");
}

TEST_CASE("loc: language packs are text-<code>.toml, English is the base") {
    CHECK(game::languagePackCode("base/text-fr.toml") == "fr");
    CHECK(game::languagePackCode("text-de.toml") == "de");
    CHECK(game::languagePackCode("mods\\text-eo.toml") == "eo");
    // The base language and non-pack files gate nothing.
    CHECK_FALSE(game::languagePackCode("base/text-en.toml").has_value());
    CHECK_FALSE(game::languagePackCode("base/village.toml").has_value());
    CHECK_FALSE(game::languagePackCode("text-.toml").has_value());
    CHECK_FALSE(game::languagePackCode("text-Tables2.toml").has_value());
    CHECK_FALSE(game::languagePackCode("context-fr.toml").has_value());
}

TEST_CASE("loc: a language pack patches text through §5 layering") {
    data::FormTypeRegistry types;
    data::registerLocFormTypes(types);
    const auto* type = types.findType("LocStringForm");

    const auto base = data::importCsv("editorId,text\ntalk.greeting,Bonjour\n",
                                      *type, kPluginId, "fr.csv");
    REQUIRE(base.has_value());

    // The pack patches the SAME record (explicit `form` guid = the base
    // row's derived identity) with creates left implicit-false semantics:
    // here we just re-create with the same guid — last writer wins per
    // field (§5), which is exactly what a pack needs.
    const str packCsv =
        "form,text\n" +
        data::csvRowGuid(kPluginId, "talk.greeting").toString() + ",Hello\n";
    const Guid packId =
        *Guid::fromString("c5000000-0000-4000-8000-0000000000f2");
    const auto pack = data::importCsv(packCsv, *type, packId, "en.csv");
    REQUIRE(pack.has_value());

    data::FormDatabase db;
    const vector<const data::Plugin*> order { &*base, &*pack };
    data::resolve(order, types, db);
    data::TextTable texts;
    texts.build(db);
    CHECK(texts.get("talk.greeting") == "Hello");
}
