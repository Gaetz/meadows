#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/plugins/BinaryFormat.hpp"
#include "data/plugins/PluginLoader.hpp"
#include "data/plugins/TomlWriter.hpp"

using core::Guid;

namespace {

data::FormTypeRegistry makeTypes() {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    return types;
}

// Every FieldKind, built programmatically (the sample forms do not use the
// vector/quat kinds yet).
data::Plugin makeKitchenSink() {
    data::Plugin plugin;
    plugin.id = Guid::generate();
    plugin.name = "kitchen-sink";
    plugin.dependencies.push_back(Guid::generate());

    data::Record record;
    record.formId = Guid::generate();
    record.typeId = 0xDEADBEEF; // unknown on purpose: binary must not care
    record.creates = true;
    record.fields.emplace(1u, reflect::Value { true });
    record.fields.emplace(2u, reflect::Value { i32 { -42 } });
    record.fields.emplace(3u, reflect::Value { u32 { 42 } });
    record.fields.emplace(4u, reflect::Value { 3.5f });
    record.fields.emplace(5u, reflect::Value { str { "héllo wörld" } });
    record.fields.emplace(6u, reflect::Value { Vec2 { 1.0f, 2.0f } });
    record.fields.emplace(7u, reflect::Value { Vec3 { 1.0f, 2.0f, 3.0f } });
    record.fields.emplace(8u,
                          reflect::Value { Vec4 { 1.0f, 2.0f, 3.0f, 4.0f } });
    record.fields.emplace(
        9u, reflect::Value { Quat { 0.7071f, 0.7071f, 0.0f, 0.0f } });
    record.fields.emplace(10u, reflect::Value { Guid::generate() });
    plugin.records.push_back(std::move(record));
    return plugin;
}

void checkPluginsEqual(const data::Plugin& a, const data::Plugin& b) {
    CHECK(a.id == b.id);
    CHECK(a.name == b.name);
    CHECK(a.dependencies == b.dependencies);
    REQUIRE(a.records.size() == b.records.size());
    for (size_t i = 0; i < a.records.size(); ++i) {
        CHECK(a.records[i].formId == b.records[i].formId);
        CHECK(a.records[i].typeId == b.records[i].typeId);
        CHECK(a.records[i].creates == b.records[i].creates);
        REQUIRE(a.records[i].fields.size() == b.records[i].fields.size());
        for (const auto& [fieldId, value] : a.records[i].fields) {
            REQUIRE(b.records[i].fields.contains(fieldId));
            CHECK(b.records[i].fields.at(fieldId) == value);
        }
    }
}

} // namespace

TEST_CASE("cooker: binary roundtrip is identity for every field kind") {
    const data::Plugin original = makeKitchenSink();

    const vector<u8> bytes = data::writePluginBinary(original);
    const auto reread = data::readPluginBinary(bytes, "test");
    REQUIRE(reread.has_value());

    checkPluginsEqual(original, *reread);
}

TEST_CASE("cooker: cooking is deterministic") {
    const data::Plugin plugin = makeKitchenSink();
    CHECK(data::writePluginBinary(plugin) == data::writePluginBinary(plugin));
}

TEST_CASE("cooker: corrupt binary is rejected, never crashes") {
    const vector<u8> good = data::writePluginBinary(makeKitchenSink());

    // Bad magic.
    vector<u8> badMagic = good;
    badMagic[0] = 'X';
    CHECK(!data::readPluginBinary(badMagic, "test").has_value());

    // Future version.
    vector<u8> badVersion = good;
    badVersion[4] = 99;
    CHECK(!data::readPluginBinary(badVersion, "test").has_value());

    // Every possible truncation point.
    for (size_t size = 0; size < good.size(); ++size) {
        const std::span<const u8> cut { good.data(), size };
        CHECK(!data::readPluginBinary(cut, "test").has_value());
    }
}

TEST_CASE("cooker: text -> binary -> text roundtrip through the registry") {
    const auto types = makeTypes();
    const auto original = data::parsePluginToml(R"toml(
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
damage = 12.5
goldValue = 25
twoHanded = true
sprite = "33333333-3333-4333-8333-333333333333"
)toml",
                                                types, "test");
    REQUIRE(original.has_value());

    // text -> binary -> Plugin
    const vector<u8> bytes = data::writePluginBinary(*original);
    const auto fromBinary = data::readPluginBinary(bytes, "test");
    REQUIRE(fromBinary.has_value());

    // Plugin -> text -> Plugin again: full circle.
    const str toml = data::writePluginToml(*fromBinary, types);
    const auto reparsed = data::parsePluginToml(toml, types, "test-reparse");
    REQUIRE(reparsed.has_value());

    checkPluginsEqual(*original, *reparsed);
}

TEST_CASE("cooker: uncooking skips unknown type ids gracefully") {
    const auto types = makeTypes();
    const data::Plugin sink = makeKitchenSink(); // typeId 0xDEADBEEF

    const str toml = data::writePluginToml(sink, types);
    const auto reparsed = data::parsePluginToml(toml, types, "test");
    REQUIRE(reparsed.has_value());
    CHECK(reparsed->records.empty()); // unknown-type record dropped, no crash
}
