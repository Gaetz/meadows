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

} // namespace gameplay
