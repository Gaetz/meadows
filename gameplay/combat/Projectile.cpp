#include "gameplay/combat/Projectile.hpp"

namespace gameplay {

Vec3 stepProjectile(Projectile& projectile, f32 dt) {
    const Vec3 from = projectile.position;
    if (projectile.planted) {
        projectile.plantedTtl -= dt;
        return from;
    }
    projectile.ttl -= dt;
    projectile.velocity.y -= projectile.gravity * dt;
    projectile.position += projectile.velocity * dt;
    return from;
}

bool projectileExpired(const Projectile& projectile) {
    return projectile.planted ? projectile.plantedTtl <= 0.0f
                              : projectile.ttl <= 0.0f;
}

} // namespace gameplay
