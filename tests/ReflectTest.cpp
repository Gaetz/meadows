#include <doctest/doctest.h>

#include "engine/reflect/Fields.hpp"
#include "engine/reflect/Reflect.hpp"
#include "engine/reflect/Registry.hpp"

namespace {

struct TestBase {
    core::Guid id;
    str editorId;

    REFLECT_BEGIN(TestBase, void)
        REFLECT_FIELD(id)
        REFLECT_FIELD(editorId)
    REFLECT_END()
};

struct TestWeapon : TestBase {
    f32 damage { 10.0f };
    i32 charges { 3 };
    bool twoHanded { false };
    Vec3 hiltOffset { 0.0f, 0.1f, 0.0f };
    f32 cachedDps { 0.0f };
    f64 preciseWeight { 0.0 };

    REFLECT_BEGIN(TestWeapon, TestBase)
        REFLECT_FIELD(damage)
        REFLECT_FIELD(charges)
        REFLECT_FIELD(twoHanded)
        REFLECT_FIELD(hiltOffset)
        REFLECT_FIELD_FLAGS(cachedDps, reflect::Transient)
        REFLECT_FIELD(preciseWeight)
    REFLECT_END()
};

} // namespace

TEST_CASE("reflect: type info carries names, ids, parent") {
    const auto& info = TestWeapon::staticTypeInfo();
    CHECK(info.name == "TestWeapon");
    CHECK(info.id == core::fnv1a("TestWeapon"));
    CHECK(info.size == sizeof(TestWeapon));
    REQUIRE(info.parent != nullptr);
    CHECK(info.parent->name == "TestBase");
    CHECK(info.fields.size() == 6);

    CHECK(info.isA(core::fnv1a("TestBase")));
    CHECK(info.isA(info.id));
    CHECK(!info.isA(core::fnv1a("SomethingElse")));
}

TEST_CASE("reflect: get and set through type-erased fields") {
    TestWeapon weapon;
    const auto& info = TestWeapon::staticTypeInfo();

    const auto* damage = info.findField("damage");
    REQUIRE(damage != nullptr);
    CHECK(damage->kind == reflect::FieldKind::F32);
    CHECK(std::get<f32>(damage->get(&weapon)) == 10.0f);

    CHECK(damage->set(&weapon, reflect::Value { 25.5f }));
    CHECK(weapon.damage == 25.5f);

    const auto* hilt = info.findField("hiltOffset");
    REQUIRE(hilt != nullptr);
    CHECK(hilt->set(&weapon, reflect::Value { Vec3 { 1.0f, 2.0f, 3.0f } }));
    CHECK(weapon.hiltOffset == Vec3 { 1.0f, 2.0f, 3.0f });

    // f64: a value beyond the f32 range proves it is not narrowed.
    const auto* weight = info.findField("preciseWeight");
    REQUIRE(weight != nullptr);
    CHECK(weight->kind == reflect::FieldKind::F64);
    CHECK(weight->set(&weapon, reflect::Value { f64 { 1.0e300 } }));
    CHECK(weapon.preciseWeight == 1.0e300);
    CHECK(std::get<f64>(weight->get(&weapon)) == 1.0e300);
}

TEST_CASE("reflect: set with mismatched kind fails without writing") {
    TestWeapon weapon;
    const auto* damage = TestWeapon::staticTypeInfo().findField("damage");
    REQUIRE(damage != nullptr);

    CHECK(!damage->set(&weapon, reflect::Value { str { "oops" } }));
    CHECK(!damage->set(&weapon, reflect::Value { i32 { 5 } })); // no coercion
    CHECK(weapon.damage == 10.0f);
}

TEST_CASE("reflect: field lookup walks the parent chain") {
    TestWeapon weapon;
    weapon.editorId = "IronSword";

    const auto& info = TestWeapon::staticTypeInfo();
    const auto* editorId = info.findField("editorId");
    REQUIRE(editorId != nullptr);
    CHECK(std::get<str>(editorId->get(&weapon)) == "IronSword");

    CHECK(info.findField("doesNotExist") == nullptr);
}

TEST_CASE("reflect: transient flag is carried") {
    const auto* cached = TestWeapon::staticTypeInfo().findField("cachedDps");
    REQUIRE(cached != nullptr);
    CHECK((cached->flags & reflect::Transient) != 0);
    const auto* damage = TestWeapon::staticTypeInfo().findField("damage");
    REQUIRE(damage != nullptr);
    CHECK((damage->flags & reflect::Transient) == 0);
}

TEST_CASE("reflect: registry finds types by id and name") {
    reflect::Registry registry;
    registry.registerType<TestBase>();
    registry.registerType<TestWeapon>();

    const auto* byName = registry.find("TestWeapon");
    REQUIRE(byName != nullptr);
    CHECK(byName == &TestWeapon::staticTypeInfo());
    CHECK(registry.find(core::fnv1a("TestBase")) ==
          &TestBase::staticTypeInfo());
    CHECK(registry.find("Unknown") == nullptr);
}

