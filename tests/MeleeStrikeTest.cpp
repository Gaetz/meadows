#include <doctest/doctest.h>

#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/GameplayTags.hpp"
#include "gameplay/combat/MeleeStrike.hpp"
#include "gameplay/cue/GameplayCues.hpp"
#include "gameplay/event/EventBus.hpp"
#include "gameplay/stats/CharacterStats.hpp"
#include "gameplay/stats/CoreAttributes.hpp"
#include "gameplay/stats/Damage.hpp"
#include "gameplay/stats/StatsTuning.hpp"

// The ONE strike resolution shared by the
// player, the NPCs and the arrows: crit window, guard cone, perfect
// parry, empty-guard punish, event dispatch and cue selection. A single
// owner keeps the two camps' exchange rules from drifting; extracting it
// from the frontend controllers is what makes them testable here.

using namespace gameplay;

namespace {

// One combatant, the TypedDamageTest stat recipe: all nine attributes at
// 20 → defense 10, armor 10%, maxPosture 70; health seeded to 300.
struct Actor {
    CoreAttributes core;
    AttributeSet vitals;
    AbilitySystem system;
    CombatState combat;

    StatBlock block() { return StatBlock { core, vitals, system, combat }; }
};

struct Duel {
    Actor attacker;
    Actor defender;
    DerivedStatRegistry derived;
    GameplayTagRegistry tags;
    StatsTuningForm tuning;
    EventBus bus;
    CueRegistry cues;
    vector<Event> events;
    vector<str> cueTags;

    Duel() {
        tags.registerTag("State.Dead");
        tags.registerTag("State.Staggered");
        tags.registerTag("State.Blocking");
        tags.registerTag("State.CriticalWeakness");
        registerCoreDerivedStats(derived);
        setup(attacker);
        setup(defender);
        bus.subscribeAll([this](const Event& e) { events.push_back(e); });
        cues.addHandler(
            [this](const CueEvent& e) { cueTags.push_back(e.tag); });
    }

    void setup(Actor& a) {
        a.core.strength = a.core.constitution = a.core.grace = 20.0f;
        a.core.dexterity = a.core.alacrity = a.core.perception = 20.0f;
        a.core.charisma = a.core.ego = a.core.insight = 20.0f;
        a.vitals.health = 300.0f;
        recomputeStats(a.core, a.vitals, a.system, derived, nullptr);
        a.combat.posture = currentValueOf(a.system, attr("maxPosture"));
    }

    StrikeContext ctx() {
        return StrikeContext { tags, derived, tuning, &bus, &cues };
    }

    // Attacker straight in FRONT of a +Z-facing defender at the origin.
    StrikeGeometry frontal(f32 guardSeconds = -1.0f) const {
        return StrikeGeometry { Vec3 { 0.0f, 0.0f, 2.0f }, Vec3 { 0.0f },
                                Vec3 { 0.0f, 0.0f, 1.0f }, guardSeconds,
                                Vec3 { 0.0f, 1.2f, 0.0f } };
    }

    void raiseGuard() {
        defender.system.tags.add(*tags.find("State.Blocking"), tags);
    }

    bool dispatched(std::string_view name) const {
        const EventKind kind = eventKind(name);
        for (const Event& e : events) {
            if (e.kind == kind) {
                return true;
            }
        }
        return false;
    }

    bool cued(std::string_view tag) const {
        for (const str& t : cueTags) {
            if (t == tag) {
                return true;
            }
        }
        return false;
    }
};

DamageEvent slash100() {
    DamageEvent event;
    event.channels = { { DamageType::Slash, 100.0f } };
    event.postureAmount = 20.0f;
    return event;
}

} // namespace

TEST_CASE("strike: a plain hit lands full damage, OnHitTaken and Cue.Hit") {
    Duel d;
    StatBlock atk = d.attacker.block();
    StatBlock def = d.defender.block();
    const StrikeOutcome out = resolveMeleeStrike(
        atk, def, {}, {}, slash100(), d.frontal(), d.ctx());
    // slash 100 through defense 10 + armor 10% = 81 (TypedDamageTest).
    CHECK(out.damage.healthDamage == doctest::Approx(81.0f));
    CHECK(!out.guard.caught);
    CHECK(!out.critical);
    CHECK(d.dispatched("OnHitTaken"));
    CHECK(!d.dispatched("OnParried"));
    CHECK(d.cued("Cue.Hit.Slash"));
}

