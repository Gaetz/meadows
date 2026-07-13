#pragma once

#include <array>
#include <optional>

#include <glm/glm.hpp> // Vec3 by value (Defines only forward-declares glm)

#include "engine/core/Defines.hpp"
#include "engine/core/Guid.hpp"              // ability guids (É6 perks)
#include "gameplay/ability/GameplayTags.hpp" // GameplayTag (É4 rule matching)

// The follow decision (FOLLOWERS É1 — docs/CHANTIER-FOLLOWERS.md). Pure and
// headless (§2.10): position in, intent out — the game-side AI package
// (NpcScheduleController::followPlayer) executes the intent through the
// existing goTo/moveNpcAlongPath idiom. All feel knobs live in
// StatsTuningForm (§5 moddable), pulled through followTuning below.

namespace core {
class Rng;
}
namespace data {
class FormDatabase; // perk children lookups (É6)
}

namespace gameplay {

struct StatsTuningForm;

// The follow feel knobs, mirrored from StatsTuningForm (follow* fields).
struct FollowTuning {
    f32 nearRadius { 3.5f };     // closer than this: stand, face the player
    f32 catchupRadius { 8.0f };  // beyond this: hurry (catchupSpeed)
    f32 catchupSpeed { 1.25f };  // speedScale while catching up
    f32 teleportRadius { 40.0f };// beyond this: reposition next to the player
};

FollowTuning followTuning(const StatsTuningForm& tuning);

// What the follower should do this frame.
struct FollowIntent {
    bool move { false };      // walk toward `target`
    Vec3 target { 0.0f };     // the player's position (the path goal)
    f32 speedScale { 1.0f };  // moveNpcAlongPath scale (catchupSpeed when far)
    bool teleport { false };  // lost him: reposition near the player
};

// Distance is HORIZONTAL (both actors are terrain-grounded; the Y gap is
// presentation, not separation). Bands, inclusive at the low edge:
//   d <= near                : idle (move = false)
//   near < d <= catchup      : walk (speedScale 1)
//   catchup < d <= teleport  : walk fast (speedScale = catchupSpeed)
//   d > teleport             : teleport
FollowIntent decideFollow(const Vec3& followerPos, const Vec3& playerPos,
                          const FollowTuning& tuning);

// ---- É2: the aggro table --------------------------------------------------
// Pure per-event decision for OnHitTaken{source, target} (§2.11: the bus
// already carries the signal — resolveMeleeStrike dispatches it; this is
// only the reaction table). The caller resolves the ROLES (follower =
// FollowerState.followerActive, hostile = Npc.hostile, friendlyTrial =
// the Combat.FriendlyTrial tag on the PLAYER) and writes the returned
// entity into Npc.combatTarget. The rules, flat:
//   - a hostile struck BY A FOLLOWER fights that follower back (struck
//     by the player it keeps its DEFAULT player targeting — iso É2);
//   - a follower struck by a hostile re-aims at its attacker, even
//     mid-fight;
//   - a follower with no live target defends the party: it adopts a
//     hostile that hit the player or a fellow follower, and adopts the
//     hostile the PLAYER strikes first;
//   - Combat.FriendlyTrial suppresses every follower adoption (the
//     doc's brawl case); hostile retaliation is never suppressed;
//   - nobody ever targets itself, and the player is never adopted.
struct AggroRoles {
    u64 self { 0 };
    bool selfFollower { false };
    bool selfHostile { false };
    bool selfHasLiveTarget { false };
    bool sourcePlayer { false };
    bool sourceFollower { false };
    bool sourceHostile { false };
    bool targetPlayer { false };
    bool targetFollower { false };
    bool targetHostile { false };
    bool friendlyTrial { false };
    // É9 (appended): the « me défendre » stance — this follower never
    // adopts on the player's INITIATIVE (rule 4 off); being hit, or a
    // hostile striking the party, still engages him. Follow(0) and
    // defend(3) differ in exactly this flag.
    bool defendOnly { false };
};

// The entity `self` should adopt as its combat target (the event's
// source or target id), or 0 = keep the current one.
u64 adoptOnHit(u64 source, u64 target, const AggroRoles& roles);

// OnDeath{target}: should a combat target pointing at `dead` be cleared?
// (A cleared follower falls back to the follow package next frame.)
bool disengageOnDeath(u64 dead, u64 combatTarget);

// ---- É3: downed, bleedout, aggravation, convalescence ---------------------
// The rules live HERE (sim, headless, doctested); the game side
// (FollowerController::updateDowned) only sweeps the live NPCs and calls
// in. Reused systems (§2.11): the CombatState timer pattern (stagger),
// the Injuries container + syncInjuryEffects, the seeded core::Rng (§8),
// StatsTuningForm knobs (§5), and updateLifeState as the ONE life-state
// write point.

struct Injury;
struct Injuries;
struct StatBlock;
struct FollowerState;
class GameplayTagRegistry;

// A wound on an already-wounded body can turn ugly (docs/FOLLOWERS.md §2:
// death is aggravation-based, never the first fall). Rolled on the seeded
// engine RNG; `alreadyInjured == false` never aggravates AND draws
// nothing (the random stream stays untouched — determinism stays legible).
// Order: the death roll first, then the worse-injury roll.
enum class Aggravation : u8 { None, WorseInjury, Death };
Aggravation rollAggravation(bool alreadyInjured, core::Rng& rng,
                            const StatsTuningForm& tuning);

// The end of an un-revived bleedout window (updateDowned returned true).
// V1 rule (stated in docs/FOLLOWERS-TEST É3): roll downedDeathChance for
// a REAL death — the protection tag is lifted and updateLifeState (the
// single write point) grants State.Dead, so the normal OnDeath flow runs.
// Otherwise he gets back up at 1 HP (a §2.9 execution calculation: the
// BaseValue write + recompute, the applyBuildupResult idiom) with a fresh
// torso cut; if he was ALREADY wounded, rollAggravation may worsen the
// wound (severity +1) — or kill after all.
enum class BleedoutOutcome : u8 { Died, Recovered };
struct BleedoutResult {
    BleedoutOutcome outcome { BleedoutOutcome::Recovered };
    bool aggravated { false }; // the fresh wound landed on a wounded body
};
BleedoutResult resolveBleedout(StatBlock& target, Injuries& injuries,
                               core::Rng& rng,
                               const GameplayTagRegistry& tags,
                               const StatsTuningForm& tuning);

// Convalescence rules: a follower whose injuries crossed the severity bar
// (any injury at severity >= 1 — i.e. wounded AGAIN before healing)
// demands rest at home. `convalescenceHours` = the longest remaining
// recovery among his injuries (the Injury.recoveryHoursRemaining clock —
// the injuries system's own timer, not a new one).
bool needsConvalescence(const Injuries& injuries);
f32 convalescenceHours(const Injuries& injuries);

// True while `state.followerDownedRecoveryHours` (an absolute GameClock
// game-hour stamp — the VendorState.lastRestockHours idiom) lies in the
// future: recruiting is refused, mirrored as the player's
// Follower.Convalescent tag for the dialogue conditions.
bool followerConvalescent(const FollowerState& state, f64 nowHours);

// ---- É4: affinity ----------------------------------------------------------
// Affinity lives on FollowerState (a plain reflected field — §2.9: it is
// NOT a GAS attribute and never moves through applyEffect). Two data-driven
// movers, both pure and doctested: passive growth per game-hour spent
// together (the VendorState hour-stamp idiom, delta computed by the game
// side from GameClock), and AffinityRuleForm children of the ActorForm
// reacting to bus events (the QuestTaskForm event+filterTag matching).

struct AffinityRuleForm;

inline constexpr f32 kAffinityMin { -100.0f };
inline constexpr f32 kAffinityMax { 100.0f };

// Moves `state.followerAffinity` by `delta`, clamped to ±100. Returns the
// APPLIED change (0 at a saturated bound).
f32 addAffinity(FollowerState& state, f32 delta);

// Passive accrual: `deltaHours` game-hours of traveling together land on
// followerHoursTogether and grow affinity by deltaHours × affinityPerHour
// (clamped). Non-positive deltas are no-ops (clock hiccups stay inert).
// Returns the applied affinity change.
f32 accrueTimeTogether(FollowerState& state, f32 deltaHours,
                       f32 affinityPerHour);

// The event side of an AffinityRuleForm match, resolved by the caller (the
// pure matcher never touches entities): the event's kind/tag plus who the
// parties are RELATIVE to the follower being evaluated.
struct AffinityEventView {
    u32 kind { 0 };       // gameplay::eventKind of the event name
    GameplayTag tag {};   // the event's categorizing tag (may be invalid)
    bool sourceIsPlayer { false };
    bool targetIsSelf { false };
};

// Sums the deltas of every rule matching the event: name equality, then
// filterTag (the event's tag must DESCEND from it — tags.isA, the
// QuestTaskForm matching) and the party filters. Pure; the caller applies
// the sum through addAffinity.
f32 affinityDelta(const vector<const AffinityRuleForm*>& rules,
                  const AffinityEventView& event,
                  const GameplayTagRegistry& tags);

// ---- É5: classes, levels, evolution -----------------------------------------
// Reused systems (§2.11): the É0 classAttributesAt curves, the equipmentMods
// StatModifiers fold (buildCharacterMods' external-mods channel), the
// recomputeStats machinery (efdf8e7 override ?? formula), and the seeded
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
// « compétence_effective = compétence_base × multiplicateur ».
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

// ---- É6: special powers, class perks, taught perks --------------------------
// Reused systems (§2.11): perks and powers ARE the GAS (§6) — AbilityForm
// granted through grantAbility (the NPC tryActivate precedent) and
// EffectForm applied through applyEffect (§2.9, nothing else ever moves
// an attribute); the perk tables are the É0 ClassPerkForm children and
// the É6 TaughtPerkForm children (childrenOf pattern); persistence is the
// É6 SavedAbilityForm child records (pattern B, the SavedItemForm mirror)
// plus the effects the save already carried. All pure/headless, doctested.

struct AttributeSet;
struct AbilitySystem;
struct ClassPerkForm;

// Grants one perk's payload — ability and/or effect, either may be null.
//   ability : grantAbility (idempotent — the É6 dedup).
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

// ---- É7: follower carry weight ----------------------------------------------
// docs/FOLLOWERS.md §5: « poids limité par ses caractéristiques et son
// modificateur d'âge ». Reused systems (§2.11): the É5 age curve (the
// PHYSICAL multiplier — carrying is a body matter) and the chantier-6
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

// ---- É9: party caps, stances, banter, ambient comments ----------------------
// Reused systems (§2.11): the caps read ActorForm.followerCategory (É0
// data) against StatsTuningForm knobs (§5); the stance is one more
// FollowerState field riding the SavedStatsForm name-match sweep; banter
// and comments are child/top-level data records (the AffinityRuleForm /
// QuestTaskForm matching) on GameClock hour stamps (the VendorState
// idiom). All pure and doctested; the game side only resolves entities.

struct FollowerBondForm;
struct BanterForm;
struct CommentForm;

// The party census by É0 category. "mount" is EXEMPT from both caps
// (docs/FOLLOWERS.md §8 — it never counts); "minor" fills the minor cap;
// anything else (the authored "major", or a modded typo) counts as major —
// the strict bucket, so bad data can never bypass the cap.
struct PartyCounts {
    i32 majors { 0 };
    i32 minors { 0 };
};
void countPartyMember(PartyCounts& counts, const str& category);

// The recruit gate (checked at recruit time, É9): does one more
// `category` fit under the caps? Caps come in as integers (the f32 tuning
// knobs truncated by the caller).
enum class RecruitVerdict : u8 { Ok, MajorsFull, MinorsFull };
RecruitVerdict canJoinParty(const str& category, const PartyCounts& counts,
                            i32 majorCap, i32 minorCap);

// The group-command stance (docs/FOLLOWERS.md §7 « stratégie modifiable
// par dialogue »). Persisted as FollowerState.followerStance (f32 — the
// save sweep); read/written ONLY through these two (the enum-over-bools
// rule: one vocabulary, one transition point). Out-of-range floats decay
// to Follow — a modded save never wedges a follower.
enum class FollowerStance : u8 { Follow = 0, Stay = 1, Attack = 2, Defend = 3 };
FollowerStance followerStance(const FollowerState& state);
void setFollowerStance(FollowerState& state, FollowerStance stance);

// É9 anti-repeat clock, one per BanterForm/CommentForm guid in a
// RUNTIME-ONLY map (v1, stated: resets on load — acceptable; the oneShot
// flag is not persisted either). Hours are GameClock game-time stamps.
struct CommentClock {
    bool fired { false };
    f64 lastHours { 0.0 };
};

// Does `comment` match this bus event? Name equality, optional filterTag
// descent (the QuestTaskForm matching — an unknown filter or a tagless
// event fails), the minValue floor (OnQuietZone enter=1 / leave=0), and
// the optional sourcePlayer party filter (the AffinityRuleForm idiom).
bool commentMatches(const CommentForm& comment, u32 eventKind,
                    const GameplayTag& eventTag, f32 eventValue,
                    bool sourceIsPlayer, const GameplayTagRegistry& tags);

// The anti-repeat + chaining gate (docs/FOLLOWERS.md §6.1): never in
// sneak; a oneShot never refires; a fired comment waits cooldownHours;
// with requiresComment set, the PREREQUISITE must have fired at least
// minGapHours ago (the ordered-chaining contract).
bool decideComment(const CommentForm& comment, f64 nowHours, bool sneaking,
                   const CommentClock& own, const CommentClock& prerequisite);

// The pair-affinity lookup over the authored bonds (symmetric — {a,b} in
// either order); no bond = 0. V1: bonds are initial values only (no
// runtime mutation — stated).
f32 pairAffinity(const vector<const FollowerBondForm*>& bonds,
                 const core::Guid& a, const core::Guid& b);

// Is this banter's PAIR the two given actors (either order)?
bool banterPairMatches(const BanterForm& banter, const core::Guid& x,
                       const core::Guid& y);

// The banter gate: pair affinity >= minPairAffinity, oneShot respected,
// cooldownHours elapsed. The caller checks presence/distance/combat/sneak
// and the global banterIntervalHours cadence.
bool decideBanter(const BanterForm& banter, f32 pairAffinityValue,
                  f64 nowHours, const CommentClock& clock);

} // namespace gameplay
