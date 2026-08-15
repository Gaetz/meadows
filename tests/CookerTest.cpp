#include <doctest/doctest.h>

#include <bit>

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
    record.fields.emplace(11u, reflect::Value { f64 { 1.0e300 } }); // beyond f32
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

// --- Golden on-disk contract ----------------------------------------------------------
// These static_asserts lock FieldKind / Value / KindOf in lockstep with
// EACH OTHER; the two cases below freeze their ACTUAL persisted values and the
// full byte layout of a cooked plugin. If either fails, an on-disk format
// change happened: either revert it, or bump kPluginBinaryVersion and write a
// migration — never ship it silently.

TEST_CASE("cooker: FieldKind on-disk ordinals are frozen (golden)") {
    using reflect::Value;
    const auto ordinal = [](const Value& value) {
        return static_cast<u32>(reflect::valueKind(value));
    };
    CHECK(ordinal(Value { true }) == 0);
    CHECK(ordinal(Value { i32 { 0 } }) == 1);
    CHECK(ordinal(Value { u32 { 0 } }) == 2);
    CHECK(ordinal(Value { 0.0f }) == 3);
    CHECK(ordinal(Value { str {} }) == 4);
    CHECK(ordinal(Value { Vec2 {} }) == 5);
    CHECK(ordinal(Value { Vec3 {} }) == 6);
    CHECK(ordinal(Value { Vec4 {} }) == 7);
    CHECK(ordinal(Value { Quat {} }) == 8);
    CHECK(ordinal(Value { Guid {} }) == 9);
    CHECK(ordinal(Value { f64 { 0.0 } }) == 10);
}

namespace {

// Golden-stream helpers: the EXPECTED bytes are assembled here from the
// documented format (little-endian, kind byte before each value payload,
// quat as x y z w), independently of the production Writer — this test IS
// the format spec.
void goldU8(vector<u8>& out, u8 value) { out.push_back(value); }
void goldU32(vector<u8>& out, u32 value) {
    for (u32 i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(value >> (i * 8)));
    }
}
void goldU64(vector<u8>& out, u64 value) {
    for (u32 i = 0; i < 8; ++i) {
        out.push_back(static_cast<u8>(value >> (i * 8)));
    }
}
void goldF32(vector<u8>& out, f32 value) {
    goldU32(out, std::bit_cast<u32>(value));
}
void goldStr(vector<u8>& out, std::string_view text) {
    goldU32(out, static_cast<u32>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}
void goldGuid(vector<u8>& out, const Guid& guid) {
    goldU64(out, guid.hi);
    goldU64(out, guid.lo);
}

} // namespace

TEST_CASE("cooker: cooked plugin byte layout is frozen (golden)") {
    // Fully deterministic plugin: one record, one field of every kind.
    const Guid pluginId { 0x0102030405060708ull, 0x090a0b0c0d0e0f10ull };
    const Guid depId { 0x1111111111111111ull, 0x2222222222222222ull };
    const Guid assetId { 0x3333333333333333ull, 0x4444444444444444ull };
    const Guid formId { 0x5555555555555555ull, 0x6666666666666666ull };
    const Guid fieldGuid { 0x7777777777777777ull, 0x8888888888888888ull };

    data::Plugin plugin;
    plugin.id = pluginId;
    plugin.name = "golden";
    plugin.dependencies.push_back(depId);
    plugin.assets.push_back({ assetId, "textures/gold.png" });

    data::Record record;
    record.formId = formId;
    record.typeId = 0xABCD1234u;
    record.creates = true;
    record.fields.emplace(1u, reflect::Value { true });
    record.fields.emplace(2u, reflect::Value { i32 { -42 } });
    record.fields.emplace(3u, reflect::Value { u32 { 42 } });
    record.fields.emplace(4u, reflect::Value { 3.5f });
    record.fields.emplace(5u, reflect::Value { str { "or" } });
    record.fields.emplace(6u, reflect::Value { Vec2 { 1.0f, 2.0f } });
    record.fields.emplace(7u, reflect::Value { Vec3 { 1.0f, 2.0f, 3.0f } });
    record.fields.emplace(8u,
                          reflect::Value { Vec4 { 1.0f, 2.0f, 3.0f, 4.0f } });
    record.fields.emplace(
        9u, reflect::Value { Quat { 4.0f, 1.0f, 2.0f, 3.0f } }); // w,x,y,z ctor
    record.fields.emplace(10u, reflect::Value { fieldGuid });
    record.fields.emplace(11u, reflect::Value { f64 { 2.0 } });
    plugin.records.push_back(std::move(record));

    // The expected stream, from the format spec.
    vector<u8> expected;
    expected.insert(expected.end(), { 'M', 'D', 'W', 'P' }); // magic
    goldU32(expected, 1);                                    // version
    goldGuid(expected, pluginId);
    goldStr(expected, "golden");
    goldU32(expected, 1); // dependency count
    goldGuid(expected, depId);
    goldU32(expected, 1); // asset count
    goldGuid(expected, assetId);
    goldStr(expected, "textures/gold.png");
    goldU32(expected, 1); // record count
    goldGuid(expected, formId);
    goldU32(expected, 0xABCD1234u);
    goldU8(expected, 1);   // creates
    goldU32(expected, 11); // field count, sorted by field id
    goldU32(expected, 1);  goldU8(expected, 0);  goldU8(expected, 1); // Bool
    goldU32(expected, 2);  goldU8(expected, 1);                       // I32
    goldU32(expected, std::bit_cast<u32>(i32 { -42 }));
    goldU32(expected, 3);  goldU8(expected, 2);  goldU32(expected, 42); // U32
    goldU32(expected, 4);  goldU8(expected, 3);  goldF32(expected, 3.5f); // F32
    goldU32(expected, 5);  goldU8(expected, 4);  goldStr(expected, "or"); // Str
    goldU32(expected, 6);  goldU8(expected, 5);                        // Vec2
    goldF32(expected, 1.0f); goldF32(expected, 2.0f);
    goldU32(expected, 7);  goldU8(expected, 6);                        // Vec3
    goldF32(expected, 1.0f); goldF32(expected, 2.0f); goldF32(expected, 3.0f);
    goldU32(expected, 8);  goldU8(expected, 7);                        // Vec4
    goldF32(expected, 1.0f); goldF32(expected, 2.0f);
    goldF32(expected, 3.0f); goldF32(expected, 4.0f);
    goldU32(expected, 9);  goldU8(expected, 8); // Quat, written x y z w
    goldF32(expected, 1.0f); goldF32(expected, 2.0f);
    goldF32(expected, 3.0f); goldF32(expected, 4.0f);
    goldU32(expected, 10); goldU8(expected, 9);  goldGuid(expected, fieldGuid);
    goldU32(expected, 11); goldU8(expected, 10); // F64
    goldU64(expected, std::bit_cast<u64>(f64 { 2.0 }));

    CHECK(data::writePluginBinary(plugin) == expected);
}
