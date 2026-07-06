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
// <scalingAttribute> %), read from the attacker's current attacker attribute.
DamageEvent weaponDamageEvent(const data::WeaponForm& weapon,
                              const AbilitySystem& attacker);

// Encumbrance (docs/STATS.md §3 Utility, chantier 6 C3). Categories gate
// movement: penalties fold through StatModifiers (the §2.9-sanctioned
// channel), never by writing a stat directly.
enum class EncumbranceCategory { Light, Medium, Heavy, Overencumbered };

// One item's unit weight, whatever its form type (0 if unknown).
f32 itemWeight(const data::FormDatabase& forms, const core::Guid& item);

// Total carried weight: Σ stacks (weight × count).
f32 inventoryWeight(const data::FormDatabase& forms, const Inventory& inventory);

// light < 40% of max ≤ medium < 70% ≤ heavy ≤ 100% < overencumbered.
// A non-positive max reads as Light (stats not computed yet).
EncumbranceCategory encumbranceCategory(f32 weight, f32 maxEncumbrance);

// Display label ("light" / "medium" / "heavy" / "overencumbered").
const char* encumbranceLabel(EncumbranceCategory category);

// Folds the category's speed/acceleration penalties into `mods`
// (medium −25%/−44%, heavy −50%/−75%, overencumbered −75%/−75%).
void encumbranceModifiers(EncumbranceCategory category, StatModifiers& mods);

} // namespace gameplay
