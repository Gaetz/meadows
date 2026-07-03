#include <memory>

#include <doctest/doctest.h>

#include "data/forms/CoreForms.hpp"
#include "data/forms/FormDatabase.hpp"
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayEffects.hpp"
#include "gameplay/inventory/Inventory.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/EquipmentStats.hpp"

using namespace gameplay;

TEST_CASE("equipment: armor folds its mitigation % into the modifiers") {
    data::ArmorForm armor;
    armor.armorSlash = 20.0f;
    armor.resistFire = 10.0f;
    StatModifiers mods;
    armorModifiers(armor, mods);
    CHECK(mods.add[attr("armorSlash")] == doctest::Approx(20.0f));
    CHECK(mods.add[attr("resistFire")] == doctest::Approx(10.0f));
}

TEST_CASE("equipment: armor stacks onto the attribute-derived stat at recompute") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core; // strength 6 → armorSlash = 0.5 × 6 = 3
    AttributeSet vitals;
    AbilitySystem sys;

    data::ArmorForm armor;
    armor.armorSlash = 20.0f;
    StatModifiers mods;
    armorModifiers(armor, mods);

    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg, &mods);
    CHECK(currentValueOf(sys, attr("armorSlash")) == doctest::Approx(23.0f)); // 3+20
}

TEST_CASE("equipment: armor raises a status-buildup endurance threshold") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core; // dexterity 6 → endurancePoison = 100 + 6×0.5 = 103
    AttributeSet vitals;
    AbilitySystem sys;

    data::ArmorForm armor;
    armor.endurancePoison = 40.0f;
    StatModifiers mods;
    armorModifiers(armor, mods);
    CHECK(mods.add[attr("endurancePoison")] == doctest::Approx(40.0f));

    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg, &mods);
    CHECK(currentValueOf(sys, attr("endurancePoison")) == doctest::Approx(143.0f));
}

TEST_CASE("equipment: negative lightning resistance lowers electrocution endurance") {
    DerivedStatRegistry reg;
    registerCoreDerivedStats(reg);
    CoreAttributes core; // insight 0 → resistLightning 0 → enduranceElectrocution 100
    AttributeSet vitals;
    AbilitySystem sys;

    // Iron-like: resistLightning −22 → enduranceElectrocution follows it (§1).
    data::ArmorForm armor;
    armor.resistLightning = -22.0f;
    StatModifiers mods;
    armorModifiers(armor, mods);

    const AttrSetRef sets[] = {
        { &CoreAttributes::staticTypeInfo(), &core },
        { &AttributeSet::staticTypeInfo(), &vitals },
    };
    recomputeCurrent(sys, sets, &reg, &mods);
    CHECK(currentValueOf(sys, attr("resistLightning")) == doctest::Approx(-22.0f));
    CHECK(currentValueOf(sys, attr("enduranceElectrocution")) == doctest::Approx(78.0f));
}

TEST_CASE("equipment: weaponDamageEvent scales by the attacker's attribute") {
    data::WeaponForm weapon;
    weapon.slashAttack = 50.0f;
    weapon.scalingAttribute = "strength";
    weapon.scalingK = 2.0f;
    weapon.postureDamage = 10.0f;

    AbilitySystem sys;
    setCurrentValue(sys, attr("strength"), 20.0f); // scale = 1 + 2 × 20/100 = 1.4

    const DamageEvent event = weaponDamageEvent(weapon, sys);
    REQUIRE(event.channels.size() == 1);
    CHECK(event.channels[0].type == DamageType::Slash);
    CHECK(event.channels[0].amount == doctest::Approx(70.0f));  // 50 × 1.4
    CHECK(event.postureAmount == doctest::Approx(14.0f));       // 10 × 1.4
}

TEST_CASE("equipment: applyEquipmentModifiers resolves slots from the database") {
    data::FormDatabase forms;
    auto armor = std::make_unique<data::ArmorForm>();
    armor->id = *core::Guid::fromString("a4000000-0000-4000-8000-000000000001");
    armor->armorBlunt = 15.0f;
    const core::Guid guid = armor->id;
    forms.add(std::move(armor), data::ArmorForm::staticTypeInfo());

    Equipment equipment;
    equipment.torso = guid;
    StatModifiers mods;
    applyEquipmentModifiers(equipment, forms, mods);
    CHECK(mods.add[attr("armorBlunt")] == doctest::Approx(15.0f));
}
