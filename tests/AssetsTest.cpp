#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/BinaryFormat.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "engine/assets/AssetDatabase.hpp"

using core::Guid;

TEST_CASE("assets: last layer wins per guid, others untouched") {
    const Guid swordTex = Guid::generate();
    const Guid grassTex = Guid::generate();

    assets::AssetDatabase db;
    // Base layer.
    db.add(swordTex, "base", "textures/iron_sword.png");
    db.add(grassTex, "base", "textures/grass.png");
    // Mod layer: overrides the sword only.
    db.add(swordTex, "mods/golden", "textures/gold_sword.png");

    CHECK(db.count() == 2);
    const auto sword = db.resolve(swordTex);
    REQUIRE(sword.has_value());
    CHECK(*sword == std::filesystem::path { "mods/golden" } /
                        "textures/gold_sword.png");
    const auto grass = db.resolve(grassTex);
    REQUIRE(grass.has_value());
    CHECK(*grass ==
          std::filesystem::path { "base" } / "textures/grass.png");

    CHECK(!db.resolve(Guid::generate()).has_value());
    db.add(Guid {}, "base", "nope.png"); // invalid guid ignored
    CHECK(db.count() == 2);
}

TEST_CASE("assets: plugin [assets] table parses, bad entries skipped") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);

    const auto plugin = data::parsePluginToml(R"toml(
[plugin]
id = "11111111-1111-4111-8111-111111111111"
name = "asset-mod"

[assets]
"33333333-3333-4333-8333-333333333333" = "textures/sword.png"
"not-a-guid" = "textures/oops.png"
"44444444-4444-4444-8444-444444444444" = ""
)toml",
                                              types, "test");
    REQUIRE(plugin.has_value());
    REQUIRE(plugin->assets.size() == 1);
    CHECK(plugin->assets[0].id ==
          *Guid::fromString("33333333-3333-4333-8333-333333333333"));
    CHECK(plugin->assets[0].path == "textures/sword.png");
}

TEST_CASE("assets: manifest survives the binary roundtrip") {
    data::Plugin plugin;
    plugin.id = Guid::generate();
    plugin.name = "with-assets";
    plugin.assets.push_back({ Guid::generate(), "textures/a.png" });
    plugin.assets.push_back({ Guid::generate(), "sounds/b.ogg" });

    const auto bytes = data::writePluginBinary(plugin);
    const auto reread = data::readPluginBinary(bytes, "test");
    REQUIRE(reread.has_value());
    REQUIRE(reread->assets.size() == 2);
    CHECK(reread->assets[0].id == plugin.assets[0].id);
    CHECK(reread->assets[0].path == plugin.assets[0].path);
    CHECK(reread->assets[1].path == plugin.assets[1].path);
}
