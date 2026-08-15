#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "gameplay/combat/MeleeSwing.hpp"
#include "gameplay/combat/Projectile.hpp"
#include "gameplay/stats/Damage.hpp" // DamageEvent (applyBlock)

// The blade-touch melee swing, sim-pure: the phase
// machine, the anim-event window override, the simulated socket arc, and
// the analytic segment-vs-capsule hit test.

using gameplay::MeleeSwing;
using gameplay::SwingPhase;
using gameplay::SwingTiming;

namespace {

// The blade tip in actor-local space for the swing's current socket —
// what actually travels the arc (blade = socket +Y, WeaponMeshes).
Vec3 bladeTip(const MeleeSwing& swing, const SwingTiming& timing,
              f32 bladeLength) {
    const Mat4 socket = gameplay::swingSocketLocal(swing, timing);
    const Vec3 grip = Vec3 { socket[3] };
    const Vec3 dir = glm::normalize(Vec3 { socket[1] });
    return grip + dir * bladeLength;
}

} // namespace

TEST_CASE("the swing runs Windup -> Active -> Recovery -> Idle on the "
          "weapon's data timings") {
    const SwingTiming timing { 0.25f, 0.20f, 0.35f };
    MeleeSwing swing;
    CHECK(swing.phase == SwingPhase::Idle);

    CHECK(gameplay::startSwing(swing));
    CHECK(swing.phase == SwingPhase::Windup);
    // A second press mid-swing is refused (one swing in flight).
    CHECK(!gameplay::startSwing(swing));

    gameplay::updateSwing(swing, 0.10f, timing);
    CHECK(swing.phase == SwingPhase::Windup);
    gameplay::updateSwing(swing, 0.20f, timing); // 0.30 > windup 0.25
    CHECK(swing.phase == SwingPhase::Active);
    gameplay::updateSwing(swing, 0.25f, timing); // > active 0.20
    CHECK(swing.phase == SwingPhase::Recovery);
    gameplay::updateSwing(swing, 0.40f, timing); // > recovery 0.35
    CHECK(swing.phase == SwingPhase::Idle);
    CHECK(gameplay::startSwing(swing)); // and it can go again
}

TEST_CASE("authored anim events override the data windows") {
    MeleeSwing swing;
    gameplay::startSwing(swing);

    // HitClose before the window opens is ignored.
    gameplay::onSwingAnimEvent(swing, "HitClose");
    CHECK(swing.phase == SwingPhase::Windup);
    // Any other event name is ignored too.
    gameplay::onSwingAnimEvent(swing, "Footstep");
    CHECK(swing.phase == SwingPhase::Windup);

    gameplay::onSwingAnimEvent(swing, "HitOpen");
    CHECK(swing.phase == SwingPhase::Active);
    gameplay::onSwingAnimEvent(swing, "HitOpen"); // re-open: no-op
    CHECK(swing.phase == SwingPhase::Active);
    gameplay::onSwingAnimEvent(swing, "HitClose");
    CHECK(swing.phase == SwingPhase::Recovery);
}

TEST_CASE("each target is struck once per swing, and a new swing resets") {
    MeleeSwing swing;
    gameplay::startSwing(swing);
    CHECK(gameplay::registerStrike(swing, 42));
    CHECK(!gameplay::registerStrike(swing, 42)); // dedup within the swing
    CHECK(gameplay::registerStrike(swing, 43));  // another target still lands

    const SwingTiming timing;
    gameplay::updateSwing(swing, 10.0f, timing); // -> Active
    gameplay::updateSwing(swing, 10.0f, timing); // -> Recovery
    gameplay::updateSwing(swing, 10.0f, timing); // -> Idle
    gameplay::startSwing(swing);
    CHECK(gameplay::registerStrike(swing, 42)); // fresh swing, fresh set
}

