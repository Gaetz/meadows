#include "game/scenes/NpcMovement.hpp"

#include <cmath>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/physics/Physics.hpp"     // phys::PhysicsWorld/RayHit
#include "engine/render/landscape/TerrainNoise.hpp" // terrain::height
#include "game/scenes/NpcDirector.hpp"    // Npc, NpcContext
#include "gameplay/ability/AbilitySystem.hpp"
#include "gameplay/ability/Attributes.hpp" // attr, currentValueOf
#include "gameplay/stats/StatsTuning.hpp"  // movementSpeedScale3D
#include "world/scene/Components.hpp"

namespace game {

// Contract in the header.
bool groundAt(const render::TerrainParams& terrain, bool interiorMode,
              phys::PhysicsWorld* physics, Vec3& position) {
    if (!interiorMode) {
        position.y =
            render::terrain::height(terrain, position.x, position.z);
        return true;
    }
    if (!physics) {
        return true; // headless: authored y is trusted
    }
    // 1.8 = 1.2 above + 0.6 below the feet: one step's worth. A longer
    // probe let an NPC ground onto surfaces meters below through a gap
    // (a shelf in a lower room's ceiling) instead of refusing the ledge.
    const phys::RayHit hit =
        physics->rayCast(position + Vec3 { 0.0f, 1.2f, 0.0f },
                         { 0.0f, -1.0f, 0.0f }, 1.8f);
    // An UPWARD snap past the step budget is a wall, not a step: the
    // cavern walls flare at the base, and accepting those hits walked
    // NPCs up the flare and into the rock.
    if (!hit.hit || hit.position.y > position.y + 0.5f) {
        return false;
    }
    position.y = hit.position.y;
    return true;
}

bool groundNpc(const NpcContext& ctx, Vec3& position) {
    return groundAt(ctx.terrainParams, ctx.interiorMode, ctx.physics,
                    position);
}

namespace {

// Kinematic depenetration for cavern walls: eight chest-height probes; a
// hit inside the capsule's radius shoves the NPC back out along the probe.
// The step-refusal gates stop them from ENTERING head-on, this backs them
// off the grazing overlaps those rays can't see (curved walls, corner-
// cutting between waypoints). Interior-only: the noisy tube walls are the
// case; outdoors the blockers are authored boxes steerBlocked handles.
void resolveWallOverlap(const NpcContext& ctx, Vec3& position) {
    if (!ctx.interiorMode || !ctx.physics) {
        return;
    }
    constexpr f32 kRadius = 0.5f; // capsule + skin margin
    for (u32 i = 0; i < 8; ++i) {
        const f32 a = static_cast<f32>(i) * (glm::two_pi<f32>() / 8.0f);
        const Vec3 dir { std::cos(a), 0.0f, std::sin(a) };
        const Vec3 chest = position + Vec3 { 0.0f, 0.9f, 0.0f };
        const phys::RayHit hit = ctx.physics->rayCast(chest, dir, kRadius);
        if (hit.hit) {
            position -= dir * (kRadius - hit.distance);
        }
    }
}

} // namespace

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
        speedScale; // §5-tunable

    const Vec3 goal = npc.path[npc.pathIndex];
    Vec3 to = goal - transform.position;
    to.y = 0.0f;
    const f32 distance = glm::length(to);
    if (distance < 0.35f) {
        ++npc.pathIndex;
        return npc.pathIndex >= npc.path.size();
    }
    const Vec3 dir = to / distance;
    const Vec3 before = transform.position;
    transform.position += dir * glm::min(walkSpeed * dt, distance);
    resolveWallOverlap(ctx, transform.position);
    if (!groundNpc(ctx, transform.position)) {
        transform.position = before; // ledge: hold the edge, keep facing
    }
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
    const Vec3 before = transform.position;
    transform.position += direction * walkSpeed * dt;
    resolveWallOverlap(ctx, transform.position);
    if (!groundNpc(ctx, transform.position)) {
        transform.position = before; // ledge: hold the edge, keep facing
    }
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
    // Three rays across the capsule's width: the single center ray ran
    // parallel to a curving wall while the shoulder sank into it.
    const Vec3 chest = from + Vec3 { 0.0f, 0.9f, 0.0f };
    const Vec3 flat { direction.z, 0.0f, -direction.x };
    const f32 flatLen = glm::length(flat);
    const Vec3 side =
        flatLen > 0.001f ? flat * (0.35f / flatLen) : Vec3 { 0.0f };
    for (const Vec3& origin : { chest - side, chest, chest + side }) {
        if (ctx.physics->rayCast(origin, direction, 0.9f).hit) {
            return true;
        }
    }
    return false;
}

} // namespace game
