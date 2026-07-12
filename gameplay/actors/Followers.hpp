#pragma once

#include <array>
#include <optional>

#include <glm/glm.hpp> // Vec3 by value (Defines only forward-declares glm)

#include "engine/core/Defines.hpp"
#include "gameplay/ability/GameplayTags.hpp" // GameplayTag (É4 rule matching)

// The follow decision (FOLLOWERS É1 — docs/CHANTIER-FOLLOWERS.md). Pure and
// headless (§2.10): position in, intent out — the game-side AI package
// (NpcScheduleController::followPlayer) executes the intent through the
// existing goTo/moveNpcAlongPath idiom. All feel knobs live in
// StatsTuningForm (§5 moddable), pulled through followTuning below.

namespace core {
class Rng;
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

} // namespace gameplay
