#include <doctest/doctest.h>

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
