#include "gameplay/stats/EquipmentStats.hpp"

#include "data/forms/FormDatabase.hpp"

namespace gameplay {

void armorModifiers(const data::ArmorForm& armor, StatModifiers& mods) {
    mods.add[attr("armorSlash")] += armor.armorSlash;
    mods.add[attr("armorBlunt")] += armor.armorBlunt;
    mods.add[attr("armorPierce")] += armor.armorPierce;
    mods.add[attr("resistFire")] += armor.resistFire;
    mods.add[attr("resistCold")] += armor.resistCold;
    mods.add[attr("resistLightning")] += armor.resistLightning;
    mods.add[attr("resistSonic")] += armor.resistSonic;
    mods.add[attr("resistChemical")] += armor.resistChemical;
    mods.add[attr("resistPsychic")] += armor.resistPsychic;
    mods.add[attr("resistHoly")] += armor.resistHoly;
    mods.add[attr("resistDark")] += armor.resistDark;
    mods.add[attr("resistEther")] += armor.resistEther;
    // Status-buildup endurances (ignition/glaciation/electrocution follow the
    // resist* fields above via the derived-stat formulas — not folded here).
    mods.add[attr("endurancePoison")] += armor.endurancePoison;
    mods.add[attr("enduranceBleed")] += armor.enduranceBleed;
    mods.add[attr("enduranceMental")] += armor.enduranceMental;
    mods.add[attr("enduranceDisease")] += armor.enduranceDisease;
    mods.add[attr("enduranceCurse")] += armor.enduranceCurse;
    mods.add[attr("enduranceDeath")] += armor.enduranceDeath;
    // weight → encumbrance, exposure → temperature: both deferred to a later pass.
}

void applyEquipmentModifiers(const Equipment& equipment,
                             const data::FormDatabase& forms, StatModifiers& mods) {
    const auto slot = [&](const core::Guid& guid) {
        if (const data::ArmorForm* armor = forms.find<data::ArmorForm>(guid)) {
            armorModifiers(*armor, mods);
        }
    };
    slot(equipment.head);
    slot(equipment.torso);
    slot(equipment.arms);
    slot(equipment.legs);
}

DamageEvent weaponDamageEvent(const data::WeaponForm& weapon,
                              const AbilitySystem& attacker) {
    const f32 scale = 1.0f + weapon.scalingK *
                                 currentValueOf(attacker, attr(weapon.scalingAttribute)) /
                                 100.0f;
    DamageEvent event;
    const auto add = [&](DamageType type, f32 base) {
        if (base > 0.0f) {
            event.channels.push_back({ type, base * scale });
        }
    };
    add(DamageType::Slash, weapon.slashAttack);
    add(DamageType::Pierce, weapon.pierceAttack);
    add(DamageType::Blunt, weapon.bluntAttack);
    add(DamageType::Fire, weapon.fireAttack);
    add(DamageType::Lightning, weapon.lightningAttack);
    event.postureAmount = weapon.postureDamage * scale;
    return event;
}

} // namespace gameplay
