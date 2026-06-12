#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormDatabase.hpp"
#include "data/forms/FormTypeRegistry.hpp"

using core::Guid;

TEST_CASE("forms: registry instantiates registered types with defaults") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);

    const auto* weaponType = types.findType("WeaponForm");
    REQUIRE(weaponType != nullptr);

    auto form = types.instantiate(weaponType->id);
    REQUIRE(form != nullptr);
    auto* weapon = static_cast<data::WeaponForm*>(form.get());
    CHECK(weapon->damage == 10.0f);   // C++ defaults = layer zero
    CHECK(weapon->twoHanded == false);

    CHECK(types.instantiate(12345) == nullptr); // unknown type
}

TEST_CASE("forms: database lookups by guid, handle, and type") {
    data::FormTypeRegistry types;
    data::registerCoreFormTypes(types);
    data::FormDatabase db;

    auto sword = std::make_unique<data::WeaponForm>();
    sword->id = Guid::generate();
    sword->editorId = "IronSword";
    const Guid swordId = sword->id;

    auto guard = std::make_unique<data::ActorForm>();
    guard->id = Guid::generate();
    const Guid guardId = guard->id;

    const auto swordHandle =
        db.add(std::move(sword), data::WeaponForm::staticTypeInfo());
    const auto guardHandle =
        db.add(std::move(guard), data::ActorForm::staticTypeInfo());
    REQUIRE(swordHandle.isValid());
    REQUIRE(guardHandle.isValid());
    CHECK(db.count() == 2);

    CHECK(db.get(swordHandle) == db.find(swordId));
    CHECK(db.handleOf(swordId) == swordHandle);
    CHECK(db.typeOf(swordHandle)->name == "WeaponForm");

    // Typed access with isA check.
    const auto* typed = db.find<data::WeaponForm>(swordId);
    REQUIRE(typed != nullptr);
    CHECK(typed->editorId == "IronSword");
    CHECK(db.find<data::WeaponForm>(guardId) == nullptr); // wrong type
    CHECK(db.find<data::ActorForm>(guardId) != nullptr);

    CHECK(db.find(Guid::generate()) == nullptr);
    CHECK(db.get(data::FormHandle {}) == nullptr);
}

TEST_CASE("forms: database rejects invalid and duplicate guids") {
    data::FormDatabase db;

    auto noId = std::make_unique<data::WeaponForm>();
    CHECK(!db.add(std::move(noId), data::WeaponForm::staticTypeInfo())
               .isValid());

    auto first = std::make_unique<data::WeaponForm>();
    first->id = Guid::generate();
    const Guid sharedId = first->id;
    CHECK(db.add(std::move(first), data::WeaponForm::staticTypeInfo())
              .isValid());

    auto duplicate = std::make_unique<data::WeaponForm>();
    duplicate->id = sharedId;
    CHECK(!db.add(std::move(duplicate), data::WeaponForm::staticTypeInfo())
               .isValid());
    CHECK(db.count() == 1);
}
