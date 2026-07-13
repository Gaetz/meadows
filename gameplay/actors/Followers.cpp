#include "gameplay/actors/Followers.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "data/forms/FormDatabase.hpp"       // perk lookups (É6)
#include "data/forms/FormQuery.hpp"          // childrenOf (É6)
#include "engine/core/Log.hpp"               // the grantedTag discipline warn (É6)
#include "engine/core/Rng.hpp"
#include "gameplay/ability/DerivedStats.hpp" // StatModifiers, attr (É5)
#include "gameplay/ability/GameplayAbility.hpp" // grantAbility (É6)
#include "gameplay/ability/GameplayEffects.hpp" // applyEffect (É6, §2.9)
#include "gameplay/actors/ActorState.hpp"    // FollowerState
#include "gameplay/actors/FollowerForms.hpp" // AffinityRuleForm (É4),
                                             //   classAttributesAt (É5)
#include "gameplay/event/EventBus.hpp"       // eventKind (É4 rule matching)
#include "gameplay/combat/Combat.hpp"        // updateLifeState (the ONE write point)
#include "gameplay/stats/CoreAttributes.hpp" // the 9 bases (É5 curves)
#include "gameplay/stats/Damage.hpp"         // StatBlock, CombatState
#include "gameplay/stats/Injuries.hpp"       // addInjury, syncInjuryEffects
#include "gameplay/stats/StatsTuning.hpp"

