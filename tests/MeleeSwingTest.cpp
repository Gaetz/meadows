#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include "gameplay/combat/MeleeSwing.hpp"

// Chantier P0 A3/A4 — the blade-touch melee swing, sim-pure: the phase
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
    const SwingTiming timing { 10.0f, 10.0f, 0.2f }; // data would never fire
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

    // Idle = the A2 guard pose: hand bottom-right, blade up-forward.
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
