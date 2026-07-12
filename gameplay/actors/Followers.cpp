#include "gameplay/actors/Followers.hpp"

#include <algorithm>

#include <glm/glm.hpp>

#include "engine/core/Rng.hpp"
#include "gameplay/actors/ActorState.hpp"    // FollowerState
#include "gameplay/actors/FollowerForms.hpp" // AffinityRuleForm (É4)
#include "gameplay/event/EventBus.hpp"       // eventKind (É4 rule matching)
#include "gameplay/combat/Combat.hpp"        // updateLifeState (the ONE write point)
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
    // Follow the player's initiative: he struck a hostile first.
    if (roles.sourcePlayer && roles.targetHostile && target != roles.self) {
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

} // namespace gameplay
