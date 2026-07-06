#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormQuery.hpp"
#include "data/forms/FormTypeRegistry.hpp"
#include "data/plugins/PluginConfig.hpp"
#include "data/plugins/Resolver.hpp"

namespace {

// Temp plugin directory fixture (removed on destruction).
struct TempPluginDir {
    std::filesystem::path dir;
    TempPluginDir() {
        dir = std::filesystem::temp_directory_path() /
              "meadows-pluginconfig-test";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
    }
    ~TempPluginDir() { std::filesystem::remove_all(dir); }
    void write(const str& name, const str& content) const {
        std::ofstream out { dir / name, std::ios::binary };
        out << content;
    }
};

constexpr const char* kPluginA = R"(
[plugin]
id = "bbbb0000-0000-4000-8000-000000000001"
name = "a"

[[records]]
form = "bbbb0001-0000-4000-8000-000000000001"
type = "WeaponForm"
new = true
[records.fields]
editorId = "Sword"
damage = 10.0
)";

constexpr const char* kPluginB = R"(
[plugin]
id = "bbbb0000-0000-4000-8000-000000000002"
name = "b"

[[records]]
form = "bbbb0001-0000-4000-8000-000000000001"
type = "WeaponForm"
[records.fields]
damage = 99.0
)";

} // namespace

TEST_CASE("plugin config: parse, write round-trip, defaults") {
    const auto config = data::parsePluginConfigToml(R"(
[[plugins]]
file = "base.toml"
[[plugins]]
file = "mod.toml"
enabled = false
)",
                                                    "test");
    REQUIRE(config.has_value());
    REQUIRE(config->entries.size() == 2);
    CHECK(config->entries[0].file == "base.toml");
    CHECK(config->entries[0].enabled);
    CHECK_FALSE(config->entries[1].enabled);

    // Round-trip through the writer.
    const auto reparsed =
        data::parsePluginConfigToml(data::writePluginConfigToml(*config),
                                    "round-trip");
    REQUIRE(reparsed.has_value());
    REQUIRE(reparsed->entries.size() == 2);
    CHECK(reparsed->entries[1].file == "mod.toml");
    CHECK_FALSE(reparsed->entries[1].enabled);

    // Empty config parses fine.
    CHECK(data::parsePluginConfigToml("", "empty").has_value());
}

TEST_CASE("plugin stack: load order applies, disabled entries skip") {
    TempPluginDir tmp;
    tmp.write("a.toml", kPluginA);
    tmp.write("b.toml", kPluginB);

    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);

    // Default config discovers both, sorted by filename.
    const data::PluginConfig defaults =
        data::defaultConfigFromDirectory(tmp.dir);
    REQUIRE(defaults.entries.size() == 2);
    CHECK(defaults.entries[0].file == "a.toml");

    // Full stack: b patches a's weapon (last writer wins).
    {
        const auto stack = data::loadPluginStack(tmp.dir, defaults, types);
        CHECK(stack.errors.empty());
        data::FormDatabase db;
        data::resolve(data::pointersOf(stack), types, db);
        CHECK(data::findByEditorId<data::WeaponForm>(db, "Sword")->damage ==
              doctest::Approx(99.0f));
    }

    // Disable b: the patch disappears.
    {
        data::PluginConfig config = defaults;
        config.entries[1].enabled = false;
        const auto stack = data::loadPluginStack(tmp.dir, config, types);
        data::FormDatabase db;
        data::resolve(data::pointersOf(stack), types, db);
        CHECK(data::findByEditorId<data::WeaponForm>(db, "Sword")->damage ==
              doctest::Approx(10.0f));
    }

    // Missing file: reported as skipped (an optional layer may list itself
    // before it exists — chantier 4), never fatal.
    {
        data::PluginConfig config = defaults;
        config.entries.push_back({ "missing.toml", true });
        const auto stack = data::loadPluginStack(tmp.dir, config, types);
        CHECK(stack.errors.empty());
        CHECK(stack.skipped.size() == 1);
        CHECK(stack.plugins.size() == 2);
    }
}
