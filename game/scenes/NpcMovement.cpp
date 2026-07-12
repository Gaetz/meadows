#include "game/scenes/NpcMovement.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/physics/Physics.hpp"     // phys::PhysicsWorld/RayHit
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "game/scenes/NpcDirector.hpp"    // Npc, NpcContext
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp" // attr, currentValueOf
#include "gameplay/stats/StatsTuning.hpp"  // movementSpeedScale3D (U4-7)
#include "world/scene/Components.hpp"

namespace game {

bool moveNpcAlongPath(const NpcContext& ctx, Npc& npc, f32 dt,
                      f32 speedScale) {
    if (npc.pathIndex >= npc.path.size()) {
        return true;
    }
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
    const f32 walkSpeed =
        gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
        ctx.statsTuning.movementSpeedScale3D * ctx.statsTuning.npcWalkFactor *
        speedScale; // U4-7: §5-tunable

    const Vec3 goal = npc.path[npc.pathIndex];
    Vec3 to = goal - transform.position;
    to.y = 0.0f;
    const f32 distance = glm::length(to);
    if (distance < 0.35f) {
        ++npc.pathIndex;
        return npc.pathIndex >= npc.path.size();
    }
    const Vec3 dir = to / distance;
    transform.position += dir * glm::min(walkSpeed * dt, distance);
    transform.position.y = render::terrain::height(
        ctx.terrainParams, transform.position.x, transform.position.z);
    const f32 goalYaw = std::atan2(dir.x, dir.z);
    f32 delta = goalYaw - npc.yaw;
    while (delta > glm::pi<f32>()) {
        delta -= glm::two_pi<f32>();
    }
    while (delta < -glm::pi<f32>()) {
        delta += glm::two_pi<f32>();
    }
    npc.yaw += delta * (1.0f - std::exp(-8.0f * dt));
    transform.rotation = glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    npc.speed += (walkSpeed - npc.speed) * (1.0f - std::exp(-10.0f * dt));
    return false;
}

void moveNpcDirect(const NpcContext& ctx, Npc& npc, f32 dt,
                   const Vec3& direction, f32 speedScale, f32 faceYaw) {
    auto& transform = npc.entity.get_mut<world::Transform>();
    const auto& sys = npc.entity.get<gameplay::AbilitySystem>();
    const f32 walkSpeed =
        gameplay::currentValueOf(sys, gameplay::attr("movementSpeed")) *
        ctx.statsTuning.movementSpeedScale3D * ctx.statsTuning.npcWalkFactor *
        speedScale;
    transform.position += direction * walkSpeed * dt;
    transform.position.y = render::terrain::height(
        ctx.terrainParams, transform.position.x, transform.position.z);
    npc.yaw = faceYaw;
    transform.rotation = glm::angleAxis(npc.yaw, Vec3 { 0.0f, 1.0f, 0.0f });
    npc.speed += (walkSpeed - npc.speed) * (1.0f - std::exp(-10.0f * dt));
    npc.steered = true; // pathless but MOVING: skip the idle speed decay
}

bool steerBlocked(const NpcContext& ctx, const Vec3& from,
                  const Vec3& direction) {
    if (!ctx.physics) {
        return false;
    }
    const Vec3 chest = from + Vec3 { 0.0f, 0.9f, 0.0f };
    const phys::RayHit hit = ctx.physics->rayCast(chest, direction, 0.9f);
    return hit.hit;
}

} // namespace game