namespace gameplay {

FollowTuning followTuning(const StatsTuningForm& tuning) {
    return FollowTuning { tuning.followNearRadius,
                          tuning.followCatchupRadius,
                          tuning.followCatchupSpeed,
                          tuning.followTeleportRadius };
}

FollowIntent decideFollow(const Vec3& followerPos, const Vec3& playerPos,
                          const FollowTuning& tuning) {
    FollowIntent intent;
    intent.target = playerPos;
    Vec3 to = playerPos - followerPos;
    to.y = 0.0f; // horizontal: both actors ride the terrain
    const f32 distance = glm::length(to);
    if (distance > tuning.teleportRadius) {
        intent.teleport = true;
        return intent;
    }
    if (distance <= tuning.nearRadius) {
        return intent; // near enough: idle (the caller faces the player)
    }
    intent.move = true;
    intent.speedScale =
        distance > tuning.catchupRadius ? tuning.catchupSpeed : 1.0f;
    return intent;
}

u64 adoptOnHit(u64 source, u64 target, const AggroRoles& roles) {
    // Hostile retaliation: struck by a follower -> fight THAT follower.
    // Struck by the player -> no adoption (default player targeting,
    // exactly the pre-É2 behavior). Never suppressed by FriendlyTrial.
    if (roles.selfHostile && roles.self == target && roles.sourceFollower &&
        source != roles.self) {
        return source;
    }
    if (!roles.selfFollower || roles.friendlyTrial) {
        return 0;
    }
    // The victim re-aims at its attacker, even with a live target: being
    // hit is the strongest signal there is.
    if (roles.self == target && roles.sourceHostile && source != roles.self) {
        return source;
    }
    if (roles.selfHasLiveTarget) {
        return 0; // committed — no target hopping on every party hit
    }
    // Defend the party: a hostile struck the player or a fellow follower.
    if (roles.sourceHostile && !roles.sourcePlayer &&
        (roles.targetPlayer || roles.targetFollower) && source != roles.self) {
        return source;
    }
    // Follow the player's initiative: he struck a hostile first. É9: the
    // « me défendre » stance turns exactly this rule off — a defender
    // only ever engages attackers of the party (the rules above).
    if (roles.sourcePlayer && roles.targetHostile && target != roles.self &&
        !roles.defendOnly) {
        return target;
    }
    return 0;
}

bool disengageOnDeath(u64 dead, u64 combatTarget) {
    return combatTarget != 0 && combatTarget == dead;
}

// ---- É3: downed, bleedout, aggravation, convalescence ---------------------

Aggravation rollAggravation(bool alreadyInjured, core::Rng& rng,
                            const StatsTuningForm& tuning) {
    if (!alreadyInjured) {
        return Aggravation::None; // no draw: the stream stays untouched
    }
    if (rng.chance(static_cast<f64>(tuning.aggravationDeathChance))) {
        return Aggravation::Death;
    }
    if (rng.chance(static_cast<f64>(tuning.aggravationWorseChance))) {
        return Aggravation::WorseInjury;
    }
    return Aggravation::None;
}

BleedoutResult resolveBleedout(StatBlock& target, Injuries& injuries,
                               core::Rng& rng,
                               const GameplayTagRegistry& tags,
                               const StatsTuningForm& tuning) {
    BleedoutResult result;
    const bool alreadyInjured = !injuries.list.empty();
    bool dies = rng.chance(static_cast<f64>(tuning.downedDeathChance));
    if (!dies && alreadyInjured) {
        // A fresh wound on a wounded body: the aggravation table.
        switch (rollAggravation(true, rng, tuning)) {
        case Aggravation::Death:       dies = true; break;
        case Aggravation::WorseInjury: result.aggravated = true; break;
        case Aggravation::None:        break;
        }
    }
    if (dies) {
        // Real death: lift the protection and let the SINGLE life-state
        // write point do the killing (health is already 0; updateLifeState
        // grants State.Dead and clears State.Downed — the normal OnDeath
        // flow fires on the game side's dead edge).
        if (const auto shield = tags.find("Follower.Protected")) {
            if (target.system.tags.has(*shield)) {
                target.system.tags.remove(*shield, tags);
            }
        }
        updateLifeState(target.system, tags);
        result.outcome = BleedoutOutcome::Died;
        return result;
    }
    // Survived: the down always costs a wound (a torso cut — the injuries
    // system's own record); an aggravation stacks its severity once more.
    addInjury(injuries, InjuryType::Cut, BodyPart::Torso);
    if (result.aggravated) {
        addInjury(injuries, InjuryType::Cut, BodyPart::Torso);
    }
    syncInjuryEffects(injuries, target.system, target.vitals, tags);
    // Back on his feet at a sliver of health — §2.9 execution calculation:
    // the pipeline's own terminal write (reflected BaseValue + recompute,
    // the applyBuildupResult idiom), then the life-state derive drops
    // State.Downed (health > 0).
    setBaseValue(target.vitals, attr("health"), 1.0f);
    recomputeCurrent(target.vitals, target.system);
    updateLifeState(target.system, tags);
    target.combat.downedSeconds = 0.0f;
    result.outcome = BleedoutOutcome::Recovered;
    return result;
}

bool needsConvalescence(const Injuries& injuries) {
    return std::any_of(
        injuries.list.begin(), injuries.list.end(),
        [](const Injury& injury) { return injury.severity >= 1; });
}

f32 convalescenceHours(const Injuries& injuries) {
    f32 hours = 0.0f;
    for (const Injury& injury : injuries.list) {
        hours = std::max(hours, injury.recoveryHoursRemaining);
    }
    return hours;
}

bool followerConvalescent(const FollowerState& state, f64 nowHours) {
    return state.followerDownedRecoveryHours > 0.0f &&
           nowHours < static_cast<f64>(state.followerDownedRecoveryHours);
}

// ---- É4: affinity ----------------------------------------------------------

f32 addAffinity(FollowerState& state, f32 delta) {
    const f32 before = state.followerAffinity;
    state.followerAffinity =
        std::clamp(before + delta, kAffinityMin, kAffinityMax);
    return state.followerAffinity - before;
}

f32 accrueTimeTogether(FollowerState& state, f32 deltaHours,
                       f32 affinityPerHour) {
    if (deltaHours <= 0.0f) {
        return 0.0f; // clock hiccup / first stamp: inert
    }
    state.followerHoursTogether += deltaHours;
    return addAffinity(state, deltaHours * affinityPerHour);
}

f32 affinityDelta(const vector<const AffinityRuleForm*>& rules,
                  const AffinityEventView& event,
                  const GameplayTagRegistry& tags) {
    f32 sum = 0.0f;
    for (const AffinityRuleForm* rule : rules) {
        if (!rule || eventKind(rule->event) != event.kind) {
            continue;
        }
        if (!rule->filterTag.empty()) {
            // The QuestTaskForm matching: the event's tag must DESCEND
            // from the filter (an unknown filter or a tagless event fails).
            const auto filter = tags.find(rule->filterTag);
            if (!filter || !event.tag.isValid() ||
                !tags.isA(event.tag, *filter)) {
                continue;
            }
        }
        if (rule->sourcePlayer && !event.sourceIsPlayer) {
            continue;
        }
        if (rule->targetSelf && !event.targetIsSelf) {
            continue;
        }
        sum += rule->delta;
    }
    return sum;
}

// ---- É5: classes, levels, evolution -----------------------------------------

namespace {

// Canonical field order — MUST match CoreAttributes / classAttributesAt.
constexpr std::array<f32 CoreAttributes::*, kCoreAttributeCount> kCoreFields {
    &CoreAttributes::strength,   &CoreAttributes::constitution,
    &CoreAttributes::grace,      &CoreAttributes::dexterity,
    &CoreAttributes::alacrity,   &CoreAttributes::perception,
    &CoreAttributes::charisma,   &CoreAttributes::ego,
    &CoreAttributes::insight,
};

// The doc's physical/mental split (docs/FOLLOWERS.md §2: « un physique,
// un mental ») over the canonical indices above.
constexpr std::array<u32, 4> kPhysicalAttrs { 0, 1, 2, 3 }; // str/con/gra/dex
constexpr std::array<u32, 5> kMentalAttrs { 4, 5, 6, 7, 8 }; // ala/per/cha/ego/ins

// The armorModifiers fold contract: multiply into an existing entry.
void foldMul(StatModifiers& mods, u32 attrId, f32 factor) {
    const auto [it, inserted] = mods.mul.try_emplace(attrId, factor);
    if (!inserted) {
        it->second *= factor;
    }
}

} // namespace

const std::array<const char*, kCoreAttributeCount> kCoreAttributeNames {
    "strength", "constitution", "grace",    "dexterity", "alacrity",
    "perception", "charisma",   "ego",      "insight",
};

f32 coreAttributeValue(const CoreAttributes& core, u32 index) {
    return core.*kCoreFields[index];
}

f32& coreAttributeRef(CoreAttributes& core, u32 index) {
    return core.*kCoreFields[index];
}

void applyFollowerClass(CoreAttributes& core, const FollowerClassForm& cls,
                        f32 level) {
    // §2.9: the sanctioned spawn-time init write — bases only, before
    // initializeActorStats derives the maxima from them.
    core = classAttributesAt(cls, level);
}

void applyClassLevelChange(CoreAttributes& core, const FollowerClassForm& cls,
                           f32 fromLevel, f32 toLevel) {
    // §2.9: the sanctioned level-up base write — the curve DELTA, so
    // accumulated bonus points / instant-effect history survive. The
    // caller recomputes through the vitals-preserving path.
    const CoreAttributes from = classAttributesAt(cls, fromLevel);
    const CoreAttributes to = classAttributesAt(cls, toLevel);
    for (u32 i = 0; i < kCoreAttributeCount; ++i) {
        core.*kCoreFields[i] += to.*kCoreFields[i] - from.*kCoreFields[i];
    }
}

AgeMultipliers ageMultipliers(f32 age, const StatsTuningForm& tuning) {
    AgeMultipliers result;
    if (age <= 0.0f) {
        return result; // ageless (ActorForm.age default)
    }
    const f32 past = std::max(0.0f, age - tuning.ageOnsetYears);
    result.physical =
        std::max(tuning.ageFloor, 1.0f - past * tuning.agePhysicalPerYear);
    result.mental =
        std::max(tuning.ageFloor, 1.0f - past * tuning.ageMentalPerYear);
    return result;
}

void foldAgeModifiers(f32 age, const StatsTuningForm& tuning,
                      StatModifiers& mods) {
    const AgeMultipliers m = ageMultipliers(age, tuning);
    if (m.physical < 1.0f) {
        for (const u32 i : kPhysicalAttrs) {
            foldMul(mods, attr(kCoreAttributeNames[i]), m.physical);
        }
    }
    if (m.mental < 1.0f) {
        for (const u32 i : kMentalAttrs) {
            foldMul(mods, attr(kCoreAttributeNames[i]), m.mental);
        }
    }
}

LevelSync syncFollowerLevel(f32 followerLevel, f32 lastSyncedFrom,
                            f32 playerLevel, bool active, bool mainCharacter) {
    LevelSync result { followerLevel, playerLevel, 0 };
    if (lastSyncedFrom < 1.0f) {
        return result; // never met: stamp only, no retroactive gain
    }
    const f32 gap = playerLevel - lastSyncedFrom;
    if (gap <= 0.0f) {
        return result; // console-lowered player: stamp, gain nothing
    }
    if (active) {
        // Traveling together: 1:1 tracking, each level earns a +1 point.
        result.level = followerLevel + gap;
        result.pointsGained = static_cast<i32>(gap + 0.5f);
    } else if (mainCharacter) {
        // The story exception (docs/FOLLOWERS.md §2): full catch-up.
        result.level = followerLevel + gap;
    } else {
        // The re-meet: half the gap accrued apart, floored.
        result.level = followerLevel + std::floor(gap * 0.5f);
    }
    return result;
}

std::optional<u32> bonusAttribute(const CoreAttributes& player,
                                    const CoreAttributes& follower) {
    std::array<u32, kCoreAttributeCount> order {};
    for (u32 i = 0; i < kCoreAttributeCount; ++i) {
        order[i] = i;
    }
    // Descending player value; stable = ties keep the canonical order.
    std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) {
        return coreAttributeValue(player, a) > coreAttributeValue(player, b);
    });
    for (const u32 i : order) {
        if (coreAttributeValue(player, i) > coreAttributeValue(follower, i)) {
            return i;
        }
    }
    return std::nullopt; // the follower matches him everywhere: no point
}

