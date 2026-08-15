#include "gameplay/combat/MeleeSwing.hpp"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "gameplay/stats/Damage.hpp" // DamageEvent (applyBlock)

namespace gameplay {

void setSwingPhase(MeleeSwing& swing, SwingPhase phase) {
    if (phase == SwingPhase::Windup) {
        swing.struck.clear(); // a fresh swing hits everyone once again
    }
    swing.phase = phase;
    swing.t = 0.0f;
}

bool startSwing(MeleeSwing& swing) {
    if (swing.phase != SwingPhase::Idle) {
        return false;
    }
    setSwingPhase(swing, SwingPhase::Windup);
    return true;
}

void updateSwing(MeleeSwing& swing, f32 dt, const SwingTiming& timing) {
    swing.t += dt;
    switch (swing.phase) {
    case SwingPhase::Idle:
        swing.t = 0.0f;
        break;
    case SwingPhase::Windup:
        if (swing.t >= timing.windup) {
            setSwingPhase(swing, SwingPhase::Active);
        }
        break;
    case SwingPhase::Active:
        if (swing.t >= timing.active) {
            setSwingPhase(swing, SwingPhase::Recovery);
        }
        break;
    case SwingPhase::Recovery:
        if (swing.t >= timing.recovery) {
            setSwingPhase(swing, SwingPhase::Idle);
        }
        break;
    }
}

void onSwingAnimEvent(MeleeSwing& swing, std::string_view name) {
    if (name == "HitOpen" && swing.phase == SwingPhase::Windup) {
        setSwingPhase(swing, SwingPhase::Active);
    } else if (name == "HitClose" && swing.phase == SwingPhase::Active) {
        setSwingPhase(swing, SwingPhase::Recovery);
    }
}

f32 swingSweepT(const MeleeSwing& swing, const SwingTiming& timing) {
    if (swing.phase != SwingPhase::Active || timing.active <= 0.0f) {
        return 0.0f;
    }
    return glm::clamp(swing.t / timing.active, 0.0f, 1.0f);
}

bool registerStrike(MeleeSwing& swing, u64 targetId) {
    if (std::find(swing.struck.begin(), swing.struck.end(), targetId) !=
        swing.struck.end()) {
        return false;
    }
    swing.struck.push_back(targetId);
    return true;
}

namespace {

// The three authored poses of the simulated swing, actor-local
// [cpp-tuning]. Guard matches the static viewmodel (hand 0.30 right,
// 0.34 below, 0.55 ahead, blade tilted 28 degrees forward).
struct SocketPose {
    Vec3 position;
    Quat rotation;
};

SocketPose guardPose() {
    return { Vec3 { 0.30f, -0.34f, -0.55f },
             glm::angleAxis(glm::radians(28.0f),
                            Vec3 { 1.0f, 0.0f, 0.0f }) };
}

// Armed: pulled high right, blade PITCHED FORWARD for the strike
// (~90 degrees about the actor's left-right axis — local -X pitch,
// since local -Z is forward) and rolled right.
SocketPose armedPose() {
    return { Vec3 { 0.45f, -0.18f, -0.50f },
             glm::angleAxis(glm::radians(-80.0f),
                            Vec3 { 1.0f, 0.0f, 0.0f }) *
                 glm::angleAxis(glm::radians(-65.0f),
                                Vec3 { 0.0f, 0.0f, 1.0f }) };
}

// Follow-through: mirrored low left, blade still forward; Recovery's
// interpolation back to guard is what stands the sword upright again.
SocketPose throughPose() {
    return { Vec3 { -0.45f, -0.28f, -0.50f },
             glm::angleAxis(glm::radians(-80.0f),
                            Vec3 { 1.0f, 0.0f, 0.0f }) *
                 glm::angleAxis(glm::radians(65.0f),
                                Vec3 { 0.0f, 0.0f, 1.0f }) };
}

Mat4 compose(const SocketPose& a, const SocketPose& b, f32 t) {
    const Vec3 position = glm::mix(a.position, b.position, t);
    const Quat rotation = glm::slerp(a.rotation, b.rotation, t);
    return glm::translate(Mat4 { 1.0f }, position) * glm::mat4_cast(rotation);
}

// Ease-in-out keeps the simulated hand from teleporting between keys.
f32 smooth(f32 t) {
    t = glm::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

Mat4 swingSocketLocal(const MeleeSwing& swing, const SwingTiming& timing) {
    switch (swing.phase) {
    case SwingPhase::Idle:
        return compose(guardPose(), guardPose(), 0.0f); // = guard pose
    case SwingPhase::Windup: {
        const f32 t = timing.windup > 0.0f ? swing.t / timing.windup : 1.0f;
        return compose(guardPose(), armedPose(), smooth(t));
    }
    case SwingPhase::Active:
        // Linear across the damage window: the sweep IS the hit test's
        // travel — easing here would bunch the samples at the edges.
        return compose(armedPose(), throughPose(),
                       swingSweepT(swing, timing));
    case SwingPhase::Recovery: {
        const f32 t =
            timing.recovery > 0.0f ? swing.t / timing.recovery : 1.0f;
        return compose(throughPose(), guardPose(), smooth(t));
    }
    }
    return Mat4 { 1.0f };
}

bool segmentHitsCapsule(const Vec3& a0, const Vec3& a1, const Vec3& capA,
                        const Vec3& capB, f32 radius) {
    // Closest distance between segments (Ericson, Real-Time Collision
    // Detection 5.1.9), squared against the capsule radius.
    const Vec3 d1 = a1 - a0;
    const Vec3 d2 = capB - capA;
    const Vec3 r = a0 - capA;
    const f32 a = glm::dot(d1, d1);
    const f32 e = glm::dot(d2, d2);
    const f32 f = glm::dot(d2, r);
    f32 s = 0.0f;
    f32 t = 0.0f;
    constexpr f32 kEpsilon = 1e-8f;
    if (a <= kEpsilon && e <= kEpsilon) {
        // Both degenerate: point vs point.
    } else if (a <= kEpsilon) {
        t = glm::clamp(f / e, 0.0f, 1.0f);
    } else {
        const f32 c = glm::dot(d1, r);
        if (e <= kEpsilon) {
            s = glm::clamp(-c / a, 0.0f, 1.0f);
        } else {
            const f32 b = glm::dot(d1, d2);
            const f32 denom = a * e - b * b;
            s = denom > kEpsilon
                    ? glm::clamp((b * f - c * e) / denom, 0.0f, 1.0f)
                    : 0.0f;
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = glm::clamp(-c / a, 0.0f, 1.0f);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = glm::clamp((b - c) / a, 0.0f, 1.0f);
            }
        }
    }
    const Vec3 closest1 = a0 + d1 * s;
    const Vec3 closest2 = capA + d2 * t;
    const Vec3 gap = closest1 - closest2;
    return glm::dot(gap, gap) < radius * radius;
}

CapsuleSegment humanoidCapsule(const Vec3& feet, bool crouched) {
    constexpr f32 kRadius = 0.4f;
    const f32 height = crouched ? 0.9f : 1.8f;
    return { feet + Vec3 { 0.0f, kRadius, 0.0f },
             feet + Vec3 { 0.0f, height - kRadius, 0.0f }, kRadius };
}

bool segmentHitsActor(const Vec3& a0, const Vec3& a1, const Vec3& feet,
                      bool crouched) {
    const CapsuleSegment capsule = humanoidCapsule(feet, crouched);
    return segmentHitsCapsule(a0, a1, capsule.a, capsule.b, capsule.radius);
}

BlockResult applyBlock(DamageEvent& event, const Vec3& defenderFacing,
                       const Vec3& defenderPos, const Vec3& attackerPos,
                       f32 blockAngleDegrees, f32 blockFactor,
                       f32 blockPostureFactor, f32 guardSeconds,
                       f32 perfectWindow, f32 defenderEnergy,
                       f32 emptyGuardPosture) {
    // Horizontal cone: guards care about compass direction, not height.
    Vec3 facing { defenderFacing.x, 0.0f, defenderFacing.z };
    Vec3 to { attackerPos.x - defenderPos.x, 0.0f,
              attackerPos.z - defenderPos.z };
    const f32 facingLen = glm::length(facing);
    const f32 toLen = glm::length(to);
    if (facingLen < 1e-4f) {
        return {}; // no facing, no guard
    }
    // Point-blank counts as in front: the attacker is ON the defender.
    if (toLen >= 1e-4f) {
        const f32 cosHalf =
            std::cos(glm::radians(glm::clamp(blockAngleDegrees, 0.0f,
                                             360.0f) *
                                  0.5f));
        if (glm::dot(facing / facingLen, to / toLen) < cosHalf) {
            return {}; // outside the guard cone
        }
    }
    const bool exhausted = defenderEnergy <= 0.0f;
    // A freshly raised guard parries CLEAN: nothing through — not even
    // the weapon's own posture damage — and the caller punishes the
    // attacker's poise instead. An EMPTY guard never parries (STATS.md
    // §4: no energy, no finesse).
    if (!exhausted && guardSeconds >= 0.0f && perfectWindow > 0.0f &&
        guardSeconds <= perfectWindow) {
        for (DamageChannel& channel : event.channels) {
            channel.amount = 0.0f;
        }
        event.postureAmount = 0.0f;
        return { true, true, false };
    }
    f32 blocked = 0.0f;
    const f32 factor = glm::clamp(blockFactor, 0.0f, 1.0f);
    for (DamageChannel& channel : event.channels) {
        const f32 cut = channel.amount * factor;
        channel.amount -= cut;
        blocked += cut;
    }
    event.postureAmount += blocked * glm::max(blockPostureFactor, 0.0f);
    if (exhausted) {
        // The empty-guard punish: crit-sensitivity% of max posture on
        // top of the normal routing — the broken guard staggers.
        event.postureAmount += glm::max(emptyGuardPosture, 0.0f);
    }
    return { true, false, exhausted };
}

void tickGuard(MeleeSwing& swing, bool blocking, f32 dt) {
    if (!blocking) {
        swing.guardSeconds = -1.0f;
    } else if (swing.guardSeconds < 0.0f) {
        swing.guardSeconds = 0.0f; // raised THIS frame: the window opens
    } else {
        swing.guardSeconds += dt;
    }
}

Mat4 guardSocketLocal() {
    // Oblique across the front: hand pulled center-low, blade rolled so
    // the tip points up-left over the shoulder line, slightly laid
    // forward. [cpp-tuning]
    return glm::translate(Mat4 { 1.0f }, Vec3 { 0.16f, -0.34f, -0.50f }) *
           glm::mat4_cast(
               glm::angleAxis(glm::radians(-18.0f),
                              Vec3 { 1.0f, 0.0f, 0.0f }) *
               glm::angleAxis(glm::radians(52.0f),
                              Vec3 { 0.0f, 0.0f, 1.0f }));
}

} // namespace gameplay
