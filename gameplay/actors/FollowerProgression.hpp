#pragma once

#include <array>
#include <optional>

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp" // ability guids (perks)

namespace data {
class FormDatabase; // perk children lookups
}

namespace gameplay {

struct StatsTuningForm;
class GameplayTagRegistry;

// ---- Classes, levels, evolution -----------------------------------------
// Reused systems (§2.11): the classAttributesAt curves, the equipmentMods
// StatModifiers fold (buildCharacterMods' external-mods channel), the
// recomputeStats machinery (override ?? formula), and the seeded
// determinism (§8 — nothing here draws randomness). All pure and headless.

struct CoreAttributes;
struct FollowerClassForm;
struct StatModifiers;

// The nine core attributes by canonical index (the CoreAttributes field
// order — the same order classAttributesAt and SavedStatsForm use).
inline constexpr u32 kCoreAttributeCount { 9 };
extern const std::array<const char*, kCoreAttributeCount> kCoreAttributeNames;
f32 coreAttributeValue(const CoreAttributes& core, u32 index);
f32& coreAttributeRef(CoreAttributes& core, u32 index);

// Writes the 9 CoreAttributes BASE fields from the class curves at `level`
// (§2.9: the sanctioned SPAWN-time init write — like the Spawner's field
// apply, it seeds bases once, before initializeActorStats derives the
// maxima and fills vitals). Fresh actors only: a saved actor keeps his
// captured bases (the SavedStatsForm sentinel, same gate as loadouts).
void applyFollowerClass(CoreAttributes& core, const FollowerClassForm& cls,
                        f32 level);

// The LEVEL-CHANGE write: adds the curve DELTA between the two levels to
// the current bases instead of overwriting them, so accumulated +1 bonus
// points (bonusAttribute below) and instant-effect history SURVIVE the
// level-up. §2.9: a sanctioned level-up base write — the caller recomputes
// through the standard vitals-PRESERVING path (recomputeStats / the next
// tickCharacter), NEVER initializeActorStats (which restores vitals to
// full: right at spawn, a free heal mid-game).
void applyClassLevelChange(CoreAttributes& core, const FollowerClassForm& cls,
                           f32 fromLevel, f32 toLevel);

// Age (docs/FOLLOWERS.md §2): two multipliers < 1 — one physical, one
// mental — from the ActorForm.age years against the StatsTuningForm curve:
//   mult = max(ageFloor, 1 - max(0, age - ageOnsetYears) × agePerYear)
// age <= 0 = ageless (both stay 1). Applied per tick as StatModifiers (the
// equipmentMods fold — §2.9: mods recomputed from data each tick, nothing
// persisted, no synthetic effects), so CURRENT attributes shrink while the
// BASE-derived primary maxima stay untouched — exactly the doc's
// "effective skill = base skill x multiplier".
struct AgeMultipliers {
    f32 physical { 1.0f }; // strength, constitution, grace, dexterity
    f32 mental { 1.0f };   // alacrity, perception, charisma, ego, insight
};
AgeMultipliers ageMultipliers(f32 age, const StatsTuningForm& tuning);

// Folds the two multipliers into `mods.mul` (multiplying into existing
// entries — the armorModifiers fold contract). No-op for the ageless.
void foldAgeModifiers(f32 age, const StatsTuningForm& tuning,
                      StatModifiers& mods);

// Level linkage (docs/FOLLOWERS.md §2). One pure decision for both call
// sites: the per-frame sweep of ACTIVE followers (active = true — the
// level tracks the player's 1:1, and each level gained grants a +1
// attribute point) and the RE-RECRUIT catch-up (active = false — half the
// gap accrued apart, floored; a mainCharacter catches up fully; no points
// for catch-up levels). lastSyncedFrom < 1 means "never met": stamp the
// player's level without any retroactive gain (fresh spawns default 0).
// A negative gap (console-lowered player) stamps and gains nothing.
struct LevelSync {
    f32 level { 1.0f };      // the follower's new level
    f32 syncedFrom { 0.0f }; // store into followerLastLevelSyncedFrom
    i32 pointsGained { 0 };  // +1 attribute points earned (active only)
};
LevelSync syncFollowerLevel(f32 followerLevel, f32 lastSyncedFrom,
                            f32 playerLevel, bool active, bool mainCharacter);

// The +1 attribute point (docs/FOLLOWERS.md §3, v1 on ATTRIBUTES — skills
// don't exist yet): walk the player's 9 attributes in DESCENDING value
// order (ties keep the canonical field order); the FIRST one strictly
// greater than the follower's same attribute wins. None greater = nullopt
// (the doc's implicit no-point case). Returns the canonical index.
std::optional<u32> bonusAttribute(const CoreAttributes& player,
                                    const CoreAttributes& follower);

// ---- Special powers, class perks, taught perks --------------------------
// Reused systems (§2.11): perks and powers ARE the GAS (§6) — AbilityForm
// granted through grantAbility (the NPC tryActivate precedent) and
// EffectForm applied through applyEffect (§2.9, nothing else ever moves
// an attribute); the perk tables are the ClassPerkForm children and
// the TaughtPerkForm children (childrenOf pattern); persistence is the
// SavedAbilityForm child records (pattern B, the SavedItemForm mirror)
// plus the effects the save already carried. All pure/headless, doctested.

struct AttributeSet;
struct AbilitySystem;
struct ClassPerkForm;

// Grants one perk's payload — ability and/or effect, either may be null.
//   ability : grantAbility (idempotent dedup).
//   effect  : REQUIRES a grantedTag (the FollowerForms.hpp discipline).
//             Tag already on the target -> AlreadyKnown (a re-sync or a
//             reload never stacks the infinite modifier); no grantedTag
//             -> Skipped with one warning (data bug, visible).
// Granted = at least one payload newly landed.
enum class PerkGrant : u8 { Granted, AlreadyKnown, Skipped };
PerkGrant grantPerk(const data::FormDatabase& forms,
                    const core::Guid& ability, const core::Guid& effect,
                    AttributeSet& set, AbilitySystem& system,
                    const GameplayTagRegistry& tags);

// Applies every ClassPerkForm child of `classGuid` with perk.level <=
// level through grantPerk. Idempotent by construction (grantAbility dedup
// + the grantedTag discipline), so it runs at EVERY spawn exit (fresh,
// pending, saved — old saves without ability rows still get their powers)
// and again on each level-up. Returns how many perks newly granted.
i32 syncClassPerks(const data::FormDatabase& forms,
                   const core::Guid& classGuid, f32 level,
                   AttributeSet& set, AbilitySystem& system,
                   const GameplayTagRegistry& tags);

// The follower's SPECIAL POWER among his granted abilities: the first one
// that is not the shared attack ability (v1 — one power per follower; the
// class perk grants it at level 1). Invalid guid = none.
core::Guid pickPower(const vector<core::Guid>& granted,
                     const core::Guid& attackAbility);

// The healer's target pick (pure, order-independent): the ally with the
// LOWEST health fraction strictly below `threshold`; ties break on the
// smaller id so the unordered sweep stays deterministic (§8). 0 = nobody
// needs healing. The caller filters the dead/downed (revive is its own
// mechanic) and includes the player and the healer herself.
struct AllyVitals {
    u64 id { 0 };
    f32 healthFraction { 1.0f };
};
u64 pickHealTarget(const vector<AllyVitals>& allies, f32 threshold);

// ---- Follower carry weight ----------------------------------------------
// docs/FOLLOWERS.md §5: "carry weight limited by his attributes and
// his age modifier". Reused systems (§2.11): the age curve (the
// PHYSICAL multiplier — carrying is a body matter) and the
// encumbrance helpers (inventoryWeight / the maxEncumbrance derived stat);
// this only adds the pure accept/reject decision at the transfer site.

// The age factor on carry capacity = ageMultipliers(...).physical.
f32 followerCarryFactor(f32 age, const StatsTuningForm& tuning);

// Can `itemWeight` more land on a follower already carrying
// `currentWeight`, given his maxEncumbrance and age factor? The player has
// no hard cap (encumbrance only slows him); a follower REFUSES the excess
// item instead. maxEncumbrance <= 0 = stats not computed yet: accept (the
// encumbranceCategory grace).
bool canCarry(f32 currentWeight, f32 itemWeight, f32 maxEncumbrance,
              f32 ageFactor);

} // namespace gameplay