// ---- É6: special powers, class perks, taught perks --------------------------

PerkGrant grantPerk(const data::FormDatabase& forms,
                    const core::Guid& ability, const core::Guid& effect,
                    AttributeSet& set, AbilitySystem& system,
                    const GameplayTagRegistry& tags) {
    bool granted = false;
    bool known = false;
    if (ability.isValid()) {
        const bool already =
            std::find(system.grantedAbilities.begin(),
                      system.grantedAbilities.end(),
                      ability) != system.grantedAbilities.end();
        if (already) {
            known = true;
        } else {
            grantAbility(system, ability);
            granted = true;
        }
    }
    if (effect.isValid()) {
        if (const EffectForm* form = forms.find<EffectForm>(effect)) {
            // The É6 discipline (FollowerForms.hpp): the effect's OWN
            // grantedTag is the dedup key — and the save-proof one
            // (SavedEffectForm persists it; restore re-adds the tag).
            std::optional<GameplayTag> tag;
            if (!form->grantedTag.empty()) {
                tag = tags.find(form->grantedTag);
            }
            if (!tag) {
                LOG_WARN("É6: perk effect '{}' has no (registered) "
                         "grantedTag — skipped (it would stack on every "
                         "sync; see FollowerForms.hpp)",
                         form->editorId);
            } else if (system.tags.has(*tag)) {
                known = true;
            } else if (applyEffect(set, system, *form, tags)) {
                granted = true;
            }
        } else {
            LOG_WARN("É6: perk effect {} not found", effect.toString());
        }
    }
    if (granted) {
        return PerkGrant::Granted;
    }
    return known ? PerkGrant::AlreadyKnown : PerkGrant::Skipped;
}