TEST_CASE("strike: a raised front guard shrinks damage and reroutes it "
          "to posture (Cue.Block)") {
    Duel d;
    d.raiseGuard();
    StatBlock atk = d.attacker.block();
    StatBlock def = d.defender.block();
    const StrikeOutcome out = resolveMeleeStrike(
        atk, def, {}, {}, slash100(), d.frontal(10.0f), d.ctx());
    CHECK(out.guard.caught);
    CHECK(!out.guard.perfect);
    // Channel cut to 30 → mitigated 18; posture 20 + 70×0.6 = 62 < 70.
    CHECK(out.damage.healthDamage == doctest::Approx(18.0f));
    CHECK(out.damage.postureDamage == doctest::Approx(62.0f));
    CHECK(!out.damage.staggered);
    CHECK(d.cued("Cue.Block"));
    CHECK(d.dispatched("OnHitTaken"));
}

TEST_CASE("strike: a guard from BEHIND catches nothing, fresh or not") {
    Duel d;
    d.raiseGuard();
    StatBlock atk = d.attacker.block();
    StatBlock def = d.defender.block();
    StrikeGeometry geo = d.frontal(0.05f); // inside the perfect window...
    geo.attackerPos = Vec3 { 0.0f, 0.0f, -2.0f }; // ...but from behind
    const StrikeOutcome out = resolveMeleeStrike(
        atk, def, {}, {}, slash100(), geo, d.ctx());
    CHECK(!out.guard.caught);
    CHECK(!out.guard.perfect);
    CHECK(out.damage.healthDamage == doctest::Approx(81.0f));
}

TEST_CASE("strike: a perfect parry lands NOTHING on the defender and "
          "punishes the attacker's poise") {
    Duel d;
    d.raiseGuard();
    StatBlock atk = d.attacker.block();
    StatBlock def = d.defender.block();
    const StrikeOutcome out = resolveMeleeStrike(
        atk, def, {}, {}, slash100(), d.frontal(0.05f), d.ctx());
    CHECK(out.guard.perfect);
    // The defender is untouched — health AND posture.
    CHECK(currentValueOf(d.defender.system, attr("health")) ==
          doctest::Approx(300.0f));
    CHECK(d.defender.combat.posture == doctest::Approx(70.0f));
    // The attacker's poise pays perfectParryPosture (10).
    CHECK(out.riposte.postureDamage ==
          doctest::Approx(d.tuning.perfectParryPosture));
    CHECK(d.attacker.combat.posture == doctest::Approx(60.0f));
    CHECK(d.dispatched("OnParried"));
    // A clean catch is NOT a hit: no OnHitTaken(0) noise on the bus.
    CHECK(!d.dispatched("OnHitTaken"));
    CHECK(d.cued("Cue.Parry"));
    CHECK(!d.cued("Cue.Block"));
}

TEST_CASE("strike: a parry that breaks the attacker's poise dispatches "
          "OnStagger") {
    Duel d;
    d.raiseGuard();
    d.attacker.combat.posture = 5.0f; // one riposte from breaking
    StatBlock atk = d.attacker.block();
    StatBlock def = d.defender.block();
    const StrikeOutcome out = resolveMeleeStrike(
        atk, def, {}, {}, slash100(), d.frontal(0.05f), d.ctx());
    CHECK(out.guard.perfect);
    CHECK(out.riposte.staggered);
    CHECK(d.dispatched("OnStagger"));
}

TEST_CASE("strike: an EMPTY guard never parries and eats the posture "
          "punish (STATS.md §4)") {
    Duel d;
    d.raiseGuard();
    d.defender.vitals.energy = 0.0f; // exhausted guard
    recomputeStats(d.defender.core, d.defender.vitals, d.defender.system,
                   d.derived, nullptr);
    StatBlock atk = d.attacker.block();
    StatBlock def = d.defender.block();
    const StrikeOutcome out = resolveMeleeStrike(
        atk, def, {}, {}, slash100(), d.frontal(0.05f), d.ctx());
    CHECK(out.guard.caught);
    CHECK(!out.guard.perfect); // no energy, no finesse
    CHECK(out.guard.exhausted);
    // The punish lands ON TOP of the normal blocked routing (62).
    CHECK(out.damage.postureDamage > 62.0f);
}