TEST_CASE("the simulated socket sweeps the blade right to left across "
          "Active") {
    const SwingTiming timing { 0.25f, 0.20f, 0.35f };
    MeleeSwing swing;

    // Idle = the guard pose: hand bottom-right, blade up-forward.
    const Mat4 guard = gameplay::swingSocketLocal(swing, timing);
    CHECK(guard[3].x == doctest::Approx(0.30f));
    CHECK(guard[3].y == doctest::Approx(-0.34f));

    gameplay::startSwing(swing);
    gameplay::setSwingPhase(swing, SwingPhase::Active);

    swing.t = 0.0f;
    const Vec3 start = bladeTip(swing, timing, 0.9f);
    swing.t = timing.active * 0.5f;
    const Vec3 middle = bladeTip(swing, timing, 0.9f);
    swing.t = timing.active;
    const Vec3 end = bladeTip(swing, timing, 0.9f);

    // The tip travels monotonically from the actor's right (+X) to its
    // left (-X) — the dev's right-to-left slash.
    CHECK(start.x > middle.x);
    CHECK(middle.x > end.x);
    CHECK(start.x > 0.3f);
    CHECK(end.x < -0.3f);
    // And stays in front of the actor (local -Z forward).
    CHECK(start.z < 0.0f);
    CHECK(end.z < 0.0f);
}

TEST_CASE("the blade segment hits a capsule only when it reaches it") {
    // An upright actor capsule: axis from 0.3 to 1.5 high, radius 0.35.
    const Vec3 capA { 0.0f, 0.3f, 0.0f };
    const Vec3 capB { 0.0f, 1.5f, 0.0f };
    const f32 radius = 0.35f;

    // A horizontal blade at chest height crossing the axis: hit.
    CHECK(gameplay::segmentHitsCapsule({ -1.0f, 1.2f, 0.0f },
                                       { 1.0f, 1.2f, 0.0f }, capA, capB,
                                       radius));
    // The same blade 30 cm short of the surface: miss.
    CHECK(!gameplay::segmentHitsCapsule({ -2.0f, 1.2f, 0.0f },
                                        { -0.7f, 1.2f, 0.0f }, capA, capB,
                                        radius));
    // The hitTolerance margin is what turns that miss into a hit: a 0.9 m
    // blade whose tip stops at 0.4 from the axis (inside radius 0.35 only
    // once extended by x1.2 -> tip at 0.28).
    const Vec3 grip { -1.3f, 1.2f, 0.0f };
    const Vec3 dir { 1.0f, 0.0f, 0.0f };
    const f32 blade = 0.9f;
    CHECK(!gameplay::segmentHitsCapsule(grip, grip + dir * blade, capA, capB,
                                        radius));
    CHECK(gameplay::segmentHitsCapsule(grip, grip + dir * (blade * 1.2f),
                                       capA, capB, radius));
    // Above the head: miss (the capsule ends, no infinite cylinder).
    CHECK(!gameplay::segmentHitsCapsule({ -1.0f, 2.4f, 0.0f },
                                        { 1.0f, 2.4f, 0.0f }, capA, capB,
                                        radius));
    // A stab along the blade axis straight into the chest: hit.
    CHECK(gameplay::segmentHitsCapsule({ 0.0f, 1.0f, -1.5f },
                                       { 0.0f, 1.0f, -0.2f }, capA, capB,
                                       radius));
}