TEST_CASE("reflect: copyFields clones every field, transient included") {
    TestWeapon src;
    src.editorId = "Cloned";
    src.damage = 42.0f;
    src.charges = 7;
    src.twoHanded = true;
    src.cachedDps = 3.5f;
    TestWeapon dst;
    reflect::copyFields(TestWeapon::staticTypeInfo(), &src, &dst);
    CHECK(dst.editorId == "Cloned"); // inherited field, parents first
    CHECK(dst.damage == 42.0f);
    CHECK(dst.charges == 7);
    CHECK(dst.twoHanded);
    CHECK(dst.cachedDps == 3.5f); // a clone, not a serialization
}

TEST_CASE("reflect: hashReflected is stable and ignores transient fields") {
    TestWeapon a;
    TestWeapon b;
    const auto hashOf = [](const TestWeapon& w) {
        core::Fnv64 h;
        reflect::hashReflected(TestWeapon::staticTypeInfo(), &w, h);
        return h.value;
    };
    CHECK(hashOf(a) == hashOf(b));
    b.cachedDps = 99.0f; // Transient: never part of the key
    CHECK(hashOf(a) == hashOf(b));
    b.damage = 11.0f;
    CHECK(hashOf(a) != hashOf(b));
    b.damage = a.damage;
    b.editorId = "x"; // an inherited field counts too
    CHECK(hashOf(a) != hashOf(b));
    CHECK(hashOf(a) != core::kFnv64Basis);
}

TEST_CASE("reflect: fieldId resolves declared names through the parent chain") {
    const auto& info = TestWeapon::staticTypeInfo();
    CHECK(reflect::fieldId(info, "damage") == core::fnv1a("damage"));
    CHECK(reflect::fieldId(info, "editorId") == core::fnv1a("editorId"));
}

namespace {

struct TestOwned {
    core::Guid owner;
    f32 legacy { 0.0f };

    REFLECT_BEGIN(TestOwned, void)
        REFLECT_FIELD_FLAGS(owner, reflect::ParentLink)
        REFLECT_FIELD_FLAGS(legacy, reflect::Deprecated)
    REFLECT_END()
};

} // namespace

TEST_CASE("reflect: ParentLink and Deprecated flags are carried") {
    const auto& info = TestOwned::staticTypeInfo();
    const auto* owner = info.findField("owner");
    REQUIRE(owner != nullptr);
    CHECK((owner->flags & reflect::ParentLink) != 0);
    CHECK((owner->flags & reflect::Deprecated) == 0);
    const auto* legacy = info.findField("legacy");
    REQUIRE(legacy != nullptr);
    CHECK((legacy->flags & reflect::Deprecated) != 0);
    CHECK((legacy->flags & reflect::Transient) == 0); // still serialized
}

namespace {

enum class TestColor { Red, Green, Blue };
constexpr std::string_view kTestColorNames[] = { "red", "green", "blue" };
constexpr reflect::EnumTable kTestColors { kTestColorNames };
constexpr reflect::EnumTable kTestColorsOrNone { kTestColorNames, true };

struct TestPainted {
    str color { "red" };
    i32 shade { 0 };

    REFLECT_BEGIN(TestPainted, void)
        REFLECT_FIELD_ENUM(color, kTestColors)
        REFLECT_FIELD_ENUM(shade, kTestColors)
    REFLECT_END()
};

} // namespace

TEST_CASE("reflect: an EnumTable parses, names and accepts its vocabulary") {
    static_assert(kTestColors.size() == 3);
    CHECK(reflect::parseEnum<TestColor>(kTestColors, "green") == TestColor::Green);
    CHECK(!reflect::parseEnum<TestColor>(kTestColors, "Green").has_value());
    CHECK(reflect::enumName(kTestColors, TestColor::Blue) == "blue");
    CHECK(kTestColors.accepts(std::string_view { "red" }));
    CHECK(!kTestColors.accepts(std::string_view {}));
    CHECK(kTestColorsOrNone.accepts(std::string_view {}));
    CHECK(kTestColors.accepts(i32 { 2 }));
    CHECK(!kTestColors.accepts(i32 { 3 }));
    CHECK(!kTestColors.accepts(i32 { -1 }));
    CHECK(kTestColors.acceptsValue(reflect::Value { str { "blue" } }));
    CHECK(!kTestColors.acceptsValue(reflect::Value { str { "mauve" } }));
    CHECK(kTestColors.acceptsValue(reflect::Value { 1.5f })); // not a vocabulary kind
    CHECK(kTestColors.joined() == "red | green | blue");

    const auto& info = TestPainted::staticTypeInfo();
    const auto* color = info.findField("color");
    REQUIRE(color != nullptr);
    CHECK(color->enumeration == &kTestColors);
    const auto* shade = info.findField("shade");
    REQUIRE(shade != nullptr);
    CHECK(shade->enumeration == &kTestColors);
    CHECK(info.findField("color")->kind == reflect::FieldKind::Str);
}
