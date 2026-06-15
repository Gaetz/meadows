#pragma once

#include "data/forms/CoreForms.hpp"           // WeaponForm, ArmorForm
#include "gameplay/ability/AbilitySystem.hpp"  // AbilitySystem, attr
#include "gameplay/ability/DerivedStats.hpp"   // StatModifiers
#include "gameplay/inventory/Inventory.hpp"    // Equipment
#include "gameplay/stats/Damage.hpp"           // DamageEvent

// The stats↔items seam (docs/STATS.md §3): armor contributes its mitigation % to
// the derived stats (through StatModifiers, folded into the recompute), and a
// weapon builds the raw typed damage it deals (scaled by the attacker).

namespace data {
class FormDatabase;
}

namespace gameplay {

// Folds one armor piece's mitigation % into `mods`, so the derived armor/
// resistance stats include it on the next recompute.
void armorModifiers(const data::ArmorForm& armor, StatModifiers& mods);

// Resolves each equipped armor slot from the database and folds its modifiers in.
void applyEquipmentModifiers(const Equipment& equipment,
                             const data::FormDatabase& forms, StatModifiers& mods);

// The raw typed damage a weapon deals: each channel = base × (1 + scalingK ·
// <scalingAttribute> %), read from the attacker's current attribute.
DamageEvent weaponDamageEvent(const data::WeaponForm& weapon,
                              const AbilitySystem& attacker);

} // namespace gameplay