TEST_CASE("a raised guard catches front-cone hits and reroutes them to "
          "posture") {
    // Defender at the origin facing +Z; 120-degree guard, 70% reduction,
    // 60% of the blocked amount lands on posture.
    const Vec3 facing { 0.0f, 0.0f, 1.0f };
    const Vec3 defender { 0.0f, 0.0f, 0.0f };
    const auto freshEvent = [] {
        gameplay::DamageEvent event;
        event.channels = { { gameplay::DamageType::Slash, 10.0f },
                           { gameplay::DamageType::Fire, 4.0f } };
        event.postureAmount = 5.0f;
        return event;
    };

    // Straight ahead: caught. Channels x0.3, posture gains 14x0.7x0.6.
    gameplay::DamageEvent front = freshEvent();
    CHECK(gameplay::applyBlock(front, facing, defender,
                               { 0.0f, 0.0f, 3.0f }, 120.0f, 0.7f, 0.6f)
              .caught);
    CHECK(front.channels[0].amount == doctest::Approx(3.0f));
    CHECK(front.channels[1].amount == doctest::Approx(1.2f));
    CHECK(front.postureAmount == doctest::Approx(5.0f + 9.8f * 0.6f));

    // 50 degrees off-axis is still inside the 120-degree cone...
    gameplay::DamageEvent inside = freshEvent();
    CHECK(gameplay::applyBlock(
              inside, facing, defender,
              { 3.0f * std::sin(glm::radians(50.0f)), 0.0f,
                3.0f * std::cos(glm::radians(50.0f)) },
              120.0f, 0.7f, 0.6f)
              .caught);

    // ...70 degrees is not: the event passes through untouched.
    gameplay::DamageEvent outside = freshEvent();
    CHECK(!gameplay::applyBlock(
               outside, facing, defender,
               { 3.0f * std::sin(glm::radians(70.0f)), 0.0f,
                 3.0f * std::cos(glm::radians(70.0f)) },
               120.0f, 0.7f, 0.6f)
               .caught);
    CHECK(outside.channels[0].amount == doctest::Approx(10.0f));
    CHECK(outside.postureAmount == doctest::Approx(5.0f));

    // From behind: never caught.
    gameplay::DamageEvent behind = freshEvent();
    CHECK(!gameplay::applyBlock(behind, facing, defender,
                                { 0.0f, 0.0f, -3.0f }, 120.0f, 0.7f, 0.6f)
               .caught);

    // The attacker's HEIGHT is irrelevant (horizontal cone): a hit from
    // above-front is still caught.
    gameplay::DamageEvent above = freshEvent();
    CHECK(gameplay::applyBlock(above, facing, defender,
                               { 0.0f, 2.0f, 3.0f }, 120.0f, 0.7f, 0.6f)
              .caught);

    // Point-blank counts as in front.
    gameplay::DamageEvent contact = freshEvent();
    CHECK(gameplay::applyBlock(contact, facing, defender, defender, 120.0f,
                               0.7f, 0.6f)
              .caught);
}

TEST_CASE("a FRESH guard perfect-parries: nothing through, and the guard "
          "clock decides") {
    const Vec3 facing { 0.0f, 0.0f, 1.0f };
    const Vec3 defender { 0.0f, 0.0f, 0.0f };
    const Vec3 attacker { 0.0f, 0.0f, 2.0f };
    const auto freshEvent = [] {
        gameplay::DamageEvent event;
        event.channels = { { gameplay::DamageType::Blunt, 8.0f } };
        event.postureAmount = 25.0f; // the weapon's own posture damage
        return event;
    };

    // Guard raised 0.1 s ago, window 0.2: PERFECT — channels AND the
    // weapon's posture damage both zeroed (the defender takes nothing).
    gameplay::DamageEvent parried = freshEvent();
    const gameplay::BlockResult perfect = gameplay::applyBlock(
        parried, facing, defender, attacker, 120.0f, 0.7f, 0.6f, 0.1f,
        0.2f);
    CHECK(perfect.caught);
    CHECK(perfect.perfect);
    CHECK(parried.channels[0].amount == doctest::Approx(0.0f));
    CHECK(parried.postureAmount == doctest::Approx(0.0f));

    // A guard held too long is an ordinary block.
    gameplay::DamageEvent held = freshEvent();
    const gameplay::BlockResult ordinary = gameplay::applyBlock(
        held, facing, defender, attacker, 120.0f, 0.7f, 0.6f, 1.5f, 0.2f);
    CHECK(ordinary.caught);
    CHECK(!ordinary.perfect);
    CHECK(held.channels[0].amount == doctest::Approx(2.4f));

    // A fresh guard facing the WRONG way parries nothing.
    gameplay::DamageEvent behind = freshEvent();
    CHECK(!gameplay::applyBlock(behind, facing, defender,
                                { 0.0f, 0.0f, -2.0f }, 120.0f, 0.7f, 0.6f,
                                0.1f, 0.2f)
               .caught);

    // The guard clock: down -> raised opens at 0 and ages; dropping
    // resets to -1 (the sentinel applyBlock reads as "no guard").
    gameplay::MeleeSwing swing;
    CHECK(swing.guardSeconds == doctest::Approx(-1.0f));
    gameplay::tickGuard(swing, true, 0.5f); // raised THIS frame
    CHECK(swing.guardSeconds == doctest::Approx(0.0f));
    gameplay::tickGuard(swing, true, 0.16f);
    CHECK(swing.guardSeconds == doctest::Approx(0.16f));
    gameplay::tickGuard(swing, false, 0.16f);
    CHECK(swing.guardSeconds == doctest::Approx(-1.0f));
}

