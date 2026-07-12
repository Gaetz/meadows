#include "world/ai/Perception.hpp"

#include <cmath>

#include <glm/glm.hpp>

namespace world {

void setAwareState(Perception& perception, AwareState state) {
    perception.state = static_cast<i32>(state);
    perception.stateSeconds = 0.0f;
}

bool inViewCone(const Perception& perception, const Vec3& selfPos,
                const Vec3& selfFacing, const Vec3& targetPos) {
    const Vec3 to = targetPos - selfPos;
    const f32 distanceSq = glm::dot(to, to);
    if (distanceSq > perception.viewDistance * perception.viewDistance) {
        return false;
    }
    if (distanceSq < 1e-6f) {
        return true; // on top of us: seen
    }
    Vec3 facing { selfFacing.x, 0.0f, selfFacing.z };
    const f32 facingLen = glm::length(facing);
    if (facingLen < 1e-4f) {
        return false;
    }
    const f32 cosHalf = std::cos(
        glm::radians(glm::clamp(perception.viewAngleDegrees, 0.0f, 360.0f) *
                     0.5f));
    return glm::dot(to / std::sqrt(distanceSq), facing / facingLen) >=
           cosHalf;
}

void updatePerception(Perception& perception, bool canSee,
                      const Vec3& targetPos, f32 dt) {
    perception.stateSeconds += dt;
    if (canSee) {
        perception.sinceSeen = 0.0f;
        perception.lastKnownPos = targetPos;
        if (awareState(perception) != AwareState::Alert) {
            setAwareState(perception, AwareState::Alert);
        }
        return;
    }
    perception.sinceSeen += dt;
    switch (awareState(perception)) {
    case AwareState::Calm:
        break;
    case AwareState::Suspicious:
        if (perception.stateSeconds >= perception.searchSeconds) {
            setAwareState(perception, AwareState::Calm);
        }
        break;
    case AwareState::Alert:
        // Sight memory: keep hunting the last known position for a
        // while, then drop to an explicit search of that spot.
        if (perception.sinceSeen >= perception.memorySeconds) {
            setAwareState(perception, AwareState::Searching);
        }
        break;
    case AwareState::Searching:
        if (perception.stateSeconds >= perception.searchSeconds) {
            setAwareState(perception, AwareState::Calm);
        }
        break;
    }
}

void hearNoise(Perception& perception, const Vec3& selfPos,
               const Vec3& position, f32 loudness) {
    const Vec3 gap = position - selfPos;
    const f32 reach = perception.hearingRadius * glm::max(loudness, 0.0f);
    if (glm::dot(gap, gap) > reach * reach) {
        return; // too far (or too quiet) to hear
    }
    switch (awareState(perception)) {
    case AwareState::Calm:
        perception.lastKnownPos = position;
        setAwareState(perception, AwareState::Suspicious);
        break;
    case AwareState::Suspicious:
    case AwareState::Searching:
        perception.lastKnownPos = position; // re-aim the investigation
        // Re-enter the state through THE transition function: the
        // patience clock restarts.
        setAwareState(perception, awareState(perception));
        break;
    case AwareState::Alert:
        break; // it already knows better than a noise
    }
}

void alertTo(Perception& perception, const Vec3& position) {
    perception.lastKnownPos = position;
    perception.sinceSeen = 0.0f; // a comrade's report is fresh intel
    setAwareState(perception, AwareState::Alert);
}

} // namespace world