i32 syncClassPerks(const data::FormDatabase& forms,
                   const core::Guid& classGuid, f32 level,
                   AttributeSet& set, AbilitySystem& system,
                   const GameplayTagRegistry& tags) {
    if (!classGuid.isValid()) {
        return 0;
    }
    i32 newlyGranted = 0;
    // childrenOf iterates in plugin/creation order — deterministic (§8).
    data::childrenOf<ClassPerkForm>(
        forms, classGuid, [&](const ClassPerkForm& perk) {
            if (perk.level > level) {
                return; // not unlocked yet — the next level-up re-syncs
            }
            if (grantPerk(forms, perk.ability, perk.effect, set, system,
                          tags) == PerkGrant::Granted) {
                ++newlyGranted;
            }
        });
    return newlyGranted;
}

core::Guid pickPower(const vector<core::Guid>& granted,
                     const core::Guid& attackAbility) {
    for (const core::Guid& ability : granted) {
        if (ability.isValid() && ability != attackAbility) {
            return ability;
        }
    }
    return core::Guid {};
}

u64 pickHealTarget(const vector<AllyVitals>& allies, f32 threshold) {
    u64 best = 0;
    f32 bestFraction = threshold;
    for (const AllyVitals& ally : allies) {
        if (ally.id == 0 || ally.healthFraction >= threshold) {
            continue;
        }
        // Strictly lower fraction wins; a tie keeps the smaller id, so
        // the verdict is independent of the sweep order (§8).
        if (best == 0 || ally.healthFraction < bestFraction ||
            (ally.healthFraction == bestFraction && ally.id < best)) {
            best = ally.id;
            bestFraction = ally.healthFraction;
        }
    }
    return best;
}