TEST_CASE("an EMPTY guard never parries and eats the crit-sens posture "
          "punish (STATS.md 4)") {
    const Vec3 facing { 0.0f, 0.0f, 1.0f };
    const Vec3 defender { 0.0f, 0.0f, 0.0f };
    const Vec3 attacker { 0.0f, 0.0f, 2.0f };
    gameplay::DamageEvent event;
    event.channels = { { gameplay::DamageType::Blunt, 10.0f } };
    event.postureAmount = 20.0f;

    // Fresh guard, perfect window open — but ZERO energy: no parry, the
    // hit is blocked normally AND the guard takes the punish (12.5 =
    // maxPosture 50 x critSens 25%).
    const gameplay::BlockResult broke = gameplay::applyBlock(
        event, facing, defender, attacker, 120.0f, 0.7f, 0.6f,
        /*guardSeconds=*/0.05f, /*perfectWindow=*/0.2f,
        /*defenderEnergy=*/0.0f, /*emptyGuardPosture=*/12.5f);
    CHECK(broke.caught);
    CHECK(!broke.perfect);
    CHECK(broke.exhausted);
    CHECK(event.channels[0].amount == doctest::Approx(3.0f));
    // 20 weapon posture + 7x0.6 routed + 12.5 punish.
    CHECK(event.postureAmount == doctest::Approx(20.0f + 4.2f + 12.5f));

    // With energy in the tank the same timing parries clean.
    gameplay::DamageEvent parried;
    parried.channels = { { gameplay::DamageType::Blunt, 10.0f } };
    const gameplay::BlockResult fine = gameplay::applyBlock(
        parried, facing, defender, attacker, 120.0f, 0.7f, 0.6f, 0.05f,
        0.2f, /*defenderEnergy=*/40.0f, 12.5f);
    CHECK(fine.perfect);
    CHECK(!fine.exhausted);
}

TEST_CASE("projectile ballistics: gravity arc, plant, expiry") {
    gameplay::Projectile arrow;
    arrow.position = { 0.0f, 2.0f, 0.0f };
    arrow.velocity = { 10.0f, 0.0f, 0.0f };
    arrow.gravity = 10.0f;
    arrow.ttl = 1.0f;

    // One step: the returned segment start is the PREVIOUS position.
    const Vec3 from = gameplay::stepProjectile(arrow, 0.1f);
    CHECK(from.x == doctest::Approx(0.0f));
    CHECK(arrow.position.x == doctest::Approx(1.0f));
    // Gravity bends the arc down.
    CHECK(arrow.velocity.y == doctest::Approx(-1.0f));
    CHECK(arrow.position.y < 2.0f);

    // Flight ttl runs out -> expired.
    gameplay::stepProjectile(arrow, 1.0f);
    CHECK(gameplay::projectileExpired(arrow));

    // A planted arrow stops flying and lingers on its own clock.
    gameplay::Projectile stuck;
    stuck.position = { 3.0f, 1.0f, 0.0f };
    stuck.velocity = { 20.0f, 0.0f, 0.0f };
    stuck.planted = true;
    stuck.plantedTtl = 0.5f;
    const Vec3 at = stuck.position;
    gameplay::stepProjectile(stuck, 0.2f);
    CHECK(stuck.position.x == doctest::Approx(at.x)); // frozen
    CHECK(!gameplay::projectileExpired(stuck));
    gameplay::stepProjectile(stuck, 0.4f);
    CHECK(gameplay::projectileExpired(stuck));
}