TEST_CASE("strike: a defender in its critical window eats the critical "
          "execution — both camps") {
    Duel d;
    d.defender.system.tags.add(*d.tags.find("State.CriticalWeakness"),
                               d.tags);
    StatBlock atk = d.attacker.block();
    StatBlock def = d.defender.block();
    const StrikeOutcome out = resolveMeleeStrike(
        atk, def, {}, {}, slash100(), d.frontal(), d.ctx());
    CHECK(out.critical);
    CHECK(out.damage.healthDamage > 81.0f); // the execution bypasses armor
}

TEST_CASE("strike tail: the arrow path dispatches OnHitTaken and "
          "OnStagger on a posture break") {
    Duel d;
    StatBlock def = d.defender.block();
    DamageEvent arrow;
    arrow.channels = { { DamageType::Pierce, 40.0f } };
    arrow.postureAmount = 80.0f; // above maxPosture 70: breaks
    const DamageResult result = resolveStrikeDamage(
        def, {}, {}, arrow, Vec3 { 0.0f, 1.2f, 0.0f }, d.ctx());
    CHECK(result.staggered);
    CHECK(d.dispatched("OnHitTaken"));
    CHECK(d.dispatched("OnStagger"));
    CHECK(d.cued("Cue.Hit.Pierce"));
}

TEST_CASE("humanoid capsule: one shared shape, crouched = half height") {
    const Vec3 feet { 0.0f };
    // Chest height crosses a standing target...
    CHECK(segmentHitsActor(Vec3 { -1.0f, 1.2f, 0.0f },
                           Vec3 { 1.0f, 1.2f, 0.0f }, feet));
    // ...sails over a crouched one...
    CHECK(!segmentHitsActor(Vec3 { -1.0f, 1.2f, 0.0f },
                            Vec3 { 1.0f, 1.2f, 0.0f }, feet, true));
    // ...which is still hit low.
    CHECK(segmentHitsActor(Vec3 { -1.0f, 0.5f, 0.0f },
                           Vec3 { 1.0f, 0.5f, 0.0f }, feet, true));
    // And a swing far away misses everyone.
    CHECK(!segmentHitsActor(Vec3 { -1.0f, 1.2f, 5.0f },
                            Vec3 { 1.0f, 1.2f, 5.0f }, feet));
}

TEST_CASE("strike: sneak attack multiplies only when sneaking AND unaware") {
    // A State.Sneaking attacker + a
    // defender whose Perception never left Calm (the caller passes the
    // flat targetUnaware bool) -> every channel x sneakAttackMultiplier.
    Duel d;
    d.tags.registerTag("State.Sneaking");
    StatBlock atk = d.attacker.block();
    StatBlock def = d.defender.block();

    // Unaware target but a NON-sneaking attacker: normal hit (81).
    StrikeGeometry unaware = d.frontal();
    unaware.targetUnaware = true;
    StrikeOutcome out =
        resolveMeleeStrike(atk, def, {}, {}, slash100(), unaware, d.ctx());
    CHECK_FALSE(out.sneakAttack);
    CHECK(out.damage.healthDamage == doctest::Approx(81.0f));

    // Sneaking attacker + AWARE target: still a normal hit.
    d.attacker.system.tags.add(*d.tags.find("State.Sneaking"), d.tags);
    Actor& fresh = d.defender;
    d.setup(fresh); // reset health/posture between exchanges
    StatBlock def2 = d.defender.block();
    out = resolveMeleeStrike(atk, def2, {}, {}, slash100(), d.frontal(),
                             d.ctx());
    CHECK_FALSE(out.sneakAttack);
    CHECK(out.damage.healthDamage == doctest::Approx(81.0f));

    // Sneaking + unaware: slash 300 -> flat 10 -> 290 -> armor 10% = 261.
    d.setup(fresh);
    StatBlock def3 = d.defender.block();
    out = resolveMeleeStrike(atk, def3, {}, {}, slash100(), unaware,
                             d.ctx());
    CHECK(out.sneakAttack);
    CHECK(out.damage.healthDamage == doctest::Approx(261.0f));
}