// ---- É7: follower carry weight ----------------------------------------------

f32 followerCarryFactor(f32 age, const StatsTuningForm& tuning) {
    return ageMultipliers(age, tuning).physical;
}

bool canCarry(f32 currentWeight, f32 itemWeight, f32 maxEncumbrance,
              f32 ageFactor) {
    if (maxEncumbrance <= 0.0f) {
        return true; // stats not computed yet — the encumbranceCategory grace
    }
    return currentWeight + itemWeight <= maxEncumbrance * ageFactor;
}

// ---- É9: party caps, stances, banter, ambient comments ----------------------

void countPartyMember(PartyCounts& counts, const str& category) {
    if (category == "mount") {
        return; // exempt from both caps (docs/FOLLOWERS.md §8)
    }
    if (category == "minor") {
        ++counts.minors;
    } else {
        ++counts.majors; // "major" and anything unrecognized: strict bucket
    }
}

RecruitVerdict canJoinParty(const str& category, const PartyCounts& counts,
                            i32 majorCap, i32 minorCap) {
    if (category == "mount") {
        return RecruitVerdict::Ok; // never counts
    }
    if (category == "minor") {
        return counts.minors < minorCap ? RecruitVerdict::Ok
                                        : RecruitVerdict::MinorsFull;
    }
    return counts.majors < majorCap ? RecruitVerdict::Ok
                                    : RecruitVerdict::MajorsFull;
}

FollowerStance followerStance(const FollowerState& state) {
    // The ONE f32 -> enum decode point. Out-of-range (a modded save, a
    // NaN) decays to Follow — never a wedged follower.
    const f32 v = state.followerStance;
    if (v == 1.0f) {
        return FollowerStance::Stay;
    }
    if (v == 2.0f) {
        return FollowerStance::Attack;
    }
    if (v == 3.0f) {
        return FollowerStance::Defend;
    }
    return FollowerStance::Follow;
}

void setFollowerStance(FollowerState& state, FollowerStance stance) {
    state.followerStance = static_cast<f32>(static_cast<u8>(stance));
}

bool commentMatches(const CommentForm& comment, u32 kind,
                    const GameplayTag& eventTag, f32 eventValue,
                    bool sourceIsPlayer, const GameplayTagRegistry& tags) {
    if (eventKind(comment.event) != kind) {
        return false;
    }
    if (!comment.filterTag.empty()) {
        // The QuestTaskForm matching: the event's tag must DESCEND from
        // the filter (unknown filter or tagless event fails).
        const auto filter = tags.find(comment.filterTag);
        if (!filter || !eventTag.isValid() || !tags.isA(eventTag, *filter)) {
            return false;
        }
    }
    if (comment.sourcePlayer && !sourceIsPlayer) {
        return false; // a place comment tracks the PLAYER's crossing
    }
    return eventValue >= comment.minValue;
}

