#include "gameplay/actors/Followers.hpp"

#include <glm/glm.hpp>

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

} // namespace gameplay
