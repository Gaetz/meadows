#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/PluginLoader.hpp"

using core::Guid;

namespace {

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    return types;
}

constexpr const char* kBasePlugin = R"toml(
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
goldValue = 25
twoHanded = false
sprite = "33333333-3333-4333-8333-333333333333"

[[records]]
form = "44444444-4444-4444-8444-444444444444"
type = "ActorForm"
new = true
[records.fields]
editorId = "Guard"
maxHealth = 150.0
)toml";

} // namespace

TEST_CASE("plugin loader: parses header, records, and typed fields") {
    const auto types = makeTypes();
    const auto plugin = data::parsePluginToml(kBasePlugin, types, "test");
    REQUIRE(plugin.has_value());

    CHECK(plugin->name == "base-game");
    CHECK(plugin->id ==
          *Guid::fromString("11111111-1111-4111-8111-111111111111"));
    REQUIRE(plugin->records.size() == 2);

    const data::Record& sword = plugin->records[0];
    CHECK(sword.creates);
    CHECK(sword.typeId == core::fnv1a("WeaponForm"));
    CHECK(sword.fields.size() == 6);

    const auto damage = sword.fields.at(core::fnv1a("damage"));
    CHECK(std::get<f32>(damage) == 12.0f);
    const auto gold = sword.fields.at(core::fnv1a("goldValue"));
    CHECK(std::get<i32>(gold) == 25);
    const auto sprite = sword.fields.at(core::fnv1a("sprite"));
    CHECK(std::get<Guid>(sprite) ==
          *Guid::fromString("33333333-3333-4333-8333-333333333333"));
}

TEST_CASE("plugin loader: a patch record carries only its fields") {
    const auto types = makeTypes();
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "55555555-5555-4555-8555-555555555555"
name = "rebalance-mod"

[[records]]
form = "22222222-2222-4222-8222-222222222222"
type = "WeaponForm"
[records.fields]
damage = 20.0
)toml",
                                              types, "test");
    REQUIRE(plugin.has_value());
    REQUIRE(plugin->records.size() == 1);
    CHECK(!plugin->records[0].creates);
    CHECK(plugin->records[0].fields.size() == 1);
}

TEST_CASE("plugin loader: tolerant of broken mod data") {
    const auto types = makeTypes();
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "66666666-6666-4666-8666-666666666666"

[[records]]
form = "not-a-guid"
type = "WeaponForm"

[[records]]
form = "77777777-7777-4777-8777-777777777777"
type = "ChairForm"

[[records]]
form = "88888888-8888-4888-8888-888888888888"
type = "WeaponForm"
[records.fields]
damage = "a lot"
unknownField = 3
weight = 2.5
)toml",
                                              types, "test");
    REQUIRE(plugin.has_value());
    // Bad-guid and unknown-type records skipped; last record survives with
    // only its valid field.
    REQUIRE(plugin->records.size() == 1);
    CHECK(plugin->records[0].fields.size() == 1);
    CHECK(plugin->records[0].fields.contains(core::fnv1a("weight")));
}

TEST_CASE("plugin loader: hard failures return nullopt") {
    const auto types = makeTypes();
    CHECK(!data::parsePluginToml("this is { not toml", types, "test")
               .has_value());
    CHECK(!data::parsePluginToml("[plugin]\nname = 'no id'", types, "test")
               .has_value());
    CHECK(!data::parsePluginToml("answer = 42", types, "test").has_value());
}

TEST_CASE("plugin loader: empty plugin and dependencies parse") {
    const auto types = makeTypes();
    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "99999999-9999-4999-8999-999999999999"
name = "asset-only"
dependencies = ["11111111-1111-4111-8111-111111111111", "garbage"]
)toml",
                                              types, "test");
    REQUIRE(plugin.has_value());
    CHECK(plugin->records.empty());
    CHECK(plugin->dependencies.size() == 1); // garbage logged and dropped
}