bool decideComment(const CommentForm& comment, f64 nowHours, bool sneaking,
                   const CommentClock& own, const CommentClock& prerequisite) {
    if (sneaking) {
        return false; // never in sneak (docs/FOLLOWERS.md §6.1)
    }
    if (own.fired) {
        if (comment.oneShot) {
            return false;
        }
        if (nowHours - own.lastHours <
            static_cast<f64>(comment.cooldownHours)) {
            return false; // the 10-game-hour anti-repeat (data default)
        }
    }
    if (comment.requiresComment.isValid()) {
        // Ordered chaining: the prerequisite must HAVE fired, at least
        // minGapHours ago.
        if (!prerequisite.fired ||
            nowHours - prerequisite.lastHours <
                static_cast<f64>(comment.minGapHours)) {
            return false;
        }
    }
    return true;
}

f32 pairAffinity(const vector<const FollowerBondForm*>& bonds,
                 const core::Guid& a, const core::Guid& b) {
    for (const FollowerBondForm* bond : bonds) {
        if (!bond) {
            continue;
        }
        if ((bond->a == a && bond->b == b) ||
            (bond->a == b && bond->b == a)) {
            return bond->affinity;
        }
    }
    return 0.0f; // strangers (no authored bond)
}

bool banterPairMatches(const BanterForm& banter, const core::Guid& x,
                       const core::Guid& y) {
    return (banter.a == x && banter.b == y) ||
           (banter.a == y && banter.b == x);
}

bool decideBanter(const BanterForm& banter, f32 pairAffinityValue,
                  f64 nowHours, const CommentClock& clock) {
    if (pairAffinityValue < banter.minPairAffinity) {
        return false; // the pair isn't close enough for this exchange
    }
    if (clock.fired) {
        if (banter.oneShot) {
            return false;
        }
        if (nowHours - clock.lastHours <
            static_cast<f64>(banter.cooldownHours)) {
            return false;
        }
    }
    return true;
}

// ---- É10: mercenaries --------------------------------------------------------

i32 mercenaryPrice(f32 basePrice, f32 playerLevel, i32 playerGold,
                   const StatsTuningForm& tuning) {
    // The documented v1 formula (Followers.hpp): a level axis (linear per
    // level above 1) times a wealth axis (linear in gold, SATURATING at
    // the pivot). Both clamp so bad inputs never discount below base.
    const f32 level = glm::max(playerLevel, 1.0f);
    const f32 levelTerm =
        1.0f + (level - 1.0f) * tuning.mercenaryLevelPricePerLevel;
    f32 wealthTerm = 1.0f;
    if (tuning.mercenaryWealthPivot > 0.0f) {
        const f32 gold = static_cast<f32>(glm::max(playerGold, 0));
        wealthTerm = 1.0f + glm::min(gold / tuning.mercenaryWealthPivot,
                                     1.0f) *
                                tuning.mercenaryWealthFactor;
    }
    // Nearest gold piece (half rounds up — lround, not a float trunc).
    return static_cast<i32>(
        std::lround(static_cast<f64>(basePrice) * levelTerm * wealthTerm));
}

ContractPhase contractPhase(f64 nowHours, f32 expiryHours,
                            f32 warningHours) {
    if (expiryHours <= 0.0f) {
        return ContractPhase::None; // no stamp = not under contract
    }
    if (nowHours >= static_cast<f64>(expiryHours)) {
        return ContractPhase::Expired;
    }
    if (nowHours >= static_cast<f64>(expiryHours) -
                        static_cast<f64>(glm::max(warningHours, 0.0f))) {
        return ContractPhase::Warning;
    }
    return ContractPhase::Engaged;
}

f32 extendContract(f64 nowHours, f32 expiryHours, f32 contractDays) {
    // Renewals stack on the RUNNING contract (max — early gold buys no
    // dead hours); a fresh hire (expiry 0) or a lapsed one starts at now.
    const f64 from = glm::max(nowHours, static_cast<f64>(expiryHours));
    return static_cast<f32>(from +
                            static_cast<f64>(contractDays) * 24.0);
}

} // namespace gameplay
