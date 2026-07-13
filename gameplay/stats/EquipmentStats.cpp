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

    // Chantier 6 C1: the attacker's flat `attack` folds into the
    // strongest PHYSICAL channel (a weapon's physical type is exclusive
    // per swing, docs/STATS.md §3); bare fists get a blunt channel.
    const f32 attack = currentValueOf(attacker, attr("attack"));
    if (attack > 0.0f) {
        DamageChannel* strongest = nullptr;
        for (DamageChannel& channel : event.channels) {
            const bool physical = channel.type == DamageType::Slash ||
                                  channel.type == DamageType::Pierce ||
                                  channel.type == DamageType::Blunt;
            if (physical && (!strongest || channel.amount > strongest->amount)) {
                strongest = &channel;
            }
        }
        if (strongest) {
            strongest->amount += attack;
        } else {
            event.channels.push_back({ DamageType::Blunt, attack });
        }
    }
    // Pens + crit multiplier ride the event; the attack site decides
    // `critical` (target in its critical window).
    event.armorPenetration =
        currentValueOf(attacker, attr("armorPenetration"));
    event.resistPenetration =
        currentValueOf(attacker, attr("resistPenetration"));
    const f32 critical = currentValueOf(attacker, attr("criticalDamage"));
    if (critical > 0.0f) {
        event.criticalMultiplier = critical;
    }
    return event;
}

f32 itemWeight(const data::FormDatabase& forms, const core::Guid& item) {
    if (const auto* weapon = forms.find<data::WeaponForm>(item)) {
        return weapon->weight;
    }
    if (const auto* armor = forms.find<data::ArmorForm>(item)) {
        return armor->weight;
    }
    if (const auto* consumable = forms.find<data::ConsumableForm>(item)) {
        return consumable->weight;
    }
    if (const auto* misc = forms.find<data::MiscItemForm>(item)) {
        return misc->weight;
    }
    return 0.0f;
}

f32 inventoryWeight(const data::FormDatabase& forms, const Inventory& inventory) {
    f32 total = 0.0f;
    for (const ItemStack& stack : inventory.items) {
        if (stack.count > 0) {
            total += itemWeight(forms, stack.item) * static_cast<f32>(stack.count);
        }
    }
    return total;
}

EncumbranceCategory encumbranceCategory(f32 weight, f32 maxEncumbrance) {
    if (maxEncumbrance <= 0.0f) {
        return EncumbranceCategory::Light;
    }
    const f32 ratio = weight / maxEncumbrance;
    if (ratio < 0.4f)  return EncumbranceCategory::Light;
    if (ratio < 0.7f)  return EncumbranceCategory::Medium;
    if (ratio <= 1.0f) return EncumbranceCategory::Heavy;
    return EncumbranceCategory::Overencumbered;
}

const char* encumbranceLabel(EncumbranceCategory category) {
    switch (category) {
    case EncumbranceCategory::Light:  return "light";
    case EncumbranceCategory::Medium: return "medium";
    case EncumbranceCategory::Heavy:  return "heavy";
    case EncumbranceCategory::Overencumbered: return "overencumbered";
    }
    return "light";
}

void encumbranceModifiers(EncumbranceCategory category, StatModifiers& mods) {
    f32 speed = 1.0f, accel = 1.0f;
    switch (category) {
    case EncumbranceCategory::Light:  return; // no effect
    case EncumbranceCategory::Medium: speed = 0.75f; accel = 0.56f; break;
    case EncumbranceCategory::Heavy:  speed = 0.50f; accel = 0.25f; break;
    case EncumbranceCategory::Overencumbered:
        speed = 0.25f; accel = 0.25f; break;
    }
    // Multiplicative fold: stacks with any other ×-modifier on the stat.
    auto fold = [&](const char* stat, f32 factor) {
        auto [it, inserted] = mods.mul.try_emplace(attr(stat), factor);
        if (!inserted) {
            it->second *= factor;
        }
    };
    fold("movementSpeed", speed);
    fold("acceleration", accel);
}

// ---- FOLLOWERS É7 -----------------------------------------------------------

bool itemUnremovable(const data::FormDatabase& forms, const core::Guid& item) {
    if (const auto* weapon = forms.find<data::WeaponForm>(item)) {
        return weapon->unremovable;
    }
    if (const auto* armor = forms.find<data::ArmorForm>(item)) {
        return armor->unremovable;
    }
    if (const auto* consumable = forms.find<data::ConsumableForm>(item)) {
        return consumable->unremovable;
    }
    if (const auto* misc = forms.find<data::MiscItemForm>(item)) {
        return misc->unremovable;
    }
    return false;
}

f32 gearPower(const data::FormDatabase& forms, const core::Guid& item) {
    if (const auto* weapon = forms.find<data::WeaponForm>(item)) {
        f32 power = weapon->damage; // legacy 2D-era fallback channel
        power = power < weapon->slashAttack ? weapon->slashAttack : power;
        power = power < weapon->pierceAttack ? weapon->pierceAttack : power;
        power = power < weapon->bluntAttack ? weapon->bluntAttack : power;
        power = power < weapon->fireAttack ? weapon->fireAttack : power;
        power = power < weapon->lightningAttack ? weapon->lightningAttack
                                                : power;
        return power;
    }
    if (const auto* armor = forms.find<data::ArmorForm>(item)) {
        return armor->armorSlash;
    }
    return 0.0f;
}

std::optional<GearSlot> isUpgrade(const data::FormDatabase& forms,
                                  const core::Guid& item,
                                  const Equipment& equipment) {
    std::optional<GearSlot> slot;
    const core::Guid* current = nullptr;
    if (forms.find<data::WeaponForm>(item)) {
        slot = GearSlot::Weapon;
        current = &equipment.weapon;
    } else if (const auto* armor = forms.find<data::ArmorForm>(item)) {
        if (armor->slot == "head") {
            slot = GearSlot::Head;
            current = &equipment.head;
        } else if (armor->slot == "torso") {
            slot = GearSlot::Torso;
            current = &equipment.torso;
        } else if (armor->slot == "arms") {
            slot = GearSlot::Arms;
            current = &equipment.arms;
        } else if (armor->slot == "legs") {
            slot = GearSlot::Legs;
            current = &equipment.legs;
        }
    }
    if (!slot || *current == item) {
        return std::nullopt; // not gear / unknown slot / already worn
    }
    const f32 incumbent =
        current->isValid() ? gearPower(forms, *current) : 0.0f;
    return gearPower(forms, item) > incumbent ? slot : std::nullopt;
}

core::Guid& gearSlotRef(Equipment& equipment, GearSlot slot) {
    switch (slot) {
    case GearSlot::Weapon: return equipment.weapon;
    case GearSlot::Head:   return equipment.head;
    case GearSlot::Torso:  return equipment.torso;
    case GearSlot::Arms:   return equipment.arms;
    case GearSlot::Legs:   return equipment.legs;
    }
    return equipment.weapon; // unreachable
}

} // namespace gameplay
